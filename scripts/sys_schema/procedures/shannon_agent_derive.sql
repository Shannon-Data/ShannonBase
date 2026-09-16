-- Copyright (c) 2026, Shannon Data AI and/or its affiliates.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

DROP PROCEDURE IF EXISTS shannon_agent_derive;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE shannon_agent_derive (
    IN in_batch INT,
    IN in_options JSON
)
COMMENT '
Description
-----------
Drains mysql.agent_derive_queue: fills in the embeddings that the agent did
not compute on the user turn.

The agent enqueues a task for every row that should carry a vector and does
not -- because the embedding model was unavailable when the row was written,
or because the row predates embedding being enabled at all. A row with no
vector is invisible to semantic recall, permanently, until something fills it
in. This is that something.

It is plain SQL on purpose. The obvious alternative, a LANGUAGE JAVASCRIPT
routine reusing the agent code, cannot be scheduled: JerryScript keeps one
heap for the whole server, so a second routine entered while a conversation is
running aborts mysqld. An EVENT that fires mid-conversation is exactly that
case, so nothing on this path may be JavaScript.

Runs as the caller (SQL SECURITY INVOKER), so it drains only what the caller
is allowed to write. The work is idempotent and the queue is shared, so two
concurrent drains are safe: each claims a disjoint batch under a lease.

Parameters
-----------
in_batch (INT):
  Tasks to claim in this run. Defaults to 100, capped at 1000 -- a batch is
  one ML_EMBED_ROW-bearing UPDATE per kind, and a failure fails the batch.

in_options (JSON):
  - model_id:      embedding model; defaults to multilingual-e5-small.
  - lease_seconds: how long a claim is held before another drainer may take
                   it (default 300). A drain killed mid-batch returns its
                   tasks to the queue after this long, not never.
  - max_attempts:  attempts before a task is parked in state=failed
                   (default 5). Parked, not deleted: a model that was
                   misconfigured for a week should be recoverable by fixing
                   the configuration and resetting the state, not by having
                   lost the backlog.
  - purge_done_days: how long completed tasks are kept (default 7).

Returns
-----------
One row: claimed, embedded, failed, and the remaining backlog.

Example
-----------
mysql> CALL sys.shannon_agent_derive(200, NULL);

To run it in the background, give it to the scheduler. This is deliberately
not done for you -- it writes to mysql.agent_* as its DEFINER and how often
that should happen is an operator decision:

  CREATE EVENT sys.shannon_agent_derive_tick
    ON SCHEDULE EVERY 1 MINUTE
    DO CALL sys.shannon_agent_derive(100, NULL);

