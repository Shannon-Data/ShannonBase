# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
--source include/have_debug_sync.inc
--source include/no_ps_protocol.inc
SET use_secondary_engine=OFF;
SET GLOBAL rapid_resource_isolation=AUTO;
SELECT @@GLOBAL.rapid_resource_isolation;
SELECT VARIABLE_VALUE LIKE '%configured=AUTO effective=%' AS auto_reported
FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_status';
CREATE TABLE ri (id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
INSERT INTO ri VALUES (1,10),(2,20);
ALTER TABLE ri SECONDARY_LOAD;
--error ER_SECONDARY_ENGINE_PLUGIN
SET GLOBAL rapid_resource_isolation=OFF;
SET DEBUG_SYNC='RESET';
connect (holder,localhost,root,,test);
SET use_secondary_engine=FORCED;
SET DEBUG_SYNC='rapid_resource_ap_admitted SIGNAL ri_admitted WAIT_FOR ri_release';
--send SELECT SUM(v) FROM ri
connection default;
SET DEBUG_SYNC='now WAIT_FOR ri_admitted';
SET use_secondary_engine=FORCED;
--error ER_SECONDARY_ENGINE_PLUGIN,ER_SECONDARY_ENGINE
SELECT SUM(v) FROM ri;
SHOW WARNINGS;
# ON must retry with the primary engine and preserve its result.
SET use_secondary_engine=ON;
SET secondary_engine_cost_threshold=0;
SELECT SUM(v) FROM ri;
--let $assert_query = SELECT SUM(v) FROM ri
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
PREPARE ri_stmt FROM 'SELECT SUM(v) FROM ri';
EXECUTE ri_stmt;
--let $assert_query = EXECUTE ri_stmt
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
DEALLOCATE PREPARE ri_stmt;
SET use_secondary_engine=OFF;
SELECT VARIABLE_VALUE LIKE 'AP active=1 %' AS held FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_status';
SET DEBUG_SYNC='now SIGNAL ri_release';
connection holder;
--reap
SET use_secondary_engine=OFF;
SELECT VARIABLE_VALUE LIKE 'AP active=0 %' AS released FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_status';
# Kill a query after admission. Cleanup must free the slot for the next query.
let $holder_id= `SELECT CONNECTION_ID()`;
SET use_secondary_engine=FORCED;
SET DEBUG_SYNC='rapid_resource_ap_admitted SIGNAL ri_kill_ready WAIT_FOR ri_kill_continue';
--send SELECT SUM(v) FROM ri
connection default;
SET DEBUG_SYNC='now WAIT_FOR ri_kill_ready';
--disable_query_log
eval KILL QUERY $holder_id;
--enable_query_log
SET DEBUG_SYNC='now SIGNAL ri_kill_continue';
connection holder;
--error ER_QUERY_INTERRUPTED
--reap
# Prepared execution must also stop on KILL instead of retrying primary.
SET use_secondary_engine=ON;
SET secondary_engine_cost_threshold=0;
PREPARE ri_kill_stmt FROM 'SELECT SUM(v) FROM ri';
SET DEBUG_SYNC='rapid_resource_ap_admitted SIGNAL ri_ps_kill_ready WAIT_FOR ri_ps_kill_continue';
--send EXECUTE ri_kill_stmt
connection default;
SET DEBUG_SYNC='now WAIT_FOR ri_ps_kill_ready';
--disable_query_log
eval KILL QUERY $holder_id;
--enable_query_log
SET DEBUG_SYNC='now SIGNAL ri_ps_kill_continue';
connection holder;
--error ER_QUERY_INTERRUPTED
--reap
DEALLOCATE PREPARE ri_kill_stmt;
connection default;
SET use_secondary_engine=FORCED;
SELECT SUM(v) FROM ri;
--let $assert_query = SELECT SUM(v) FROM ri
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
SET use_secondary_engine=OFF;
SELECT VARIABLE_VALUE LIKE 'AP active=0 %' AS kill_released FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_status';
ALTER TABLE ri SECONDARY_UNLOAD;
DROP TABLE ri;
disconnect holder;
SET DEBUG_SYNC='RESET';
SET GLOBAL rapid_resource_isolation=OBSERVE;
SELECT @@GLOBAL.rapid_resource_isolation;
SET GLOBAL rapid_resource_isolation=OFF;
SELECT @@GLOBAL.rapid_resource_isolation;
--error ER_SECONDARY_ENGINE_PLUGIN
SET GLOBAL rapid_resource_isolation=STRICT_CPU;
SELECT @@GLOBAL.rapid_resource_isolation;
