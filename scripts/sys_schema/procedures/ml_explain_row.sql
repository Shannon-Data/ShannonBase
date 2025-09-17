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

DROP PROCEDURE IF EXISTS ml_explain_row;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_explain_row (
    IN input_data JSON,
    IN model_handle VARCHAR(64),
    IN options JSON
)
COMMENT '
Description
-----------
Generate explanations for one or more rows of unlabeled data using a trained ML model.
This procedure limits explanations to the 100 most relevant features.

Supported Models:
- Classification
- Regression

NOT Supported Models:
- Recommendation
- Anomaly detection
- Anomaly detection for logs
- Topic modeling
- Forecasting

Parameters
-----------
input_data (JSON):
  The data to generate explanations for in JSON format.
  Column names must match the feature column names used to train the model.

model_handle (VARCHAR(64)):
  The model handle or session variable containing the model handle.

options (JSON):
  Optional parameters in JSON format:
  - "prediction_explainer": {"permutation_importance"|"shap"|NULL}
    - "permutation_importance": Default explainer
    - "shap": SHAP explainer based on Shapley values
'
SQL SECURITY INVOKER
NOT DETERMINISTIC
READS SQL DATA
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name VARCHAR(64);
    DECLARE v_model_type VARCHAR(32);
    DECLARE v_model_status VARCHAR(32);
    DECLARE v_prediction_explainer VARCHAR(64) DEFAULT 'permutation_importance';
    DECLARE v_error_msg TEXT;
    DECLARE v_schema_name VARCHAR(64);
    DECLARE v_shap_exists INT DEFAULT 0;

    DECLARE v_array_length INT DEFAULT 0;
    DECLARE v_i INT DEFAULT 0;
    DECLARE v_row_data JSON;

    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
        SET v_error = TRUE;
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
    END;

    -- Get current user and database
    SELECT USER() INTO v_user_name;
    SELECT DATABASE() INTO v_db_name;

    -- Extract schema name from username
    SET v_user_name = SUBSTRING_INDEX(v_user_name, '@', 1);
    SET v_schema_name = CONCAT('ML_SCHEMA_', v_user_name);

    -- Validate parameters
    IF input_data IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data parameter cannot be NULL';
    END IF;

    IF model_handle IS NULL OR model_handle = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'model_handle parameter cannot be NULL or empty';
    END IF;

    -- Parse options
    IF options IS NOT NULL THEN
        IF JSON_VALID(options) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'options parameter must be valid JSON';
        END IF;

        IF JSON_EXTRACT(options, '$.prediction_explainer') IS NOT NULL THEN
            SET v_prediction_explainer = JSON_UNQUOTE(JSON_EXTRACT(options, '$.prediction_explainer'));
        END IF;

        IF v_prediction_explainer NOT IN ('permutation_importance', 'shap') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prediction_explainer must be "permutation_importance" or "shap"';
        END IF;
    END IF;

    -- Check model catalog
    SET @sql_model_check = CONCAT(
        'SELECT COUNT(*) INTO @tmp_model_exists ',
        'FROM ', v_schema_name, '.MODEL_CATALOG WHERE model_handle = ?'
    );
    PREPARE stmt FROM @sql_model_check;
    SET @mh := model_handle;
    EXECUTE stmt USING @mh;
    DEALLOCATE PREPARE stmt;

    IF @tmp_model_exists = 0 THEN
        SET v_error_msg = CONCAT('Model handle not found: ', model_handle);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;
    SET @tmp_model_exists = NULL;

    -- Get model type and status
    SET @sql_model_info = CONCAT(
        'SELECT model_type, model_status INTO @tmp_model_type, @tmp_model_status ',
        'FROM ', v_schema_name, '.MODEL_CATALOG WHERE model_handle = ?'
    );
    PREPARE stmt FROM @sql_model_info;
    SET @mh := model_handle;
    EXECUTE stmt USING @mh;
    DEALLOCATE PREPARE stmt;

    SET v_model_type = @tmp_model_type;
    SET v_model_status = @tmp_model_status;
    SET @tmp_model_type = NULL;
    SET @tmp_model_status = NULL;

    IF v_model_status != 'READY' THEN
        SET v_error_msg = CONCAT('Model is not ready. Current status: ', v_model_status);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;

    IF v_model_type IN ('recommendation','anomaly_detection','topic_modeling','anomaly_detection_logs','forecasting') THEN
        SET v_error_msg = CONCAT('ML_EXPLAIN_ROW does not support ', v_model_type, ' models');
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;

    IF JSON_VALID(input_data) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data must be valid JSON';
    END IF;

    IF JSON_TYPE(input_data) = 'ARRAY' THEN
        SET v_array_length = JSON_LENGTH(input_data);
        IF v_array_length = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data array cannot be empty';
        END IF;

        SET v_i = 0;
        WHILE v_i < v_array_length DO
            SET v_row_data = JSON_EXTRACT(input_data, CONCAT('$[', v_i, ']'));
            IF JSON_VALID(v_row_data) = 0 OR JSON_LENGTH(v_row_data) = 0 THEN
                SET v_error_msg = CONCAT('Invalid or empty row at index ', v_i);
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
            END IF;
            SET v_i = v_i + 1;
        END WHILE;
    ELSE
        IF JSON_LENGTH(input_data) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data cannot be empty';
        END IF;
    END IF;

    IF v_prediction_explainer = 'shap' THEN
        SET @sql_shap_check = CONCAT(
            'SELECT COUNT(*) INTO @tmp_shap ',
            'FROM ', v_schema_name, '.MODEL_EXPLAINERS ',
            'WHERE model_handle = ? AND explainer_type = ''shap'' AND status = ''READY'''
        );
        PREPARE stmt FROM @sql_shap_check;
        SET @mh := model_handle;
        EXECUTE stmt USING @mh;
        DEALLOCATE PREPARE stmt;

        SET v_shap_exists = @tmp_shap;
        SET @tmp_shap = NULL;

        IF v_shap_exists = 0 THEN
            SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'SHAP explainer not found or not ready. Please run ML_EXPLAIN with "shap" explainer first.';
        END IF;
    END IF;

    IF v_error THEN
        RESIGNAL;
    END IF;

    -- Call native function ML_EXPLAIN_ROW
    SELECT ML_EXPLAIN_ROW(input_data, model_handle, options);

END$$

DELIMITER ;