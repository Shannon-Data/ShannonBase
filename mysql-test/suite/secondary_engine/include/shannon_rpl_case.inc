# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
--connection master
CREATE TABLE rpl_rapid(id INT PRIMARY KEY, v BIGINT NOT NULL);
INSERT INTO rpl_rapid VALUES(1,10),(2,20),(3,30);
--source include/rpl/sync_to_replica.inc
# Replicated source load requires an engine on both ends. Unload and remove
# it locally on the replica before DML when that end is disabled.
--connection master
if ($source_enabled)
{
  ALTER TABLE rpl_rapid SECONDARY_ENGINE=rapid;
  ALTER TABLE rpl_rapid SECONDARY_LOAD;
  --source include/rpl/sync_to_replica.inc
  if (!$replica_enabled)
  {
    SET sql_log_bin=0;
    ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
    ALTER TABLE rpl_rapid SECONDARY_ENGINE=NULL;
    SET sql_log_bin=1;
  }
}
--connection slave
if (!$source_enabled)
{
  if ($replica_enabled)
  {
    ALTER TABLE rpl_rapid SECONDARY_ENGINE=rapid;
    ALTER TABLE rpl_rapid SECONDARY_LOAD;
  }
}
--connection master
INSERT INTO rpl_rapid VALUES(4,40);
UPDATE rpl_rapid SET v=v+1 WHERE id=1;
DELETE FROM rpl_rapid WHERE id=2;
BEGIN;
INSERT INTO rpl_rapid VALUES(5,50);
UPDATE rpl_rapid SET v=v+7 WHERE id=3;
DELETE FROM rpl_rapid WHERE id=4;
COMMIT;
BEGIN;
UPDATE rpl_rapid SET v=999;
INSERT INTO rpl_rapid VALUES(6,60);
ROLLBACK;
--source include/rpl/sync_to_replica.inc
--source include/rpl/assert_replica_no_error.inc
--echo # Replica primary result
SELECT id,v FROM rpl_rapid ORDER BY id;
if ($replica_enabled)
{
  --echo # Replica Rapid result
  SET use_secondary_engine=FORCED;
  SELECT id,v FROM rpl_rapid ORDER BY id;
  --let $actual= `SELECT COUNT(*)=3 AND SUM(v)=98 AND SUM(id)=9 FROM rpl_rapid`
  if (!$actual)
  {
    --die Rapid result does not match committed replicated data
  }
  SET use_secondary_engine=OFF;
}
--connection master
--echo # Source primary result
SELECT id,v FROM rpl_rapid ORDER BY id;
if ($source_enabled)
{
  --echo # Source Rapid result
  SET use_secondary_engine=FORCED;
  SELECT id,v FROM rpl_rapid ORDER BY id;
  --let $actual= `SELECT COUNT(*)=3 AND SUM(v)=98 AND SUM(id)=9 FROM rpl_rapid`
  if (!$actual)
  {
    --die Rapid result does not match committed replicated data
  }
  SET use_secondary_engine=OFF;
  SET sql_log_bin=0;
  ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
  SET sql_log_bin=1;
}
--connection slave
if ($replica_enabled)
{
  SET sql_log_bin=0;
  ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
  SET sql_log_bin=1;
}
--connection master
DROP TABLE rpl_rapid;
--source include/rpl/sync_to_replica.inc
