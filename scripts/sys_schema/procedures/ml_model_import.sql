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
    DECLARE v_model_handle_name VARCHAR(64);

    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
    SET v_sys_meta_catalog_name = CONCAT('ML_SCHEMA_', v_user_name, '.MODEL_CATALOG');
    SET v_sys_meta_catalog_obj_name = CONCAT('ML_SCHEMA_', v_user_name, '.MODEL_OBJECT_CATALOG');

    IF in_metadata IS NULL THEN
    SIGNAL SQLSTATE 'HY000'
       SET MESSAGE_TEXT = "The options missed.";
    END IF;

   IF in_model_handle_name IS NULL THEN
      SET in_model_handle_name = CONCAT('ML_SCHEMA_', v_user_name, '_', SUBSTRING(MD5(RAND()), 1, 10));
   END IF;

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

   -- import from a tabble. in_model_content set to null.
   IF JSON_CONTAINS_PATH(in_metadata, 'one', '$.schema') AND JSON_CONTAINS_PATH(in_metadata, 'one', '$.table')
   THEN
      SELECT in_metadata->'$.schema', in_metadata->'$.table' INTO v_schema, v_table;

      SELECT COUNT(*) INTO v_import_obj_check
      FROM INFORMATION_SCHEMA.TABLES
      WHERE TABLE_SCHEMA = v_schema AND TABLE_NAME = v_table;
      IF v_import_obj_check = 0 THEN
          SET v_db_err_msg = CONCAT(in_table_name, ' used to import from does not exists.');
            SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
      END IF;

      -- The table should have the following columns, and their recommended parameters:
      SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.COLUMNS ',
                                            ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                            ' AND COLUMN_NAME IN (\"chunk_id\", \"model_object\", \"model_metadata\");');
      PREPARE check_stmt FROM @v_model_meta_check_stmt;
      EXECUTE check_stmt;
      SELECT @MODEL_CNT into v_model_cnt;
      DEALLOCATE PREPARE check_stmt;
      IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE 'HY000'
           SET MESSAGE_TEXT = "The table definition imported is wrong.";
      END IF;

      SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE ',
                                          ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                          ' AND CONSTRAINT_NAME = \"PRIMARY\" AND COLUMN_NAME = \"CHUNK_ID\";');
      PREPARE check_stmt FROM @v_model_meta_check_stmt;
      EXECUTE check_stmt;
      SELECT @MODEL_CNT into v_model_cnt;
      DEALLOCATE PREPARE check_stmt;
      IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE 'HY000'
           SET MESSAGE_TEXT = "The column definition of CHUNK_ID imported is wrong.";
      END IF;

      SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE ',
                                          ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                          ' AND COLUMN_NAME = \"MODEL_OBJECT\" AND IS_NULLABLE = \"NO\" AND DATA_TYPE = \"LONGTEXT\";');

      PREPARE check_stmt FROM @v_model_meta_check_stmt;
      EXECUTE check_stmt;
      SELECT @MODEL_CNT into v_model_cnt;
      DEALLOCATE PREPARE check_stmt;
      IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE 'HY000'
           SET MESSAGE_TEXT = "The column definition of model_object imported is wrong.";
      END IF;

      SET @v_model_meta_check_stmt = CONCAT('SELECT COUNT(1) INTO @MODEL_CNT FROM INFORMATION_SCHEMA.COLUMNS ',
                                          ' WHERE TABLE_SCHEMA = \"', v_schema, '\" AND TABLE_NAME = \"', v_table, '\"',
                                          ' AND COLUMN_NAME = \"MODEL_METADATA\" AND COLUMN_DEFAULT IS NULL AND COLUMN_TYPE = \"JSON\";');

      PREPARE check_stmt FROM @v_model_meta_check_stmt;
      EXECUTE check_stmt;
      SELECT @MODEL_CNT into v_model_cnt;
      DEALLOCATE PREPARE check_stmt;
      IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE 'HY000'
           SET MESSAGE_TEXT = "The column definition of MODEL_METADATA imported is wrong.";
      END IF;

      -- There must be one row, and only one row, in the table with chunk_id = 1.
      SET @select_model_stm = CONCAT('SELECT COUNT(MODEL_METADATA) INTO @MODEL_CNT FROM ',
                                      v_schema, '.', v_table,
                                      ' WHERE chunk_id = 1;');
      PREPARE select_model_stmt FROM @select_model_stm;
      EXECUTE select_model_stmt;
      SELECT @MODEL_CNT into v_model_cnt;
      DEALLOCATE PREPARE select_model_stmt;
      IF v_model_cnt != 1 THEN
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = "the table your imported more than one row with CHUNK_ID = 1.";
      END IF;


      SET @model_obj_stm = CONCAT('SELECT MODEL_OBJECT, MODEL_METADATA INTO @MODEL_OBJ, @MODEL_META FROM',
                                  v_schema, '.', v_table, ' WHERE CHUNK_ID = 1;');
      PREPARE select_model_stmt FROM @model_obj_stm;
      EXECUTE select_model_stmt;
      SELECT @MODEL_OBJ, @model_metadata into v_model_object, v_model_meta;
      DEALLOCATE PREPARE select_model_stmt;

      SET @model_obj_size = OCTET_LENGTH(v_model_object);
      SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_name,
                                  '(MODEL_HANDLE, MODEL_OBJECT, MODEL_OWNER, MODEL_OBJECT_SIZE, MODEL_METADATA)',
                                  ' VALUES(', '\"', in_model_handle_name, '\", NULL,',
                                  '\"', v_user_name, '\",', @model_obj_size, ',',
                                  '''',JSON_UNQUOTE(JSON_EXTRACT(v_model_meta, '$')), ''');');
      PREPARE select_model_stmt FROM @model_obj_stm;
      EXECUTE select_model_stmt;
      DEALLOCATE PREPARE select_model_stmt;

      SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_obj_name, '(MODEL_HANDLE, MODEL_OBJECT)',
                                    ' VALUES(', '\"', in_model_handle_name, '\",',
                                    JSON_QUOTE(CAST(v_model_object AS CHAR)), ');');
      PREPARE select_model_stmt FROM @model_obj_stm;
      EXECUTE select_model_stmt;
      DEALLOCATE PREPARE select_model_stmt;
   ELSE
    -- The preprocessed model object.
      SET @model_obj_size = OCTET_LENGTH(in_model_content);
      SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_name,
                                  '(MODEL_HANDLE, MODEL_OBJECT, MODEL_OWNER, MODEL_OBJECT_SIZE, MODEL_METADATA)',
                                  ' VALUES(', '\"', in_model_handle_name, '\", NULL,',
                                  '\"', v_user_name, '\",', @model_obj_size, ',',
                                  '''',JSON_UNQUOTE(JSON_EXTRACT(in_metadata, '$')), ''');');
      PREPARE select_model_stmt FROM @model_obj_stm;
      EXECUTE select_model_stmt;
      DEALLOCATE PREPARE select_model_stmt;

      SET @model_obj_stm = CONCAT('INSERT INTO ', v_sys_meta_catalog_obj_name, '(MODEL_HANDLE, MODEL_OBJECT)',
                                    ' VALUES(', '\"', in_model_handle_name, '\",',
                                    JSON_QUOTE(CAST(in_model_content AS CHAR)), ');');
      PREPARE select_model_stmt FROM @model_obj_stm;
      EXECUTE select_model_stmt;
      DEALLOCATE PREPARE select_model_stmt;
   END IF;
END$$
DELIMITER ;
