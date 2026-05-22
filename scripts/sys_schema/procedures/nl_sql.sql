-- Copyright (c) 2014, 2026, Oracle and/or its affiliates.
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
-- Copyright (c) 2026, Shannon Data AI and/or its affiliates.

DROP PROCEDURE IF EXISTS nl_sql;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE nl_sql(
    IN  in_query_text TEXT,
    OUT out_output    JSON,
    IN  in_options    JSON
)
COMMENT '
Description
-----------
Converts a natural-language query into a SQL SELECT statement and optionally
executes it. The procedure returns metadata about the generated SQL together
with the generated statement, validation status, and the tables/schemas that
were considered.

The procedure supports the following options:
  - execute         : true|false — whether the generated SQL should be executed.
                      Default: true.
  - schemas         : JSON_ARRAY of database names to consider (max 5).
    - tables          : JSON_ARRAY of JSON_OBJECT(''schema_name'',''DBName'',
                                            ''table_name'',''TableName'') to consider (max 50).
                      The schemas and tables options are mutually exclusive.
  - model_id        : Large language model identifier. Default:
                      meta.llama-3.3-70b-instruct.
  - verbose         : 0|1|2 — 0 suppresses generated-SQL output, 1 prints the
                      generated SQL, and 2 prints debugging details.
                      Default: 1.
  - include_comments: true|false — include table/column comments in schema
                      metadata supplied to the LLM. Default: true.
  - use_retry       : true|false — retry SQL generation when generated SQL is
                      syntactically invalid. Default: true.

