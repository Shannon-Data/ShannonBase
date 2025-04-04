# Populate a table with 1000 records. Allow the replica to sync with the master.
# Run concurrent threads that run OLTP transactions on master.
# Kill the master database server at random points.
# Check the table against the replica.
# Reinvoke the threads.


# Set python interpreter to use (On Windows executable is python.exe)
--let $PYTHON = python3
if (`SELECT CONVERT(@@VERSION_COMPILE_OS USING latin1) IN ('Win32', 'Win64', 'Windows')`)
{
  --let $PYTHON = python
}

# create the directory for temporary log files.
--mkdir $MYSQL_TMP_DIR/load_generator

if ($fake_changes)
{
  --mkdir $MYSQL_TMP_DIR/load_generator_slave
}

--connection master

CREATE TABLE errors (msg TEXT);

--sync_slave_with_master

--connection master
--let $pid_file = `SELECT @@pid_file`
--let $crash_num = 0
--let $source_host = 127.0.0.1
--let $table = test
--let $user = root
--let $checksum = 0
--let $secondary_index_checks = 0

if ($do_checksum)
{
    # populate the table and store its checksum before any load.
    let $exec =
$PYTHON $MYSQL_TEST_DIR/suite/innodb_stress/t/load_generator.py $pid_file $kill_db_after
$num_records 0 0 $user $source_host $MASTER_MYPORT
$table 0 $max_rows $MYSQL_TMP_DIR/load_generator 0 0 0;
    exec $exec >> $MYSQL_TMP_DIR/load_generator/errors.log;
    let $checksum=query_get_value(CHECKSUM TABLE t1, Checksum, 1);
    # Master needs to be restarted to start with an empty buffer pool so
    # that logical read ahead gets used.
    let rpl_server_number = 1;
    source include/rpl/restart_server.inc;
    connection slave;
    # Start replica to avoid I/O thread retry errors
    disable_warnings;
    source include/rpl/start_replica.inc;
    enable_warnings;
    let $num_records = 0;
}

while ($num_crashes)
{
  connection master;
  exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect;
  if ($crash_num)
  {
    let $num_records = 0; # do not populate the table except for the first run.
  }

  if ($use_blob)
  {
    let $exec =
$PYTHON $MYSQL_TEST_DIR/suite/innodb_stress/t/load_generator.py $pid_file $kill_db_after
$num_records  $num_workers $num_transactions $user $source_host $MASTER_MYPORT
$table 1 $max_rows $MYSQL_TMP_DIR/load_generator 0 $checksum $secondary_index_checks;
  }
  if (!$use_blob)
  {
    let $exec =
$PYTHON $MYSQL_TEST_DIR/suite/innodb_stress/t/load_generator.py $pid_file $kill_db_after
$num_records  $num_workers $num_transactions $user $source_host $MASTER_MYPORT
$table 0 $max_rows $MYSQL_TMP_DIR/load_generator 0 $checksum $secondary_index_checks;
  }

  exec $exec >> $MYSQL_TMP_DIR/load_generator/errors.log;

  if ($do_crash)
  {
    --echo Wait for reconnect
    # Call script that will poll the server waiting for it to be back online again
    source include/wait_until_connected_again.inc;
    connection slave;
    source include/wait_until_connected_again.inc;
    connection master;
  }

  --echo Checksum master
  let $master_checksum = query_get_value(CHECKSUM TABLE t1, Checksum, 1);

  # if sync_slave_with_master had a configurable timeout this would not be needed
  let $slave_sync_timeout = 7200;
  --source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

  connection slave;
  if ($fake_changes)
  {
    --echo applying fake updates to the slave
    let $slave_pid_file = `SELECT @@pid_file`;
    let $slave_exec =
$PYTHON $MYSQL_TEST_DIR/suite/innodb_stress/t/load_generator.py $slave_pid_file $kill_db_after
0  $num_workers $num_transactions $user $source_host $SLAVE_MYPORT
$table 0 $max_rows $MYSQL_TMP_DIR/load_generator_slave 1 $checksum $secondary_index_checks;
    exec $slave_exec >> $MYSQL_TMP_DIR/load_generator/errors.log;
  }
  --echo Checksum slave
  let $slave_checksum=query_get_value(CHECKSUM TABLE t1, Checksum, 1);
  let $not_same = `SELECT $master_checksum-$slave_checksum`;
  if ($not_same)
  {
    let $msg =
The checksums of table t1 for master and slave do not match for $crash_num th
crash. This may happen if there is a corrupt recovery log or a bug in crash
recovery. You can take a look at the logs in $MYSQL_TMP_DIR/load_generator to see the
queries issued before the crash.;
    echo $msg;

    connection master;
    eval select * into outfile '$MYSQLTEST_VARDIR/tmp/master_all' from t1 order by id;
    eval select id into outfile '$MYSQLTEST_VARDIR/tmp/master_id' from t1 order by id;
    show binary log status;

    connection slave;
    eval select * into outfile '$MYSQLTEST_VARDIR/tmp/slave_all' from t1 order by id;
    eval select id into outfile '$MYSQLTEST_VARDIR/tmp/slave_id' from t1 order by id;
    SHOW REPLICA STATUS;

    die;
  }
  dec $num_crashes;
  inc $crash_num;
}

# final cleanup
--connection master

let $primary=`select count(*) from t1 use index (primary)`;
let $secondary=`select count(*) from t1 use index (msg_i)`;
if ($primary != $secondary)
{
  --echo Secondary index inconsistent!  $primary != $secondary
  --die
}

if (!$do_crash)
{
  --cat_file $MYSQL_TMP_DIR/load_generator/errors.log
  SELECT * FROM errors;
}

DROP TABLE t1;
DROP TABLE errors;

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

--connection slave
--source include/rpl/stop_replica.inc
# For stress tests sometimes the replication thread can not connect to master
# temporarily. This is either because the master crashed and it is recovering
# or the master is too busy and could not service the slave's requests.
# mtr's internal check requires that there be no errors in slave status.
# restarting replication clears the errors.
--source include/rpl/start_replica.inc
--source include/rpl/stop_replica.inc

connection master;

--force-rmdir $MYSQL_TMP_DIR/load_generator
if ($fake_changes)
{
  --force-rmdir $MYSQL_TMP_DIR/load_generator_slave
}

# Reconnect client in case master had been killed
if ($kill_db_after)
{
  --let $rpl_server_number = 1
  --source include/rpl/reconnect.inc
}

--let $rpl_only_running_threads= 1
--source include/rpl/deinit.inc
