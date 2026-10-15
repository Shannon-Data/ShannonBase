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
