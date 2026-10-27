# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
#
# Copyright (c) 2026, Shannon Data AI and/or its affiliates.

# $shannon_resource_case: admission or apply_pool.
# Admission additionally takes $shannon_admission_mode: SOFT or AUTO.
--let $_shannon_resource_case_found = 0

if ($shannon_resource_case == 'admission') {
  --let $_shannon_resource_case_found = 1
  --source include/have_debug_sync.inc
  --source include/no_ps_protocol.inc
  SET use_secondary_engine=OFF;
  --eval SET GLOBAL rapid_resource_isolation=$shannon_admission_mode
  if ($shannon_admission_mode == 'AUTO') {
    SELECT @@GLOBAL.rapid_resource_isolation;
    SELECT VARIABLE_VALUE LIKE '%configured=AUTO effective=%' AS auto_reported
    FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_status';
  }
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
}

if ($shannon_resource_case == 'apply_pool') {
  --let $_shannon_resource_case_found = 1
  # Exercise more logical tables than physical AUTO apply workers.
  --disable_query_log
  CREATE TABLE pool_a(id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
  CREATE TABLE pool_b(id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
  CREATE TABLE pool_c(id INT PRIMARY KEY,v INT) SECONDARY_ENGINE=rapid;
  INSERT INTO pool_a VALUES(1,0);
  INSERT INTO pool_b VALUES(1,0);
  INSERT INTO pool_c VALUES(1,0);
  ALTER TABLE pool_a SECONDARY_LOAD;
  ALTER TABLE pool_b SECONDARY_LOAD;
  ALTER TABLE pool_c SECONDARY_LOAD;
  let $rounds=300;
  while ($rounds)
  {
    BEGIN;
    UPDATE pool_a SET v=v+1;
    UPDATE pool_b SET v=v+1;
    UPDATE pool_c SET v=v+1;
    COMMIT;
    dec $rounds;
  }
  SET use_secondary_engine=FORCED;
  --enable_query_log
  SELECT SUM(a.v),SUM(b.v),SUM(c.v) FROM pool_a a JOIN pool_b b ON a.id=b.id JOIN pool_c c ON a.id=c.id;
  --let $assert_query = SELECT SUM(a.v),SUM(b.v),SUM(c.v) FROM pool_a a JOIN pool_b b ON a.id=b.id JOIN pool_c c ON a.id=c.id
  --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
  --disable_query_log
  SET use_secondary_engine=OFF;
  --enable_query_log
  SELECT VARIABLE_VALUE REGEXP 'apply_backend=BOUNDED_POOL apply_workers=[12] apply_capacity=[12] logical_tables=3' AS bounded_pool,
         VARIABLE_VALUE LIKE '%AP limit=% source=DEFAULT SYNC limit=% source=DEFAULT MAINT limit=% source=DEFAULT%' AS automatic_limits,
         VARIABLE_VALUE LIKE '%flush_trigger=67108864;%' AS trigger_reported
  FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_plan';
  --disable_query_log
  SET use_secondary_engine=OFF;
  ALTER TABLE pool_b SECONDARY_UNLOAD;
  UPDATE pool_b SET v=v+1;
  ALTER TABLE pool_b SECONDARY_LOAD;
  UPDATE pool_b SET v=v+1;
  SET use_secondary_engine=FORCED;
  --enable_query_log
  SELECT SUM(v) FROM pool_b;
  --let $assert_query = SELECT SUM(v) FROM pool_b
  --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
  --disable_query_log
  SET use_secondary_engine=OFF;
  ALTER TABLE pool_a SECONDARY_UNLOAD;
  ALTER TABLE pool_b SECONDARY_UNLOAD;
  ALTER TABLE pool_c SECONDARY_UNLOAD;
  DROP TABLE pool_a,pool_b,pool_c;
  --enable_query_log
  SELECT VARIABLE_VALUE LIKE '%apply_workers=0 %' AS pool_stopped
  FROM performance_schema.global_status WHERE VARIABLE_NAME='rapid_resource_plan';
}

if (!$_shannon_resource_case_found) {
  --die Unknown shannon_resource_case
}
