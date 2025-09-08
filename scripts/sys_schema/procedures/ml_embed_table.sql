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

DROP PROCEDURE IF EXISTS ml_embed_table;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_embed_table (
    IN in_input_table_column VARCHAR(255),
    IN in_output_table_column VARCHAR(255),
    IN in_options JSON
)
COMMENT '
Description
-----------
The ML_EMBED_TABLE routine uses the specified embedding model to encode text data 
from an input table column into vector embeddings and stores them in an output table column.

Parameters
-----------
in_input_table_column (VARCHAR(255)):
  Specifies the input database, table, and column in format: DBName.TableName.ColumnName
  
in_output_table_column (VARCHAR(255)):
  Specifies the output database, table, and column in format: DBName.TableName.ColumnName

in_options (JSON):
  Specifies optional parameters as key-value pairs in JSON format:
  - model_id: The embedding model to use (required)
  - truncate: Whether to truncate inputs longer than maximum token size (true|false, default: true)
  - batch_size: Number of rows to process in each batch (1-1000, default: 1000)
  - details_column: Name of column to store error details (default: "details")

Example
-----------
mysql> CALL sys.ML_EMBED_TABLE(
    "demo_db.input_table.Input", 
    "demo_db.output_table.Output",
    JSON_OBJECT(
        "model_id", "all_minilm_l12_v2",
        "batch_size", 500,
        "truncate", true
    )
);
'
SQL SECURITY INVOKER
MODIFIES SQL DATA
BEGIN
    DECLARE v_input_db VARCHAR(64);
    DECLARE v_input_table VARCHAR(64);
    DECLARE v_input_column VARCHAR(64);
    DECLARE v_output_db VARCHAR(64);
    DECLARE v_output_table VARCHAR(64);
    DECLARE v_output_column VARCHAR(64);
    DECLARE v_model_id VARCHAR(255);
    DECLARE v_truncate BOOLEAN DEFAULT TRUE;
    DECLARE v_batch_size INT DEFAULT 1000;
    DECLARE v_details_column VARCHAR(64) DEFAULT 'details';
    DECLARE v_input_same_as_output BOOLEAN DEFAULT FALSE;
    DECLARE v_table_exists INT DEFAULT 0;
    DECLARE v_column_exists INT DEFAULT 0;
    DECLARE v_has_primary_key INT DEFAULT 0;
    DECLARE v_is_external_table INT DEFAULT 0;
    DECLARE v_sql TEXT;
    DECLARE v_error_msg TEXT DEFAULT '';
    DECLARE v_total_rows INT DEFAULT 0;
    DECLARE v_processed_rows INT DEFAULT 0;
    DECLARE v_batch_start INT DEFAULT 0;

    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1
            v_error_msg = MESSAGE_TEXT;
        RESIGNAL SET MESSAGE_TEXT = v_error_msg;
    END;

    -- Parse input table column (DBName.TableName.ColumnName)
    IF CHAR_LENGTH(in_input_table_column) - CHAR_LENGTH(REPLACE(in_input_table_column, '.', '')) != 2 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid input table column format. Use DBName.TableName.ColumnName';
    END IF;

    SET v_input_db = SUBSTRING_INDEX(in_input_table_column, '.', 1);
    SET v_input_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_input_table_column, '.', 2), '.', -1);
    SET v_input_column = SUBSTRING_INDEX(in_input_table_column, '.', -1);

    -- Parse output table column (DBName.TableName.ColumnName)
    IF CHAR_LENGTH(in_output_table_column) - CHAR_LENGTH(REPLACE(in_output_table_column, '.', '')) != 2 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid output table column format. Use DBName.TableName.ColumnName';
    END IF;

    SET v_output_db = SUBSTRING_INDEX(in_output_table_column, '.', 1);
    SET v_output_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_output_table_column, '.', 2), '.', -1);
    SET v_output_column = SUBSTRING_INDEX(in_output_table_column, '.', -1);

    -- Validate no backticks or periods in names
    IF v_input_db REGEXP '`|\\..*\\.' OR v_input_table REGEXP '`|\\..*\\.' OR 
       v_output_db REGEXP '`|\\..*\\.' OR v_output_table REGEXP '`|\\..*\\.' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Database and table names cannot contain backticks or periods';
    END IF;

    -- Extract options from JSON
    IF in_options IS NOT NULL AND JSON_VALID(in_options) THEN
        SET v_model_id = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.model_id'));
        SET v_truncate = COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.truncate')) AS UNSIGNED), TRUE);
        SET v_batch_size = COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.batch_size')) AS UNSIGNED), 1000);
        SET v_details_column = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.details_column')), 'details');
    END IF;

    -- Validate required parameters
    IF v_model_id IS NULL OR v_model_id = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'model_id is required in options';
    END IF;

    -- Validate batch_size range
    IF v_batch_size < 1 OR v_batch_size > 1000 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'batch_size must be between 1 and 1000';
    END IF;

    -- Check if input table exists
    SELECT COUNT(*) INTO v_table_exists
    FROM information_schema.tables 
    WHERE table_schema = v_input_db AND table_name = v_input_table;

    IF v_table_exists = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table does not exist';
    END IF;

    -- Check if input table has primary key
    SELECT COUNT(*) INTO v_has_primary_key
    FROM information_schema.table_constraints 
    WHERE table_schema = v_input_db 
    AND table_name = v_input_table 
    AND constraint_type = 'PRIMARY KEY';

    IF v_has_primary_key = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input table must have a primary key';
    END IF;

    -- Check if input column exists and is valid
    SELECT COUNT(*) INTO v_column_exists
    FROM information_schema.columns 
    WHERE table_schema = v_input_db 
    AND table_name = v_input_table 
    AND column_name = v_input_column
    AND data_type IN ('text', 'varchar', 'char');

    IF v_column_exists = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input column does not exist or is not a text/varchar type';
    END IF;

    -- Check if input column is part of primary key
    SELECT COUNT(*) INTO v_column_exists
    FROM information_schema.key_column_usage k
    JOIN information_schema.table_constraints t ON k.constraint_name = t.constraint_name
    WHERE k.table_schema = v_input_db 
    AND k.table_name = v_input_table 
    AND k.column_name = v_input_column
    AND t.constraint_type = 'PRIMARY KEY';
    
    IF v_column_exists > 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Input column cannot be part of the primary key';
    END IF;

    -- Check if input and output tables are the same
    IF v_input_db = v_output_db AND v_input_table = v_output_table THEN
        SET v_input_same_as_output = TRUE;

        -- Check if output column already exists
        SELECT COUNT(*) INTO v_column_exists
        FROM information_schema.columns 
        WHERE table_schema = v_output_db 
        AND table_name = v_output_table 
        AND column_name = v_output_column;

        IF v_column_exists > 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output column already exists in the table';
        END IF;

        -- Add vector column to existing table
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN `', v_output_column, '` VECTOR');

        -- Add details column if specified and doesn't exist
        SELECT COUNT(*) INTO v_column_exists
        FROM information_schema.columns 
        WHERE table_schema = v_output_db 
        AND table_name = v_output_table 
        AND column_name = v_details_column;

        IF v_column_exists = 0 THEN
            SET v_sql = CONCAT(v_sql, ', ADD COLUMN `', v_details_column, '` TEXT');
        END IF;

        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

    ELSE
        -- Create new output table
        -- First check if output table already exists
        SELECT COUNT(*) INTO v_table_exists
        FROM information_schema.tables 
        WHERE table_schema = v_output_db AND table_name = v_output_table;

        IF v_table_exists > 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Output table already exists';
        END IF;

        -- Get primary key columns from input table
        SET v_sql = CONCAT('CREATE TABLE `', v_output_db, '`.`', v_output_table, '` AS 
                           SELECT ');

        -- Add primary key columns
        SELECT GROUP_CONCAT(CONCAT('`', column_name, '`') ORDER BY ordinal_position)
        INTO @pk_columns
        FROM information_schema.key_column_usage k
        JOIN information_schema.table_constraints t ON k.constraint_name = t.constraint_name
        WHERE k.table_schema = v_input_db 
        AND k.table_name = v_input_table 
        AND t.constraint_type = 'PRIMARY KEY';

        SET v_sql = CONCAT(v_sql, @pk_columns, ' FROM `', v_input_db, '`.`', v_input_table, '`');

        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        -- Add vector column
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN `', v_output_column, '` VECTOR,
                           ADD COLUMN `', v_details_column, '` TEXT');

        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        -- Create primary key on new table
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD PRIMARY KEY (', @pk_columns, ')');

        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;

    -- Process embeddings in batches using ML_EMBED_ROW
    IF v_input_same_as_output THEN
        -- Update same table with embeddings
        SET v_sql = CONCAT('UPDATE `', v_output_db, '`.`', v_output_table, '` 
                           SET `', v_output_column, '` = sys.ML_EMBED_ROW(`', v_input_column, '`, 
                           JSON_OBJECT("model_id", "', v_model_id, '", "truncate", ', v_truncate, ')),
                           `', v_details_column, '` = 
                           CASE 
                               WHEN `', v_input_column, '` IS NULL OR TRIM(`', v_input_column, '`) = "" 
                               THEN "Empty or null input text"
                               ELSE NULL 
                           END
                           WHERE `', v_input_column, '` IS NOT NULL AND TRIM(`', v_input_column, '`) != ""');
    ELSE
        -- Insert data with embeddings into new table
        SELECT GROUP_CONCAT(CONCAT('s.`', column_name, '`') ORDER BY ordinal_position)
        INTO @pk_columns_prefixed
        FROM information_schema.key_column_usage k
        JOIN information_schema.table_constraints t ON k.constraint_name = t.constraint_name
        WHERE k.table_schema = v_input_db 
        AND k.table_name = v_input_table 
        AND t.constraint_type = 'PRIMARY KEY';

        SET v_sql = CONCAT('INSERT INTO `', v_output_db, '`.`', v_output_table, '` 
                           SELECT ', @pk_columns, ',
                           sys.ML_EMBED_ROW(s.`', v_input_column, '`, 
                           JSON_OBJECT("model_id", "', v_model_id, '", "truncate", ', v_truncate, ')) as `', v_output_column, '`,
                           CASE 
                               WHEN s.`', v_input_column, '` IS NULL OR TRIM(s.`', v_input_column, '`) = "" 
                               THEN "Empty or null input text"
                               ELSE NULL 
                           END as `', v_details_column, '`
                           FROM `', v_input_db, '`.`', v_input_table, '` s
                           WHERE s.`', v_input_column, '` IS NOT NULL AND TRIM(s.`', v_input_column, '`) != ""');
    END IF;

    SET @sql = v_sql;
    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;

END$$

DELIMITER ;