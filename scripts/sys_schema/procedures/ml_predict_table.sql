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
        in_sch_tb_name varchar(256),
        in_model_handle_name VARCHAR(256),
        in_output_table_name VARCHAR(256),
        in_model_option JSON
    )
    COMMENT '
ML_PREDICT_TABLE generates predictions for an entire table of unlabeled data and saves the results to an output table.
Parameters
-----------

in_sch_tb_name varchar(256):
  Specifies the fully qualified name of the input table (schema_name.table_name). The input table should contain the same feature columns as the training dataset but no target column.
in_model_handle_name (VARCHAR(256)):
  Specifies the model handle or a session variable containing the model handle.
in_output_table_name VARCHAR(256):
  Specifies the table where predictions are stored. The table is created if it does not exist. A fully qualified table name must be specified (schema_name.table_name). If the table already exists, an error is returned.
in_model_option JSON:
  A set of options in JSON format.
Example
-----------
mysql> CALL sys.ML_PREDICT_TABLE(JSON_OBJECT(\'in_sch_tb_name\', \'in_model_handle_name\',
          \'in_output_table_name\', in_model_option);
    '
    SQL SECURITY INVOKER
    DETERMINISTIC
    CONTAINS SQL
BEGIN
    DECLARE v_db_err_msg TEXT;
    DECLARE v_pred_table_obj_check JSON;
    DECLARE v_in_schema_name VARCHAR(64);
    DECLARE v_in_table_name VARCHAR(64);
    DECLARE v_out_schema_name VARCHAR(64);
    DECLARE v_out_table_name VARCHAR(64);

    DECLARE v_table_count INT;
    DECLARE v_model_user_name VARCHAR(64);
    DECLARE v_model_sch_name VARCHAR(64);
    DECLARE v_model_target_col_name VARCHAR(255);

    SET @pk_col = '_4aad19ca6e_pk_id';

    -- CHECK input sch_tb_name format
    IF in_sch_tb_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
    END IF;

    -- CHECK OUTPUT TABLE NAME FORMAT
    IF in_output_table_name NOT REGEXP '^[^.]+\.[^.]+$' THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'Invalid schema.table format, please using fully qualified name of the table.';
    END IF;

    SELECT SUBSTRING_INDEX(in_sch_tb_name, '.', 1) INTO v_in_schema_name;
    SELECT SUBSTRING_INDEX(in_sch_tb_name, '.', -1) INTO v_in_table_name;
    
    SELECT COUNT(*) INTO v_table_count
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_in_schema_name AND TABLE_NAME = v_in_table_name;
    IF v_table_count = 0 THEN
        SET v_db_err_msg = CONCAT(in_sch_tb_name, ' you specified as input does not exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    SELECT SUBSTRING_INDEX(in_output_table_name, '.', 1) INTO v_out_schema_name;
    SELECT SUBSTRING_INDEX(in_output_table_name, '.', -1) INTO v_out_table_name;
    
    SELECT COUNT(*) INTO v_table_count
    FROM INFORMATION_SCHEMA.TABLES
    WHERE TABLE_SCHEMA = v_out_schema_name AND TABLE_NAME = v_out_table_name;
    IF v_table_count != 0 THEN
        SET v_db_err_msg = CONCAT(in_output_table_name, ' you specified as input already exists.');
        SIGNAL SQLSTATE 'HY000'
            SET MESSAGE_TEXT = v_db_err_msg;
    END IF;

    SELECT COUNT(*) INTO @has_pk FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = v_in_schema_name
    AND TABLE_NAME = v_in_table_name
    AND CONSTRAINT_TYPE = 'PRIMARY KEY';

    SELECT COUNT(*) INTO @conflict  FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = @schema_name
    AND TABLE_NAME = @table_name
    AND COLUMN_NAME = @pk_col
    AND (COLUMN_KEY != 'PRI' OR COLUMN_KEY IS NULL);

    IF @conflict > 0 THEN
      SIGNAL SQLSTATE '45000' 
      SET MESSAGE_TEXT = 'Conflict: Column name already exists and is not a primary key.';
    END IF;

    SET @sql = CONCAT(
      'CREATE TABLE ', v_out_schema_name, '.', v_out_table_name, ' LIKE ', v_in_schema_name, '.', v_in_table_name, ';'
    );
    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT(
      'ALTER TABLE ', v_out_schema_name, '.', v_out_table_name, ' secondary_engine NULL;'
    );
    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;

    -- add the primary key if it do not exists.
    IF @has_pk = 0 THEN
     SET @sql = CONCAT(
       'ALTER TABLE ', v_out_schema_name, '.', v_out_table_name, 
       ' ADD COLUMN `', @pk_col, '` INT AUTO_INCREMENT PRIMARY KEY;'
      );
      PREPARE stmt FROM @sql;
      EXECUTE stmt;
      DEALLOCATE PREPARE stmt;
    END IF;

    -- add the new column.
    SET @sql = CONCAT(
      'ALTER TABLE ', v_out_schema_name, '.', v_out_table_name,
      ' ADD COLUMN prediction VARCHAR(256),',
      ' ADD COLUMN ml_results JSON;'
    );
    PREPARE stmt FROM @sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;

    SELECT ML_MODEL_PREDICT_TABLE(in_sch_tb_name, in_model_handle_name, in_output_table_name, in_model_option) INTO v_pred_table_obj_check;
        IF v_pred_table_obj_check != 0 THEN
        SET @sql = CONCAT('DROP TABLE ', v_out_schema_name, '.', v_out_table_name, ';');
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET v_db_err_msg = CONCAT('ML_MODEL_PREDICT_TABLE failed.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = v_db_err_msg;
    END IF;
END$$
DELIMITER ;
