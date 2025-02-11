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

DROP FUNCTION IF EXISTS ml_predict_row;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION ml_predict_row (
        in_predict_data JSON,
        in_model_handle_name VARCHAR(64),
        in_model_option JSON
    )
    RETURNS JSON
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
mysql> SELECT sys.ML_PREDICT_ROW(JSON_OBJECT(\'column_name\', value,
          \'column_name\', value, ...),
          model_handle, options);
'
    SQL SECURITY INVOKER
    DETERMINISTIC
    CONTAINS SQL
BEGIN
    DECLARE v_db_err_msg TEXT;
    DECLARE v_pred_row_obj_check JSON;

   SELECT ML_MODEL_PREDICT_ROW(in_input_data, in_model_handle_name, in_model_option) INTO v_pred_row_obj_check;
   IF v_pred_row_obj_check IS NULL THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_PREDICT_ROW failed.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = v_db_err_msg;
   END IF;

   RETURN v_pred_row_obj_check;
END$$

DELIMITER ;
