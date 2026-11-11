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

DROP PROCEDURE IF EXISTS ml_explain;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_explain (
        IN in_sche_tb_name VARCHAR(256),
        IN in_target_name VARCHAR(64),
        IN in_model_handle VARCHAR(255),
        IN in_option JSON
    )
    COMMENT '
Description
-----------

Run the ml_explain routine on a labeled training dataset to produce a trained machine learning model.

Parameters
-----------

in_sche_tb_name (VARCHAR(256)):
  fully qualified name of the table containing the training dataset.
in_target_name (VARCHAR(64)):
  name of the column in \'table_name\' representing the target, i.e. ground truth values (required for some tasks)
in_model_handle (VARCHAR(255))
   user-defined session variable storing the ML model handle for the duration of the connection
in_option (JSON)
 Optional parameters specified as key-value pairs in JSON format. If an option is not specified, the default setting is used.
 If you specify NULL in place of the JSON argument, the default Permutation 
 Importance model explainer is trained, and no prediction explainer is trained. 
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_EXPLAIN(\'ml_data.iris_train\', \'class\',
          \'ml_data.iris_train_user1_1636729526\', 
          JSON_OBJECT(\'model_explainer\', \'fast_shap\', \'prediction_explainer\', \'shap\'));
...
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);
    DECLARE v_exp_obj_check INT;

    DECLARE v_db_err_msg TEXT;
    DECLARE v_catalog_exists INT DEFAULT 0;
    DECLARE v_model_id INT;
    DECLARE v_trained_cnt INT DEFAULT 0;
    DECLARE v_final_option JSON;

   IF in_sche_tb_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
   END IF;

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
   SET @explain_handle = in_model_handle;
   SET @select_model_stm = CONCAT('SELECT MODEL_ID INTO @MODEL_ID FROM `', REPLACE(v_sys_schema_name,'`','``'),
                                  '`.MODEL_CATALOG WHERE MODEL_HANDLE = ?');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt USING @explain_handle;
   SELECT @MODEL_ID into v_model_id;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_model_id IS NULL) THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model you explaining does NOT exist.";
   END IF;

   -- The model must be the one trained on the supplied table.
   SET @explain_table = in_sche_tb_name;
   SET @select_model_stm = CONCAT('SELECT COUNT(*) INTO @trained_cnt FROM `', REPLACE(v_sys_schema_name,'`','``'),
                                  '`.MODEL_CATALOG WHERE MODEL_HANDLE = ? AND TRAIN_TABLE_NAME = ?');
   PREPARE select_model_stmt FROM @select_model_stm;
   EXECUTE select_model_stmt USING @explain_handle, @explain_table;
   SELECT @trained_cnt into v_trained_cnt;
   DEALLOCATE PREPARE select_model_stmt;

   IF (v_trained_cnt = 0) THEN
     SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = "The model was not trained on the specified table.";
   END IF;

   IF in_option IS NULL THEN
     SET v_final_option = JSON_OBJECT('model_explainer', 'permutation_importance');
   ELSE
     SET v_final_option = in_option;
   END IF;

   SELECT ML_MODEL_EXPLAIN(in_sche_tb_name, in_target_name, in_model_handle, v_final_option) INTO v_exp_obj_check;
   IF v_exp_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_EXPLAIN failed.');
        SIGNAL SQLSTATE 'HY000'
          SET MESSAGE_TEXT = v_db_err_msg;
   END IF;
END$$

DELIMITER ;
