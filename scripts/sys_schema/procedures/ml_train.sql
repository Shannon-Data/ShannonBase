-- Copyright (c) 2014, 2023, Oracle and/or its affiliates.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
-- Copyright (c) 2023, Shannon Data AI and/or its affiliates.

DROP PROCEDURE IF EXISTS ml_train;
DELIMITER $$

CREATE PROCEDURE ml_train (
        IN in_table_name VARCHAR(64), IN in_target_name VARCHAR(64), IN in_option JSON, IN in_model_handle VARCHAR(64)
    )
    COMMENT '
Description
-----------

Run the ML_TRAIN routine on a labeled training dataset to produce a trained machine learning model.

Parameters
-----------
in_table_name (VARCHAR(64)):
  fully qualified name of the table containing the training dataset.
in_target_name (VARCHAR(64)):
  name of the column in \'table_name\' representing the target, i.e. ground truth values (required for some tasks)
in_option (JSON)
  optional training parameters as key-value pairs in JSON format.
    1: The most important parameter is \'task\', which specifies the ML task to be performed (if not specified, \'classification\' is assumed)
    2: Other parameters allow finer-grained control on the training task
in_model_handle (VARCHAR(64))
   user-defined session variable storing the ML model handle for the duration of the connection
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_TRAIN(\'ml_data.iris_train\', \'class\', 
          JSON_OBJECT(\'task\', \'classification\'), 
          @iris_model);
...    
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    CONTAINS SQL
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name_check VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);
    DECLARE v_db_err_msg TEXT;

    DECLARE v_train_obj_check INT;
    DECLARE v_train_schema_name VARCHAR(64);
    DECLARE v_train_table_name VARCHAR(64);

    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;  
    SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);
  
    SELECT SCHEMA_NAME INTO v_db_name_check
      FROM INFORMATION_SCHEMA.SCHEMATA
    WHERE SCHEMA_NAME = v_sys_schema_name;

    IF v_db_name_check IS NULL THEN
        SET @create_db_stmt = CONCAT('CREATE DATABASE ', v_sys_schema_name, ';');
        PREPARE create_db_stmt FROM @create_db_stmt;
        EXECUTE create_db_stmt;
        DEALLOCATE PREPARE create_db_stmt;

        SET @create_tb_stmt = CONCAT(' CREATE TABLE ', v_sys_schema_name, '.MODEL_CATALOG(
                                        MODEL_ID INT NOT NULL AUTO_INCREMENT,
                                        MODEL_HANDLE VARCHAR(255),
                                        MODEL_OBJECT JSON,
                                        MODEL_OWNER VARCHAR(64),
                                        BUILD_TIMESTAMP TIMESTAMP,
                                        TARGET_COLUMN_NAME VARCHAR(64),
                                        TRAIN_TABLE_NAME VARCHAR(255),
                                        MODEL_OBJECT_SIZE INT,
                                        MODEL_TYPE  VARCHAR(64),
                                        TASK  VARCHAR(64),
                                        COLUMN_NAMES VARCHAR(1024),
                                        MODEL_EXPLANATION NUMERIC,
                                        LAST_ACCESSED TIMESTAMP,
                                        MODEL_METADATA JSON,
                                        NOTES VARCHAR(1024),
                                        PRIMARY KEY (MODEL_ID));');
        PREPARE create_tb_stmt FROM @create_tb_stmt;
        EXECUTE create_tb_stmt;
        DEALLOCATE PREPARE create_tb_stmt;
    END IF;

    SELECT SUBSTRING_INDEX(in_table_name, '.', 1) INTO v_train_schema_name;
    SELECT SUBSTRING_INDEX(in_table_name, '.', -1) INTO v_train_table_name;
    
    SELECT COUNT(*) INTO v_train_obj_check
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name;
    IF v_train_obj_check = 0 THEN
        SET v_db_err_msg = CONCAT(in_table_name, ' does not exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;
  
    SELECT COUNT(COLUMN_NAME) INTO v_train_obj_check
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name AND COLUMN_NAME = in_target_name;
    IF v_train_obj_check = 0 THEN
        SET v_db_err_msg = CONCAT(in_target_name, ' does not exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

END$$
DELIMITER ;