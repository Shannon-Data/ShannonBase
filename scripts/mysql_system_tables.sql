-- Copyright (c) 2007, 2026, Oracle and/or its affiliates.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License, version 2.0,
-- as published by the Free Software Foundation.
--
-- This program is designed to work with certain software (including
-- but not limited to OpenSSL) that is licensed under separate terms,
-- as designated in a particular file or component or in included license
-- documentation.  The authors of MySQL hereby grant you an additional
-- permission to link the program and your derivative works with the
-- separately licensed software that they have either included with
-- the program or referenced in the documentation.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License, version 2.0, for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

--
-- The system tables of MySQL Server
--

set @have_innodb= (select count(engine) from information_schema.engines where engine='INNODB' and support != 'NO');

-- Tables below are NOT treated as DD tables by MySQL server yet.

SET FOREIGN_KEY_CHECKS= 1;

# Added sql_mode elements and making it as SET, instead of ENUM

set default_storage_engine=InnoDB;

SET @cmd = "CREATE TABLE IF NOT EXISTS db
(
Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
Db char(64) binary DEFAULT '' NOT NULL,
User char(32) binary DEFAULT '' NOT NULL,
Select_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Insert_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Update_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Delete_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Drop_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Grant_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
References_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Index_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Alter_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_tmp_table_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Lock_tables_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_view_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Show_view_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_routine_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Alter_routine_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Execute_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Event_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Trigger_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
PRIMARY KEY Host (Host,User,Db), KEY User (User)
)
engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Database privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


-- Remember for later if db table already existed
set @had_db_table= @@warning_count != 0;

