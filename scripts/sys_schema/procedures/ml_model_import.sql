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
        IN in_model_handle_name VARCHAR(255)
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
    DECLARE v_user VARCHAR(64);
    DECLARE v_ml_schema VARCHAR(64);
    DECLARE v_catalog VARCHAR(160);
    DECLARE v_objects VARCHAR(160);
    DECLARE v_schema VARCHAR(64);
    DECLARE v_table VARCHAR(64);
    DECLARE v_source VARCHAR(300);
    DECLARE v_metadata JSON;
    DECLARE v_chunks BIGINT DEFAULT 1;
    DECLARE v_size BIGINT;
    DECLARE v_tx BOOLEAN DEFAULT FALSE;
    DECLARE v_prepared BOOLEAN DEFAULT FALSE;
    DECLARE v_metadata_only BOOLEAN DEFAULT FALSE;
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        IF v_tx THEN ROLLBACK; END IF;
        IF v_prepared THEN
            BEGIN
                DECLARE CONTINUE HANDLER FOR SQLEXCEPTION BEGIN END;
                DEALLOCATE PREPARE import_stmt;
            END;
        END IF;
        RESIGNAL;
    END;

    IF ML_SESSION_IN_TRANSACTION() OR @@SESSION.autocommit = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'ML routine requires autocommit and no active transaction';
    END IF;
    IF in_metadata IS NULL THEN
        -- Model metadata is optional: an ONNX model can be imported without it.
        SET v_metadata = JSON_OBJECT();
    ELSE
        SET v_metadata = in_metadata;
    END IF;
    SET v_user = SUBSTRING_INDEX(CURRENT_USER(), '@', 1);
    IF INSTR(v_user, '.') > 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'ML routines do not support account names containing a period';
    END IF;
    SET v_ml_schema = CONCAT('ML_SCHEMA_', v_user);
    SET v_catalog = CONCAT('`', REPLACE(v_ml_schema,'`','``'), '`.MODEL_CATALOG');
    SET v_objects = CONCAT('`', REPLACE(v_ml_schema,'`','``'), '`.MODEL_OBJECT_CATALOG');
    SET @import_handle = COALESCE(in_model_handle_name, CONCAT('ML_SCHEMA_',v_user,'_',SUBSTRING(MD5(RAND()),1,10)));

    -- First import bootstraps this user's model catalog, as HeatWave does.
    IF (SELECT COUNT(*) FROM information_schema.TABLES
        WHERE TABLE_SCHEMA=v_ml_schema AND TABLE_NAME IN ('MODEL_CATALOG','MODEL_OBJECT_CATALOG')) < 2 THEN
        SET @ml_catalog_ddl = CONCAT('CREATE DATABASE IF NOT EXISTS `', REPLACE(v_ml_schema,'`','``'), '`');
        PREPARE ml_catalog_stmt FROM @ml_catalog_ddl;
        EXECUTE ml_catalog_stmt; DEALLOCATE PREPARE ml_catalog_stmt;
        SET @ml_catalog_ddl = CONCAT('CREATE TABLE IF NOT EXISTS `', REPLACE(v_ml_schema,'`','``'),
            '`.MODEL_CATALOG (',
            'MODEL_ID BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,',
            'MODEL_HANDLE VARCHAR(255) NOT NULL,',
            'MODEL_OBJECT JSON DEFAULT NULL,',
            'MODEL_OWNER VARCHAR(255) DEFAULT NULL,',
            'BUILD_TIMESTAMP BIGINT UNSIGNED NOT NULL DEFAULT (unix_timestamp()),',
            'TARGET_COLUMN_NAME VARCHAR(64) DEFAULT NULL,',
            'TRAIN_TABLE_NAME VARCHAR(255) NOT NULL,',
            'MODEL_OBJECT_SIZE BIGINT UNSIGNED NOT NULL DEFAULT 0,',
            'MODEL_TYPE VARCHAR(128) DEFAULT NULL,',
            'TASK VARCHAR(64) DEFAULT NULL,',
            'COLUMN_NAMES JSON DEFAULT NULL,',
            'MODEL_EXPLANATION JSON DEFAULT NULL,',
            'LAST_ACCESSED BIGINT UNSIGNED DEFAULT NULL,',
            'MODEL_METADATA JSON DEFAULT NULL,',
            'NOTES TEXT DEFAULT NULL,',
            'PRIMARY KEY (MODEL_ID),',
            'UNIQUE KEY uk_model_handle (MODEL_HANDLE),',
            'KEY idx_owner (MODEL_OWNER),',
            'KEY idx_train_table (TRAIN_TABLE_NAME),',
            'KEY idx_task (TASK),',
            'KEY idx_last_accessed (LAST_ACCESSED),',
            'KEY idx_build_time (BUILD_TIMESTAMP)',
            ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci');
        PREPARE ml_catalog_stmt FROM @ml_catalog_ddl;
        EXECUTE ml_catalog_stmt; DEALLOCATE PREPARE ml_catalog_stmt;
        SET @ml_catalog_ddl = CONCAT('CREATE TABLE IF NOT EXISTS `', REPLACE(v_ml_schema,'`','``'),
            '`.MODEL_OBJECT_CATALOG (',
            'CHUNK_ID INT UNSIGNED NOT NULL,',
            'MODEL_HANDLE VARCHAR(255) NOT NULL,',
            'MODEL_OBJECT LONGTEXT NOT NULL,',
            'PRIMARY KEY uk_handle_chunk (MODEL_HANDLE, CHUNK_ID),',
            'KEY idx_handle (MODEL_HANDLE)',
            ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci');
        PREPARE ml_catalog_stmt FROM @ml_catalog_ddl;
        EXECUTE ml_catalog_stmt; DEALLOCATE PREPARE ml_catalog_stmt;
    END IF;
    START TRANSACTION;
    SET v_tx = TRUE;

    IF in_model_content IS NULL THEN
        SET v_schema = JSON_UNQUOTE(JSON_EXTRACT(in_metadata,'$.schema'));
        SET v_table = JSON_UNQUOTE(JSON_EXTRACT(in_metadata,'$.table'));
        IF v_schema IS NULL OR v_table IS NULL THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table import requires schema and table';
        END IF;
        SET v_source = CONCAT('`',REPLACE(v_schema,'`','``'),'`.`',REPLACE(v_table,'`','``'),'`');
        IF (SELECT COUNT(*) FROM information_schema.COLUMNS
            WHERE TABLE_SCHEMA=v_schema AND TABLE_NAME=v_table
              AND COLUMN_NAME IN ('chunk_id','model_object','model_metadata')) <> 3 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'The table must have chunk_id, model_object, and model_metadata columns.';
        END IF;
        IF (SELECT COUNT(*) FROM information_schema.KEY_COLUMN_USAGE
            WHERE TABLE_SCHEMA=v_schema AND TABLE_NAME=v_table AND CONSTRAINT_NAME='PRIMARY') <> 1
           OR NOT EXISTS (SELECT 1 FROM information_schema.KEY_COLUMN_USAGE
            WHERE TABLE_SCHEMA=v_schema AND TABLE_NAME=v_table AND CONSTRAINT_NAME='PRIMARY' AND COLUMN_NAME='chunk_id') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'The chunk_id column must be the PRIMARY KEY.';
        END IF;
        SET @import_sql = CONCAT('SELECT COUNT(*), MIN(chunk_id), MAX(chunk_id), SUM(OCTET_LENGTH(model_object)), ',
            'SUM(model_object IS NULL) INTO @import_chunks,@import_min,@import_max,@import_size,@import_nulls FROM ',v_source);
        PREPARE import_stmt FROM @import_sql; SET v_prepared=TRUE;
        EXECUTE import_stmt; DEALLOCATE PREPARE import_stmt; SET v_prepared=FALSE;
        -- chunk_id = 0 with a NULL object is the metadata-only export form.
        SET v_metadata_only = (@import_chunks = 1 AND @import_min = 0 AND @import_max = 0 AND @import_nulls = 1);
        IF NOT v_metadata_only AND
           (@import_chunks=0 OR @import_min<>1 OR @import_max<>@import_chunks OR @import_nulls<>0) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model chunks must be non-null and numbered consecutively from 1';
        END IF;
        IF v_metadata_only THEN
            SET v_chunks=0;
            SET v_size=0;
            SET @import_sql=CONCAT('SELECT model_metadata INTO @import_meta FROM ',v_source,' WHERE chunk_id=0');
        ELSE
            SET v_chunks=@import_chunks;
            SET v_size=@import_size;
            SET @import_sql=CONCAT('SELECT model_metadata INTO @import_meta FROM ',v_source,' WHERE chunk_id=1');
        END IF;
        SET @import_meta=NULL;
        PREPARE import_stmt FROM @import_sql; SET v_prepared=TRUE;
        EXECUTE import_stmt; DEALLOCATE PREPARE import_stmt; SET v_prepared=FALSE;
        SET v_metadata=COALESCE(@import_meta,JSON_OBJECT());
        IF NOT v_metadata_only THEN
            SET @import_sql=CONCAT('INSERT INTO ',v_objects,
                ' (MODEL_HANDLE,CHUNK_ID,MODEL_OBJECT) SELECT ?,chunk_id,model_object FROM ',v_source,' ORDER BY chunk_id');
            PREPARE import_stmt FROM @import_sql; SET v_prepared=TRUE;
            EXECUTE import_stmt USING @import_handle;
            DEALLOCATE PREPARE import_stmt; SET v_prepared=FALSE;
        END IF;
    ELSE
        SET v_size=OCTET_LENGTH(in_model_content);
        SET @import_content=in_model_content;
        SET @import_sql=CONCAT('INSERT INTO ',v_objects,' (MODEL_HANDLE,CHUNK_ID,MODEL_OBJECT) VALUES (?,1,?)');
        PREPARE import_stmt FROM @import_sql; SET v_prepared=TRUE;
        EXECUTE import_stmt USING @import_handle,@import_content;
        DEALLOCATE PREPARE import_stmt; SET v_prepared=FALSE;
    END IF;
    IF NOT JSON_CONTAINS_PATH(v_metadata,'one','$.chunks') THEN
        SET v_metadata=JSON_SET(v_metadata,'$.chunks',v_chunks);
    END IF;
    SET @import_owner=v_user;
    SET @import_time=UNIX_TIMESTAMP();
    SET @import_target=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(v_metadata,'$.target_column_name')),'target');
    SET @import_train=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(v_metadata,'$.train_table_name')),'imported_model');
    SET @import_type=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(v_metadata,'$.model_type')),'imported');
    SET @import_task=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(v_metadata,'$.task')),'classification');
    SET @import_columns=COALESCE(JSON_EXTRACT(v_metadata,'$.column_names'),JSON_ARRAY());
    SET @import_meta=v_metadata;
    SET @import_size=v_size;
    SET @import_sql=CONCAT('INSERT INTO ',v_catalog,
        ' (MODEL_HANDLE,MODEL_OWNER,BUILD_TIMESTAMP,TARGET_COLUMN_NAME,TRAIN_TABLE_NAME,MODEL_OBJECT_SIZE,',
        'MODEL_TYPE,TASK,COLUMN_NAMES,MODEL_METADATA,LAST_ACCESSED) VALUES (?,?,?,?,?,?,?,?,?,?,?)');
    PREPARE import_stmt FROM @import_sql; SET v_prepared=TRUE;
    EXECUTE import_stmt USING @import_handle,@import_owner,@import_time,@import_target,@import_train,
        @import_size,@import_type,@import_task,@import_columns,@import_meta,@import_time;
    DEALLOCATE PREPARE import_stmt; SET v_prepared=FALSE;
    COMMIT;
    SET v_tx=FALSE;
END$$
DELIMITER ;
