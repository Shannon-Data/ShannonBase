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
    IN table_name VARCHAR(128),
    IN model_handle VARCHAR(64), 
    IN output_table_name VARCHAR(128),
    IN options JSON
)
COMMENT '
Description
-----------
Generate explanations for an entire table of unlabeled data using a trained ML model.
This is a memory-intensive process with specific row limits based on MySQL version.

Memory and Performance Guidelines:
- MySQL 9.4.1+: Max 100 rows, 10 rows if >10 columns
- Before MySQL 9.4.1: Use batch_size option (10-100 rows)
- Limited to 100 most relevant features

Output Table Features:
- Includes primary key from input table or auto-generated _4aad19ca6e_pk_id
- Contains all original columns plus attribution columns
- Includes ml_results column with JSON explanations
- Can append extra columns not used in training

Supported Models:
- Classification
- Regression

NOT Supported Models:
- Forecasting
- Recommendation  
- Anomaly detection
- Anomaly detection for logs
- Topic modeling

Parameters
-----------
table_name (VARCHAR(128)):
  Fully qualified name of input table (schema.table_name).
  Must contain same feature columns as training table.
  Target column (if present) is ignored during explanation.

model_handle (VARCHAR(64)):
  The model handle for the trained and loaded model.

output_table_name (VARCHAR(128)):
  Fully qualified name for output table (schema.table_name).
  As of MySQL 9.4.1, can be same as input table under specific conditions.

options (JSON):
  Optional parameters in JSON format:
  - "prediction_explainer": {"permutation_importance"|"shap"}
  - "batch_size": N (deprecated in MySQL 9.4.1+)

Examples
---------
-- Basic table explanation with default explainer
CALL sys.ml_explain_table(
    "census_data.census_test", 
    @census_model, 
    "census_data.census_explanations",
    JSON_OBJECT("prediction_explainer", "permutation_importance")
);

-- With SHAP explainer (must be pre-trained)
CALL sys.ml_explain_table(
    "census_data.census_test",
    @census_model,
    "census_data.census_shap_explanations", 
    JSON_OBJECT("prediction_explainer", "shap")
);

