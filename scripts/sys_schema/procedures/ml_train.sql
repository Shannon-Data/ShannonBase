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

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_train (
        IN in_table_name VARCHAR(64),
        IN in_target_name VARCHAR(64),
        IN in_option JSON,
        IN in_model_handle VARCHAR(64)
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
    MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name_check VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);
    DECLARE v_db_err_msg TEXT;

    DECLARE v_train_obj_check INT;
    DECLARE v_train_schema_name VARCHAR(64);
    DECLARE v_train_table_name VARCHAR(64);
    DECLARE v_model_name VARCHAR(255);

    DECLARE table_size_gb DECIMAL(10, 2);
    DECLARE table_rows BIGINT;
    DECLARE column_count INT;

    IF in_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
    END IF;

    SELECT SUBSTRING_INDEX(in_table_name, '.', 1) INTO v_train_schema_name;
    SELECT SUBSTRING_INDEX(in_table_name, '.', -1) INTO v_train_table_name;

    SELECT ROUND((DATA_LENGTH + INDEX_LENGTH) / 1024 / 1024 / 1024, 2) INTO table_size_gb
    FROM information_schema.TABLES  WHERE TABLE_SCHEMA = v_train_schema_name  AND TABLE_NAME = v_train_table_name;

    SELECT TABLE_ROWS  INTO table_rows FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name;

    SELECT COUNT(*) INTO column_count FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name;

    IF table_size_gb > 10 THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'The table cannot exceed 10 GB';
    END IF;

    IF table_rows > 100000000 THEN
        SIGNAL SQLSTATE '45001'
        SET MESSAGE_TEXT = 'The table cannot exceed 100 million rows';
    END IF;

    IF column_count > 1017 THEN
        SIGNAL SQLSTATE '45002'
        SET MESSAGE_TEXT = 'The table cannot exceed 1017 columns';
    END IF;

    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;  
    SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);
  
    SELECT SCHEMA_NAME INTO v_db_name_check
      FROM INFORMATION_SCHEMA.SCHEMATA
    WHERE SCHEMA_NAME = v_sys_schema_name;

    IF v_db_name_check IS NULL THEN
        START TRANSACTION;
        SET @create_db_stmt = CONCAT('CREATE DATABASE ', v_sys_schema_name, ';');
        PREPARE create_db_stmt FROM @create_db_stmt;
        EXECUTE create_db_stmt;
        DEALLOCATE PREPARE create_db_stmt;

        SET @create_tb_stmt = CONCAT(' CREATE TABLE ', v_sys_schema_name, '.MODEL_CATALOG(
                                        MODEL_ID INT NOT NULL AUTO_INCREMENT,
                                        MODEL_HANDLE VARCHAR(255) UNIQUE,
                                        MODEL_OBJECT JSON DEFAULT NULL,
                                        MODEL_OWNER VARCHAR(255) DEFAULT NULL,
                                        MODEL_OBJECT_SIZE INT DEFAULT 0,
                                        MODEL_METADATA JSON DEFAULT NULL,
                                        PRIMARY KEY (MODEL_ID));');
        PREPARE create_tb_stmt FROM @create_tb_stmt;
        EXECUTE create_tb_stmt;
        DEALLOCATE PREPARE create_tb_stmt;

        SET @create_tb_stmt = CONCAT(' CREATE TABLE ', v_sys_schema_name, '.MODEL_OBJECT_CATALOG (
                                        CHUNK_ID INT NOT NULL AUTO_INCREMENT,
                                        MODEL_HANDLE VARCHAR(255),
                                        MODEL_OBJECT JSON DEFAULT NULL,
                                        PRIMARY KEY (CHUNK_ID, MODEL_HANDLE));');
        PREPARE create_tb_stmt FROM @create_tb_stmt;
        EXECUTE create_tb_stmt;
        DEALLOCATE PREPARE create_tb_stmt;

        SET @add_fk_tb_stmt = CONCAT(' ALTER TABLE ', v_sys_schema_name, '.MODEL_OBJECT_CATALOG
                                     ADD CONSTRAINT fk_cat_handle_objcat_handl FOREIGN KEY (MODEL_HANDLE)',
                                     'REFERENCES ', v_sys_schema_name, '.MODEL_CATALOG(MODEL_HANDLE);');
        PREPARE add_fk_tb_stmt FROM @add_fk_tb_stmt;
        EXECUTE add_fk_tb_stmt;
        DEALLOCATE PREPARE add_fk_tb_stmt;
        COMMIT;
    END IF;
    
    SELECT COUNT(*) INTO v_train_obj_check
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name;
    IF v_train_obj_check = 0 THEN
        SET v_db_err_msg = CONCAT(in_table_name, ' used to do training does not exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    IF in_model_handle IS NOT NULL THEN
      SET @select_model_stm = CONCAT('SELECT COUNT(MODEL_HANDLE) INTO @MODEL_HANDLE_COUNT  FROM ',  v_sys_schema_name,
                                     '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', in_model_handle, '\";');
      PREPARE select_model_stmt FROM @select_model_stm;
      EXECUTE select_model_stmt;
      SELECT @MODEL_HANDLE_COUNT INTO v_train_obj_check;
      DEALLOCATE PREPARE select_model_stmt;

      IF v_train_obj_check > 0 THEN
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = "The model has already existed.";
      END IF;
    ELSE
      SET in_model_handle = CONCAT(in_table_name, '_', v_user_name, '_', SUBSTRING(MD5(RAND()), 1, 10));
    END IF;

    IF in_target_name IS NOT NULL THEN
      SELECT COUNT(COLUMN_NAME) INTO v_train_obj_check
      FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = v_train_schema_name AND TABLE_NAME = v_train_table_name AND COLUMN_NAME = in_target_name;
      IF v_train_obj_check = 0 THEN
          SET v_db_err_msg = CONCAT('column ', in_target_name, ' labelled does not exists in ', v_train_table_name);
          SIGNAL SQLSTATE 'HY000'
              SET MESSAGE_TEXT = v_db_err_msg;
      END IF;
    END IF;

    -- DN NOT REMOVE STAART TRANSACTION AND COMMIT STATEMENTS. THEY ARE REQUIRED FOR THE TRANSACTIONAL CONSISTENCY
    -- IN ML_TRAIN, WE INSERT META INFO INTO MODEL_CATALOG TABLE
    IF in_option IS NULL THEN
      SET in_option = JSON_OBJECT('task', 'classification');
    END IF;

    START TRANSACTION;
    SELECT ML_TRAIN(in_table_name, in_target_name, in_option, in_model_handle) INTO v_train_obj_check;
    COMMIT;

    IF v_train_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_TRAIN failed.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;
END$$
DELIMITER ;
