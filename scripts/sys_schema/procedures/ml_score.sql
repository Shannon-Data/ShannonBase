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

DROP PROCEDURE IF EXISTS ml_score;
DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_score (
        IN in_table_name VARCHAR(64), IN in_target_name VARCHAR(64), IN in_handle_name VARCHAR(64),
        IN in_metric_name VARCHAR(64), OUT in_score_var FLOAT, IN in_option JSON
    )
    COMMENT '
Description
-----------

Run the ml_score routine on a labeled training dataset to predict to ground truth values in the target column of the labeled dataset.

Parameters
-----------
 in_table_name VARCHAR(64): fully qualified name of the table containing the dataset used to compute model quality.
 in_target_name VARCHAR(64): name of the target column in \'table_name\' containing ground truth values.
 in_handle_name VARCHAR(64): explicit model handle string or session variable containing the model handle.
 in_metric_name VARCHAR(64): specifies which metric should be used to evaluate model quality. Different values can be used depending on ML task and
target variable (e.g. f1, precision, recall, roc_auc, f1_weighted, balanced_accuracy…).
 in_score_var VARCHAR(64): user-defined session variable name storing the computed score for the duration of the connection.
 in_option JSON[option]: a set of optional key-value pairs, can be specified only starting in MySQL 8.0.32 and only for some tasks.
Example
-----------
mysql> CALL sys.ML_SCORE(\'ml_data.iris_validate\', \'class\', @iris_model, \'balanced_accuracy\', @score, NULL);
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE in_user_name VARCHAR(64);
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);

    DECLARE v_db_err_msg TEXT;
    DECLARE v_score_obj_check INT;
    DECLARE v_model_id INT;

   IF in_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
   END IF;

   IF in_user_name IS NULL THEN
     SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', v_user_name);
     SET in_user_name = v_user_name;
   ELSE
     SET v_sys_schema_name = CONCAT('ML_SCHEMA_', in_user_name);
   END IF;

   SET @select_model_stm = CONCAT('SELECT MODEL_ID INTO @MODEL_ID FROM ',  v_sys_schema_name,
                                  '.MODEL_CATALOG WHERE MODEL_HANDLE = \"', in_handle_name, '\";');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt;
   SELECT @MODEL_ID into v_model_id;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_model_id IS NULL) THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model you scoring does NOT exist.";
   END IF;

   SELECT ML_MODEL_SCORE(in_table_name, in_target_name, in_handle_name, v_user_name, in_metric_name, in_option) INTO in_score_var;

   -- UPDATE LAST ACCESS TIME.
   SET @score_model_acces_time_stm = CONCAT('UPDATE ',  v_sys_schema_name,
                                            '.MODEL_CATALOG SET LAST_ACCESSED = now() WHERE MODEL_HANDLE = \"',
                                            in_handle_name, '\";');
   PREPARE score_model_stmt FROM @score_model_acces_time_stm;
   EXECUTE score_model_stmt;
   DEALLOCATE PREPARE score_model_stmt;
END$$
DELIMITER ;
