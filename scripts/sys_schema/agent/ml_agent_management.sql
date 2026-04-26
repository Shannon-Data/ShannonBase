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

DROP PROCEDURE IF EXISTS sys.shannon_agent_register_plugin;
 
DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost'
PROCEDURE sys.shannon_agent_register_plugin(
    IN  p_plugin_name   VARCHAR(128),   -- Unique plugin name, e.g., 'my_company_agent'
    IN  p_schema_name   VARCHAR(64),    -- Schema where the function resides, e.g., 'mydb'
    IN  p_function_name VARCHAR(64),    -- Function name, e.g., 'shannon_agent'
    IN  p_priority      INT,            -- Priority (lower value = higher priority)
    IN  p_description   TEXT,           -- Plugin description
    OUT p_result        VARCHAR(512)    -- Execution result message
)  COMMENT '
Description
-----------
Registers a JavaScript function as a pluggable agent plugin.
'
    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    -- CHECK EXISTENCE OF FUNCTION
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.ROUTINES
        WHERE ROUTINE_TYPE   = 'FUNCTION'
          AND ROUTINE_SCHEMA = p_schema_name
          AND ROUTINE_NAME   = p_function_name
    ) THEN
        SET p_result = CONCAT('ERROR: function `', p_schema_name, '`.`', p_function_name,
                              '` does not exist, please create it before registering.');
    ELSE
        INSERT INTO mysql.shannon_agent_plugins
            (plugin_name, schema_name, function_name, enabled, priority, description)
        VALUES
            (p_plugin_name, p_schema_name, p_function_name, 1, p_priority, p_description)
        ON DUPLICATE KEY UPDATE
            schema_name   = p_schema_name,
            function_name = p_function_name,
            enabled       = 1,
            priority      = p_priority,
            description   = p_description;
 
        SET p_result = CONCAT('OK: plugin [', p_plugin_name, '] registered → ',
                              p_schema_name, '.', p_function_name,
                              '(priority=', p_priority, ')');
    END IF;
END$$

DELIMITER ; 

DROP PROCEDURE IF EXISTS sys.shannon_agent_unregister_plugin;
 
DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost'
PROCEDURE sys.shannon_agent_unregister_plugin(
    IN  p_plugin_name  VARCHAR(128),
    OUT p_result       VARCHAR(512)
)   SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    DELETE FROM mysql.shannon_agent_plugins WHERE plugin_name = p_plugin_name;
    
    IF ROW_COUNT() > 0 THEN
        SET p_result = CONCAT('OK: plugin [', p_plugin_name, '] unregistered.');
    ELSE
        SET p_result = CONCAT('WARN: plugin [', p_plugin_name, '] does not exist.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = p_result;
    END IF;
END$$

DELIMITER ;


DROP PROCEDURE IF EXISTS sys.shannon_agent_toggle_plugin;
 
DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost'
PROCEDURE sys.shannon_agent_toggle_plugin(
    IN  p_plugin_name  VARCHAR(128),
    IN  p_enabled      TINYINT(1),     -- 1=enable, 0=disable
    OUT p_result       VARCHAR(512)
) SQL SECURITY INVOKER
    NOT DETERMINISTIC
    MODIFIES SQL DATA
BEGIN
    UPDATE mysql.shannon_agent_plugins
    SET    enabled = p_enabled
    WHERE  plugin_name = p_plugin_name;
 
    IF ROW_COUNT() > 0 THEN
        SET p_result = CONCAT('OK: plugin [', p_plugin_name, '] ',
                              IF(p_enabled, 'enabled', 'disabled'), '.');
    ELSE
        SET p_result = CONCAT('WARN: plugin [', p_plugin_name, '] does not exist.');
        SIGNAL SQLSTATE 'HY000'
        SET MESSAGE_TEXT = p_result;
    END IF;
END$$

DELIMITER ;

DROP PROCEDURE IF EXISTS sys.shannon_agent_list_plugins;
 
DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost'
PROCEDURE sys.shannon_agent_list_plugins() 
  SQL SECURITY INVOKER
  NOT DETERMINISTIC
  MODIFIES SQL DATA
BEGIN
    SELECT
        p.plugin_name,
        p.schema_name,
        p.function_name,
        p.enabled,
        p.priority,
        IF(r.ROUTINE_NAME IS NOT NULL, 'YES', 'NO') AS func_exists,
        p.creator,
        p.description,
        p.created_at,
        p.updated_at
    FROM mysql.shannon_agent_plugins p
    LEFT JOIN information_schema.ROUTINES r
           ON r.ROUTINE_TYPE   = 'FUNCTION'
          AND r.ROUTINE_SCHEMA = p.schema_name
          AND r.ROUTINE_NAME   = p.function_name
    ORDER BY p.enabled DESC, p.priority ASC, p.created_at ASC;
END$$

DELIMITER ;
