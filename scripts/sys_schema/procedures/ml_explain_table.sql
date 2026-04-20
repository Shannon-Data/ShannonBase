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
    IN table_name VARCHAR(255),
    IN model_handle VARCHAR(64),
    IN output_table_name VARCHAR(255),
    IN options JSON
)
COMMENT '
Description
-----------
Generate explanations for an entire table of unlabeled data using a trained ML model.
This procedure limits explanations to the 100 most relevant features.

This implementation uses ml_explain_row function to process each row individually.

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
table_name: Input table name (db.table)
model_handle: ML model handle
output_table_name: Output table name (db.table)
options: JSON options
'
    SQL SECURITY INVOKER
BEGIN
    DECLARE v_error_msg TEXT;
    DECLARE v_prediction_explainer VARCHAR(64) DEFAULT 'permutation_importance';
    DECLARE v_batch_size INT DEFAULT 100;
    DECLARE v_db_name VARCHAR(64);
    DECLARE v_table_name VARCHAR(64);
    DECLARE v_output_db_name VARCHAR(64);
    DECLARE v_output_table VARCHAR(64);
    DECLARE v_has_primary_key BOOLEAN DEFAULT FALSE;
    DECLARE v_primary_key_column VARCHAR(64);
    DECLARE v_column_list TEXT;
    DECLARE v_select_columns TEXT;
    DECLARE v_sql TEXT;

    -- Exception handler
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
        SET @err_msg = LEFT(CONCAT('ML Error: ', v_error_msg), 128);
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = @err_msg;
    END;

    SET SESSION group_concat_max_len = 1000000;

    -- Validate parameters
    IF table_name IS NULL OR table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table name required';
    END IF;

    IF model_handle IS NULL OR model_handle = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model handle required';
    END IF;

    IF output_table_name IS NULL OR output_table_name = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output table required';
    END IF;

    -- Parse options
    IF options IS NOT NULL THEN
        IF JSON_VALID(options) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid JSON';
        END IF;

        IF JSON_EXTRACT(options, '$.prediction_explainer') IS NOT NULL THEN
            SET v_prediction_explainer = JSON_UNQUOTE(JSON_EXTRACT(options, '$.prediction_explainer'));
        END IF;

        IF JSON_EXTRACT(options, '$.batch_size') IS NOT NULL THEN
            SET v_batch_size = JSON_UNQUOTE(JSON_EXTRACT(options, '$.batch_size'));
        END IF;

        IF v_prediction_explainer NOT IN ('permutation_importance', 'shap') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid explainer';
        END IF;
    END IF;

    -- Parse database and table names
    IF INSTR(table_name, '.') > 0 THEN
        SET v_db_name = SUBSTRING_INDEX(table_name, '.', 1);
        SET v_table_name = SUBSTRING_INDEX(table_name, '.', -1);
    ELSE
        SET v_db_name = DATABASE();
        SET v_table_name = table_name;
    END IF;

    IF INSTR(output_table_name, '.') > 0 THEN
        SET v_output_db_name = SUBSTRING_INDEX(output_table_name, '.', 1);
        SET v_output_table = SUBSTRING_INDEX(output_table_name, '.', -1);
    ELSE
        SET v_output_db_name = DATABASE();
        SET v_output_table = output_table_name;
    END IF;

    -- Check if input table exists
    SET @check_sql = CONCAT(
        'SELECT COUNT(*) INTO @table_exists FROM information_schema.tables ',
        'WHERE table_schema = ''', v_db_name, ''' AND table_name = ''', v_table_name, ''''
    );
    PREPARE stmt FROM @check_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    IF @table_exists = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Table not found';
    END IF;

    -- Check row count
    SET @count_sql = CONCAT('SELECT COUNT(*) INTO @row_count FROM ', v_db_name, '.', v_table_name);
    PREPARE stmt FROM @count_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    IF @row_count > 100 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Row limit 100 exceeded';
    END IF;

    -- Check primary key
    SET @pk_check_sql = CONCAT(
        'SELECT COUNT(*) INTO @pk_exists FROM information_schema.table_constraints tc ',
        'WHERE tc.constraint_type = ''PRIMARY KEY'' ',
        'AND tc.table_schema = ''', v_db_name, ''' AND tc.table_name = ''', v_table_name, ''''
    );
    PREPARE stmt FROM @pk_check_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    SET v_has_primary_key = (@pk_exists > 0);
    
    -- Get primary key column name if exists
    IF v_has_primary_key THEN
        SET @pk_name_sql = CONCAT(
            'SELECT kcu.column_name INTO @pk_column FROM information_schema.table_constraints tc ',
            'JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name ',
            'WHERE tc.constraint_type = ''PRIMARY KEY'' ',
            'AND tc.table_schema = ''', v_db_name, ''' AND tc.table_name = ''', v_table_name, ''' ',
            'LIMIT 1'
        );
        PREPARE stmt FROM @pk_name_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        SET v_primary_key_column = @pk_column;
    END IF;

    -- Build column list for JSON_OBJECT
    SET @col_sql = CONCAT(
        'SELECT GROUP_CONCAT(CONCAT(''"'', column_name, ''", `'', column_name, ''`'') SEPARATOR '','') INTO @col_list ',
        'FROM information_schema.columns ',
        'WHERE table_schema = ''', v_db_name, ''' AND table_name = ''', v_table_name, ''''
    );
    PREPARE stmt FROM @col_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    SET v_column_list = @col_list;
    
    -- Build select columns for INSERT
    SET @sel_sql = CONCAT(
        'SELECT GROUP_CONCAT(CONCAT(''`'', column_name, ''`'') SEPARATOR '','') INTO @sel_list ',
        'FROM information_schema.columns ',
        'WHERE table_schema = ''', v_db_name, ''' AND table_name = ''', v_table_name, ''''
    );
    PREPARE stmt FROM @sel_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    SET v_select_columns = @sel_list;
    
    -- Drop output table
    SET @drop_sql = CONCAT('DROP TABLE IF EXISTS ', v_output_db_name, '.', v_output_table);
    PREPARE stmt FROM @drop_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
    -- Create output table
    IF v_has_primary_key THEN
        SET @create_sql = CONCAT('CREATE TABLE ', v_output_db_name, '.', v_output_table, ' LIKE ', v_db_name, '.', v_table_name);
        PREPARE stmt FROM @create_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET @alter_sql = CONCAT('ALTER TABLE ', v_output_db_name, '.', v_output_table, ' SECONDARY_ENGINE=NULL');
        PREPARE stmt FROM @alter_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        SET @alter_sql = CONCAT('ALTER TABLE ', v_output_db_name, '.', v_output_table, ' ADD COLUMN ml_results JSON');
        PREPARE stmt FROM @alter_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        SET @insert_sql = CONCAT(
            'INSERT INTO ', v_output_db_name, '.', v_output_table, ' ',
            'SELECT t.*, sys.ml_explain_row(',
            'JSON_OBJECT(', v_column_list, '), ',
            '''', model_handle, ''', ',
            'JSON_OBJECT(''prediction_explainer'', ''', v_prediction_explainer, ''')',
            ') FROM ', v_db_name, '.', v_table_name, ' t'
        );
    ELSE
        SET @col_defs_sql = CONCAT(
            'SELECT GROUP_CONCAT(CONCAT(''`'', column_name, ''` '', column_type, ',
            'IF(is_nullable = ''NO'' AND column_key != ''PRI'', '' NOT NULL'', '' ''), ',
            'IF(column_default IS NOT NULL, CONCAT('' DEFAULT '', QUOTE(column_default)), ''''), ',
            'IF(extra != '''', CONCAT('' '', extra), '''')',
            ') SEPARATOR '','') INTO @col_defs ',
            'FROM information_schema.columns ',
            'WHERE table_schema = ''', v_db_name, ''' AND table_name = ''', v_table_name, ''''
        );
        PREPARE stmt FROM @col_defs_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        SET @create_sql = CONCAT(
            'CREATE TABLE ', v_output_db_name, '.', v_output_table, ' (',
            'pk_id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, ',
            'ml_results JSON, ',
            @col_defs,
            ')'
        );
        PREPARE stmt FROM @create_sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        SET @insert_sql = CONCAT(
            'INSERT INTO ', v_output_db_name, '.', v_output_table, ' ',
            '(pk_id, ml_results, ', v_select_columns, ') ',
            'SELECT @rn := @rn + 1, ',
            'sys.ml_explain_row(JSON_OBJECT(', v_column_list, '), ''', model_handle, ''', ',
            'JSON_OBJECT(''prediction_explainer'', ''', v_prediction_explainer, ''')), ',
            v_select_columns,
            ' FROM ', v_db_name, '.', v_table_name, ', (SELECT @rn := 0) r'
        );
    END IF;
    
    PREPARE stmt FROM @insert_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
END$$

DELIMITER ;
