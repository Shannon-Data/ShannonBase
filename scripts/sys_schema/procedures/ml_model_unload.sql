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

DROP PROCEDURE IF EXISTS ml_model_unload;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_model_unload (
        IN in_model_handle_name VARCHAR(255)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_UNLOAD routine unload the trained model from memory.

Parameters
-----------

in_model_handle_name (VARCHAR(255)):
   user-defined session variable storing the ML model handle for the duration of the connection
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_MODEL_UNLOAD(@iris_model);
...
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_unload_obj_check INT;
    DECLARE v_model_id INT;
    DECLARE v_catalog_exists INT DEFAULT 0;

   SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
   IF INSTR(v_user_name, '.') > 0 THEN
     SIGNAL SQLSTATE '45000'
       SET MESSAGE_TEXT = 'ML routines do not support account names containing a period';
   END IF;
   SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);

   SELECT COUNT(*) INTO v_catalog_exists
   FROM INFORMATION_SCHEMA.TABLES
   WHERE TABLE_SCHEMA = v_sys_schema_name AND TABLE_NAME = 'MODEL_CATALOG';
   IF v_catalog_exists = 0 THEN
     SIGNAL SQLSTATE '45000'
       SET MESSAGE_TEXT = 'No model catalog for the current user, train a model first';
   END IF;

   SET @MODEL_ID = NULL;
   SET @unload_handle = in_model_handle_name;
   SET @select_model_stm = CONCAT('SELECT MODEL_ID INTO @MODEL_ID FROM `', REPLACE(v_sys_schema_name,'`','``'),
                                  '`.MODEL_CATALOG WHERE MODEL_HANDLE = ?');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt USING @unload_handle;
   SELECT @MODEL_ID into v_model_id;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_model_id IS NULL) THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model you unloading does NOT exist.";
   END IF;

   SELECT ML_MODEL_UNLOAD(in_model_handle_name) INTO v_unload_obj_check;
   IF v_unload_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_UNLOAD failed.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = v_db_err_msg;
   END IF;
END$$

DELIMITER ;
