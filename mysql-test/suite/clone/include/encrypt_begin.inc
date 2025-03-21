--echo # Prepare for creating Encrypted Table
--source include/big_test.inc
--source include/not_parallel.inc
--source include/have_component_keyring_file.inc

--let $ENCRYPT_DATADIR = $MYSQL_TMP_DIR/data_encrypt
--let $ENCRYPT_ERROR_FILE = $MYSQL_TMP_DIR/boot_encrypt_log

--let BOOTSTRAP_SQL=$MYSQL_TMP_DIR/boot.sql
--let $START_PAGE_SIZE= `select @@innodb_page_size`

--echo # create bootstrap file

write_file $BOOTSTRAP_SQL;
CREATE DATABASE test DEFAULT CHARACTER SET latin1;
EOF

--mkdir $ENCRYPT_DATADIR

--echo # Create and start mysqld with keyring plugin.
--source include/shutdown_mysqld.inc

--replace_result $MYSQLD MYSQLD $BOOTSTRAP_SQL BOOTSTRAP_SQL $ENCRYPT_DATADIR ENCRYPT_DATADIR $START_PAGE_SIZE PAGE_SIZE $ENCRYPT_ERROR_FILE ENCRYPT_ERROR_FILE
--let NEW_CMD = $MYSQLD --no-defaults --innodb_dedicated_server=OFF --initialize-insecure --init-file=$BOOTSTRAP_SQL --innodb_page_size=$START_PAGE_SIZE --datadir=$ENCRYPT_DATADIR > $ENCRYPT_ERROR_FILE  2>&1

--exec $NEW_CMD
--source include/start_mysqld.inc

--source ../mysql-test/suite/component_keyring_file/inc/setup_component_customized.inc

--replace_result $MYSQL_TMP_DIR MYSQL_TMP_DIR $ENCRYPT_DATADIR ENCRYPT_DATADIR $PLUGIN_DIR_OPT PLUGIN_DIR_OPT
--let $restart_parameters=restart: --datadir=$ENCRYPT_DATADIR $PLUGIN_DIR_OPT

--source include/restart_mysqld.inc
