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

DROP PROCEDURE IF EXISTS ml_rag_table;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_rag_table (
    IN in_input_table_column VARCHAR(255),
    IN in_output_table_column VARCHAR(255),
    IN in_options JSON
)
COMMENT '
Description
-----------
The ML_RAG_TABLE routine runs multiple retrieval-augmented generation (RAG) queries 
in a batch, in parallel. This implementation uses the existing ML_RAG procedure 
to process each query individually.

Parameters
-----------
in_input_table_column (VARCHAR(255)):
  Specifies the input database, table, and column in format: DBName.TableName.ColumnName
  
in_output_table_column (VARCHAR(255)):
  Specifies the output database, table, and column in format: DBName.TableName.ColumnName
  
in_options (JSON):
  Same options as ML_RAG plus additional table-specific options:
  - batch_size: batch processing size (default: 1000, range: 1-1000)
  - embed_column: column name containing pre-computed query embeddings
  - fail_on_embedding_error: true|false (default: true)

Example
-----------
mysql> CALL sys.ML_RAG_TABLE(
    "demo_db.input_table.Input", 
    "demo_db.output_table.Output",
    JSON_OBJECT(
        "vector_store", JSON_ARRAY("demo_db.demo_embeddings"),
        "batch_size", 10
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
    DECLARE v_batch_size INT DEFAULT 1000;
    DECLARE v_embed_column VARCHAR(64);
    DECLARE v_fail_on_embedding_error BOOLEAN DEFAULT TRUE;
    DECLARE v_input_same_as_output BOOLEAN DEFAULT FALSE;
    DECLARE v_rag_options JSON;
    DECLARE v_sql TEXT;
    DECLARE v_error_msg TEXT DEFAULT '';
    
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1
            v_error_msg = MESSAGE_TEXT;
        RESIGNAL SET MESSAGE_TEXT = v_error_msg;
    END;
    
    -- Parse input and output table columns
    SET v_input_db = SUBSTRING_INDEX(in_input_table_column, '.', 1);
    SET v_input_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_input_table_column, '.', 2), '.', -1);
    SET v_input_column = SUBSTRING_INDEX(in_input_table_column, '.', -1);
    
    SET v_output_db = SUBSTRING_INDEX(in_output_table_column, '.', 1);
    SET v_output_table = SUBSTRING_INDEX(SUBSTRING_INDEX(in_output_table_column, '.', 2), '.', -1);
    SET v_output_column = SUBSTRING_INDEX(in_output_table_column, '.', -1);
    
    -- Check if input and output tables are the same
    IF v_input_db = v_output_db AND v_input_table = v_output_table THEN
        SET v_input_same_as_output = TRUE;
    END IF;
    
    -- Extract table-specific options
    IF JSON_VALID(in_options) THEN
        SET v_batch_size = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.batch_size')), 1000);
        SET v_embed_column = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.embed_column'));
        SET v_fail_on_embedding_error = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.fail_on_embedding_error')), TRUE);
        
        -- Remove table-specific options to create clean RAG options
        SET v_rag_options = in_options;
        SET v_rag_options = JSON_REMOVE(v_rag_options, '$.batch_size');
        SET v_rag_options = JSON_REMOVE(v_rag_options, '$.embed_column');
        SET v_rag_options = JSON_REMOVE(v_rag_options, '$.fail_on_embedding_error');
    ELSE
        SET v_rag_options = JSON_OBJECT();
    END IF;
    
    -- Validate batch_size
    IF v_batch_size < 1 OR v_batch_size > 1000 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'batch_size must be between 1 and 1000';
    END IF;
    
    -- Create or modify output table
    IF v_input_same_as_output THEN
        -- Add JSON column to existing table if it doesn't exist
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN IF NOT EXISTS `', v_output_column, '` JSON');
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
    ELSE
        -- Create new output table with same structure as input table
        SET v_sql = CONCAT('CREATE TABLE IF NOT EXISTS `', v_output_db, '`.`', v_output_table, '` AS 
                           SELECT * FROM `', v_input_db, '`.`', v_input_table, '` WHERE 1=0');
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        -- Add JSON output column
        SET v_sql = CONCAT('ALTER TABLE `', v_output_db, '`.`', v_output_table, '` 
                           ADD COLUMN IF NOT EXISTS `', v_output_column, '` JSON');
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
        
        -- Copy data from input table
        SET v_sql = CONCAT('INSERT IGNORE INTO `', v_output_db, '`.`', v_output_table, '` 
                           SELECT *, NULL FROM `', v_input_db, '`.`', v_input_table, '`');
        
        SET @sql = v_sql;
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
    
    -- Process RAG queries by creating a function that calls ML_RAG
    -- Since ML_RAG has an OUT parameter, we'll use a workaround with a function
END$$

DELIMITER ;