The event runs as its own definer, so create it as an account that holds
UPDATE on mysql.agent_memory and mysql.agent_semantic_fact.
'
SQL SECURITY INVOKER
MODIFIES SQL DATA
BEGIN
  DECLARE v_token         VARCHAR(64);
  DECLARE v_model         VARCHAR(64);
  DECLARE v_batch         INT;
  DECLARE v_lease         INT;
  DECLARE v_max_attempts  INT;
  DECLARE v_purge_days    INT;
  DECLARE v_claimed       INT DEFAULT 0;
  DECLARE v_embedded      INT DEFAULT 0;
  DECLARE v_failed        INT DEFAULT 0;
  DECLARE v_pending       INT DEFAULT 0;
  DECLARE v_err           VARCHAR(512) DEFAULT '';
  DECLARE v_batch_failed  TINYINT DEFAULT 0;

  -- A batch that cannot be embedded is the expected case, not an exception:
  -- it means the model is not loaded. Catch it, park the batch with backoff
  -- and report, rather than propagating an error to a scheduler that will
  -- only log it somewhere nobody reads.
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
  BEGIN
    GET DIAGNOSTICS CONDITION 1 v_err = MESSAGE_TEXT;
    SET v_batch_failed = 1;
  END;

  SET v_batch        = LEAST(GREATEST(COALESCE(in_batch, 100), 1), 1000);
  SET v_model        = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.model_id')),
                                'multilingual-e5-small');
  SET v_lease        = COALESCE(JSON_EXTRACT(in_options, '$.lease_seconds'), 300);
  SET v_max_attempts = COALESCE(JSON_EXTRACT(in_options, '$.max_attempts'), 5);
  SET v_purge_days   = COALESCE(JSON_EXTRACT(in_options, '$.purge_done_days'), 7);
  SET v_token        = CONCAT(CONNECTION_ID(), '-', UNIX_TIMESTAMP(), '-',
                              SUBSTRING(SHA2(RAND(), 256), 1, 8));

  -- 1. Reclaim tasks whose drainer died holding them.
  UPDATE mysql.agent_derive_queue
     SET state = 'pending', lease_owner = '', lease_expires_at = NULL
   WHERE state = 'running' AND lease_expires_at IS NOT NULL AND lease_expires_at < NOW();

  -- 2. Claim a batch. The token is what makes the claim exclusive: every
  --    statement below addresses rows by lease_owner, never by state alone,
  --    so a concurrent drainer's batch is invisible to this one.
  UPDATE mysql.agent_derive_queue
     SET state = 'running', lease_owner = v_token,
         lease_expires_at = NOW() + INTERVAL v_lease SECOND,
         attempts = attempts + 1
   WHERE state = 'pending' AND available_at <= NOW()
   ORDER BY task_id
   LIMIT v_batch;
  SET v_claimed = ROW_COUNT();

  IF v_claimed > 0 THEN
    -- 3. A task whose target row is gone is complete, not stuck. Retention
    --    deletes expired memory, and without this the queue would retry a
    --    vanished row until it hit max_attempts.
    UPDATE mysql.agent_derive_queue q
      LEFT JOIN mysql.agent_memory m ON m.id = q.target_id
       SET q.state = 'done', q.lease_owner = '', q.lease_expires_at = NULL
     WHERE q.lease_owner = v_token AND q.kind = 'embed_memory' AND m.id IS NULL;

    UPDATE mysql.agent_derive_queue q
      LEFT JOIN mysql.agent_semantic_fact f ON f.fact_id = q.target_id
       SET q.state = 'done', q.lease_owner = '', q.lease_expires_at = NULL
     WHERE q.lease_owner = v_token AND q.kind = 'embed_fact' AND f.fact_id IS NULL;

    -- 4. The derivation itself, one set-based UPDATE per kind. LEFT() bounds
    --    the model input the same way the agent does when it embeds inline,
    --    so a row embedded here and a row embedded there get the same vector.
    UPDATE mysql.agent_memory m
      JOIN mysql.agent_derive_queue q
        ON q.lease_owner = v_token AND q.kind = 'embed_memory' AND q.target_id = m.id
       SET m.embedding = sys.ML_EMBED_ROW(LEFT(m.content, 1800),
                                          JSON_OBJECT('model_id', v_model, 'truncate', true)),
           m.embed_model_id = v_model
     WHERE m.embedding IS NULL AND m.content IS NOT NULL AND m.content <> '';
    SET v_embedded = v_embedded + ROW_COUNT();

    UPDATE mysql.agent_semantic_fact f
      JOIN mysql.agent_derive_queue q
        ON q.lease_owner = v_token AND q.kind = 'embed_fact' AND q.target_id = f.fact_id
       SET f.embedding = sys.ML_EMBED_ROW(LEFT(f.statement, 1800),
                                          JSON_OBJECT('model_id', v_model, 'truncate', true))
     WHERE f.embedding IS NULL AND f.statement <> '';
    SET v_embedded = v_embedded + ROW_COUNT();

    -- 5. Completion is verified against the target, not assumed from the
    --    UPDATE returning without error: ROW_COUNT() counts rows changed,
    --    and a row whose embedding came back NULL is not done.
    UPDATE mysql.agent_derive_queue q
      JOIN mysql.agent_memory m ON m.id = q.target_id
       SET q.state = 'done', q.lease_owner = '', q.lease_expires_at = NULL, q.last_error = NULL
     WHERE q.lease_owner = v_token AND q.kind = 'embed_memory' AND m.embedding IS NOT NULL;

    UPDATE mysql.agent_derive_queue q
      JOIN mysql.agent_semantic_fact f ON f.fact_id = q.target_id
       SET q.state = 'done', q.lease_owner = '', q.lease_expires_at = NULL, q.last_error = NULL
     WHERE q.lease_owner = v_token AND q.kind = 'embed_fact' AND f.embedding IS NOT NULL;

    -- 6. Whatever is still held by this token did not derive. Park it past
    --    max_attempts, otherwise back off and let a later run retry.
    UPDATE mysql.agent_derive_queue
       SET state = IF(attempts >= v_max_attempts, 'failed', 'pending'),
           lease_owner = '', lease_expires_at = NULL,
           last_error = LEFT(NULLIF(v_err, ''), 512),
           available_at = NOW() + INTERVAL LEAST(POW(2, attempts), 3600) SECOND
     WHERE lease_owner = v_token AND state = 'running';
    SET v_failed = ROW_COUNT();
  END IF;

  -- 7. Completed tasks are a log, and a log needs an end.
  DELETE FROM mysql.agent_derive_queue
   WHERE state = 'done' AND updated_at < NOW() - INTERVAL v_purge_days DAY
   LIMIT 1000;

  SELECT COUNT(*) INTO v_pending FROM mysql.agent_derive_queue WHERE state = 'pending';

  SELECT v_claimed  AS claimed,
         v_embedded AS embedded,
         v_failed   AS deferred_or_failed,
         v_pending  AS pending_backlog,
         NULLIF(v_err, '') AS last_error;
END$$

DELIMITER ;
