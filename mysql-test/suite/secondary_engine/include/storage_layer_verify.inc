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
