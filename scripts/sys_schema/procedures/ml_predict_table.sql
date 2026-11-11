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

DROP PROCEDURE IF EXISTS ml_predict_table;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_predict_table (
        in_sch_tb_name      VARCHAR(256),
        in_model_handle_name VARCHAR(255),
        in_output_table_name VARCHAR(256),
        in_model_option      JSON
    )
    COMMENT '
ML_PREDICT_TABLE generates predictions for an entire table of unlabeled data
and saves the results to an output table, using sys.ml_predict_row internally.

Parameters
-----------
in_sch_tb_name VARCHAR(256):
  Fully qualified input table name (schema_name.table_name).
in_model_handle_name VARCHAR(256):
  Model handle or session variable containing the model handle.
in_output_table_name VARCHAR(256):
  Fully qualified output table name (schema_name.table_name).
  Must not already exist. The table holds the input columns, a Prediction
  column and the ml_results JSON column. When the input table has no primary
  key, an auto-increment primary key named _4aad19ca6e_pk_id is added first.
in_model_option JSON:
  Options forwarded to ml_predict_row (threshold, topk, recommend, etc.).
  Pass NULL if not needed.

Example
-----------
mysql> CALL sys.ML_PREDICT_TABLE(
         \'census_data.census_train\',
         @census_model,
         \'census_data.census_predictions\',
         NULL);
    '
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_db_err_msg   TEXT;
    DECLARE v_in_schema    VARCHAR(64);
    DECLARE v_in_table     VARCHAR(64);
    DECLARE v_out_schema   VARCHAR(64);
    DECLARE v_out_table    VARCHAR(64);
    DECLARE v_table_count  INT DEFAULT 0;

    DECLARE v_output_created BOOLEAN DEFAULT FALSE;
    DECLARE v_ml_schema VARCHAR(64);
    DECLARE v_catalog_exists INT DEFAULT 0;
    DECLARE v_column_clash INT DEFAULT 0;
    DECLARE v_saved_concat BIGINT UNSIGNED;

    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_db_err_msg = MESSAGE_TEXT;

        IF v_output_created THEN
            SET @_cleanup = CONCAT(
                'DROP TABLE IF EXISTS `', v_out_schema, '`.`', v_out_table, '`;'
            );
            PREPARE _s FROM @_cleanup;
            EXECUTE _s;
            DEALLOCATE PREPARE _s;
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
    SET @pk_col = '_4aad19ca6e_pk_id';

    -- Validate name formats
    IF in_sch_tb_name NOT REGEXP '^[^.]+\\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Invalid schema.table format for input table.';
    END IF;

    IF in_output_table_name NOT REGEXP '^[^.]+\\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Invalid schema.table format for output table.';
    END IF;

    -- Parse schema / table names
    SET v_in_schema  = SUBSTRING_INDEX(in_sch_tb_name,      '.', 1);
    SET v_in_table   = SUBSTRING_INDEX(in_sch_tb_name,      '.', -1);
    SET v_out_schema = SUBSTRING_INDEX(in_output_table_name, '.', 1);
    SET v_out_table  = SUBSTRING_INDEX(in_output_table_name, '.', -1);

    -- Input table must exist
    SELECT COUNT(*) INTO v_table_count
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_in_schema AND TABLE_NAME = v_in_table;

    IF v_table_count = 0 THEN
        SET v_db_err_msg = CONCAT('Input table ', in_sch_tb_name, ' does not exist.');
        SIGNAL SQLSTATE 'HY000' SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    -- Output table must NOT exist
    SELECT COUNT(*) INTO v_table_count
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_out_schema AND TABLE_NAME = v_out_table;

    IF v_table_count != 0 THEN
        SET v_db_err_msg = CONCAT('Output table ', in_output_table_name, ' already exists.');
        SIGNAL SQLSTATE 'HY000' SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    -- Forecasting needs a task-specific table implementation, not row prediction.
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

    -- Forecasting models are rejected further down, by the native predict path.
    SET @_task = NULL;
    SET @_handle = in_model_handle_name;
    SET @sql = CONCAT('SELECT TASK INTO @_task FROM `', REPLACE(v_ml_schema,'`','``'),
                     '`.MODEL_CATALOG WHERE MODEL_HANDLE=?');
    PREPARE stmt FROM @sql; EXECUTE stmt USING @_handle; DEALLOCATE PREPARE stmt;
    IF @_task IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model handle does not exist in the model catalog';
    END IF;

    -- Check for PK and reserved column name conflict
    SELECT COUNT(*) INTO @has_pk
    FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = v_in_schema
      AND TABLE_NAME   = v_in_table
      AND CONSTRAINT_TYPE = 'PRIMARY KEY';

    SELECT COUNT(*) INTO @has_conflict
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = v_in_schema
      AND TABLE_NAME   = v_in_table
      AND COLUMN_NAME  = @pk_col
      AND COLUMN_KEY  != 'PRI';       -- exists but is not already the PK

    IF @has_conflict > 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Conflict: auto-PK column name already exists and is not a primary key.';
    END IF;

    SELECT COUNT(*) INTO v_column_clash
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = v_in_schema
      AND TABLE_NAME   = v_in_table
      AND COLUMN_NAME IN ('Prediction', 'ml_results');

    IF v_column_clash > 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Input table already has a column named Prediction or ml_results';
    END IF;

    -- Create output table (structure clone)
    SET @sql = CONCAT(
        'CREATE TABLE `', v_out_schema, '`.`', v_out_table, '`'
        ' LIKE `', v_in_schema, '`.`', v_in_table, '`;'
    );
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET v_output_created = TRUE;

    -- Detach from HeatWave secondary engine
    SET @sql = CONCAT(
        'ALTER TABLE `', v_out_schema, '`.`', v_out_table, '` secondary_engine NULL;'
    );
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    -- Add the surrogate PK first, then the result columns, as documented.
    IF @has_pk = 0 THEN
        SET @sql = CONCAT(
            'ALTER TABLE `', v_out_schema, '`.`', v_out_table, '`'
            ' ADD COLUMN `', @pk_col, '` INT AUTO_INCREMENT PRIMARY KEY FIRST;'
        );
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    END IF;

    SET @sql = CONCAT(
        'ALTER TABLE `', v_out_schema, '`.`', v_out_table, '`'
        ' ADD COLUMN `Prediction` VARCHAR(256),'
        ' ADD COLUMN `ml_results` JSON;'
    );
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    -- Build dynamic column list and JSON_OBJECT(...) expression
    -- @col_list : `col1`, `col2`, ...
    -- @json_kv  : "col1", `col1`, "col2", `col2`, ...
    SELECT
        GROUP_CONCAT(
            CONCAT('`', COLUMN_NAME, '`')
            ORDER BY ORDINAL_POSITION
            SEPARATOR ', '
        ),
        GROUP_CONCAT(
            CONCAT('"', REPLACE(COLUMN_NAME, '"', '\\"'), '", `', COLUMN_NAME, '`')
            ORDER BY ORDINAL_POSITION
            SEPARATOR ', '
        )
    INTO @col_list, @json_kv
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = v_in_schema AND TABLE_NAME = v_in_table;

    -- Pass model handle and options via session variables so prepared
    -- statement avoids quoting / escaping hazards entirely.
    SET @_row_option = NULL;

    IF in_model_option IS NOT NULL THEN
        SET @_row_option = JSON_REMOVE(
            in_model_option,
            -- only removes the key if it actually exists; no-op otherwise
            IF(JSON_CONTAINS_PATH(in_model_option, 'one', '$.batch_size'),
               '$.batch_size',        '$._noop'),
            IF(JSON_CONTAINS_PATH(in_model_option, 'one', '$.prediction_interval'),
               '$.prediction_interval','$._noop'),
            IF(JSON_CONTAINS_PATH(in_model_option, 'one', '$.item_metadata'),
               '$.item_metadata',     '$._noop'),
            IF(JSON_CONTAINS_PATH(in_model_option, 'one', '$.user_metadata'),
               '$.user_metadata',     '$._noop'),
            IF(JSON_CONTAINS_PATH(in_model_option, 'one', '$.logad_options'),
               '$.logad_options',     '$._noop')
        );

        -- If stripping left an empty object {}, treat it as NULL so
        -- ml_predict_row receives a clean NULL rather than '{}'.
        IF JSON_LENGTH(@_row_option) = 0 THEN
            SET @_row_option = NULL;
        END IF;
    END IF;

    SET @_model_handle = in_model_handle_name;
    SET @_model_option = @_row_option;
    SET @sql = CONCAT(
        'INSERT INTO `', v_out_schema, '`.`', v_out_table, '`'
        ' (', @col_list, ', `Prediction`, ml_results) ',
        -- ml_predict_row returns {<features>, "Prediction": .., "ml_results": {..}}.
        -- The member is spelled "Prediction" and JSON paths are case sensitive,
        -- so '$.prediction' matched nothing and every prediction came out NULL.
        -- The ml_results column takes the nested object, not the whole row, so
        -- '$.probabilities' / '$.predictions' resolve against it as documented.
        'SELECT ', @col_list, ',',
        '       JSON_UNQUOTE(JSON_EXTRACT(_pred, ''$.Prediction'')),',
        '       JSON_EXTRACT(_pred, ''$.ml_results'') ',
        'FROM (',
        '  SELECT ', @col_list, ',',
        '         sys.ml_predict_row(',
        '             JSON_OBJECT(', @json_kv, '),',
        '             @_model_handle,',
        '             @_model_option',
        '         ) AS _pred',
        '  FROM `', v_in_schema, '`.`', v_in_table, '`',
        ') _t;'
    );

    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;

    SET SESSION group_concat_max_len = v_saved_concat;
END$$

DELIMITER ;
