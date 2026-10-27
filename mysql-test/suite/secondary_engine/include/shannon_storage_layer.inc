# Shared storage recovery phases: baseline, manual_load, verify.
# Callers set $shannon_storage_layer_case before each source.
--let $_shannon_storage_layer_case_found = 0

if ($shannon_storage_layer_case == 'baseline') {
--let $_shannon_storage_layer_case_found = 1
# Initial reload establishes a committed native-format recovery generation.
--let $storage_restart_debug =
--source include/have_debug.inc
--source include/no_ps_protocol.inc
# Optimistic checkpoint publication may lose a race with source progress;
# below we still require a committed manifest and each recovery test checks its route.
--disable_query_log
CALL mtr.add_suppression("CheckpointScheduler: on-demand checkpoint failed sh_storage_recovery.t");
--enable_query_log
CREATE DATABASE sh_storage_recovery;
USE sh_storage_recovery;
CREATE TABLE t (id INT PRIMARY KEY, v INT NOT NULL) SECONDARY_ENGINE=rapid;
INSERT INTO t VALUES (1,10),(2,20),(3,30);
ALTER TABLE t SECONDARY_LOAD;
--let $restart_parameters = restart: --rapid_reload_on_restart=ON
--source include/restart_mysqld.inc
USE sh_storage_recovery;
--let $wait_condition = SELECT COUNT(*)=1 FROM performance_schema.rpd_table_id WHERE SCHEMA_NAME='sh_storage_recovery' AND TABLE_NAME='t'
--source include/wait_condition.inc
--let $datadir = `SELECT @@datadir`
--let $checkpoint_dir = $datadir/rapid_snapshots/sh_storage_recovery/t/checkpoints
--source suite/secondary_engine/include/wait_native_checkpoint.inc
# A valid checkpoint must really exist; falling back to reload cannot pass
# the tests that require storage_restores after the following crash.
}

if ($shannon_storage_layer_case == 'manual_load') {
--let $_shannon_storage_layer_case_found = 1
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
}

if ($shannon_storage_layer_case == 'verify') {
--let $_shannon_storage_layer_case_found = 1
--let $restart_parameters = restart: --rapid_reload_on_restart=ON $storage_restart_debug
--source include/kill_and_restart_mysqld.inc
USE sh_storage_recovery;
--let $wait_condition = SELECT COUNT(*)=1 FROM performance_schema.rpd_table_id WHERE SCHEMA_NAME='sh_storage_recovery' AND TABLE_NAME='t'
--source include/wait_condition.inc
--let $wait_condition = SELECT CAST(VARIABLE_VALUE AS UNSIGNED)=1 FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_recovery_$storage_route'
--source include/wait_condition.inc
SET use_secondary_engine=OFF;
--let $storage_actual = `SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) FROM t`
if ($storage_actual != $storage_expected) {
  --die Source rows after crash differ from the independently expected committed state
}
SET use_secondary_engine=FORCED;
SELECT id,v FROM t ORDER BY id;
--let $assert_digest = SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) FROM t
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
# Independent Rapid WAL files must not be recreated anywhere in this instance.
--perl
use File::Find;
find(sub {
  die "obsolete Rapid WAL exists: $File::Find::name" if
    /^(rapid_wal|cu_wal|capture_wal)\.log$/;
}, $ENV{'MYSQLTEST_VARDIR'});
EOF
# A recovered table must accept fresh writes and retain AP correctness.
UPDATE t SET v=v+1 WHERE id=1;
--let $assert_digest = SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) FROM t
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
DROP DATABASE sh_storage_recovery;
}

if (!$_shannon_storage_layer_case_found) {
  --die Unknown shannon_storage_layer_case
}