-- With batch_size (pre MySQL 9.4.1)
CALL sys.ml_explain_table(
    "large_data.test_table",
    @model,
    "large_data.explanations",
    JSON_OBJECT("prediction_explainer", "permutation_importance", "batch_size", 50)
);
'
SQL SECURITY INVOKER
NOT DETERMINISTIC
MODIFIES SQL DATA
BEGIN
    DECLARE v_error BOOLEAN DEFAULT FALSE;
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_db_name VARCHAR(64);
    DECLARE v_input_schema VARCHAR(64);
    DECLARE v_input_table VARCHAR(64);
    DECLARE v_output_schema VARCHAR(64);
    DECLARE v_output_table VARCHAR(64);
    DECLARE v_model_type VARCHAR(32);
    DECLARE v_model_status VARCHAR(32);
    DECLARE v_prediction_explainer VARCHAR(64) DEFAULT 'permutation_importance';
    DECLARE v_batch_size INT DEFAULT NULL;
    DECLARE v_error_msg TEXT;
    DECLARE v_input_table_exists INT DEFAULT 0;
    DECLARE v_output_schema_exists INT DEFAULT 0;
    DECLARE v_row_count INT DEFAULT 0;
    DECLARE v_column_count INT DEFAULT 0;
    DECLARE v_mysql_version VARCHAR(20);
    DECLARE v_has_primary_key INT DEFAULT 0;
    DECLARE v_pk_column_conflict INT DEFAULT 0;
    DECLARE v_model_exists INT DEFAULT 0;
    DECLARE v_shap_exists INT DEFAULT 0;

    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
        SET v_error = TRUE;
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
    END;

    -- Get current user, database and MySQL version
    SELECT USER() INTO v_user_name;
    SELECT DATABASE() INTO v_db_name;
    SELECT VERSION() INTO v_mysql_version;

    -- Validate input parameters
    IF table_name IS NULL OR table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'table_name parameter cannot be NULL or empty';
    END IF;

    IF model_handle IS NULL OR model_handle = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'model_handle parameter cannot be NULL or empty';
    END IF;

    IF output_table_name IS NULL OR output_table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'output_table_name parameter cannot be NULL or empty';
    END IF;

    -- Parse input table name
    IF LOCATE('.', table_name) > 0 THEN
        SET v_input_schema = SUBSTRING_INDEX(table_name, '.', 1);
        SET v_input_table = SUBSTRING_INDEX(table_name, '.', -1);
    ELSE
        SET v_input_schema = v_db_name;
        SET v_input_table = table_name;
    END IF;

    -- Parse output table name
    IF LOCATE('.', output_table_name) > 0 THEN
        SET v_output_schema = SUBSTRING_INDEX(output_table_name, '.', 1);
        SET v_output_table = SUBSTRING_INDEX(output_table_name, '.', -1);
    ELSE
        SET v_output_schema = v_db_name;
        SET v_output_table = output_table_name;
    END IF;

    -- Parse options if provided
    IF options IS NOT NULL THEN
        IF JSON_VALID(options) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'options parameter must be valid JSON';
        END IF;

        -- Extract prediction_explainer if specified
        IF JSON_EXTRACT(options, '$.prediction_explainer') IS NOT NULL THEN
            SET v_prediction_explainer = JSON_UNQUOTE(JSON_EXTRACT(options, '$.prediction_explainer'));
        END IF;

        -- Extract batch_size if specified
        IF JSON_EXTRACT(options, '$.batch_size') IS NOT NULL THEN
            SET v_batch_size = CAST(JSON_UNQUOTE(JSON_EXTRACT(options, '$.batch_size')) AS UNSIGNED);

            -- Check if batch_size is deprecated (MySQL 9.4.1+)
            IF v_mysql_version >= '9.4.1' THEN
                SELECT 'WARNING: batch_size option is deprecated in MySQL 9.4.1+. Manually limit input table to 100 rows (10 rows if >10 columns).' AS warning_message;
            END IF;

            IF v_batch_size < 1 OR v_batch_size > 100 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'batch_size must be between 1 and 100';
            END IF;
        END IF;

        -- Validate prediction_explainer values
        IF v_prediction_explainer NOT IN ('permutation_importance', 'shap') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prediction_explainer must be "permutation_importance" or "shap"';
        END IF;
    END IF;

    -- Check if input table exists and get its properties
    SELECT COUNT(*) INTO v_input_table_exists
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_input_schema
    AND TABLE_NAME = v_input_table;

    IF v_input_table_exists = 0 THEN
        SET v_error_msg = CONCAT('Input table not found: ', table_name);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;

    -- Check if output schema exists
    SELECT COUNT(*) INTO v_output_schema_exists
    FROM information_schema.SCHEMATA
    WHERE SCHEMA_NAME = v_output_schema;

    IF v_output_schema_exists = 0 THEN
        SET v_error_msg = CONCAT('Output schema not found: ', v_output_schema);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;

    -- Get table row count using prepared statement (FIXED)
    SET @row_count_sql = CONCAT('SELECT COUNT(*) FROM `', v_input_schema, '`.`', v_input_table, '` INTO @temp_row_count');
    PREPARE stmt FROM @row_count_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    SET v_row_count = @temp_row_count;

    -- Get table column count
    SELECT COUNT(*) INTO v_column_count
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = v_input_schema
    AND TABLE_NAME = v_input_table;

    -- Check row count limits based on MySQL version
    IF v_mysql_version >= '9.4.1' THEN
        IF v_column_count > 10 AND v_row_count > 10 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table has >10 columns and >10 rows. Limit to 10 rows for tables with >10 columns in MySQL 9.4.1+';
        ELSEIF v_row_count > 100 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table exceeds 100 row limit for MySQL 9.4.1+';
        END IF;
    END IF;

    -- Check if model exists (simplified check)
    SELECT COUNT(*) INTO v_model_exists
    FROM performance_schema.ml_model_endpoints 
    WHERE model_handle = model_handle;

    IF v_model_exists = 0 THEN
        SET v_error_msg = CONCAT('Model handle not found: ', model_handle);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
    END IF;

    -- Check if SHAP explainer is requested but not trained
    IF v_prediction_explainer = 'shap' THEN
        SELECT COUNT(*) INTO v_shap_exists
        FROM performance_schema.ml_model_explainers 
        WHERE model_handle = model_handle 
        AND explainer_type = 'shap' 
        AND status = 'READY';

        IF v_shap_exists = 0 THEN
            SET v_error_msg = 'SHAP explainer not found or not ready. Please run ML_EXPLAIN with "shap" explainer first.';
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
        END IF;
    END IF;

    -- Check for primary key in input table
    SELECT COUNT(*) INTO v_has_primary_key
    FROM information_schema.KEY_COLUMN_USAGE k
    JOIN information_schema.TABLE_CONSTRAINTS t
        ON k.CONSTRAINT_NAME = t.CONSTRAINT_NAME
        AND k.TABLE_SCHEMA = t.TABLE_SCHEMA
        AND k.TABLE_NAME = t.TABLE_NAME
    WHERE k.TABLE_SCHEMA = v_input_schema
    AND k.TABLE_NAME = v_input_table
    AND t.CONSTRAINT_TYPE = 'PRIMARY KEY';

    -- Check for problematic column name conflicts
    IF v_has_primary_key = 0 THEN
        IF v_mysql_version >= '8.4.1' THEN
            SELECT COUNT(*) INTO v_pk_column_conflict
            FROM information_schema.COLUMNS
            WHERE TABLE_SCHEMA = v_input_schema
            AND TABLE_NAME = v_input_table
            AND COLUMN_NAME = '_4aad19ca6e_pk_id';

            IF v_pk_column_conflict > 0 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table cannot have column named "_4aad19ca6e_pk_id" that is not a primary key';
            END IF;
        ELSE
            SELECT COUNT(*) INTO v_pk_column_conflict
            FROM information_schema.COLUMNS
            WHERE TABLE_SCHEMA = v_input_schema
            AND TABLE_NAME = v_input_table
            AND COLUMN_NAME = '_id';

            IF v_pk_column_conflict > 0 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table cannot have column named "_id" that is not a primary key';
            END IF;
        END IF;
    END IF;

    IF v_error THEN
        RESIGNAL;
    END IF;

    -- Call native function ML_EXPLAIN_TABLE
    SELECT ML_EXPLAIN_TABLE(table_name, model_handle, output_table_name, options);
END$$

DELIMITER ;