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

DROP PROCEDURE IF EXISTS ml_model_active;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_model_active (
        IN  in_user_name   VARCHAR(64),
        OUT out_model_info JSON
    )
    COMMENT '
Description
-----------
Reports which ML models are currently loaded and active in ShannonBase memory.
Introduced in MySQL 9.0.0.

Parameters
-----------
in_user_name VARCHAR(64):
  Scope of the query. Accepted values:
    current  - models owned by the invoking user (default).
    NULL     - equivalent to current.
    all      - models for every user (requires SUPER or ML_ADMIN privilege).

out_model_info JSON:
  Output session variable. Receives a JSON array of two objects:
    current/NULL mode:
      [
        {"total model size(bytes)": <N>},
        {"<model_handle>": <model_metadata>, ...}
      ]
    all mode:
      [
        {"total model size(bytes)": <N>},
        {"<username>": [{"<model_handle>": <model_metadata>}, ...], ...}
      ]

Example
-----------
mysql> CALL sys.ML_MODEL_ACTIVE(\'current\', @model_info);
mysql> SELECT JSON_PRETTY(@model_info);
    '
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    READS SQL DATA
BEGIN
    DECLARE v_db_err_msg      TEXT;
    DECLARE v_active_ret      INT;
    DECLARE v_effective_user  VARCHAR(64);
    DECLARE v_sys_schema_name VARCHAR(64);
    DECLARE v_schema_count    INT DEFAULT 0;

    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_db_err_msg = MESSAGE_TEXT;
        SET out_model_info = NULL;
        SIGNAL SQLSTATE 'HY000' SET MESSAGE_TEXT = v_db_err_msg;
    END;

    -- Normalise: NULL -> 'current'; validate accepted values
    SET v_effective_user = LOWER(IFNULL(in_user_name, 'current'));

    IF v_effective_user NOT IN ('current', 'all') THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Invalid value for user: expected current, all, or NULL.';
    END IF;

    -- For 'current', verify the invoking user's ML_SCHEMA exists
    IF v_effective_user = 'current' THEN
        SET v_sys_schema_name = CONCAT(
            'ML_SCHEMA_',
            SUBSTRING_INDEX(CURRENT_USER(), '@', 1)
        );

        SELECT COUNT(*) INTO v_schema_count
        FROM   information_schema.SCHEMATA
        WHERE  SCHEMA_NAME = v_sys_schema_name;

        IF v_schema_count = 0 THEN
            SET v_db_err_msg = CONCAT(
                'No ML schema found for current user (expected: ',
                v_sys_schema_name, '). Has any model been trained?'
            );
            SIGNAL SQLSTATE 'HY000' SET MESSAGE_TEXT = v_db_err_msg;
        END IF;
    END IF;

    SELECT ML_MODEL_ACTIVE(v_effective_user) into out_model_info;

END$$

DELIMITER ;
