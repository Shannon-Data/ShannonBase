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
  - truncate: Whether to truncate existing data (true|false, default: false)
  - batch_size: Number of rows to process in each batch (default: 1000)
  - details_column: Name of column to store error details (optional)

Example
-----------
mysql> CALL sys.ML_EMBED_TABLE(
    "mydb.articles.content", 
    "mydb.articles.content_embedding",
    JSON_OBJECT(
        "model_id", "all_minilm_l12_v2",
        "batch_size", 500,
        "truncate", false
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
    DECLARE v_truncate BOOLEAN DEFAULT FALSE;
    DECLARE v_batch_size INT DEFAULT 1000;
    DECLARE v_details_column VARCHAR(64) DEFAULT NULL;
    DECLARE v_input_same_as_output BOOLEAN DEFAULT FALSE;
    DECLARE v_sql TEXT;
    DECLARE v_done INT DEFAULT FALSE;
    DECLARE v_error_msg TEXT DEFAULT '';
    
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1
            v_error_msg = MESSAGE_TEXT;
        RESIGNAL SET MESSAGE_TEXT = v_error_msg;
    END;
    
    -- Parse input table column (DBName.TableName.ColumnName)
    SET v_input_db = SUBSTRING_INDEX(in_input_table_column, '.', 1);
    SET v_input_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_input_table_column, '.', 2), '.', -1);
    SET v_input_column = SUBSTRING_INDEX(in_input_table_column, '.', -1);
    
    -- Parse output table column (DBName.TableName.ColumnName)
    SET v_output_db = SUBSTRING_INDEX(in_output_table_column, '.', 1);
    SET v_output_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_output_table_column, '.', 2), '.', -1);
    SET v_output_column = SUBSTRING_INDEX(in_output_table_column, '.', -1);
    
    -- Check if input and output tables are the same
    IF v_input_db = v_output_db AND v_input_table = v_output_table THEN
        SET v_input_same_as_output = TRUE;
    END IF;
    
    -- Extract options from JSON
    IF JSON_VALID(in_options) THEN
        SET v_model_id = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.model_id'));
        SET v_truncate = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.truncate')), FALSE);
        SET v_batch_size = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.batch_size')), 1000);
        SET v_details_column = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.details_column'));
    END IF;
    
    -- Validate required parameters
    IF v_model_id IS NULL OR v_model_id = '' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'model_id is required in options';
    END IF;
    
    -- Validate input table exists and has primary key
    SET v_sql = CONCAT('SELECT COUNT(*) FROM information_schema.tables 
                       WHERE table_schema = "', v_input_db, '" 
                       AND table_name = "', v_input_table, '"');
    
    -- Create or modify output table
    IF v_input_same_as_output THEN
        -- Add vector column to existing table
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN `', v_output_column, '` VECTOR');
        
        -- Add details column if specified
        IF v_details_column IS NOT NULL THEN
            SET v_sql = CONCAT(v_sql, ', ADD COLUMN `', v_details_column, '` TEXT');
        END IF;
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
    ELSE
        -- Create new output table with primary key from input table
        SET v_sql = CONCAT('CREATE TABLE `', v_output_db, '`.`', v_output_table, '` AS 
                           SELECT * FROM `', v_input_db, '`.`', v_input_table, '` WHERE 1=0');
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        -- Add vector column
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN `', v_output_column, '` VECTOR');
        
        -- Add details column if specified
        IF v_details_column IS NOT NULL THEN
            SET v_sql = CONCAT(v_sql, ', ADD COLUMN `', v_details_column, '` TEXT');
        END IF;
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
    
    -- Process embeddings in batches
    SET v_sql = CONCAT('UPDATE `', v_output_db, '`.`', v_output_table, '` 
                       SET `', v_output_column, '` = sys.ML_EMBED_ROW(`', v_input_column, '`, 
                       JSON_OBJECT("model_id", "', v_model_id, '"))');
    
    -- Add error handling if details column specified
    IF v_details_column IS NOT NULL THEN
        SET v_sql = CONCAT(v_sql, ', `', v_details_column, '` = 
                           CASE 
                               WHEN `', v_input_column, '` IS NULL OR `', v_input_column, '` = "" 
                               THEN "Empty or null input text"
                               ELSE NULL 
                           END');
    END IF;
    
    SET @sql = v_sql;
    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
    
END$$

DELIMITER ;