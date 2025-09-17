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
    -- Variable declarations for validation and processing
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name_check VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);
    DECLARE v_db_err_msg TEXT;

    -- Table and model validation variables
    DECLARE v_train_obj_check INT;
    DECLARE v_train_schema_name VARCHAR(64);
    DECLARE v_train_table_name VARCHAR(64);
    DECLARE v_model_name VARCHAR(255);

    -- Table constraint variables
    DECLARE table_size_gb DECIMAL(10, 2);
    DECLARE table_rows BIGINT;
    DECLARE column_count INT;

    -- Task and option processing variables
    DECLARE v_task VARCHAR(64) DEFAULT 'classification';
    DECLARE v_datetime_index VARCHAR(64);
    DECLARE v_endogenous_vars JSON;
    DECLARE v_exogenous_vars JSON;
    DECLARE v_model_list JSON;
    DECLARE v_exclude_models JSON;
    DECLARE v_optimization_metric VARCHAR(64);
    DECLARE v_contamination DECIMAL(5,4);
    DECLARE v_users_column VARCHAR(64);
    DECLARE v_items_column VARCHAR(64);
    DECLARE v_feedback VARCHAR(64);
    DECLARE v_document_column VARCHAR(64);

    -- Step 1: Validate table name format
    IF in_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
    END IF;

    -- Step 2: Extract schema and table names
    SELECT SUBSTRING_INDEX(in_table_name, '.', 1) INTO v_train_schema_name;
    SELECT SUBSTRING_INDEX(in_table_name, '.', -1) INTO v_train_table_name;

    -- Step 3: Validate table size constraints
    SELECT ROUND((DATA_LENGTH + INDEX_LENGTH) / 1024 / 1024 / 1024, 2) INTO table_size_gb
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name
    AND TABLE_NAME = v_train_table_name;

    SELECT TABLE_ROWS INTO table_rows
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name
    AND TABLE_NAME = v_train_table_name;

    SELECT COUNT(*) INTO column_count
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = v_train_schema_name
    AND TABLE_NAME = v_train_table_name;

    -- Enforce size constraints
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

    -- Step 4: Setup ML schema for user
    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;  
    SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);

    SELECT SCHEMA_NAME INTO v_db_name_check
    FROM INFORMATION_SCHEMA.SCHEMATA
    WHERE SCHEMA_NAME = v_sys_schema_name;

    -- Create ML schema and catalog tables if they don't exist
    IF v_db_name_check IS NULL THEN
        START TRANSACTION;

        -- Create ML schema
        SET @create_db_stmt = CONCAT('CREATE DATABASE ', v_sys_schema_name, ';');
        PREPARE create_db_stmt FROM @create_db_stmt;
        EXECUTE create_db_stmt;
        DEALLOCATE PREPARE create_db_stmt;

        -- Create MODEL_CATALOG table
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

        -- Create MODEL_OBJECT_CATALOG table
        SET @create_tb_stmt = CONCAT(' CREATE TABLE ', v_sys_schema_name, '.MODEL_OBJECT_CATALOG (
                                        CHUNK_ID INT NOT NULL AUTO_INCREMENT,
                                        MODEL_HANDLE VARCHAR(255),
                                        MODEL_OBJECT JSON DEFAULT NULL,
                                        PRIMARY KEY (CHUNK_ID, MODEL_HANDLE));');
        PREPARE create_tb_stmt FROM @create_tb_stmt;
        EXECUTE create_tb_stmt;
        DEALLOCATE PREPARE create_tb_stmt;

        -- Add foreign key constraint
        SET @add_fk_tb_stmt = CONCAT(' ALTER TABLE ', v_sys_schema_name, '.MODEL_OBJECT_CATALOG
                                     ADD CONSTRAINT fk_cat_handle_objcat_handl FOREIGN KEY (MODEL_HANDLE) ',
                                     'REFERENCES ', v_sys_schema_name, '.MODEL_CATALOG(MODEL_HANDLE);');
        PREPARE add_fk_tb_stmt FROM @add_fk_tb_stmt;
        EXECUTE add_fk_tb_stmt;
        DEALLOCATE PREPARE add_fk_tb_stmt;

        COMMIT;
    END IF;

    -- Step 5: Validate training table exists
    SELECT COUNT(*) INTO v_train_obj_check
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name
    AND TABLE_NAME = v_train_table_name;

    IF v_train_obj_check = 0 THEN
        SET v_db_err_msg = CONCAT(in_table_name, ' used to do training does not exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    -- Step 6: Handle model handle - check if exists or generate new one
    IF in_model_handle IS NOT NULL THEN
        SET @select_model_stm = CONCAT('SELECT COUNT(MODEL_HANDLE) INTO @MODEL_HANDLE_COUNT FROM ',  
                                       v_sys_schema_name,
                                       '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', 
                                       in_model_handle, '\";');
        PREPARE select_model_stmt FROM @select_model_stm;
        EXECUTE select_model_stmt;
        SELECT @MODEL_HANDLE_COUNT INTO v_train_obj_check;
        DEALLOCATE PREPARE select_model_stmt;

        IF v_train_obj_check > 0 THEN
            SIGNAL SQLSTATE 'HY000'
                SET MESSAGE_TEXT = "The model has already existed.";
        END IF;
    ELSE
        -- Generate unique model handle
        SET in_model_handle = CONCAT(in_table_name, '_', v_user_name, '_', 
                                   SUBSTRING(MD5(RAND()), 1, 10));
    END IF;

    -- Step 7: Validate target column exists (if specified)
    IF in_target_name IS NOT NULL THEN
        SELECT COUNT(COLUMN_NAME) INTO v_train_obj_check
        FROM INFORMATION_SCHEMA.COLUMNS
        WHERE TABLE_SCHEMA = v_train_schema_name
        AND TABLE_NAME = v_train_table_name 
        AND COLUMN_NAME = in_target_name;

        IF v_train_obj_check = 0 THEN
            SET v_db_err_msg = CONCAT('column ', in_target_name, 
                                    ' labelled does not exists in ', v_train_table_name);
            SIGNAL SQLSTATE 'HY000'
                SET MESSAGE_TEXT = v_db_err_msg;
        END IF;
    END IF;

    -- Step 8: Process training options and extract task-specific parameters
    IF in_option IS NULL THEN
        SET in_option = JSON_OBJECT('task', 'classification');
    END IF;

    -- Extract task type
    SET v_task = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.task')), 'classification');

    -- Extract common parameters
    SET v_model_list = JSON_EXTRACT(in_option, '$.model_list');
    SET v_exclude_models = JSON_EXTRACT(in_option, '$.exclude_model_list');
    SET v_optimization_metric = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.optimization_metric'));

    -- Extract task-specific parameters
    CASE v_task
        WHEN 'forecasting' THEN
            SET v_datetime_index = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.datetime_index'));
            SET v_endogenous_vars = JSON_EXTRACT(in_option, '$.endogenous_variables');
            SET v_exogenous_vars = JSON_EXTRACT(in_option, '$.exogenous_variables');

            -- Validate required forecasting parameters
            IF v_datetime_index IS NULL OR v_endogenous_vars IS NULL THEN
                SIGNAL SQLSTATE 'HY000'
                    SET MESSAGE_TEXT = 'Forecasting requires datetime_index and endogenous_variables';
            END IF;

        WHEN 'anomaly_detection' THEN
            SET v_contamination = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.contamination'));
            IF v_contamination IS NULL THEN
                SET v_contamination = 0.01;
            END IF;

            -- Validate contamination range
            IF v_contamination <= 0 OR v_contamination >= 0.5 THEN
                SIGNAL SQLSTATE 'HY000'
                    SET MESSAGE_TEXT = 'Contamination must be between 0 and 0.5';
            END IF;

        WHEN 'recommendation' THEN
            SET v_users_column = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.users'));
            SET v_items_column = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.items'));
            SET v_feedback = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.feedback')), 'explicit');

            -- Validate required recommendation parameters
            IF v_users_column IS NULL OR v_items_column IS NULL THEN
                SIGNAL SQLSTATE 'HY000'
                    SET MESSAGE_TEXT = 'Recommendation requires users and items columns';
            END IF;

        WHEN 'topic_modeling' THEN
            SET v_document_column = JSON_UNQUOTE(JSON_EXTRACT(in_option, '$.document_column'));

            IF v_document_column IS NULL THEN
                SIGNAL SQLSTATE 'HY000'
                    SET MESSAGE_TEXT = 'Topic modeling requires document_column';
            END IF;
        ELSE
            -- Classification and regression use defaults
            SET v_task = v_task;
    END CASE;

    -- Step 9: Execute the actual ML training
    -- Note: This calls the native ML_TRAIN function which handles the actual model training
    START TRANSACTION;

    -- Call the native ML_TRAIN function with validated parameters
    SELECT ML_TRAIN(in_table_name, in_target_name, in_option, in_model_handle) INTO v_train_obj_check;

    COMMIT;

    -- Step 10: Check training result and handle errors
    IF v_train_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_TRAIN failed with error code: ', v_train_obj_check);
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    -- Step 11: Log successful training completion
    -- The model metadata and objects are automatically stored by the native ML_TRAIN function

END$$
DELIMITER ;
