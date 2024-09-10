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

DROP PROCEDURE IF EXISTS sys.ml_model_import;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE sys.ml_model_import (
        IN in_model_content LONGTEXT,
        IN in_option JSON,
        IN in_model_handle_name VARCHAR(64)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_IMPORT routine to import an existed trained model into a new model handle.

Parameters
-----------

in_model_content LONGTEXT:
  model content.
in_option (JSON)
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
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE in_user_name VARCHAR(64);
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_train_obj_check INT;
    DECLARE v_model_meta JSON;

   IF in_user_name IS NULL THEN
     SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);
     SET in_user_name = v_user_name;
   ELSE
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', in_user_name);
   END IF;

   IF in_option IS NULL THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The options missed.";
   END IF;

   SET @MODEL_META = NULL;
   SET @select_model_stm = CONCAT('SELECT MODEL_METADATA INTO  @MODEL_META FROM ',  v_sys_schema_name,
                                  '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', in_model_handle_name, '\";');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt;
   SELECT @MODEL_META into v_model_meta;
   DEALLOCATE PREPARE select_model_stmt;

   IF v_model_meta IS NOT NULL THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model you importing already exists.";
   END IF;

   SELECT in_option into v_model_meta;
   SELECT ML_MODEL_IMPORT(in_model_handle_name, in_user_name, v_model_meta, in_model_content) INTO v_train_obj_check;
   IF v_train_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_IMPORT failed.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
   END IF;
END$$
DELIMITER ;