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

DROP PROCEDURE IF EXISTS ml_predict_row;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_predict_row (
        IN in_input_data JSON,
        IN in_model_handle_name VARCHAR(64),
        IN in_model_option JSON
    )
    COMMENT '
Description
-----------

Run the ml_predict_row routine on a labeled training dataset to produce a trained machine learning model.

Parameters
-----------

in_input_data JSON:
  the data to be predicted.
in_model_handle_name (VARCHAR(64)):
  user-defined session variable storing the ML model handle for the duration of the connection.
in_model_option JSON:
  This parameter only supports the recommendation and anomaly_detection tasks.
  For all other tasks, set this parameter to NULL.
Example
-----------
mysql> SELECT sys.ML_PREDICT_ROW(JSON_OBJECT(\"column_name\", value,
          \"column_name\", value, ...),
          model_handle, options);
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_pred_row_obj_check INT;
    DECLARE v_model_id INT;

   SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
   SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);

   SET @select_model_stm = CONCAT('SELECT MODEL_ID INTO @MODEL_ID FROM ',  v_sys_schema_name,
                                  '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', in_model_handle_name, '\";');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt;
   SELECT @MODEL_ID into v_model_id;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_model_id IS NULL) THEN
      SIGNAL SQLSTATE 'HY000'
      SET MESSAGE_TEXT = "The model you try to do predict row does NOT exist.";
   END IF;

   SELECT ML_MODEL_PREDICT_ROW(in_input_data, in_model_handle_name, in_model_option) INTO v_pred_row_obj_check;
   IF v_pred_row_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_PREDICT_ROW failed.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = v_db_err_msg;
   END IF;
END$$
DELIMITER ;
