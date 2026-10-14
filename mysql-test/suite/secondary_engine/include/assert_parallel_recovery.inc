USE sh_parallel_recovery;
--let $wait_condition = SELECT CAST(VARIABLE_VALUE AS UNSIGNED)=1 FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_recovery_storage_restores'
--source include/wait_condition.inc
--let $assert_text = Native recovery does not silently fall back to InnoDB
--let $assert_cond = [SELECT CAST(VARIABLE_VALUE AS UNSIGNED)=0 FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_recovery_primary_reloads']
--source include/assert.inc
--let $assert_digest = SELECT GROUP_CONCAT(CONCAT_WS(':',id,IFNULL(HEX(code),'NULL'),IFNULL(HEX(note),'NULL'),IFNULL(CAST(amount AS CHAR),'NULL')) ORDER BY id SEPARATOR '|') FROM t
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--let $assert_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $assert_count_condition = >= 1
