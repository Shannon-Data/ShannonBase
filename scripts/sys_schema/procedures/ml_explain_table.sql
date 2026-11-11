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
    IN in_table_name VARCHAR(256),
    IN in_model_handle VARCHAR(255),
    IN in_output_table_name VARCHAR(256),
    IN in_options JSON
)
COMMENT '
Description
-----------
Generate explanations for an entire table of unlabeled data using a trained ML model.
Explanations are limited to the 100 most relevant features per row.

The output table holds the input columns, a Prediction column and the ml_results
JSON column. When the input table has no primary key, an auto-increment primary key
named _4aad19ca6e_pk_id is added as the first column.

Supported Models:
- Classification
- Regression

NOT Supported Models:
- Recommendation
- Anomaly detection
- Topic modeling
- Forecasting

Parameters
-----------
in_table_name: Input table name (db.table)
in_model_handle: ML model handle
in_output_table_name: Output table name (db.table)
in_options: JSON options:
  - prediction_explainer: permutation_importance (default) or shap
  - batch_size: accepted for compatibility, 1 to 100; the table is explained in one pass
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_error_msg TEXT;
    DECLARE v_prediction_explainer VARCHAR(64) DEFAULT 'permutation_importance';
    DECLARE v_batch_size INT DEFAULT 100;
    DECLARE v_db_name VARCHAR(64);
    DECLARE v_table_name VARCHAR(64);
    DECLARE v_output_db_name VARCHAR(64);
    DECLARE v_output_table VARCHAR(64);
    DECLARE v_ml_schema VARCHAR(64);
    DECLARE v_has_primary_key BOOLEAN DEFAULT FALSE;
    DECLARE v_column_list LONGTEXT;
    DECLARE v_select_columns LONGTEXT;
    DECLARE v_features JSON;
    DECLARE v_attr_definitions LONGTEXT;
    DECLARE v_attr_columns LONGTEXT;
    DECLARE v_attr_values LONGTEXT;
    DECLARE v_result_column VARCHAR(64);
    DECLARE v_catalog_exists INT DEFAULT 0;
    DECLARE v_table_exists INT DEFAULT 0;
    DECLARE v_pk_exists INT DEFAULT 0;
    DECLARE v_column_clash INT DEFAULT 0;

    DECLARE v_output_created BOOLEAN DEFAULT FALSE;
    DECLARE v_saved_concat BIGINT UNSIGNED;

    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
        IF v_output_created THEN
            SET @drop_sql = CONCAT('DROP TABLE IF EXISTS `', REPLACE(v_output_db_name,'`','``'), '`.`',
                                   REPLACE(v_output_table,'`','``'), '`');
            PREPARE stmt FROM @drop_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        END IF;
        IF v_saved_concat IS NOT NULL THEN SET SESSION group_concat_max_len = v_saved_concat; END IF;
        RESIGNAL;
    END;

    -- These routines create persistent tables and cannot join a caller transaction.
    IF ML_SESSION_IN_TRANSACTION() OR @@SESSION.autocommit = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'ML routine requires autocommit and no active transaction';
    END IF;

    SET v_saved_concat = @@SESSION.group_concat_max_len;
    SET SESSION group_concat_max_len = 1000000;

    IF in_table_name IS NULL OR in_table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table name required';
    END IF;

    IF in_model_handle IS NULL OR in_model_handle = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model handle required';
    END IF;

    IF in_output_table_name IS NULL OR in_output_table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output table required';
    END IF;

    IF in_options IS NOT NULL THEN
        IF JSON_VALID(in_options) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid JSON';
        END IF;

        IF JSON_EXTRACT(in_options, '$.prediction_explainer') IS NOT NULL THEN
            SET v_prediction_explainer = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.prediction_explainer'));
        END IF;

        IF v_prediction_explainer NOT IN ('permutation_importance', 'shap') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid explainer';
        END IF;

        IF JSON_EXTRACT(in_options, '$.batch_size') IS NOT NULL THEN
            SET v_batch_size = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.batch_size'));
            IF v_batch_size < 1 OR v_batch_size > 100 THEN
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'batch_size must be between 1 and 100';
            END IF;
        END IF;
    END IF;

    IF INSTR(in_table_name, '.') > 0 THEN
        SET v_db_name = SUBSTRING_INDEX(in_table_name, '.', 1);
        SET v_table_name = SUBSTRING_INDEX(in_table_name, '.', -1);
    ELSE
        SET v_db_name = IFNULL(DATABASE(), '');
        SET v_table_name = in_table_name;
    END IF;

    IF INSTR(in_output_table_name, '.') > 0 THEN
        SET v_output_db_name = SUBSTRING_INDEX(in_output_table_name, '.', 1);
        SET v_output_table = SUBSTRING_INDEX(in_output_table_name, '.', -1);
    ELSE
        SET v_output_db_name = IFNULL(DATABASE(), '');
        SET v_output_table = in_output_table_name;
    END IF;

    IF v_db_name = '' OR v_output_db_name = '' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Use fully qualified schema.table names when no default database is selected';
    END IF;

    IF BINARY v_output_db_name = BINARY v_db_name AND BINARY v_output_table = BINARY v_table_name THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output table must differ from the input table';
    END IF;

    -- The per-user model catalog must exist before the model can be validated.
    SET v_ml_schema = CONCAT('ML_SCHEMA_', SUBSTRING_INDEX(CURRENT_USER(), '@', 1));
    IF INSTR(SUBSTRING_INDEX(v_ml_schema, '.', 2), '.') > 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'ML routines do not support account names containing a period';
    END IF;

    SELECT COUNT(*) INTO v_catalog_exists
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_ml_schema AND TABLE_NAME = 'MODEL_CATALOG';
    IF v_catalog_exists = 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'No model catalog for the current user, train a model first';
    END IF;

    SET @_handle = in_model_handle;
    -- Aggregate lookup always assigns a row, including for a missing handle.
    SET @sql = CONCAT('SELECT COUNT(*), MIN(TASK), ANY_VALUE(JSON_EXTRACT(MODEL_METADATA, ''$.column_names'')) ',
                      'INTO @_model_count, @_task, @_features FROM `', REPLACE(v_ml_schema,'`','``'),
                      '`.MODEL_CATALOG WHERE MODEL_HANDLE = ?');
    PREPARE stmt FROM @sql; EXECUTE stmt USING @_handle; DEALLOCATE PREPARE stmt;
    IF @_model_count = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model handle does not exist in the model catalog';
    END IF;
    SET v_features = @_features;
    IF v_features IS NULL OR JSON_TYPE(v_features) <> 'ARRAY' OR JSON_LENGTH(v_features) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model feature metadata is missing';
    END IF;
    IF LOWER(@_task) NOT IN ('classification', 'regression') THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'ML_EXPLAIN_TABLE supports only classification and regression models';
    END IF;

    IF EXISTS (SELECT 1 FROM information_schema.TABLES t
               WHERE t.TABLE_SCHEMA = v_output_db_name AND t.TABLE_NAME = v_output_table) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output table already exists';
    END IF;

    SELECT COUNT(*) INTO v_table_exists
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = v_db_name AND TABLE_NAME = v_table_name;
    IF v_table_exists = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table not found';
    END IF;

    -- Require every trained feature, while allowing additional input columns.
    IF EXISTS (
        SELECT 1 FROM JSON_TABLE(v_features, '$[*]' COLUMNS (feature_name VARCHAR(64) PATH '$')) f
        WHERE NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS c
            WHERE c.TABLE_SCHEMA=v_db_name AND c.TABLE_NAME=v_table_name AND c.COLUMN_NAME=f.feature_name)
    ) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table is missing a model feature column';
    END IF;
    IF EXISTS (SELECT 1 FROM JSON_TABLE(v_features, '$[*]' COLUMNS (feature_name VARCHAR(64) PATH '$')) f
               WHERE CHAR_LENGTH(f.feature_name) + CHAR_LENGTH('_attribution') > 64) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Feature attribution column name exceeds 64 characters';
    END IF;

    SELECT COUNT(*) INTO v_column_clash
    FROM information_schema.COLUMNS c
    WHERE c.TABLE_SCHEMA = v_db_name AND c.TABLE_NAME = v_table_name
      AND (c.COLUMN_NAME IN ('Prediction', 'ml_results', 'Notes')
           OR (c.COLUMN_NAME = '_4aad19ca6e_pk_id' AND c.COLUMN_KEY <> 'PRI')
           OR EXISTS (SELECT 1 FROM JSON_TABLE(v_features, '$[*]' COLUMNS (feature_name VARCHAR(64) PATH '$')) f
                      WHERE c.COLUMN_NAME=CONCAT(f.feature_name,'_attribution')));
    IF v_column_clash > 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table has a conflicting explanation output column';
    END IF;

    SELECT COUNT(*) INTO v_pk_exists
    FROM information_schema.TABLE_CONSTRAINTS
    WHERE CONSTRAINT_TYPE = 'PRIMARY KEY'
      AND TABLE_SCHEMA = v_db_name AND TABLE_NAME = v_table_name;
    SET v_has_primary_key = (v_pk_exists > 0);

    -- Canonical model spelling matters for JSON keys, unlike SQL column names.
    SELECT
        GROUP_CONCAT(CONCAT(QUOTE(COALESCE(f.feature_name,c.COLUMN_NAME)), ', `', REPLACE(c.COLUMN_NAME,'`','``'), '`')
                     ORDER BY c.ORDINAL_POSITION SEPARATOR ', '),
        GROUP_CONCAT(CONCAT('`', REPLACE(c.COLUMN_NAME,'`','``'), '`')
                     ORDER BY c.ORDINAL_POSITION SEPARATOR ', ')
    INTO v_column_list, v_select_columns
    FROM information_schema.COLUMNS c
    LEFT JOIN JSON_TABLE(v_features, '$[*]' COLUMNS (feature_name VARCHAR(64) PATH '$')) f
      ON c.COLUMN_NAME=f.feature_name
    WHERE c.TABLE_SCHEMA=v_db_name AND c.TABLE_NAME=v_table_name;

    SET v_result_column=CONCAT('_ml_result_',REPLACE(UUID(),'-',''));
    SET @_row_options=JSON_OBJECT('prediction_explainer',v_prediction_explainer);

    SELECT
        GROUP_CONCAT(CONCAT('ADD COLUMN `',REPLACE(f.feature_name,'`','``'),'_attribution` DOUBLE')
                     ORDER BY f.position SEPARATOR ', '),
        GROUP_CONCAT(CONCAT('`',REPLACE(f.feature_name,'`','``'),'_attribution`')
                     ORDER BY f.position SEPARATOR ', '),
        GROUP_CONCAT(CONCAT('JSON_EXTRACT(`',v_result_column,'`,',
                     QUOTE(CONCAT('$.ml_results.attributions.',JSON_QUOTE(f.feature_name))),')')
                     ORDER BY f.position SEPARATOR ', ')
    INTO v_attr_definitions,v_attr_columns,v_attr_values
    FROM JSON_TABLE(v_features, '$[*]' COLUMNS (position FOR ORDINALITY, feature_name VARCHAR(64) PATH '$')) f;

    SET @create_sql = CONCAT('CREATE TABLE `', REPLACE(v_output_db_name,'`','``'), '`.`',
                             REPLACE(v_output_table,'`','``'), '` LIKE `', REPLACE(v_db_name,'`','``'), '`.`',
                             REPLACE(v_table_name,'`','``'), '`');
    PREPARE stmt FROM @create_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET v_output_created = TRUE;

    SET @alter_sql = CONCAT('ALTER TABLE `', REPLACE(v_output_db_name,'`','``'), '`.`',
                            REPLACE(v_output_table,'`','``'), '` SECONDARY_ENGINE = NULL');
    PREPARE stmt FROM @alter_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    IF NOT v_has_primary_key THEN
        SET @alter_sql = CONCAT('ALTER TABLE `', REPLACE(v_output_db_name,'`','``'), '`.`',
                                REPLACE(v_output_table,'`','``'),
                                '` ADD COLUMN `_4aad19ca6e_pk_id` INT NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST');
        PREPARE stmt FROM @alter_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    END IF;

    SET @alter_sql = CONCAT('ALTER TABLE `', REPLACE(v_output_db_name,'`','``'), '`.`',
                            REPLACE(v_output_table,'`','``'),
                            '` ADD COLUMN `Prediction` VARCHAR(256), ',v_attr_definitions,
                            ', ADD COLUMN `Notes` LONGTEXT, ADD COLUMN `ml_results` JSON');
    PREPARE stmt FROM @alter_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    -- Materialize the derived result once per row. An explicit SQL temporary
    -- table would require extra privileges on the routine's sys schema.
    SET @insert_sql = CONCAT('INSERT INTO `',REPLACE(v_output_db_name,'`','``'),'`.`',
        REPLACE(v_output_table,'`','``'),'` (',v_select_columns,', `Prediction`, ',v_attr_columns,', `Notes`, `ml_results`) ',
        'SELECT /*+ NO_MERGE(explanations) */ ',v_select_columns,', JSON_UNQUOTE(JSON_EXTRACT(`',v_result_column,'`,''$.Prediction'')), ',
        v_attr_values,', JSON_UNQUOTE(JSON_EXTRACT(`',v_result_column,'`,''$.ml_results.notes'')), ',
        'JSON_EXTRACT(`',v_result_column,'`,''$.ml_results'') FROM (SELECT ',v_select_columns,
        ', sys.ml_explain_row(JSON_OBJECT(',v_column_list,'),@_handle,@_row_options) AS `',v_result_column,
        '` FROM `',REPLACE(v_db_name,'`','``'),'`.`',REPLACE(v_table_name,'`','``'),'`) explanations');
    PREPARE stmt FROM @insert_sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET SESSION group_concat_max_len = v_saved_concat;
END$$

DELIMITER ;
