# Initial reload establishes a committed native-format recovery generation.
--let $storage_restart_debug =
--source include/have_debug.inc
--source include/no_ps_protocol.inc
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
--let $manifest = $datadir/rapid_snapshots/sh_storage_recovery/t/checkpoints/checkpoint-1.manifest
--file_exists $manifest 60
# A valid checkpoint must really exist; falling back to reload cannot pass
# the tests that require storage_restores after the following crash.
