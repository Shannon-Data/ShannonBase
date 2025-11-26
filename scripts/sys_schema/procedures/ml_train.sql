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
    DECLARE v_user_name          VARCHAR(64);
    DECLARE v_sys_schema_name    VARCHAR(64);
    DECLARE v_db_name_check      VARCHAR(64);
    DECLARE v_train_schema_name  VARCHAR(64);
    DECLARE v_train_table_name   VARCHAR(64);
    DECLARE v_task               VARCHAR(64) DEFAULT 'classification';
    DECLARE v_model_handle       VARCHAR(255);
    DECLARE v_err_msg            TEXT DEFAULT '';

    DECLARE v_train_obj_check    INT DEFAULT 0;
    DECLARE table_size_gb        DECIMAL(10,2);
    DECLARE table_rows           BIGINT;
    DECLARE column_count         INT;
    DECLARE v_temp_count         INT DEFAULT 0;

    DECLARE o_model_object       JSON;
    DECLARE o_model_metadata     JSON;

    DECLARE v_model_object_size  BIGINT UNSIGNED DEFAULT 0;
    DECLARE v_model_object_text  LONGTEXT;
    DECLARE v_chunk_size         INT DEFAULT 16777216; -- 16MB
    DECLARE v_chunk_id           INT DEFAULT 1;
    DECLARE v_offset             BIGINT DEFAULT 0;
    DECLARE v_remain             BIGINT;

    -- Step 1-3 : Basic Validation (table name, size, rows, columns)
    IF in_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Invalid schema.table format';
    END IF;

    SELECT SUBSTRING_INDEX(in_table_name,'.',1)  INTO v_train_schema_name;
    SELECT SUBSTRING_INDEX(in_table_name,'.',-1) INTO v_train_table_name;

    SELECT ROUND((DATA_LENGTH + INDEX_LENGTH)/1024/1024/1024,2)
           INTO table_size_gb
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name
      AND TABLE_NAME   = v_train_table_name;

    SELECT TABLE_ROWS INTO table_rows
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_train_schema_name
      AND TABLE_NAME   = v_train_table_name;

    SELECT COUNT(*) INTO column_count
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = v_train_schema_name
      AND TABLE_NAME   = v_train_table_name;

    IF table_size_gb > 10 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table cannot exceed 10 GB';
    END IF;
    IF table_rows > 100000000 THEN
        SIGNAL SQLSTATE '45001' SET MESSAGE_TEXT = 'Table cannot exceed 100 million rows';
    END IF;
    IF column_count > 1017 THEN
        SIGNAL SQLSTATE '45002' SET MESSAGE_TEXT = 'Table cannot exceed 1017 columns';
    END IF;

    -- Step 4 : Create User-specific ML_SCHEMA_xxx
    SELECT SUBSTRING_INDEX(CURRENT_USER(),'@',1) INTO v_user_name;
    SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);

    SELECT SCHEMA_NAME INTO v_db_name_check
    FROM INFORMATION_SCHEMA.SCHEMATA
    WHERE SCHEMA_NAME = v_sys_schema_name;

    IF v_db_name_check IS NULL THEN
        START TRANSACTION;

        SET @sql = CONCAT('CREATE DATABASE ', v_sys_schema_name);
        PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

        -- -------- MODEL_CATALOG (latest official structure) ----------
        SET @sql = CONCAT('
            CREATE TABLE ', v_sys_schema_name, '.MODEL_CATALOG (
                MODEL_ID            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
                MODEL_HANDLE        VARCHAR(255) NOT NULL,
                MODEL_OBJECT        JSON DEFAULT NULL COMMENT "Always NULL - stored in MODEL_OBJECT_CATALOG",
                MODEL_OWNER         VARCHAR(255) DEFAULT NULL,
                BUILD_TIMESTAMP     BIGINT UNSIGNED NOT NULL DEFAULT (unix_timestamp()),
                TARGET_COLUMN_NAME  VARCHAR(64)  DEFAULT NULL,
                TRAIN_TABLE_NAME    VARCHAR(255) NOT NULL,
                MODEL_OBJECT_SIZE   BIGINT UNSIGNED NOT NULL DEFAULT 0,
                MODEL_TYPE          VARCHAR(128) DEFAULT NULL,
                TASK                VARCHAR(64)  DEFAULT NULL,
                COLUMN_NAMES        JSON         DEFAULT NULL,
                MODEL_EXPLANATION   JSON         DEFAULT NULL,
                LAST_ACCESSED       BIGINT UNSIGNED DEFAULT NULL,
                MODEL_METADATA      JSON         DEFAULT NULL,
                NOTES               TEXT         DEFAULT NULL,
                PRIMARY KEY (MODEL_ID),
                UNIQUE KEY uk_model_handle (MODEL_HANDLE),
                KEY idx_owner (MODEL_OWNER),
                KEY idx_train_table (TRAIN_TABLE_NAME),
                KEY idx_task (TASK),
                KEY idx_last_accessed (LAST_ACCESSED),
                KEY idx_build_time (BUILD_TIMESTAMP)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
            COMMENT="ShannonBase ML Model Catalog"');
        PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

        -- -------- MODEL_OBJECT_CATALOG (store large models in chunks) ----------
        SET @sql = CONCAT('
            CREATE TABLE ', v_sys_schema_name, '.MODEL_OBJECT_CATALOG (
                CHUNK_ID      INT UNSIGNED NOT NULL,
                MODEL_HANDLE  VARCHAR(255) NOT NULL,
                MODEL_OBJECT  LONGTEXT NOT NULL,
                PRIMARY KEY uk_handle_chunk (MODEL_HANDLE, CHUNK_ID),
                KEY idx_handle (MODEL_HANDLE)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
            COMMENT="Large model binaries stored in chunks"
        ');
        PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;
        COMMIT;
    END IF;

    -- Step 5-7 : Table existence, handle, target column validation.
    IF (SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES
        WHERE TABLE_SCHEMA=v_train_schema_name AND TABLE_NAME=v_train_table_name) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Training table does not exist';
    END IF;

    IF in_model_handle IS NOT NULL THEN
        SET v_model_handle = in_model_handle;
        SET @v_temp_count = 0;
        SET @sql_check = CONCAT('SELECT COUNT(*) INTO @v_temp_count FROM `',
                                v_sys_schema_name, '`.MODEL_CATALOG WHERE MODEL_HANDLE = ',
                                QUOTE(v_model_handle));
        PREPARE stmt FROM @sql_check;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET v_temp_count = @v_temp_count;

        IF v_temp_count > 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model handle already exists';
        END IF;
    ELSE
        SET v_model_handle = CONCAT(in_table_name,'_',v_user_name,'_',SUBSTRING(MD5(RAND()),1,10));
    END IF;

    IF in_target_name IS NOT NULL AND
       (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS
        WHERE TABLE_SCHEMA=v_train_schema_name
          AND TABLE_NAME=v_train_table_name
          AND COLUMN_NAME=in_target_name) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Target column does not exist';
    END IF;

    -- Step 8 : Options processing
    IF in_option IS NULL THEN
        SET in_option = JSON_OBJECT('task','classification');
    END IF;
    SET v_task = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_option,'$.task')),'classification');

    -- Step 9 : Call underlying native ML_TRAIN
    START TRANSACTION;
    SELECT ML_TRAIN(in_table_name,
                    in_target_name,
                    in_option,
                    v_model_handle,
                    o_model_object,
                    o_model_metadata) INTO v_train_obj_check;
    COMMIT;

    IF v_train_obj_check != 0 THEN
        SET v_err_msg = CONCAT('ML_TRAIN native function failed with code ', v_train_obj_check);
        ROLLBACK;
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_err_msg;
    END IF;

    START TRANSACTION;
    SET @model_json_extracted = NULL;
    -- see the ref: `Utils::ML_train`
    SET @model_json_extracted = JSON_EXTRACT(o_model_object, '$.SHANNON_LIGHTGBM_CONTENT');
    IF @model_json_extracted IS NOT NULL AND @model_json_extracted != 'null' THEN
        SET v_model_object_text = JSON_UNQUOTE(@model_json_extracted);
    ELSE
        SET v_model_object_text = JSON_UNQUOTE(o_model_object);
    END IF;
    SET v_model_object_size = LENGTH(v_model_object_text);

    -- insert into catalog
    SET @cat_sql = CONCAT(
        'INSERT INTO `', v_sys_schema_name, '`.MODEL_CATALOG ',
        '(MODEL_HANDLE, MODEL_OWNER, TARGET_COLUMN_NAME, TRAIN_TABLE_NAME, ',
        'MODEL_OBJECT_SIZE, TASK, MODEL_METADATA, NOTES) VALUES (',
        QUOTE(v_model_handle), ',',
        QUOTE(v_user_name), ',',
        IF(in_target_name IS NULL, 'NULL', QUOTE(in_target_name)), ',',
        QUOTE(in_table_name), ',',
        v_model_object_size, ',',
        QUOTE(v_task), ',',
        QUOTE(o_model_metadata), ',',
        QUOTE('Model created by ml_train - compressed'),
        ')'
    );

    PREPARE s FROM @cat_sql; EXECUTE s; DEALLOCATE PREPARE s;

    -- write in chunks
    SET v_remain = v_model_object_size;
    SET v_offset = 0;
    SET v_chunk_id = 1;

    chunk_loop: WHILE v_remain > 0 DO
        SET @chunk = SUBSTRING(v_model_object_text, v_offset + 1, LEAST(v_chunk_size, v_remain));
        
        SET @v_model_handle = v_model_handle;
        SET @v_chunk_id = v_chunk_id;
        SET @chunk_sql = CONCAT('INSERT INTO `', v_sys_schema_name, '`.MODEL_OBJECT_CATALOG ',
                        '(MODEL_HANDLE, CHUNK_ID, MODEL_OBJECT) VALUES (?, ?, ?)');
        PREPARE s FROM @chunk_sql;
        EXECUTE s USING @v_model_handle, @v_chunk_id, @chunk;
        DEALLOCATE PREPARE s;

        SET v_offset = v_offset + v_chunk_size;
        SET v_remain = v_remain - v_chunk_size;
        SET v_chunk_id = v_chunk_id + 1;
    END WHILE chunk_loop;
COMMIT;
END$$
DELIMITER ;