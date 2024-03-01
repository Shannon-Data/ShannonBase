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

DROP PROCEDURE IF EXISTS ml_explain_table;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_explain_table (
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
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name_check VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

END$$
DELIMITER ;