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

DROP PROCEDURE IF EXISTS ml_predict_table_create;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_predict_table_create (
        IN in_sch_tb_name VARCHAR(64),
        IN in_model_handle_name VARCHAR(64),
        IN in_output_table_name VARCHAR(255)
    )
    COMMENT 'ONLY USED IN FUNCTION ML_PREDICT_TABLE'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    -- to check whether output_table is exists or not. exists raise err.
    IF in_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
    END IF;
  
    SET schema_name = SUBSTRING_INDEX(in_output_table_name, '.', 1);
    SET table_name = SUBSTRING_INDEX(in_output_table_name, '.', -1);
    SELECT COUNT(*) INTO table_count 
    FROM INFORMATION_SCHEMA.TABLES 
    WHERE TABLE_SCHEMA = schema_name AND TABLE_NAME = table_name;
    IF table_count > 0 THEN
        SET v_db_err_msg = CONCAT(in_output_table_name, 'already exists.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_model_user_name;
    SET v_model_sch_name = CONCAT('ML_SCHEMA_', v_model_user_name);

    SELECT TARGET_COLUMN_NAME INTO @model_target_col_name
    FROM v_model_sch_name
    WHERE MODEL_HANDLE = in_model_handle_name;

    SELECT COLUMN_TYPE INTO @col_type
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = in_schema  AND TABLE_NAME = in_table AND COLUMN_NAME = @model_target_col_name LIMIT 1; 

    CREATE TABLE in_output_table_name LIKE in_sch_table_name;
    ALTER TABLE in_output_table_name 
      ADD COLUMN CONCAT('_', SUBSTRING(MD5(RAND()), 1, 10), '_pk_id') INT NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST,
      ADD COLUMN 'prediction' @col_type,
      ADD 'ML_RESULTS' JSON;

   SELECT ML_MODEL_LOAD(in_model_handle_name, in_user_name) INTO v_load_obj_check;
   IF v_load_obj_check != 0 THEN
        SET v_db_err_msg = CONCAT('ML_MODEL_LOAD failed.');
        SIGNAL SQLSTATE 'HY000'
          SET MESSAGE_TEXT = v_db_err_msg;
   END IF;

END$$
DELIMITER ;