This routine generates SELECT statements only. It validates the generated SQL
with PREPARE and retries up to three times for syntactically invalid output.
'
SQL SECURITY INVOKER
READS SQL DATA
BEGIN
    DECLARE v_execute          BOOLEAN DEFAULT TRUE;
    DECLARE v_schemas          JSON    DEFAULT NULL;
    DECLARE v_tables           JSON    DEFAULT NULL;
    DECLARE v_model_id         VARCHAR(255) DEFAULT 'llama-3.3-70b-instruct';
    DECLARE v_verbose          INT     DEFAULT 1;
    DECLARE v_include_comments BOOLEAN DEFAULT TRUE;
    DECLARE v_use_retry        BOOLEAN DEFAULT TRUE;

    DECLARE v_attempt          INT     DEFAULT 0;
    DECLARE v_max_attempts     INT     DEFAULT 3;
    DECLARE v_sql_query        TEXT    DEFAULT '';
    DECLARE v_generated_text   TEXT    DEFAULT '';
    DECLARE v_schema_metadata  TEXT    DEFAULT '';
    DECLARE v_license          TEXT    DEFAULT '';
    DECLARE v_table_list       JSON    DEFAULT JSON_ARRAY();
    DECLARE v_schema_list      JSON    DEFAULT JSON_ARRAY();
    DECLARE v_stmt_error       TEXT    DEFAULT '';
    DECLARE v_is_sql_valid     BOOLEAN DEFAULT FALSE;
    DECLARE v_base_prompt      TEXT    DEFAULT '';
    DECLARE v_query_prompt     TEXT    DEFAULT '';
    DECLARE v_sql_trim         TEXT    DEFAULT '';
    DECLARE v_first_token      VARCHAR(16) DEFAULT '';
    DECLARE v_pos_select       INT     DEFAULT 0;
    DECLARE v_pos_with         INT     DEFAULT 0;
    DECLARE v_start_pos        INT     DEFAULT 0;
    DECLARE v_table_count      INT     DEFAULT 0;
    DECLARE v_schema_count     INT     DEFAULT 0;
    DECLARE v_schema_name      VARCHAR(64);
    DECLARE v_table_name       VARCHAR(64);
    DECLARE v_idx              INT     DEFAULT 0;
    DECLARE v_error_msg        VARCHAR(256) DEFAULT '';
    DECLARE v_prep_errno   INT     DEFAULT 0;
    DECLARE v_retry_hint   TEXT    DEFAULT '';
    DECLARE v_has_bare_table_ref BOOLEAN DEFAULT FALSE;

    IF in_query_text IS NULL OR TRIM(in_query_text) = '' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'in_query_text cannot be NULL or empty';
    END IF;

    IF in_options IS NOT NULL AND JSON_VALID(in_options) THEN
        IF JSON_EXTRACT(in_options, '$.execute') IS NOT NULL THEN
            SET v_execute = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.execute'))
                            IN ('true', '1', TRUE);
        END IF;
        SET v_schemas = JSON_EXTRACT(in_options, '$.schemas');
        SET v_tables  = JSON_EXTRACT(in_options, '$.tables');
        IF JSON_EXTRACT(in_options, '$.model_id') IS NOT NULL THEN
            SET v_model_id = JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.model_id'));
        END IF;
        IF JSON_EXTRACT(in_options, '$.verbose') IS NOT NULL THEN
            SET v_verbose = CAST(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.verbose')) AS SIGNED);
        END IF;
        IF JSON_EXTRACT(in_options, '$.include_comments') IS NOT NULL THEN
            SET v_include_comments =
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.include_comments'))
                IN ('true', '1', TRUE);
        END IF;
        IF JSON_EXTRACT(in_options, '$.use_retry') IS NOT NULL THEN
            SET v_use_retry =
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.use_retry'))
                IN ('true', '1', TRUE);
        END IF;
    END IF;

    IF v_schemas IS NOT NULL AND v_tables IS NOT NULL THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Options "schemas" and "tables" are mutually exclusive';
    END IF;
    IF v_verbose NOT IN (0, 1, 2) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'verbose must be 0, 1, or 2';
    END IF;
    IF v_schemas IS NOT NULL AND JSON_LENGTH(v_schemas) > 5 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'schemas array exceeds the limit of 5 entries';
    END IF;
    IF v_tables IS NOT NULL AND JSON_LENGTH(v_tables) > 50 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'tables array exceeds the limit of 50 entries';
    END IF;

    DROP TEMPORARY TABLE IF EXISTS tmp_nl_sql_tables;
    CREATE TEMPORARY TABLE tmp_nl_sql_tables (
        schema_name VARCHAR(64) COLLATE utf8mb4_0900_ai_ci NOT NULL,
        table_name  VARCHAR(64) COLLATE utf8mb4_0900_ai_ci NOT NULL,
        PRIMARY KEY (schema_name, table_name)
    ) ENGINE=MEMORY;

    IF v_tables IS NOT NULL AND JSON_LENGTH(v_tables) > 0 THEN
        SET v_table_count = JSON_LENGTH(v_tables);
        SET v_idx = 0;
        WHILE v_idx < v_table_count DO
            SET v_schema_name = JSON_UNQUOTE(
                JSON_EXTRACT(v_tables, CONCAT('$[', v_idx, '].schema_name')));
            SET v_table_name  = JSON_UNQUOTE(
                JSON_EXTRACT(v_tables, CONCAT('$[', v_idx, '].table_name')));
            IF v_schema_name IS NULL OR v_table_name IS NULL THEN
                SIGNAL SQLSTATE '45000'
                    SET MESSAGE_TEXT =
                        'tables entries must include schema_name and table_name';
            END IF;
            IF NOT EXISTS (
                SELECT 1 FROM information_schema.TABLES
                WHERE  TABLE_SCHEMA = v_schema_name
                  AND  TABLE_NAME   = v_table_name
                  AND  TABLE_TYPE   = 'BASE TABLE'
            ) THEN
                SET v_error_msg = CONCAT(
                    'Table not found or not a BASE TABLE: ',
                    v_schema_name, '.', v_table_name);
                SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = v_error_msg;
            END IF;
            INSERT IGNORE INTO tmp_nl_sql_tables VALUES (v_schema_name, v_table_name);
            SET v_idx = v_idx + 1;
        END WHILE;
    ELSEIF v_schemas IS NOT NULL AND JSON_LENGTH(v_schemas) > 0 THEN
        SET v_schema_count = JSON_LENGTH(v_schemas);
        SET v_idx = 0;
        WHILE v_idx < v_schema_count DO
            SET v_schema_name = JSON_UNQUOTE(
                JSON_EXTRACT(v_schemas, CONCAT('$[', v_idx, ']')));
            INSERT IGNORE INTO tmp_nl_sql_tables
            SELECT TABLE_SCHEMA, TABLE_NAME
            FROM   information_schema.TABLES
            WHERE  TABLE_SCHEMA = v_schema_name
              AND  TABLE_TYPE   = 'BASE TABLE';
            SET v_idx = v_idx + 1;
        END WHILE;
    ELSE
        IF DATABASE() IS NOT NULL AND DATABASE() != '' THEN
            INSERT IGNORE INTO tmp_nl_sql_tables
            SELECT TABLE_SCHEMA, TABLE_NAME
            FROM   information_schema.TABLES
            WHERE  TABLE_SCHEMA = DATABASE()
              AND  TABLE_TYPE   = 'BASE TABLE';
        END IF;
    END IF;

    IF (SELECT COUNT(*) FROM tmp_nl_sql_tables) = 0 THEN
        IF v_verbose >= 1 THEN
            SELECT CONCAT(
                'WARNING: No BASE TABLEs found for the specified schemas/tables. ',
                'Check privileges on information_schema.TABLES.'
            ) AS nl_sql_warning;
        END IF;
    END IF;

    SELECT COALESCE(JSON_ARRAYAGG(tbl_dot), JSON_ARRAY())
    INTO v_table_list
    FROM (
        SELECT DISTINCT CONCAT(schema_name, '.', table_name) AS tbl_dot
        FROM tmp_nl_sql_tables
        ORDER BY tbl_dot
    ) t;

    SELECT COALESCE(JSON_ARRAYAGG(schema_name), JSON_ARRAY())
    INTO v_schema_list
    FROM (
        SELECT DISTINCT schema_name
        FROM tmp_nl_sql_tables
        ORDER BY schema_name
    ) s;

    IF v_schema_list IS NULL OR JSON_LENGTH(v_schema_list) = 0 THEN
        SET v_schema_list = COALESCE(v_schemas, JSON_ARRAY());
    END IF;

    SELECT COALESCE(
        GROUP_CONCAT(
            CONCAT(
                'TABLE ', tbl.TABLE_SCHEMA, '.', tbl.TABLE_NAME,
                IF(v_include_comments
                   AND COALESCE(tbl.TABLE_COMMENT, '') != '',
                   CONCAT(' /* ',
                          REPLACE(tbl.TABLE_COMMENT, '*/', '*\\/'),
                          ' */'),
                   ''),
                ' (', col_meta.col_def, ')'
            )
            ORDER BY tbl.TABLE_SCHEMA, tbl.TABLE_NAME
            SEPARATOR '\n\n'
        ),
        ''
    ) INTO v_schema_metadata
    FROM information_schema.TABLES AS tbl
    INNER JOIN (
        SELECT
            c.TABLE_SCHEMA,
            c.TABLE_NAME,
            GROUP_CONCAT(
                CONCAT(
                    c.COLUMN_NAME, ' ', c.COLUMN_TYPE,
                    IF(v_include_comments
                       AND COALESCE(c.COLUMN_COMMENT, '') != '',
                       CONCAT(' /* ',
                              REPLACE(c.COLUMN_COMMENT, '*/', '*\\/'),
                              ' */'),
                       '')
                )
                ORDER BY c.ORDINAL_POSITION
                SEPARATOR ', '
            ) AS col_def
        FROM information_schema.COLUMNS c
        INNER JOIN tmp_nl_sql_tables t
            ON  c.TABLE_SCHEMA = t.schema_name
            AND c.TABLE_NAME   = t.table_name
        GROUP BY c.TABLE_SCHEMA, c.TABLE_NAME
    ) AS col_meta
        ON  tbl.TABLE_SCHEMA = col_meta.TABLE_SCHEMA
        AND tbl.TABLE_NAME   = col_meta.TABLE_NAME;

    IF v_schema_metadata = '' THEN
        SET v_schema_metadata =
            'No schema metadata available for requested tables/schemas';
    END IF;

    IF LOWER(v_model_id) LIKE '%llama%' THEN
        SET v_license =
            'Your use of the Llama model is subject to the Llama Community License.';
    ELSE
        SET v_license = CONCAT(
            'Your use of the model ', v_model_id,
            ' is subject to its provider license.');
    END IF;

    SET v_base_prompt = CONCAT(
        'Generate ONLY one valid MySQL SELECT statement. ',
        'Do not output markdown, comments, explanation, or code fences. ',

        'STRICT RULES: ',
        '1. EVERY table in FROM/JOIN MUST use fully qualified schema.table format. ',
        '2. NEVER use bare table names. ',
        '3. NEVER omit schema names. ',
        '4. Use ONLY tables and columns from the schema metadata below. ',
        '5. Output MUST begin with SELECT or WITH. ',

        'Examples: ',
        'WRONG: SELECT * FROM example_table; ',
        'CORRECT: SELECT * FROM example_schema.example_table ',

        'If the request asks for count/total/how many, use COUNT(*). ',
        'If the request asks list/show/query/details/info, return detailed rows. ',

        '\n\n=== SCHEMA METADATA ===\n',
        v_schema_metadata,

        '\n\n=== NATURAL LANGUAGE QUERY ===\n',
        TRIM(in_query_text),

        '\n\nReturn ONLY the SQL statement.'
    );

    IF v_verbose = 2 THEN
        SELECT v_base_prompt AS debug_prompt;
    END IF;

    SET v_attempt      = 0;
    SET v_is_sql_valid = FALSE;
    SET v_query_prompt = v_base_prompt;

    read_loop: LOOP
        SET v_attempt = v_attempt + 1;

        SELECT sys.ML_GENERATE(
            v_query_prompt,
            JSON_OBJECT(
                'task',        'generation',
                'model_id',    v_model_id,
                'temperature', IF(v_attempt = 1, 0.0, 0.3),
                'max_tokens',  1024
            )
        ) INTO v_generated_text;

        IF v_generated_text IS NULL OR TRIM(v_generated_text) = '' THEN
            SET v_is_sql_valid = FALSE;
            SET v_stmt_error   = 'ML_GENERATE returned NULL or empty response';
            IF v_verbose = 2 THEN
                SELECT v_stmt_error AS ml_generate_error, v_attempt AS attempt;
            END IF;
            IF v_attempt >= v_max_attempts OR NOT v_use_retry THEN
                LEAVE read_loop;
            END IF;
            IF v_prep_errno = 1064 THEN
                SET v_retry_hint =
                    '\nThe SQL has a syntax error. Fix it and return only the corrected SELECT statement.';
            ELSEIF v_prep_errno IN (1146, 1054, 1052) THEN
                SET v_retry_hint = CONCAT(
                    '\nThe previous SQL is INVALID.\n',
                    'You MUST use fully qualified schema.table names.\n',
                    'Every table in FROM or JOIN must include schema name.\n',
                    'Do NOT use bare table names.\n',
                    'Return ONLY corrected SQL.\n',
                    'Example:\n',
                    'WRONG : SELECT * FROM example_table\n',
                    'CORRECT: SELECT * FROM example_schema.example_table'
                );
            ELSE
                SET v_retry_hint =
                    '\nFix the error and return only the corrected SQL SELECT statement.';
            END IF;

            SET v_query_prompt = CONCAT(
                v_base_prompt,
                '\n\nPrevious attempt (', v_attempt, ') failed:\n',
                'SQL   : ', COALESCE(v_sql_trim, '(empty)'), '\n',
                'Error : ', v_stmt_error,
                v_retry_hint
            );
            ITERATE read_loop;
        END IF;

        SET v_sql_query = TRIM(v_generated_text);

        IF v_sql_query LIKE '```%' THEN
            SET v_sql_query = REGEXP_REPLACE(
                v_sql_query, '^```[a-zA-Z]*[[:space:]]*', '');
            SET v_sql_query = REGEXP_REPLACE(
                v_sql_query, '[[:space:]]*```[[:space:]]*$', '');
            SET v_sql_query = TRIM(v_sql_query);
        END IF;

        SET v_pos_select = LOCATE('SELECT', UPPER(v_sql_query));
        SET v_pos_with   = LOCATE('WITH',   UPPER(v_sql_query));
        IF v_pos_select = 0 THEN SET v_pos_select = 2147483647; END IF;
        IF v_pos_with   = 0 THEN SET v_pos_with   = 2147483647; END IF;
        SET v_start_pos = LEAST(v_pos_select, v_pos_with);
        IF v_start_pos < 2147483647 THEN
            SET v_sql_query = TRIM(SUBSTRING(v_sql_query, v_start_pos));
        END IF;

        SET v_sql_query   = TRIM(TRAILING ';' FROM TRIM(v_sql_query));
        SET v_sql_trim    = v_sql_query;
        SET v_first_token = UPPER(SUBSTRING_INDEX(v_sql_trim, ' ', 1));

        IF v_sql_trim = '' OR v_first_token NOT IN ('SELECT', 'WITH') THEN
            SET v_is_sql_valid = FALSE;
            SET v_stmt_error   = 'Generated text does not start with SELECT or WITH';
        ELSE
            SET v_is_sql_valid = TRUE;
            SET v_stmt_error   = '';
            SET v_prep_errno   = 0;

            BEGIN
                DECLARE v_prep_failed BOOLEAN DEFAULT FALSE;
                DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
                BEGIN
                    GET DIAGNOSTICS CONDITION 1
                        v_stmt_error = MESSAGE_TEXT,
                        v_prep_errno = MYSQL_ERRNO;
                    SET v_prep_failed  = TRUE;
                    SET v_is_sql_valid = FALSE;
                END;

                SET @_nl_sql_stmt = v_sql_trim;
                PREPARE _nl_sql_prep FROM @_nl_sql_stmt;
                IF NOT v_prep_failed THEN
                    DEALLOCATE PREPARE _nl_sql_prep;
                END IF;
            END;
            IF v_is_sql_valid THEN
                SELECT EXISTS (
                    SELECT 1
                    FROM tmp_nl_sql_tables
                    WHERE REGEXP_LIKE(
                        v_sql_trim,
                        CONCAT(
                            '(^|[[:space:](,])',
                            '(FROM|JOIN)[[:space:]]+`?',
                            table_name,
                            '`?([[:space:],);]|$)'
                        ),
                        'i'
                    )
                )
                INTO v_has_bare_table_ref;

                IF v_has_bare_table_ref THEN
                    SET v_is_sql_valid = FALSE;
                    SET v_stmt_error =
                        'Non fully-qualified table name detected. Every table in FROM/JOIN must use schema.table format.';
                    SET v_prep_errno = 1054;
                END IF;
            END IF;            
        END IF;

        IF v_is_sql_valid THEN
            LEAVE read_loop;
        END IF;

        IF v_attempt >= v_max_attempts OR NOT v_use_retry THEN
            LEAVE read_loop;
        END IF;

        SET v_query_prompt = CONCAT(
            v_base_prompt,
            '\n\nPrevious attempt (', v_attempt,
            ') generated invalid SQL:\n',
            'SQL   : ', COALESCE(v_sql_trim, '(empty)'), '\n',
            'Error : ', v_stmt_error, '\n\n',
            'Fix the error and return only the corrected SQL SELECT statement. ',
            'Every table in FROM/JOIN MUST use schema.table format. ',
            'Do NOT use bare table names.'
        );

        IF v_verbose = 2 THEN
            SELECT v_query_prompt AS debug_retry_prompt,
                   v_attempt      AS current_attempt;
        END IF;

    END LOOP;

    SET out_output = JSON_OBJECT(
        'tables',       COALESCE(v_table_list,  JSON_ARRAY()),
        'schemas',      COALESCE(v_schema_list, JSON_ARRAY()),
        'model_id',     v_model_id,
        'sql_query',    v_sql_query,
        'is_sql_valid', IF(v_is_sql_valid, 1, 0),
        'license',      v_license
    );

    IF v_verbose >= 1 AND v_sql_query != '' THEN
        SELECT v_sql_query AS 'Generated SQL statement';
    END IF;
    IF v_verbose = 2 AND NOT v_is_sql_valid AND v_stmt_error != '' THEN
        SELECT v_stmt_error AS validation_error;
    END IF;

    IF v_is_sql_valid AND v_execute AND v_sql_query != '' THEN
        SET @_nl_sql_exec = v_sql_query;
        PREPARE _nl_sql_exec_stmt FROM @_nl_sql_exec;
        EXECUTE _nl_sql_exec_stmt;
        DEALLOCATE PREPARE _nl_sql_exec_stmt;
    END IF;

    DROP TEMPORARY TABLE IF EXISTS tmp_nl_sql_tables;
END$$

DELIMITER ;