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

DROP PROCEDURE IF EXISTS ml_model_export;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_model_export (
        IN in_model_handle_name varchar(256),
        IN in_output_table_name VARCHAR(256)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_EXPORT routine to export an existed trained model.

Parameters
-----------

in_model_handle_name (VARCHAR(64))
   user-defined session variable storing the ML model handle for the duration of the connection.
in_output_table_name VARCHAR(256):
  output table name.
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_MODEL_EXPORT(@iris_model,\'ML_SCHEMA_user1.model_export\');
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
END$$
DELIMITER ;