SET @cmd = "CREATE TABLE IF NOT EXISTS user
(
Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
User char(32) binary DEFAULT '' NOT NULL,
Select_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Insert_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Update_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Delete_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Drop_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Reload_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Shutdown_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Process_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
File_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Grant_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
References_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Index_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Alter_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Show_db_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Super_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_tmp_table_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Lock_tables_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Execute_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Repl_slave_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Repl_client_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_view_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Show_view_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_routine_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Alter_routine_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_user_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Event_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Trigger_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_tablespace_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
ssl_type enum('','ANY','X509', 'SPECIFIED') COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
ssl_cipher BLOB NOT NULL,
x509_issuer BLOB NOT NULL,
x509_subject BLOB NOT NULL,
max_questions int unsigned DEFAULT 0  NOT NULL,
max_updates int unsigned DEFAULT 0  NOT NULL,
max_connections int unsigned DEFAULT 0  NOT NULL,
max_user_connections int unsigned DEFAULT 0  NOT NULL,
plugin char(64) DEFAULT 'caching_sha2_password' NOT NULL,
authentication_string TEXT,
password_expired ENUM('N', 'Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
password_last_changed timestamp NULL DEFAULT NULL,
password_lifetime smallint unsigned NULL DEFAULT NULL,
account_locked ENUM('N', 'Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Create_role_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Drop_role_priv enum('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
Password_reuse_history smallint unsigned NULL DEFAULT NULL,
Password_reuse_time smallint unsigned NULL DEFAULT NULL,
Password_require_current enum('N', 'Y') COLLATE utf8mb3_general_ci DEFAULT NULL,
User_attributes JSON DEFAULT NULL,
PRIMARY KEY Host (Host,User)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Users and global privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


SET @cmd = "CREATE TABLE IF NOT EXISTS default_roles
(
HOST CHAR(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
USER CHAR(32) BINARY DEFAULT '' NOT NULL,
DEFAULT_ROLE_HOST CHAR(255) CHARACTER SET ASCII DEFAULT '%' NOT NULL,
DEFAULT_ROLE_USER CHAR(32) BINARY DEFAULT '' NOT NULL,
PRIMARY KEY (HOST, USER, DEFAULT_ROLE_HOST, DEFAULT_ROLE_USER)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Default roles' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


SET @cmd = "CREATE TABLE IF NOT EXISTS role_edges
(
FROM_HOST CHAR(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
FROM_USER CHAR(32) BINARY DEFAULT '' NOT NULL,
TO_HOST CHAR(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
TO_USER CHAR(32) BINARY DEFAULT '' NOT NULL,
WITH_ADMIN_OPTION ENUM('N', 'Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
PRIMARY KEY (FROM_HOST,FROM_USER,TO_HOST,TO_USER)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Role hierarchy and role grants' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


SET @cmd = "CREATE TABLE IF NOT EXISTS global_grants
(
USER CHAR(32) BINARY DEFAULT '' NOT NULL,
HOST CHAR(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
PRIV CHAR(32) COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
WITH_GRANT_OPTION ENUM('N','Y') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL,
PRIMARY KEY (USER,HOST,PRIV)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Extended global grants' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


-- Remember for later if user table already existed
set @had_user_table= @@warning_count != 0;

SET @cmd = "CREATE TABLE IF NOT EXISTS password_history
(
  Host CHAR(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
  User CHAR(32) BINARY DEFAULT '' NOT NULL,
  Password_timestamp TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  Password TEXT,
  PRIMARY KEY(Host, User, Password_timestamp DESC)
 ) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin
 comment='Password history for user accounts' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS func (  name char(64) binary DEFAULT '' NOT NULL, ret tinyint DEFAULT '0' NOT NULL, dl char(128) DEFAULT '' NOT NULL, type enum ('function','aggregate') COLLATE utf8mb3_general_ci NOT NULL, PRIMARY KEY (name) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin   comment='User defined functions' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS plugin ( name varchar(64) DEFAULT '' NOT NULL, dl varchar(128) DEFAULT '' NOT NULL, PRIMARY KEY (name) ) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci comment='MySQL plugins' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS help_topic ( help_topic_id int unsigned not null, name char(64) not null, help_category_id smallint unsigned not null, description text not null, example text not null, url text not null, primary key (help_topic_id), unique index (name) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 comment='help topics' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS help_category ( help_category_id smallint unsigned not null, name  char(64) not null, parent_category_id smallint unsigned null, url text not null, primary key (help_category_id), unique index (name) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 comment='help categories' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS help_relation ( help_topic_id int unsigned not null, help_keyword_id  int unsigned not null, primary key (help_keyword_id, help_topic_id) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 comment='keyword-topic relation' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS servers ( Server_name char(64) NOT NULL DEFAULT '', Host char(255) CHARACTER SET ASCII NOT NULL DEFAULT '', Db char(64) NOT NULL DEFAULT '', Username char(64) NOT NULL DEFAULT '', Password char(64) NOT NULL DEFAULT '', Port INT NOT NULL DEFAULT '0', Socket char(64) NOT NULL DEFAULT '', Wrapper char(64) NOT NULL DEFAULT '', Owner char(64) NOT NULL DEFAULT '', PRIMARY KEY (Server_name)) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 comment='MySQL Foreign Servers table' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS tables_priv
(
  Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
  Db char(64) binary DEFAULT '' NOT NULL,
  User char(32) binary DEFAULT '' NOT NULL,
  Table_name char(64) binary DEFAULT '' NOT NULL,
  Grantor varchar(288) DEFAULT '' NOT NULL,
  Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  Table_priv set('Select','Insert','Update','Delete','Create','Drop','Grant','References','Index','Alter','Create View','Show view','Trigger') COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
  Column_priv set('Select','Insert','Update','References') COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
  PRIMARY KEY (Host,User,Db,Table_name),
  KEY Grantor (Grantor)
) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='Table privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS columns_priv
(
  Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
  Db char(64) binary DEFAULT '' NOT NULL,
  User char(32) binary DEFAULT '' NOT NULL,
  Table_name char(64) binary DEFAULT '' NOT NULL,
  Column_name char(64) binary DEFAULT '' NOT NULL,
  Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  Column_priv set('Select','Insert','Update','References') COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
  PRIMARY KEY (Host,User,Db,Table_name,Column_name)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin   comment='Column privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS help_keyword (   help_keyword_id  int unsigned not null, name char(64) not null, primary key (help_keyword_id), unique index (name) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 comment='help keywords' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS time_zone_name (   Name char(64) NOT NULL, Time_zone_id int unsigned NOT NULL, PRIMARY KEY Name (Name) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3   comment='Time zone names' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS time_zone (   Time_zone_id int unsigned NOT NULL auto_increment, Use_leap_seconds enum('Y','N') COLLATE utf8mb3_general_ci DEFAULT 'N' NOT NULL, PRIMARY KEY TzId (Time_zone_id) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3   comment='Time zones' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS time_zone_transition (   Time_zone_id int unsigned NOT NULL, Transition_time bigint signed NOT NULL, Transition_type_id int unsigned NOT NULL, PRIMARY KEY TzIdTranTime (Time_zone_id, Transition_time) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3   comment='Time zone transitions' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS time_zone_transition_type (   Time_zone_id int unsigned NOT NULL, Transition_type_id int unsigned NOT NULL, Offset int signed DEFAULT 0 NOT NULL, Is_DST tinyint unsigned DEFAULT 0 NOT NULL, Abbreviation char(8) DEFAULT '' NOT NULL, PRIMARY KEY TzIdTrTId (Time_zone_id, Transition_type_id) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3   comment='Time zone transition types' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS time_zone_leap_second (   Transition_time bigint signed NOT NULL, Correction int signed NOT NULL, PRIMARY KEY TranTime (Transition_time) ) engine=INNODB STATS_PERSISTENT=0 CHARACTER SET utf8mb3   comment='Leap seconds information for time zones' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;



SET @cmd = "CREATE TABLE IF NOT EXISTS procs_priv
(
  Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL,
  Db char(64) binary DEFAULT '' NOT NULL,
  User char(32) binary DEFAULT '' NOT NULL,
  Routine_name char(64) COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
  Routine_type enum('FUNCTION','PROCEDURE') NOT NULL,
  Grantor varchar(288) DEFAULT '' NOT NULL,
  Proc_priv set('Execute','Alter Routine','Grant') COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL,
  Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (Host,User,Db,Routine_name,Routine_type),
  KEY Grantor (Grantor)
) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin   comment='Procedure privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


-- bug#92988: Temporarily turn off sql_require_primary_key for the
-- next 2 tables as this is not necessary as they do not replicate.
SET @old_sql_require_primary_key = @@session.sql_require_primary_key;
SET @@session.sql_require_primary_key = 0;

-- Create general_log
CREATE TABLE IF NOT EXISTS general_log (event_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host MEDIUMTEXT NOT NULL, thread_id BIGINT UNSIGNED NOT NULL, server_id INTEGER UNSIGNED NOT NULL, command_type VARCHAR(64) NOT NULL, argument MEDIUMBLOB NOT NULL) engine=CSV CHARACTER SET utf8mb3 comment="General log";

-- Create slow_log
CREATE TABLE IF NOT EXISTS slow_log (start_time TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), user_host MEDIUMTEXT NOT NULL, query_time TIME(6) NOT NULL, lock_time TIME(6) NOT NULL, rows_sent INTEGER NOT NULL, rows_examined INTEGER NOT NULL, db VARCHAR(512) NOT NULL, last_insert_id INTEGER NOT NULL, insert_id INTEGER NOT NULL, server_id INTEGER UNSIGNED NOT NULL, sql_text MEDIUMBLOB NOT NULL, thread_id BIGINT UNSIGNED NOT NULL) engine=CSV CHARACTER SET utf8mb3 comment="Slow log";

SET @@session.sql_require_primary_key = @old_sql_require_primary_key;

SET @cmd = "CREATE TABLE IF NOT EXISTS component ( component_id int unsigned NOT NULL AUTO_INCREMENT, component_group_id int unsigned NOT NULL, component_urn text NOT NULL, PRIMARY KEY (component_id)) engine=INNODB DEFAULT CHARSET=utf8mb3 COMMENT 'Components' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


SET @cmd="CREATE TABLE IF NOT EXISTS slave_relay_log_info (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file or rows in the table. Used to version table definitions.',
  Relay_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The name of the current relay log file.',
  Relay_log_pos BIGINT UNSIGNED COMMENT 'The relay log position of the last executed event.',
  Master_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The name of the master binary log file from which the events in the relay log file were read.',
  Master_log_pos BIGINT UNSIGNED COMMENT 'The master log position of the last executed event.',
  Sql_delay INTEGER COMMENT 'The number of seconds that the slave must lag behind the master.',
  Number_of_workers INTEGER UNSIGNED,
  Id INTEGER UNSIGNED COMMENT 'Internal Id that uniquely identifies this record.',
  Channel_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'The channel on which the replica is connected to a source. Used in Multisource Replication',
  Privilege_checks_username VARCHAR(32) COLLATE utf8mb3_bin DEFAULT NULL COMMENT 'Username part of PRIVILEGE_CHECKS_USER.',
  Privilege_checks_hostname VARCHAR(255) CHARACTER SET ascii COLLATE ascii_general_ci DEFAULT NULL COMMENT 'Hostname part of PRIVILEGE_CHECKS_USER.',
  Require_row_format BOOLEAN NOT NULL COMMENT 'Indicates whether the channel shall only accept row based events.',
  Require_table_primary_key_check ENUM('STREAM','ON','OFF','GENERATE') NOT NULL DEFAULT 'STREAM' COMMENT 'Indicates what is the channel policy regarding tables without primary keys on create and alter table queries',
  Assign_gtids_to_anonymous_transactions_type ENUM('OFF', 'LOCAL', 'UUID')  NOT NULL DEFAULT 'OFF' COMMENT 'Indicates whether the channel will generate a new GTID for anonymous transactions. OFF means that anonymous transactions will remain anonymous. LOCAL means that anonymous transactions will be assigned a newly generated GTID based on server_uuid. UUID indicates that anonymous transactions will be assigned a newly generated GTID based on Assign_gtids_to_anonymous_transactions_value',
  Assign_gtids_to_anonymous_transactions_value TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin  COMMENT 'Indicates the UUID used while generating GTIDs for anonymous transactions',
  PRIMARY KEY(Channel_name)) DEFAULT CHARSET=utf8mb3 STATS_PERSISTENT=0 COMMENT 'Relay Log Information'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd= "CREATE TABLE IF NOT EXISTS slave_master_info (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file.',
  Master_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL COMMENT 'The name of the master binary log currently being read from the master.',
  Master_log_pos BIGINT UNSIGNED NOT NULL COMMENT 'The master log position of the last read event.',
  Host VARCHAR(255) CHARACTER SET ASCII COMMENT 'The host name of the source.',
  User_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The user name used to connect to the master.',
  User_password TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The password used to connect to the master.',
  Port INTEGER UNSIGNED NOT NULL COMMENT 'The network port used to connect to the master.',
  Connect_retry INTEGER UNSIGNED NOT NULL COMMENT 'The period (in seconds) that the slave will wait before trying to reconnect to the master.',
  Enabled_ssl BOOLEAN NOT NULL COMMENT 'Indicates whether the server supports SSL connections.',
  Ssl_ca TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The file used for the Certificate Authority (CA) certificate.',
  Ssl_capath TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The path to the Certificate Authority (CA) certificates.',
  Ssl_cert TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The name of the SSL certificate file.',
  Ssl_cipher TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The name of the cipher in use for the SSL connection.',
  Ssl_key TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The name of the SSL key file.',
  Ssl_verify_server_cert BOOLEAN NOT NULL COMMENT 'Whether to verify the server certificate.',
  Heartbeat FLOAT NOT NULL COMMENT '',
  Bind TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'Displays which interface is employed when connecting to the MySQL server',
  Ignored_server_ids TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The number of server IDs to be ignored, followed by the actual server IDs',
  Uuid TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The master server uuid.',
  Retry_count BIGINT UNSIGNED NOT NULL COMMENT 'Number of reconnect attempts, to the master, before giving up.',
  Ssl_crl TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The file used for the Certificate Revocation List (CRL)',
  Ssl_crlpath TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The path used for Certificate Revocation List (CRL) files',
  Enabled_auto_position BOOLEAN NOT NULL COMMENT 'Indicates whether GTIDs will be used to retrieve events from the master.',
  Channel_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'The channel on which the replica is connected to a source. Used in Multisource Replication',
  Tls_version TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'Tls version',
  Public_key_path TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'The file containing public key of master server.',
  Get_public_key BOOLEAN NOT NULL COMMENT 'Preference to get public key from master.',
  Network_namespace TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin COMMENT 'Network namespace used for communication with the master server.',
  Master_compression_algorithm VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL COMMENT 'Compression algorithm supported for data transfer between source and replica.',
  Master_zstd_compression_level INTEGER UNSIGNED NOT NULL COMMENT 'Compression level associated with zstd compression algorithm.',
  Tls_ciphersuites TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin DEFAULT NULL COMMENT 'Ciphersuites used for TLS 1.3 communication with the master server.',
  Source_connection_auto_failover BOOLEAN NOT NULL DEFAULT FALSE COMMENT 'Indicates whether the channel connection failover is enabled.',
  Gtid_only BOOLEAN NOT NULL DEFAULT FALSE COMMENT 'Indicates if this channel only uses GTIDs and does not persist positions.',
  PRIMARY KEY(Channel_name)) DEFAULT CHARSET=utf8mb3 STATS_PERSISTENT=0 COMMENT 'Master Information'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd= "CREATE TABLE IF NOT EXISTS slave_worker_info (
  Id INTEGER UNSIGNED NOT NULL,
  Relay_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL,
  Relay_log_pos BIGINT UNSIGNED NOT NULL,
  Master_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL,
  Master_log_pos BIGINT UNSIGNED NOT NULL,
  Checkpoint_relay_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL,
  Checkpoint_relay_log_pos BIGINT UNSIGNED NOT NULL,
  Checkpoint_master_log_name TEXT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin NOT NULL,
  Checkpoint_master_log_pos BIGINT UNSIGNED NOT NULL,
  Checkpoint_seqno INT UNSIGNED NOT NULL,
  Checkpoint_group_size INTEGER UNSIGNED NOT NULL,
  Checkpoint_group_bitmap BLOB NOT NULL,
  Channel_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'The channel on which the replica is connected to a source. Used in Multisource Replication',
  PRIMARY KEY(Channel_name, Id)) DEFAULT CHARSET=utf8mb3 STATS_PERSISTENT=0 COMMENT 'Worker Information'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd= "CREATE TABLE IF NOT EXISTS gtid_executed (
    source_uuid CHAR(36) NOT NULL COMMENT 'uuid of the source where the transaction was originally executed.',
    interval_start BIGINT NOT NULL COMMENT 'First number of interval.',
    interval_end BIGINT NOT NULL COMMENT 'Last number of interval.',
    gtid_tag CHAR(32) NOT NULL COMMENT 'GTID Tag.',
    PRIMARY KEY(source_uuid, gtid_tag, interval_start)) STATS_PERSISTENT=0";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- replication_asynchronous_connection_failover table
SET @cmd= "CREATE TABLE IF NOT EXISTS replication_asynchronous_connection_failover (
    Channel_name CHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'The replication channel name that connects source and replica.',
    Host CHAR(255) CHARACTER SET ASCII NOT NULL COMMENT 'The source hostname that the replica will attempt to switch over the replication connection to in case of a failure.',
    Port INTEGER UNSIGNED NOT NULL COMMENT 'The source port that the replica will attempt to switch over the replication connection to in case of a failure.',
    Network_namespace CHAR(64) COMMENT 'The source network namespace that the replica will attempt to switch over the replication connection to in case of a failure. If its value is empty, connections use the default (global) namespace.',
    Weight TINYINT UNSIGNED NOT NULL COMMENT 'The order in which the replica shall try to switch the connection over to when there are failures. Weight can be set to a number between 1 and 100, where 100 is the highest weight and 1 the lowest.',
    Managed_name CHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL DEFAULT '' COMMENT 'The name of the group which this server belongs to.',
    PRIMARY KEY(Channel_name, Host, Port, Network_namespace, Managed_name), KEY(Channel_name, Managed_name)) DEFAULT CHARSET=utf8mb3 STATS_PERSISTENT=0 COMMENT 'The source configuration details'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- replication_asynchronous_connection_failover_managed table
SET @cmd= "CREATE TABLE IF NOT EXISTS replication_asynchronous_connection_failover_managed (
    Channel_name CHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'The replication channel name that connects source and replica.',
    Managed_name CHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL DEFAULT '' COMMENT 'The name of the source which needs to be managed.',
    Managed_type CHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL DEFAULT '' COMMENT 'Determines the managed type.',
    Configuration JSON DEFAULT NULL COMMENT 'The data to help manage group. For Managed_type = GroupReplication, Configuration value should contain {\"Primary_weight\": 80, \"Secondary_weight\": 60}, so that it assigns weight=80 to PRIMARY of the group, and weight=60 for rest of the members in mysql.replication_asynchronous_connection_failover table.',
    PRIMARY KEY(Channel_name, Managed_name)) DEFAULT CHARSET=utf8mb3 STATS_PERSISTENT=0 COMMENT 'The managed source configuration details'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- replication_group_member_actions
SET @cmd= "CREATE TABLE IF NOT EXISTS replication_group_member_actions (
    name CHAR(255) CHARACTER SET ASCII NOT NULL COMMENT 'The action name.',
    event CHAR(64) CHARACTER SET ASCII NOT NULL COMMENT 'The event that will trigger the action.',
    enabled BOOLEAN NOT NULL COMMENT 'Whether the action is enabled.',
    type CHAR(64) CHARACTER SET ASCII NOT NULL COMMENT 'The action type.',
    priority TINYINT UNSIGNED NOT NULL COMMENT 'The order on which the action will be run, value between 1 and 100, lower values first.',
    error_handling CHAR(64) CHARACTER SET ASCII NOT NULL COMMENT 'On errors during the action will be handled: IGNORE, CRITICAL.',
    PRIMARY KEY(name, event), KEY(event)) DEFAULT CHARSET=utf8mb4 STATS_PERSISTENT=0 COMMENT 'The member actions configuration.'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- replication_group_configuration_version
SET @cmd= "CREATE TABLE IF NOT EXISTS replication_group_configuration_version (
    name CHAR(255) CHARACTER SET ASCII NOT NULL COMMENT 'The configuration name.',
    version BIGINT UNSIGNED NOT NULL COMMENT 'The version of the configuration name.',
    PRIMARY KEY(name)) DEFAULT CHARSET=utf8mb4 STATS_PERSISTENT=0 COMMENT 'The group configuration version.'";

SET @str=IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- Optimizer Cost Model configuration
-- (Note: Column definition for default_value needs to be updated when a
--        default value is changed).
--

-- Server cost constants

SET @cmd = "CREATE TABLE IF NOT EXISTS server_cost (
  cost_name   VARCHAR(64) NOT NULL,
  cost_value  FLOAT DEFAULT NULL,
  last_update TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  comment     VARCHAR(1024) DEFAULT NULL,
  default_value FLOAT GENERATED ALWAYS AS
    (CASE cost_name
       WHEN 'disk_temptable_create_cost' THEN 20.0
       WHEN 'disk_temptable_row_cost' THEN 0.5
       WHEN 'key_compare_cost' THEN 0.05
       WHEN 'memory_temptable_create_cost' THEN 1.0
       WHEN 'memory_temptable_row_cost' THEN 0.1
       WHEN 'row_evaluate_cost' THEN 0.1
       ELSE NULL
     END) VIRTUAL,
  PRIMARY KEY (cost_name)
) ENGINE=InnoDB CHARACTER SET=utf8mb3 COLLATE=utf8mb3_general_ci STATS_PERSISTENT=0 ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


INSERT IGNORE INTO server_cost(cost_name) VALUES
  ("row_evaluate_cost"), ("key_compare_cost"),
  ("memory_temptable_create_cost"), ("memory_temptable_row_cost"),
  ("disk_temptable_create_cost"), ("disk_temptable_row_cost");

-- Engine cost constants

SET @cmd = "CREATE TABLE IF NOT EXISTS engine_cost (
  engine_name VARCHAR(64) NOT NULL,
  device_type INTEGER NOT NULL,
  cost_name   VARCHAR(64) NOT NULL,
  cost_value  FLOAT DEFAULT NULL,
  last_update TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  comment     VARCHAR(1024) DEFAULT NULL,
  default_value FLOAT GENERATED ALWAYS AS
    (CASE cost_name
       WHEN 'io_block_read_cost' THEN 1.0
       WHEN 'memory_block_read_cost' THEN 0.25
       ELSE NULL
     END) VIRTUAL,
  PRIMARY KEY (cost_name, engine_name, device_type)
) ENGINE=InnoDB CHARACTER SET=utf8mb3 COLLATE=utf8mb3_general_ci STATS_PERSISTENT=0 ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


INSERT IGNORE INTO engine_cost(engine_name, device_type, cost_name) VALUES
  ("default", 0, "memory_block_read_cost"),
  ("default", 0, "io_block_read_cost");


SET @cmd = "CREATE TABLE IF NOT EXISTS proxies_priv (Host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL, User char(32) binary DEFAULT '' NOT NULL, Proxied_host char(255) CHARACTER SET ASCII DEFAULT '' NOT NULL, Proxied_user char(32) binary DEFAULT '' NOT NULL, With_grant BOOL DEFAULT 0 NOT NULL, Grantor varchar(288) DEFAULT '' NOT NULL, Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY Host (Host,User,Proxied_host,Proxied_user), KEY Grantor (Grantor) ) engine=InnoDB STATS_PERSISTENT=0 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin comment='User proxy privileges' ROW_FORMAT=DYNAMIC TABLESPACE=mysql";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;


-- Remember for later if proxies_priv table already existed
set @had_proxies_priv_table= @@warning_count != 0;


--
-- Only create the ndb_binlog_index table if the server is built with ndb.
-- Create this table last among the tables in the mysql schema to make it
-- easier to keep tests agnostic wrt. the existence of this table.
--
SET @have_ndb= (select count(engine) from information_schema.engines where engine='ndbcluster');
SET @cmd="CREATE TABLE IF NOT EXISTS ndb_binlog_index (
  Position BIGINT UNSIGNED NOT NULL,
  File VARCHAR(255) NOT NULL,
  epoch BIGINT UNSIGNED NOT NULL,
  inserts INT UNSIGNED NOT NULL,
  updates INT UNSIGNED NOT NULL,
  deletes INT UNSIGNED NOT NULL,
  schemaops INT UNSIGNED NOT NULL,
  orig_server_id INT UNSIGNED NOT NULL,
  orig_epoch BIGINT UNSIGNED NOT NULL,
  gci INT UNSIGNED NOT NULL,
  next_position BIGINT UNSIGNED NOT NULL,
  next_file VARCHAR(255) NOT NULL,
  PRIMARY KEY(epoch, orig_server_id, orig_epoch)
  ) ENGINE=INNODB CHARACTER SET latin1 STATS_PERSISTENT=0 ROW_FORMAT=DYNAMIC TABLESPACE=mysql";

SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
SET @str = IF(@have_ndb = 1, @str, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

--
-- This table is used to store the schema embeddings for the cost model. It is only needed if the cost model is enabled and the server is built with support for it.
--
SET @cmd = "CREATE TABLE IF NOT EXISTS schema_embeddings (
  id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  schema_name   VARCHAR(64)  NOT NULL,
  table_name    VARCHAR(64)  NOT NULL,
  doc_text      MEDIUMTEXT   NOT NULL,
  embedding     VECTOR(384)  COMMENT 'default model:multilingual-e5-small.',
  status        TINYINT NOT NULL DEFAULT 0 COMMENT '0=pending, 1=done, 2=error',
  updated_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY unique_schema_table (schema_name, table_name)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci  STATS_PERSISTENT=0 COMMENT='ShannonBase ML schema metadata embeddings, managed by system' ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_memory (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    conversation_id VARCHAR(64) NOT NULL,
    seq             BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Monotonic per-conversation sequence; replaces created_at for ordering (TIMESTAMP only resolves to whole seconds)',
    turn_no         INT         NOT NULL DEFAULT 0 COMMENT 'Shared by the user and assistant rows of one turn',
    role            VARCHAR(16) NOT NULL,
    content         TEXT NOT NULL,
    thought         TEXT,
    route           VARCHAR(16) NOT NULL DEFAULT '' COMMENT 'catalog/rule/rag/agent_loop/review',
    importance      TINYINT     NOT NULL DEFAULT 0 COMMENT '0=normal, 1=high (preference/fact)',
    content_hash    CHAR(64)    NOT NULL DEFAULT '' COMMENT 'SHA2(content,256); dedup key',
    embed_model_id  VARCHAR(64) DEFAULT NULL COMMENT 'Embedding model actually used for this row',
    embed_dim       SMALLINT    DEFAULT NULL COMMENT 'Embedding dimension actually written; NULL when no vector was stored',
    document_name   VARCHAR(255) DEFAULT NULL COMMENT 'Isolation key: principal_prefix. sys.ML_RAG filters on this column, so it is what keeps one principal from retrieving another principal memory',
    segment_number  INT         NOT NULL DEFAULT 0 COMMENT 'Reserved for ML_RAG segment overlap; 0 = not part of an overlap group',
    meta            JSON        DEFAULT NULL,
    expires_at      TIMESTAMP   NULL DEFAULT NULL COMMENT 'Retention policy; NULL = never expires',
    embedding       VECTOR(384) DEFAULT NULL COMMENT 'optional, for semantic retrieval, model: multilingual-e5-small',
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    turn_id         VARCHAR(64) GENERATED ALWAYS AS (meta->>'$.turn_id') STORED COMMENT 'Extracted from meta so a turn joins to its cost row in agent_memory_audit and to its tool ledger in agent_sql_trace; the JSON stays the source of truth. STORED rather than VIRTUAL, and not by preference: MyISAM cannot take an index on a virtual generated column, so main.system_tables_myisam -- which walks every mysql.* table doing ALTER ... ENGINE=MyISAM and expects one of three specific errors -- died on an unhandled 1478 here. Changing this column was the right side of that to give way on: the alternative was editing an upstream test, and keeping non-Rapid code in step with upstream is the constraint this work is under. STORED costs 16 bytes a row and nothing else; indexed STORED columns, unindexed VIRTUAL ones, VECTOR columns and FULLTEXT ... WITH PARSER ngram all convert cleanly',
    UNIQUE KEY uk_conv_seq (conversation_id, seq),
    INDEX idx_conv_time (conversation_id, created_at),
    INDEX idx_conv_id  (conversation_id, id),
    INDEX idx_role     (role),
    INDEX idx_hash     (content_hash),
    INDEX idx_doc      (document_name(64)),
    INDEX idx_expires  (expires_at),
    INDEX idx_turn_id  (turn_id),
    FULLTEXT KEY ftx_content (content) WITH PARSER ngram
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci  STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Memory'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_conversation_summary (
    conversation_id  VARCHAR(64)     NOT NULL COMMENT 'Conversation ID',
    summary          MEDIUMTEXT      NOT NULL COMMENT 'Rolling summary of the turns already folded in',
    covered_upto_seq BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Highest agent_memory.seq covered by this summary; monotonic, CAS-protected',
    summary_tokens   INT             NOT NULL DEFAULT 0 COMMENT 'Estimated token cost of the summary',
    version          INT             NOT NULL DEFAULT 1 COMMENT 'Bumped on each successful compaction',
    updated_at       TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (conversation_id)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent conversation rolling summary'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- uk_fact's statement prefix is 175, not the usual 191, because the key also
-- carries principal_prefix: 16*4 + 175*4 = 764 bytes, which fits InnoDB's
-- 768-byte maximum at innodb_page_size=4k. At 191 the key is 828 bytes, and
-- the server does not merely skip the index -- it aborts the upgrade with
-- ER_TOO_LONG_KEY and refuses to start on any 4K-page datadir. Dedup
-- therefore compares the first 175 characters of a statement; statements are
-- capped at 512 by the remember_fact tool spec.
SET @cmd = "CREATE TABLE IF NOT EXISTS agent_semantic_fact (
    fact_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    principal_prefix CHAR(16)     NOT NULL COMMENT 'First 16 hex of SHA2(CURRENT_USER(),256); isolation key, never defaulted',
    scope            VARCHAR(64)  NOT NULL DEFAULT '' COMMENT 'Optional scope hint such as a schema or table name',
    statement        TEXT         NOT NULL COMMENT 'Natural-language fact, e.g. the user prefers Chinese answers',
    subject          VARCHAR(128) DEFAULT NULL,
    predicate        VARCHAR(64)  DEFAULT NULL,
    object           VARCHAR(512) DEFAULT NULL,
    embedding        VECTOR(384)  DEFAULT NULL COMMENT 'model: multilingual-e5-small',
    confidence       TINYINT      NOT NULL DEFAULT 80 COMMENT '0-100',
    source_conversation_id VARCHAR(64) DEFAULT NULL,
    source_turn_no   INT          DEFAULT NULL,
    use_count        INT          NOT NULL DEFAULT 0,
    last_used_at     TIMESTAMP    NULL DEFAULT NULL,
    expires_at       TIMESTAMP    NULL DEFAULT NULL,
    created_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_fact (principal_prefix, statement(175)),
    KEY idx_principal_pred (principal_prefix, predicate),
    KEY idx_expires (expires_at),
    FULLTEXT KEY ftx_statement (statement) WITH PARSER ngram
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent long-term semantic facts'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_memory_audit (
    id               BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    principal_prefix CHAR(16)     NOT NULL COMMENT 'Isolation key of the principal the operation ran as',
    conversation_id  VARCHAR(64)  DEFAULT NULL,
    op               VARCHAR(16)  NOT NULL COMMENT 'recall/write/forget/purge/compact/degraded',
    tier             VARCHAR(8)   NOT NULL COMMENT 'L1/L2/L2a/L2b/L3',
    store            VARCHAR(128) DEFAULT NULL COMMENT 'Target table or adaptation point',
    detail           VARCHAR(512) DEFAULT NULL,
    hit_count        INT          NOT NULL DEFAULT 0,
    elapsed_ms       INT          NOT NULL DEFAULT 0 COMMENT 'Vector recall latency; the evidence for whether ANN is ever worth asking upstream for',
    vector_mode      VARCHAR(8)   DEFAULT NULL COMMENT 'scan|auto',
    created_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_p_time (principal_prefix, created_at)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent memory audit'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- ---------------------------------------------------------------------------
-- agent_artifact: the agent's large-object store.
--
-- Tool results used to be truncated to 1200 characters and thrown away, so a
-- query that answered the user's question in 5000 characters was reported to
-- the model as a fragment ending mid-row, with no way to ask for the rest.
-- The full text lands here and the model gets a handle plus a preview; the
-- read_artifact tool pages through it.
--
-- This is the only table in the agent schema whose payload is unbounded by
-- design, which is why it is the only one NOT in TABLESPACE=innodb_system:
-- the system tablespace never gives space back, so a burst of large results
-- would permanently inflate ibdata1. Its own file-per-table tablespace can be
-- reclaimed with OPTIMIZE TABLE after the retention sweep.
--
-- uk_artifact_hash is what finally gives agent_memory.content_hash's twin a
-- consumer: re-running the same query stores one row and bumps read_count,
-- rather than storing the same megabyte once per turn.
SET @cmd = "CREATE TABLE IF NOT EXISTS agent_artifact (
    artifact_id      VARCHAR(64)  NOT NULL COMMENT 'Opaque handle quoted back by the model',
    principal_prefix CHAR(16)     NOT NULL COMMENT 'Isolation key; a read with no matching prefix returns nothing',
    conversation_id  VARCHAR(64)  DEFAULT NULL,
    turn_id          VARCHAR(64)  DEFAULT NULL COMMENT 'Joins to agent_memory.turn_id',
    kind             VARCHAR(16)  NOT NULL DEFAULT 'result_set' COMMENT 'result_set/text/json/error',
    mime             VARCHAR(64)  NOT NULL DEFAULT 'text/plain',
    content          LONGTEXT     NOT NULL COMMENT 'Full payload; size_bytes is the authority on how much of it there is',
    size_bytes       BIGINT UNSIGNED NOT NULL DEFAULT 0,
    row_count        BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Rows in the result set, 0 when not a result set',
    truncated        TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '1 = the producer itself hit a cap, so content is not the whole result',
    content_hash     CHAR(64)     NOT NULL DEFAULT '' COMMENT 'SHA2(content,256); dedup key',
    source_sql       TEXT         DEFAULT NULL,
    meta             JSON         DEFAULT NULL,
    read_count       INT          NOT NULL DEFAULT 0,
    expires_at       TIMESTAMP    NULL DEFAULT NULL,
    created_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (artifact_id),
    UNIQUE KEY uk_artifact_hash (principal_prefix, content_hash),
    KEY idx_principal_time (principal_prefix, created_at),
    KEY idx_expires (expires_at)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent artifact store (large tool results)'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_file_per_table";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- ---------------------------------------------------------------------------
-- agent_memory_edge: typed relations between things the agent remembers.
--
-- Vector recall answers "what text is similar to this question". It cannot
-- answer "which tables does the fact I just recalled actually talk about",
-- because that is a join, not a distance. The edges make that reachable with
-- a recursive CTE over an indexed table.
--
-- edge_key exists because the natural unique key -- the whole (src, rel, dst)
-- tuple -- is far past InnoDB's 768-byte index limit at innodb_page_size=4k.
-- Hashing the tuple into a generated column keeps the uniqueness and brings
-- the key down to 320 bytes. CHAR(31) is the unit separator, so a value
-- containing the delimiter cannot forge a different tuple's key.
SET @cmd = "CREATE TABLE IF NOT EXISTS agent_memory_edge (
    edge_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    principal_prefix CHAR(16)     NOT NULL COMMENT 'Isolation key; every walk is rooted inside one principal',
    src_kind         VARCHAR(16)  NOT NULL COMMENT 'fact/turn/table/column/artifact',
    src_id           VARCHAR(191) NOT NULL,
    rel              VARCHAR(64)  NOT NULL COMMENT 'mentions/joins/derived_from/supersedes/produced',
    dst_kind         VARCHAR(16)  NOT NULL,
    dst_id           VARCHAR(191) NOT NULL,
    weight           FLOAT        NOT NULL DEFAULT 1,
    meta             JSON         DEFAULT NULL,
    expires_at       TIMESTAMP    NULL DEFAULT NULL,
    created_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    edge_key         CHAR(64) GENERATED ALWAYS AS
                       (SHA2(CONCAT_WS(CHAR(31), src_kind, src_id, rel, dst_kind, dst_id), 256)) STORED
                       COMMENT 'Hash of the edge tuple; stands in for a unique key too wide to index directly',
    UNIQUE KEY uk_edge (principal_prefix, edge_key),
    KEY idx_src (principal_prefix, src_kind, src_id, rel),
    KEY idx_dst (principal_prefix, dst_kind, dst_id, rel),
    KEY idx_expires (expires_at)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent memory graph edges'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- ---------------------------------------------------------------------------
-- agent_derive_queue: work the agent should NOT do on the user's turn.
--
-- A row that should carry a vector and does not is invisible to semantic
-- recall, permanently: when the embedding model was unavailable the write was
-- retried without it, audited as degraded, and never looked at again. The
-- queue makes that recoverable -- the row is recorded here and the vector is
-- filled in on a later turn's maintenance slot, by MEM.derive.drain() in
-- scripts/sys_schema/agent/js/lib_memory_registry.js.
--
-- The drain was a stored procedure (sys.shannon_agent_derive) driven by a
-- scheduler EVENT, because two concurrent JerryScript calls used to abort the
-- server and an EVENT firing mid-conversation was exactly that case. That is
-- fixed -- the engine context is per-thread now -- and the procedure it
-- forced was a system object with no caller: nothing ever created the EVENT.
--
-- uk_task collapses repeat enqueues of the same row into one task, so a retry
-- loop cannot grow the queue without bound.
SET @cmd = "CREATE TABLE IF NOT EXISTS agent_derive_queue (
    task_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    principal_prefix CHAR(16)     NOT NULL,
    kind             VARCHAR(24)  NOT NULL COMMENT 'embed_memory/embed_fact/embed_trace',
    target_table     VARCHAR(64)  NOT NULL COMMENT 'Unqualified name inside the mysql schema',
    target_id        BIGINT UNSIGNED NOT NULL COMMENT 'Primary key of the row to derive from',
    payload          JSON         DEFAULT NULL,
    state            VARCHAR(12)  NOT NULL DEFAULT 'pending' COMMENT 'pending/running/done/failed',
    attempts         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    last_error       VARCHAR(512) DEFAULT NULL,
    lease_owner      VARCHAR(64)  NOT NULL DEFAULT '' COMMENT 'Claim token; a lease that expires returns the task to pending',
    lease_expires_at TIMESTAMP    NULL DEFAULT NULL,
    available_at     TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Retry backoff floor',
    created_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_task (kind, target_table, target_id),
    KEY idx_claim (state, available_at, task_id),
    KEY idx_lease (state, lease_expires_at)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent asynchronous derivation queue'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

-- ---------------------------------------------------------------------------
-- agent_usage: per-principal, per-day metering.
--
-- The only quota that existed was a row cap on long-term facts. Nothing
-- counted model calls, tokens or stored bytes, so one principal could spend
-- an instance's entire model budget and the only trace of it was scattered
-- across audit detail strings.
SET @cmd = "CREATE TABLE IF NOT EXISTS agent_usage (
    principal_prefix  CHAR(16)   NOT NULL,
    usage_date        DATE       NOT NULL,
    turns             BIGINT UNSIGNED NOT NULL DEFAULT 0,
    llm_calls         BIGINT UNSIGNED NOT NULL DEFAULT 0,
    prompt_tokens     BIGINT UNSIGNED NOT NULL DEFAULT 0,
    completion_tokens BIGINT UNSIGNED NOT NULL DEFAULT 0,
    embed_calls       BIGINT UNSIGNED NOT NULL DEFAULT 0,
    tool_calls        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    artifact_bytes    BIGINT UNSIGNED NOT NULL DEFAULT 0,
    llm_ms            BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at        TIMESTAMP  NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (principal_prefix, usage_date)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent per-principal daily usage counters'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_sql_trace  (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY COMMENT 'Primary key',
    conversation_id VARCHAR(64)  NOT NULL COMMENT 'Conversation ID',
    turn_no         INT          NOT NULL COMMENT 'Dialogue turn number',
    step_no         INT          NOT NULL COMMENT 'Step number within the turn (useful when plan_sql has multiple steps)',
    route           VARCHAR(16)  NOT NULL COMMENT 'Route type: catalog/rule/rag/agent_loop',
    tool            VARCHAR(32)  COMMENT 'Tool name: query_db/explain_sql/update_data/plan_sql...',
    sql_text        TEXT         COMMENT 'Actual SQL executed',
    desc_text       TEXT         COMMENT 'Step description (thought or plan step description)',
    result_preview  TEXT         COMMENT 'Truncated result preview',
    is_write        TINYINT(1)   DEFAULT 0 COMMENT 'Distinguish read-only vs write operations for auditing',
    embedding       VECTOR(384)  DEFAULT NULL COMMENT 'embedding of desc_text for few-shot retrieval, model: multilingual-e5-small',
    turn_id         VARCHAR(64)  DEFAULT NULL COMMENT 'Conversation turn this step belongs to. turn_no above is the agent loop iteration, which is a different thing and was the only join key this ledger had; turn_id is what agent_memory.turn_id joins to',
    created_at      TIMESTAMP    DEFAULT CURRENT_TIMESTAMP COMMENT 'Creation timestamp',
    INDEX idx_conv (conversation_id, turn_no, step_no),
    INDEX idx_turn_id (turn_id),
    FULLTEXT KEY ftx_desc (desc_text) WITH PARSER ngram
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent SQL Trace'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_tx_lease (
    conversation_id VARCHAR(64) NOT NULL COMMENT 'Conversation ID',
    session_conn_id BIGINT NOT NULL DEFAULT 0 COMMENT 'MySQL CONNECTION_ID() owning the lease',
    plan_id         VARCHAR(64) DEFAULT '' COMMENT 'Related review plan ID if any',
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at      TIMESTAMP NOT NULL COMMENT 'Lease expiration time',
    PRIMARY KEY (conversation_id),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Transaction Lease — explicit tx ownership tracking'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_review_plan (
    plan_id          VARCHAR(64) NOT NULL COMMENT 'Approval plan identifier',
    conversation_id  VARCHAR(64) NOT NULL COMMENT 'Conversation ID',
    status           VARCHAR(32) NOT NULL DEFAULT 'awaiting_approval' COMMENT 'Plan status: awaiting_approval/completed/rejected/error',
    current_step_index INT       NOT NULL DEFAULT 0 COMMENT 'Index of the next pending step (0-based)',
    total_steps      INT       NOT NULL DEFAULT 0 COMMENT 'Total number of steps in the plan',
    description      TEXT      COMMENT 'Optional plan description',
    plan_json        TEXT      COMMENT 'Serialized plan metadata and step summaries',
    created_at       TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (plan_id),
    INDEX idx_conv_status (conversation_id, status)
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Review Plans'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_review_plan_step (
    plan_id        VARCHAR(64) NOT NULL COMMENT 'Approval plan identifier',
    step_no        INT       NOT NULL COMMENT 'Step number within the plan',
    tool           VARCHAR(32) COMMENT 'Tool name or action for this step',
    sql_text       TEXT      COMMENT 'SQL to execute for this step',
    args_json      TEXT      COMMENT 'Serialized tool args',
    affected_tables TEXT     COMMENT 'Comma-separated affected tables',
    writes         TINYINT(1) DEFAULT 0 COMMENT 'Whether the step writes data',
    ddl            TINYINT(1) DEFAULT 0 COMMENT 'Whether the step changes schema',
    transactional  TINYINT(1) DEFAULT 1 COMMENT 'Whether to wrap in BEGIN/COMMIT transaction',
    risk           VARCHAR(16) COMMENT 'Risk classification',
    estimated_rows VARCHAR(64) COMMENT 'Estimated affected row count',
    status         VARCHAR(32) DEFAULT 'pending' COMMENT 'Step status: pending/approved/failed',
    result_preview TEXT      COMMENT 'Execution result summary',
    error_text     TEXT      COMMENT 'Execution error details',
    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (plan_id, step_no),
    INDEX idx_plan_step (plan_id, step_no)
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Review Plan Steps'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_review_history (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY COMMENT 'Primary key',
    conversation_id VARCHAR(64) NOT NULL COMMENT 'Conversation ID',
    plan_id        VARCHAR(64) NOT NULL COMMENT 'Approval plan identifier',
    step_no        INT      NOT NULL COMMENT 'Step number within the plan',
    action         VARCHAR(32) NOT NULL COMMENT 'Action taken: approve/reject/modify',
    user_cmd       TEXT     COMMENT 'Original user command text',
    comment        TEXT     COMMENT 'Additional commentary or reason',
    result_preview TEXT     COMMENT 'Result summary after action',
    rollback_flag  TINYINT(1) DEFAULT 0 COMMENT 'Whether the step was rolled back',
    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_history_plan (plan_id, step_no)
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Review History'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS agent_rollback_log (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY COMMENT 'Primary key',
    conversation_id VARCHAR(64) NOT NULL COMMENT 'Conversation ID',
    plan_id        VARCHAR(64) NOT NULL COMMENT 'Approval plan identifier',
    step_no        INT      NOT NULL COMMENT 'Step number within the plan',
    sql_text       TEXT     COMMENT 'SQL that triggered the rollback',
    error_text     TEXT     COMMENT 'Error details',
    rollback_reason TEXT    COMMENT 'Reason for rollback',
    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_rollback_plan (plan_id, step_no)
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Rollback Log'
  ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS shannon_agent_plugins  (
    plugin_name    VARCHAR(128)  NOT NULL COMMENT 'plugin name, must be unique',
    schema_name    VARCHAR(64)   NOT NULL COMMENT 'plugin agent function schema',
    function_name  VARCHAR(64)   NOT NULL COMMENT 'function name (must match shannon_chat signature)',
    enabled        TINYINT(1)    NOT NULL DEFAULT 1  COMMENT '1=enabled, 0=disabled',
    priority       INT           NOT NULL DEFAULT 100 COMMENT 'priority (smaller is higher)',
    description    TEXT                               COMMENT 'plugin description',
    creator        VARCHAR(128)  NOT NULL DEFAULT (CURRENT_USER()) COMMENT 'creator',
    created_at     TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at     TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP
                                          ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (plugin_name),
    KEY idx_enabled_priority (enabled, priority)
) ENGINE=InnoDB CHARACTER SET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci  STATS_PERSISTENT=0 COMMENT='ShannonBase Agent Plugins' ROW_FORMAT=DYNAMIC TABLESPACE=innodb_system";
SET @str = CONCAT(@cmd, " ENCRYPTION='", @is_mysql_encrypted, "'");
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;
