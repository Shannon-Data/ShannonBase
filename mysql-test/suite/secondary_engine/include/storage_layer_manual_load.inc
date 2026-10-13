--disable_query_log
CALL mtr.add_suppression("recovery eligibility is revoked or cannot be verified; automatic recovery stopped");
CALL mtr.add_suppression("RecoveryManager: a source change reached sh_storage_recovery.t");
--enable_query_log
# A known eligibility failure stops automatic recovery without publishing AP.
--let $restart_parameters = restart: --rapid_reload_on_restart=ON $storage_restart_debug
--source include/kill_and_restart_mysqld.inc
USE sh_storage_recovery;
--let $grep_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $grep_pattern = sh_storage_recovery.t recovery eligibility is revoked or cannot be verified; automatic recovery stopped
--let $wait_timeout = 60
--source include/wait_for_pattern_in_file.inc
SELECT COUNT(*) AS automatically_loaded FROM performance_schema.rpd_table_id WHERE SCHEMA_NAME='sh_storage_recovery' AND TABLE_NAME='t';
SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_recovery_primary_reloads';
SET use_secondary_engine=OFF;
--let $storage_actual = `SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) FROM t`
if ($storage_actual != $storage_expected) {
  --die Primary committed data must survive the AP recovery failure
}
SET use_secondary_engine=ON;
SELECT id,v FROM t ORDER BY id;
SET use_secondary_engine=FORCED;
--error ER_SECONDARY_ENGINE_PLUGIN
SELECT id,v FROM t ORDER BY id;
SET use_secondary_engine=OFF;
# The primary remains writable while AP is not loaded.
UPDATE t SET v=v+1 WHERE id=1;
ALTER TABLE t SECONDARY_LOAD;
--let $assert_digest = SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) FROM t
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
DROP DATABASE sh_storage_recovery;
