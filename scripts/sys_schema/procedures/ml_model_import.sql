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

DROP PROCEDURE IF EXISTS ml_model_import;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_model_import (
        IN in_model_content LONGTEXT,
        IN in_metadata JSON,
        IN in_model_handle_name VARCHAR(256)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_IMPORT routine to import an existed trained model into a new model handle.

Parameters
-----------

in_model_content LONGTEXT:
  model content.
in_metadata (JSON)
  optional training parameters as key-value pairs in JSON format.
    1: The most important parameter is \'task\', which specifies the ML task to be performed (if not specified, \'classification\' is assumed)
    2: Other parameters allow finer-grained control on the training task
in_model_handle_name (VARCHAR(64))
   user-defined session variable storing the ML model handle for the duration of the connection
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_MODEL_IMPORT(\'xxxx\', JSON_OBJECT(\'task\', \'classification\'),
          @iris_model);
...
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_meta_catalog_name VARCHAR(64);
    DECLARE v_sys_meta_catalog_obj_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_import_obj_check INT;
    DECLARE v_schema  VARCHAR(64);
    DECLARE v_table VARCHAR(64);
    DECLARE v_model_cnt INT;
    DECLARE v_model_meta JSON;
    DECLARE v_model_object LONGTEXT;
    DECLARE v_target_column VARCHAR(64);
    DECLARE v_train_table_name VARCHAR(128);
    DECLARE v_model_type VARCHAR(64);
    DECLARE v_task VARCHAR(64);
    DECLARE v_column_names JSON;
    DECLARE v_current_timestamp BIGINT;
    DECLARE v_format VARCHAR(64);
    DECLARE v_chunks INT;
    DECLARE v_total_size BIGINT;
    DECLARE v_chunk_id INT;
    DECLARE v_final_metadata JSON;

    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
    SET v_sys_meta_catalog_name = CONCAT('ML_SCHEMA_', v_user_name, '.MODEL_CATALOG');
    SET v_sys_meta_catalog_obj_name = CONCAT('ML_SCHEMA_', v_user_name, '.MODEL_OBJECT_CATALOG');
    SET v_current_timestamp = UNIX_TIMESTAMP();

    IF in_metadata IS NULL THEN
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = "The options missed.";
    END IF;

    IF in_model_handle_name IS NULL THEN
        SET in_model_handle_name = CONCAT('ML_SCHEMA_', v_user_name, '_', SUBSTRING(MD5(RAND()), 1, 10));
    END IF;

    -- Check if model handle already exists
    SET @select_model_stm = CONCAT('SELECT COUNT(MODEL_ID) INTO @MODEL_CNT FROM ',  v_sys_meta_catalog_name,
                                  ' WHERE MODEL_HANDLE = \"', in_model_handle_name, '\";');
    PREPARE select_model_stmt FROM @select_model_stm;
    EXECUTE select_model_stmt;
    SELECT @MODEL_CNT into v_model_cnt;
    DEALLOCATE PREPARE select_model_stmt;

    IF (v_model_cnt != 0) THEN
        SIGNAL SQLSTATE 'HY000'
           SET MESSAGE_TEXT = "The handle name you specify is already exists.";
    END IF;

    -- Extract metadata values with defaults
    SET v_target_column = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.target_column_name'), 
                            JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.target_column_name')), 'target');
    SET v_train_table_name = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.train_table_name'), 
                               JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.train_table_name')), 'imported_model');
    SET v_model_type = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.model_type'), 
                         JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.model_type')), 'imported');
    SET v_task = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.task'), 
                   JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.task')), 'classification');
    SET v_column_names = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.column_names'), 
                           JSON_EXTRACT(in_metadata, '$.column_names'), JSON_ARRAY());
    SET v_format = IF(JSON_CONTAINS_PATH(in_metadata, 'one', '$.format'), 
                     JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.format')), 'ONNX');

    -- v_final_metadata
    SET v_final_metadata = in_metadata;

    -- Import from a table (in_model_content is NULL as per documentation)
    IF in_model_content IS NULL AND JSON_CONTAINS_PATH(in_metadata, 'one', '$.schema') AND JSON_CONTAINS_PATH(in_metadata, 'one', '$.table') THEN
        SELECT JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.schema')), JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$.table')) INTO v_schema, v_table;

        -- Check if table exists
        SELECT COUNT(*) INTO v_import_obj_check
        FROM INFORMATION_SCHEMA.TABLES
        WHERE TABLE_SCHEMA = v_schema AND TABLE_NAME = v_table;
        IF v_import_obj_check = 0 THEN
            SET v_db_err_msg = CONCAT('Table ', v_schema, '.', v_table, ' used to import from does not exist.');
            SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
        END IF;

        -- Verify table has required columns according to official documentation
        SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.COLUMNS ',
                                              ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                              ' AND COLUMN_NAME IN (\"chunk_id\", \"model_object\", \"model_metadata\");');
        PREPARE check_stmt FROM @v_model_meta_check_stmt;
        EXECUTE check_stmt;
        SELECT @MODEL_CNT into v_model_cnt;
        DEALLOCATE PREPARE check_stmt;
        IF v_model_cnt < 3 THEN
            SIGNAL SQLSTATE 'HY000'
               SET MESSAGE_TEXT = "The table must have chunk_id, model_object, and model_metadata columns.";
        END IF;

        -- Check primary key constraint on chunk_id
        SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE ',
                                              ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                              ' AND CONSTRAINT_NAME = \"PRIMARY\" AND COLUMN_NAME = \"chunk_id\";');
        PREPARE check_stmt FROM @v_model_meta_check_stmt;
        EXECUTE check_stmt;
        SELECT @MODEL_CNT into v_model_cnt;
        DEALLOCATE PREPARE check_stmt;
        IF v_model_cnt = 0 THEN
            SIGNAL SQLSTATE 'HY000'
               SET MESSAGE_TEXT = "The chunk_id column must be the PRIMARY KEY.";
        END IF;

        -- There must be one row with chunk_id = 1
        SET @select_model_stm = CONCAT('SELECT COUNT(*) INTO @MODEL_CNT FROM ',
                                        v_schema, '.', v_table,
                                        ' WHERE chunk_id = 1;');
        PREPARE select_model_stmt FROM @select_model_stm;
        EXECUTE select_model_stmt;
        SELECT @MODEL_CNT into v_model_cnt;
        DEALLOCATE PREPARE select_model_stmt;
        IF v_model_cnt != 1 THEN
            SIGNAL SQLSTATE 'HY000'
                SET MESSAGE_TEXT = "There must be exactly one row with chunk_id = 1.";
        END IF;

        -- Get model metadata from chunk_id = 1
        SET @model_obj_stm = CONCAT('SELECT model_metadata INTO @MODEL_META FROM ',
                                    v_schema, '.', v_table, ' WHERE chunk_id = 1;');
        PREPARE select_model_stmt FROM @model_obj_stm;
        EXECUTE select_model_stmt;
        SELECT @MODEL_META into v_model_meta;
        DEALLOCATE PREPARE select_model_stmt;

        -- Update metadata with values from the imported model if available
        IF v_model_meta IS NOT NULL THEN
            -- Extract metadata from the model_metadata column
            SET v_target_column = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.target_column_name'), 
                                    JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$.target_column_name')), v_target_column);
            SET v_train_table_name = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.train_table_name'), 
                                       JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$.train_table_name')), v_train_table_name);
            SET v_model_type = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.model_type'), 
                                 JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$.model_type')), v_model_type);
            SET v_task = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.task'), 
                           JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$.task')), v_task);
            SET v_column_names = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.column_names'), 
                                   JSON_EXTRACT(v_model_meta, '$.column_names'), v_column_names);
            SET v_format = IF(JSON_CONTAINS_PATH(v_model_meta, 'one', '$.format'), 
                             JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$.format')), v_format);
            
            SET v_final_metadata = v_model_meta;
        END IF;

        -- Get total number of chunks and calculate total size
        SET v_total_size = 0;
        
        -- We'll use prepared statements to handle dynamic table names
        SET @get_chunk_count = CONCAT('SELECT COUNT(*) INTO @TOTAL_CHUNKS FROM ', v_schema, '.', v_table);
        PREPARE stmt FROM @get_chunk_count;
        EXECUTE stmt;
        SELECT @TOTAL_CHUNKS into v_chunks;
        DEALLOCATE PREPARE stmt;

        -- Import all chunks to model_object_catalog
        SET v_chunk_id = 1;
        WHILE v_chunk_id <= v_chunks DO
            -- Get chunk content and size
            SET @chunk_query = CONCAT('SELECT model_object, LENGTH(model_object) INTO @CHUNK_CONTENT, @CHUNK_SIZE FROM ', 
                                     v_schema, '.', v_table, ' WHERE chunk_id = ', v_chunk_id);
            PREPARE chunk_select FROM @chunk_query;
            EXECUTE chunk_select;
            DEALLOCATE PREPARE chunk_select;
            
            -- Add to total size
            SET v_total_size = v_total_size + @CHUNK_SIZE;
            
            -- Insert chunk into model_object_catalog
            SET @insert_chunk = CONCAT('INSERT INTO ', v_sys_meta_catalog_obj_name, 
                                      ' (MODEL_HANDLE, CHUNK_ID, MODEL_OBJECT) VALUES(\"', 
                                      in_model_handle_name, '\", ', v_chunk_id, ', \"', 
                                      REPLACE(@CHUNK_CONTENT, '\"', '\"\"'), '\")');
            PREPARE chunk_insert FROM @insert_chunk;
            EXECUTE chunk_insert;
            DEALLOCATE PREPARE chunk_insert;
            
            SET v_chunk_id = v_chunk_id + 1;
        END WHILE;

        -- If chunks is not set in metadata and we have multiple chunks, update metadata
        IF v_final_metadata IS NOT NULL AND NOT JSON_CONTAINS_PATH(v_final_metadata, 'one', '$.chunks') AND v_chunks > 1 THEN
            SET v_final_metadata = JSON_SET(v_final_metadata, '$.chunks', v_chunks);
        END IF;

    ELSE
        -- Direct model content import (non-table import)
        SET v_total_size = OCTET_LENGTH(in_model_content);
        
        -- Insert into model_object_catalog (single chunk)
        SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_obj_name, 
                                  ' (MODEL_HANDLE, CHUNK_ID, MODEL_OBJECT)',
                                  ' VALUES(\"', in_model_handle_name, '\", 1, \"',
                                  REPLACE(in_model_content, '\"', '\"\"'), '\");');
        PREPARE select_model_stmt FROM @model_obj_stm;
        EXECUTE select_model_stmt;
        DEALLOCATE PREPARE select_model_stmt;
    END IF;

    SET @param_model_handle = in_model_handle_name;
    SET @param_user_name = v_user_name;
    SET @param_timestamp = v_current_timestamp;
    SET @param_target_column = IFNULL(v_target_column, '');
    SET @param_train_table = IFNULL(v_train_table_name, 'imported_model');
    SET @param_total_size = v_total_size;
    SET @param_model_type = IFNULL(v_model_type, 'imported');
    SET @param_task = IFNULL(v_task, 'classification');
    SET @param_column_names = IF(JSON_LENGTH(v_column_names) > 0, JSON_UNQUOTE(v_column_names), '[]');
    SET @param_metadata = JSON_UNQUOTE(JSON_EXTRACT(v_final_metadata, '$'));

    SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_name,
                             ' (MODEL_HANDLE, MODEL_OBJECT, MODEL_OWNER, BUILD_TIMESTAMP, ',
                             'TARGET_COLUMN_NAME, TRAIN_TABLE_NAME, MODEL_OBJECT_SIZE, ',
                             'MODEL_TYPE, TASK, COLUMN_NAMES, MODEL_METADATA, LAST_ACCESSED)',
                             ' VALUES(?, NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)');
    PREPARE select_model_stmt FROM @model_obj_stm;
    EXECUTE select_model_stmt USING 
        @param_model_handle, 
        @param_user_name, 
        @param_timestamp, 
        @param_target_column, 
        @param_train_table, 
        @param_total_size, 
        @param_model_type, 
        @param_task, 
        @param_column_names, 
        @param_metadata, 
        @param_timestamp;
    DEALLOCATE PREPARE select_model_stmt;
END$$
DELIMITER ;