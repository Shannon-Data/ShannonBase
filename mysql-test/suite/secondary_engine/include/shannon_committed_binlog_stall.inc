# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
--source include/have_debug.inc
--source include/no_ps_protocol.inc
CALL mtr.add_suppression("Rapid COMMITTED_BINLOG: COMMITTED_BINLOG consumer synchronization timed out");
SET @saved_debug=@@GLOBAL.debug;
SET use_secondary_engine=OFF;
CREATE DATABASE cb_stall;
USE cb_stall;
CREATE TABLE a(id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
CREATE TABLE b(id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
INSERT INTO a VALUES(1,10);
INSERT INTO b VALUES(1,20);
ALTER TABLE a SECONDARY_LOAD;
ALTER TABLE b SECONDARY_LOAD;
SET use_secondary_engine=FORCED;
SELECT a.v,b.v FROM a JOIN b USING(id);
--let $assert_query = SELECT a.v,b.v FROM a JOIN b USING(id)
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
SET use_secondary_engine=OFF;
--eval SET GLOBAL debug='+d,$stall_debug'
--disable_query_log
SET @tp_started=NOW(6);
BEGIN;
UPDATE a SET v=11;
UPDATE b SET v=21;
COMMIT;
SET @tp_elapsed=TIMESTAMPDIFF(MICROSECOND,@tp_started,NOW(6));
--enable_query_log
SELECT @tp_elapsed < 2000000 AS primary_commit_did_not_wait_for_apply;
--let $primary_commit_fast = `SELECT @tp_elapsed < 2000000`
if ($primary_commit_fast != 1) {
  --die Primary commit waited for stalled background apply
}
--let $wait_condition = SELECT COUNT(*)=2 FROM performance_schema.rpd_tables t JOIN performance_schema.rpd_table_id i ON t.ID=i.ID WHERE i.SCHEMA_NAME='cb_stall' AND t.STALE_REASON='RELOAD_REQUIRED'
--source include/wait_condition.inc
SET use_secondary_engine=FORCED;
--error ER_SECONDARY_ENGINE
SELECT a.v,b.v FROM a JOIN b USING(id);
SET use_secondary_engine=ON;
SELECT a.v,b.v FROM a JOIN b USING(id);
--let $assert_query = SELECT a.v,b.v FROM a JOIN b USING(id)
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
SET use_secondary_engine=OFF;
UPDATE a SET v=12;
UPDATE b SET v=22;
SET GLOBAL debug=@saved_debug;
ALTER TABLE a SECONDARY_UNLOAD;
ALTER TABLE b SECONDARY_UNLOAD;
ALTER TABLE a SECONDARY_LOAD;
ALTER TABLE b SECONDARY_LOAD;
SET use_secondary_engine=FORCED;
SELECT a.v,b.v FROM a JOIN b USING(id);
--let $assert_query = SELECT a.v,b.v FROM a JOIN b USING(id)
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
SET use_secondary_engine=OFF;
ALTER TABLE a SECONDARY_UNLOAD;
ALTER TABLE b SECONDARY_UNLOAD;
DROP DATABASE cb_stall;
