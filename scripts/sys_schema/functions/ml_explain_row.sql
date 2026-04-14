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

DROP FUNCTION IF EXISTS ml_explain_row;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION ml_explain_row (
    input_data JSON,
    model_handle VARCHAR(64),
    options JSON
) RETURNS JSON
COMMENT '
Description
-----------
Generate explanations for one or more rows of unlabeled data using a trained ML model.
This FUNCTION limits explanations to the 100 most relevant features.

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

Returns
-----------
JSON object containing the prediction explanation results.
'
    SQL SECURITY INVOKER
    DETERMINISTIC
    CONTAINS SQL
BEGIN
    -- All DECLARE statements must be first
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_prediction_explainer VARCHAR(64) DEFAULT 'permutation_importance';
    DECLARE v_error_msg TEXT;
    DECLARE v_result JSON;

    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
        SET v_error = TRUE;
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
    END;

    -- Validate parameters
    IF input_data IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data parameter cannot be NULL';
    END IF;

    IF model_handle IS NULL OR model_handle = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'model_handle parameter cannot be NULL or empty';
    END IF;

    -- Validate JSON format
    IF JSON_VALID(input_data) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data must be valid JSON';
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

    -- Check if input_data is empty
    IF JSON_TYPE(input_data) = 'ARRAY' THEN
        IF JSON_LENGTH(input_data) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data array cannot be empty';
        END IF;
    ELSE
        IF JSON_LENGTH(input_data) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'input_data cannot be empty';
        END IF;
    END IF;

    -- Call native ML_MODEL_EXPLAIN_ROW function
    -- Note: This bypasses the model validation, but the native function will validate
    SELECT ML_MODEL_EXPLAIN_ROW(input_data, model_handle, options) INTO v_result;
    
    RETURN v_result;
END$$

DELIMITER ;