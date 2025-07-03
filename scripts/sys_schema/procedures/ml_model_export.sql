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

DROP PROCEDURE IF EXISTS ml_model_export;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_model_export (
        IN in_model_handle_name varchar(256),
        IN in_output_table_name VARCHAR(256)
    )
    COMMENT '
Description
-----------

Run the ML_MODEL_EXPORT routine to export an existed trained model.

Parameters
-----------

in_model_handle_name (VARCHAR(64))
   user-defined session variable storing the ML model handle for the duration of the connection.
in_output_table_name VARCHAR(256):
  output table name.
Example
-----------
mysql> SET @iris_model = \'iris_manual\';
mysql> CALL sys.ML_MODEL_EXPORT(@iris_model,\'ML_SCHEMA_user1.model_export\');
...
'
SQL SECURITY INVOKER
NOT DETERMINISTIC
MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name VARCHAR(64);
    DECLARE v_sys_meta_catalog_name VARCHAR(128);
    DECLARE v_schema  VARCHAR(64);
    DECLARE v_table   VARCHAR(64);
    DECLARE v_model_cnt INT;
    DECLARE v_import_obj_check INT;

    -- Step 1: gets current username 
    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;

    -- Step 2: get ml_schema catalog name
    SET v_sys_meta_catalog_name = CONCAT('ML_SCHEMA_', v_user_name, '.MODEL_CATALOG');

    -- Step 3: check existence.
    SET @check_sql = CONCAT(
        'SELECT COUNT(*) INTO @cnt FROM ', v_sys_meta_catalog_name,
        ' WHERE model_handle = ', '\"', in_model_handle_name , '\";'
    );
    PREPARE check_stmt FROM @check_sql;
    EXECUTE check_stmt;
    DEALLOCATE PREPARE check_stmt;

    SELECT @cnt INTO v_model_cnt;
    IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'The model_handle your input does not exist in MODEL_CATALOG.';
    END IF;

    -- Step 4: gets schema and table name of output table
    SELECT SUBSTRING_INDEX(in_output_table_name, '.', 1) INTO v_schema;
    SELECT SUBSTRING_INDEX(in_output_table_name, '.', -1) INTO v_table;

    -- Step 5: check output table existence
    SET @check_sql = CONCAT(
        'SELECT COUNT(*) INTO @cnt FROM INFORMATION_SCHEMA.TABLES', 
        ' WHERE TABLE_SCHEMA = "', v_schema , '\" AND TABLE_NAME = "', v_table, '\";'
    );
    PREPARE check_stmt FROM @check_sql;
    EXECUTE check_stmt;
    DEALLOCATE PREPARE check_stmt;

    SELECT @cnt INTO v_import_obj_check;

    IF v_import_obj_check = 0 THEN
        -- create schema
        SET @sql_create_db = CONCAT('CREATE DATABASE IF NOT EXISTS `', v_schema, '`;');
        PREPARE stmt FROM @sql_create_db;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        -- create table
        SET @sql_create_table = CONCAT(
            'CREATE TABLE IF NOT EXISTS `', v_schema, '`.`', v_table, '` (',
            'chunk_id INT AUTO_INCREMENT PRIMARY KEY,',
            'model_object LONGTEXT DEFAULT NULL,',
            'model_metadata JSON',
            ');'
        );
        PREPARE stmt FROM @sql_create_table;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;

    -- Step 6: insert data.
    SET @insert_sql = CONCAT(
        'INSERT INTO `', v_schema, '`.`', v_table, '` (model_object, model_metadata) ',
        'SELECT model_object, model_metadata FROM ', v_sys_meta_catalog_name,
        ' WHERE model_handle = ', '\"', in_model_handle_name, '\";'
    );
    PREPARE stmt FROM @insert_sql;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
END$$

DELIMITER ;