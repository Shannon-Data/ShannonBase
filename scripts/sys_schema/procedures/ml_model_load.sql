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

DROP PROCEDURE IF EXISTS sys.ml_model_load;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE sys.ml_model_load (
        IN in_model_handle_name VARCHAR(64),
        IN in_user_name VARCHAR(64)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_LOAD routine load the trained model into memory.

Parameters
-----------

in_model_handle_name (VARCHAR(64)):
   user-defined session variable storing the ML model handle for the duration of the connection
in_user_name (VARCHAR(64)):
  name of user
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_MODEL_LOAD(@iris_model, \'root\');
...
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_train_obj_check INT;
    DECLARE v_model_meta JSON;
    DECLARE v_model_data LONGTEXT;

   IF in_user_name IS NULL THEN
     SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);
     SET in_user_name = v_user_name;
   ELSE
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', in_user_name);
   END IF;

   SET @select_model_stm = CONCAT('SELECT MODEL_METADATA, MODEL_OBJECT INTO  @MODEL_META, @MODEL_DATA FROM ',  v_sys_schema_name,
                                  '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', in_model_handle_name, '\";');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt;
   SELECT @MODEL_META, @MODEL_DATA into v_model_meta, v_model_data;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_model_meta IS NULL) OR (v_model_data IS NULL) THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model you loading does NOT exist.";
   END IF;

   SELECT ML_MODEL_LOAD(in_model_handle_name, v_model_meta, v_model_data) INTO v_train_obj_check;
   IF v_train_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_LOAD failed.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
   END IF;
END$$
DELIMITER ;