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
        IN in_model_handle_name VARCHAR(256),
        IN in_output_table_name VARCHAR(256)
    )
    COMMENT '
Description
-----------
Export an existing trained model from the model catalog to a user-defined
table. This can be used for various purposes such as model backup, 
sharing the model with other users or tools, or migrating the model to a different environment.

Parameters
-----------
in_model_handle_name (VARCHAR(256))
  The model handle identifying the model in MODEL_CATALOG.

in_output_table_name (VARCHAR(256))
  Fully-qualified output table name (schema.table).

Example
-----------
mysql> SET @iris_model = "iris_manual";
mysql> CALL sys.ML_MODEL_EXPORT(@iris_model, "ML_SCHEMA_user1.model_export");
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DECLARE v_user_name          VARCHAR(64);
    DECLARE v_catalog_fqn        VARCHAR(256);   -- ML_SCHEMA_<user>.MODEL_CATALOG
    DECLARE v_obj_catalog_fqn    VARCHAR(256);   -- ML_SCHEMA_<user>.MODEL_OBJECT_CATALOG
    DECLARE v_out_schema         VARCHAR(128);
    DECLARE v_out_table          VARCHAR(128);
    DECLARE v_model_cnt          INT DEFAULT 0;
    DECLARE v_chunk_cnt          INT DEFAULT 0;

    -- Step 1: resolve current user and catalog table names
    SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1) INTO v_user_name;

    SET v_catalog_fqn     = CONCAT('`ML_SCHEMA_', v_user_name, '`.`MODEL_CATALOG`');
    SET v_obj_catalog_fqn = CONCAT('`ML_SCHEMA_', v_user_name, '`.`MODEL_OBJECT_CATALOG`');

    -- Step 2: verify the model handle exists in MODEL_CATALOG
    SET @sql_chk_model = CONCAT(
        'SELECT COUNT(*) INTO @v_model_cnt FROM ', v_catalog_fqn,
        ' WHERE model_handle = ?'
    );
    PREPARE _stmt FROM @sql_chk_model;
    SET @_handle = in_model_handle_name;
    EXECUTE _stmt USING @_handle;
    DEALLOCATE PREPARE _stmt;

    SET v_model_cnt = @v_model_cnt;
    IF v_model_cnt = 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'model_handle does not exist in MODEL_CATALOG.';
    END IF;

    -- Step 3: count chunks in MODEL_OBJECT_CATALOG (0 = old/small model)
    SET @sql_chk_obj = CONCAT(
        'SELECT COUNT(*) INTO @v_chunk_cnt FROM ', v_obj_catalog_fqn,
        ' WHERE model_handle = ?'
    );
    PREPARE _stmt FROM @sql_chk_obj;
    EXECUTE _stmt USING @_handle;
    DEALLOCATE PREPARE _stmt;

    SET v_chunk_cnt = @v_chunk_cnt;

    -- Step 4: parse and prepare output schema / table
    SELECT SUBSTRING_INDEX(in_output_table_name, '.', 1)  INTO v_out_schema;
    SELECT SUBSTRING_INDEX(in_output_table_name, '.', -1) INTO v_out_table;

    -- Create schema if absent
    SET @sql_create_db = CONCAT('CREATE DATABASE IF NOT EXISTS `', v_out_schema, '`;');
    PREPARE _stmt FROM @sql_create_db;
    EXECUTE _stmt;
    DEALLOCATE PREPARE _stmt;

    -- Create output table if absent.
    --   chunk_id       INT AUTO_INCREMENT PRIMARY KEY
    --   model_object   LONGTEXT DEFAULT NULL
    --   model_metadata JSON DEFAULT NULL
    SET @sql_create_tbl = CONCAT(
        'CREATE TABLE IF NOT EXISTS `', v_out_schema, '`.`', v_out_table, '` (',
            '`chunk_id`       INT          NOT NULL AUTO_INCREMENT,',
            '`model_object`   LONGTEXT     DEFAULT NULL,',
            '`model_metadata` JSON         DEFAULT NULL,',
            'PRIMARY KEY (`chunk_id`)',
        ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;'
    );
    PREPARE _stmt FROM @sql_create_tbl;
    EXECUTE _stmt;
    DEALLOCATE PREPARE _stmt;

    IF v_chunk_cnt = 0 THEN

        SET @sql_insert = CONCAT(
            'INSERT INTO `', v_out_schema, '`.`', v_out_table,
            '` (chunk_id, model_object, model_metadata) ',
            'SELECT 0, NULL, model_metadata ',
            'FROM ', v_catalog_fqn,
            ' WHERE model_handle = ?;'
        );
        PREPARE _stmt FROM @sql_insert;
        EXECUTE _stmt USING @_handle;
        DEALLOCATE PREPARE _stmt;
    ELSE

        SET @sql_insert = CONCAT(
            'INSERT INTO `', v_out_schema, '`.`', v_out_table,
            '` (chunk_id, model_object, model_metadata) ',
            'SELECT ',
            '    oc.chunk_id, ',
            '    oc.model_object, ',
            '    IF(oc.chunk_id = 1, mc.model_metadata, NULL) ',
            'FROM ', v_obj_catalog_fqn, ' AS oc ',
            'JOIN ', v_catalog_fqn,     ' AS mc ',
            '    ON mc.model_handle = oc.model_handle ',
            'WHERE oc.model_handle = ? ',
            'ORDER BY oc.chunk_id;'
        );
        PREPARE _stmt FROM @sql_insert;
        EXECUTE _stmt USING @_handle;
        DEALLOCATE PREPARE _stmt;

    END IF;
END$$

DELIMITER ;
