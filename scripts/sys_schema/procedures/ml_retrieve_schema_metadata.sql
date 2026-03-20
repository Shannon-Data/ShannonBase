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

DROP PROCEDURE IF EXISTS ml_retrieve_schema_metadata;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_retrieve_schema_metadata (
    IN  in_query_text  TEXT,
    OUT out_output     TEXT,
    IN  in_options     JSON
)
COMMENT '
Description
-----------
Retrieves the most relevant tables to a given natural-language statement
and ranks them in the order of their relevance.

The output can be used to enhance LLM prompts, NL_SQL workflows, and
retrieval-augmented generation (RAG)-based analytics.

Parameters
-----------
in_query_text (TEXT):
  The natural-language statement used to find relevant tables.

out_output (TEXT):
  A concise set of abridged CREATE TABLE statements for the tables that
  are most relevant to the query, ranked in order of relevance.

in_options (JSON):
  Optional parameters as key-value pairs:
  - schemas          : JSON array of database names to consider (max 128).
                       Default: all available databases.
  - tables           : JSON array of JSON_OBJECT(''schema_name'',''db'',
                       ''table_name'',''tbl'') to consider (max 128).
                       Default: all tables.
                       NOTE: schemas and tables are mutually exclusive.
  - include_comments : true|false — include table/column comments in both
                       similarity search and output. Default: true.
  - n_results        : number of tables to return (default: 10, range: 1-128).
  - embed_model_id   : embedding model to use.
                       Default: all-MiniLM-L12-v2.

Example
-----------
mysql> CALL sys.ML_RETRIEVE_SCHEMA_METADATA(
         "How many flights are there in total?", @output, NULL);
mysql> SELECT @output;
'
SQL SECURITY INVOKER
READS SQL DATA
BEGIN
    -- Variable declarations
    DECLARE v_schemas          JSON;
    DECLARE v_tables           JSON;
    DECLARE v_include_comments BOOLEAN  DEFAULT TRUE;
    DECLARE v_n_results        INT      DEFAULT 10;
    DECLARE v_embed_model_id   VARCHAR(255) DEFAULT 'all-MiniLM-L12-v2';

    DECLARE v_query_embedding  VECTOR(1536);
    DECLARE v_vector_string    TEXT;
    DECLARE v_error_msg        TEXT     DEFAULT '';

    -- Schemas-filter loop
    DECLARE v_schema_count     INT      DEFAULT 0;
    DECLARE v_schema_filter    TEXT     DEFAULT '';
    DECLARE v_one_schema       TEXT;

    -- Tables-filter loop
    DECLARE v_table_count      INT      DEFAULT 0;
    DECLARE v_table_filter     TEXT     DEFAULT '';
    DECLARE v_one_schema_name  TEXT;
    DECLARE v_one_table_name   TEXT;

    -- Result-cursor state
    DECLARE v_done             INT      DEFAULT FALSE;
    DECLARE v_curr_schema      VARCHAR(64);
    DECLARE v_curr_table       VARCHAR(64);

    -- DDL-assembly buffers
    DECLARE v_output           TEXT     DEFAULT '';
    DECLARE v_ddl              TEXT;
    DECLARE v_col_def          TEXT;
    DECLARE v_fk_def           TEXT;
    DECLARE v_table_comment    TEXT;

    -- Cursor over the ranked temp table (populated by dynamic SQL)
    DECLARE result_cursor CURSOR FOR
        SELECT schema_name, table_name
        FROM   temp_ranked_tables
        ORDER  BY distance ASC;

    DECLARE CONTINUE HANDLER FOR NOT FOUND SET v_done = TRUE;

    -- Global exception handler
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
        DROP TEMPORARY TABLE IF EXISTS temp_ranked_tables;
        SET out_output = CONCAT('/* ML_RETRIEVE_SCHEMA_METADATA error: ',
                                v_error_msg, ' */');
        RESIGNAL;
    END;

    -- Input validation
    IF in_query_text IS NULL OR TRIM(in_query_text) = '' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'in_query_text cannot be NULL or empty';
    END IF;

    -- Parse options
    IF in_options IS NOT NULL AND JSON_VALID(in_options) THEN
        SET v_schemas = JSON_EXTRACT(in_options, '$.schemas');
        SET v_tables  = JSON_EXTRACT(in_options, '$.tables');

        SET v_include_comments = IF(
            JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.include_comments')) = 'false',
            FALSE, TRUE
        );

        SET v_n_results = COALESCE(
            CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.n_results')) AS UNSIGNED),
            10
        );

        SET v_embed_model_id = COALESCE(
            JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.embed_model_id')),
            'all-MiniLM-L12-v2'
        );
    END IF;

    -- Parameter validation
    IF v_schemas IS NOT NULL AND v_tables IS NOT NULL THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT =
                'Options "schemas" and "tables" are mutually exclusive';
    END IF;

    IF v_n_results < 1 OR v_n_results > 128 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'n_results must be between 1 and 128';
    END IF;

    -- Limit schema / table list to 128 entries (spec requirement)
    IF v_schemas IS NOT NULL AND JSON_LENGTH(v_schemas) > 128 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'schemas array exceeds the limit of 128 entries';
    END IF;

    IF v_tables IS NOT NULL AND JSON_LENGTH(v_tables) > 128 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'tables array exceeds the limit of 128 entries';
    END IF;

    -- Generate query embedding
    SELECT ML_MODEL_EMBED_ROW(
        -- When include_comments=false the query still uses the same model;
        -- no change is needed on the query side.
        in_query_text,
        JSON_OBJECT('model_id', v_embed_model_id, 'truncate', TRUE)
    ) INTO v_query_embedding;

    IF v_query_embedding IS NULL THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Failed to generate query embedding';
    END IF;

    -- Escape single-quotes so the vector literal is safe inside dynamic SQL
    SET v_vector_string = REPLACE(
        VECTOR_TO_STRING(v_query_embedding), '''', ''''''
    );

    -- Build schema-name IN-list  (used when v_schemas is set)
    IF v_schemas IS NOT NULL AND JSON_LENGTH(v_schemas) > 0 THEN
        SET v_schema_count = JSON_LENGTH(v_schemas);
        WHILE v_schema_count > 0 DO
            SET v_schema_count  = v_schema_count - 1;
            SET v_one_schema    = REPLACE(
                JSON_UNQUOTE(JSON_EXTRACT(v_schemas,
                    CONCAT('$[', v_schema_count, ']'))),
                '''', ''''''
            );
            SET v_schema_filter = CONCAT(
                v_schema_filter,
                IF(v_schema_filter = '', '', ', '),
                '''', v_one_schema, ''''
            );
        END WHILE;
    END IF;

    -- Build (schema_name, table_name) IN-list  (used when v_tables is set)
    -- Each element is JSON_OBJECT('schema_name','db','table_name','tbl')
    IF v_tables IS NOT NULL AND JSON_LENGTH(v_tables) > 0 THEN
        SET v_table_count = JSON_LENGTH(v_tables);
        WHILE v_table_count > 0 DO
            SET v_table_count     = v_table_count - 1;
            SET v_one_schema_name = REPLACE(
                JSON_UNQUOTE(JSON_EXTRACT(v_tables,
                    CONCAT('$[', v_table_count, '].schema_name'))),
                '''', ''''''
            );
            SET v_one_table_name  = REPLACE(
                JSON_UNQUOTE(JSON_EXTRACT(v_tables,
                    CONCAT('$[', v_table_count, '].table_name'))),
                '''', ''''''
            );
            SET v_table_filter = CONCAT(
                v_table_filter,
                IF(v_table_filter = '', '', ', '),
                '(''', v_one_schema_name, ''', ''', v_one_table_name, ''')'
            );
        END WHILE;
    END IF;

    -- Ranked retrieval via vector similarity on schema_embeddings
    DROP TEMPORARY TABLE IF EXISTS temp_ranked_tables;
    CREATE TEMPORARY TABLE temp_ranked_tables (
        schema_name  VARCHAR(64)    NOT NULL,
        table_name   VARCHAR(64)    NOT NULL,
        distance     DECIMAL(10, 6) NOT NULL,
        INDEX idx_distance (distance)
    ) ENGINE = InnoDB;

    -- Base query: distance-ranked lookup against mysql.schema_embeddings
    -- Only rows with status=1 (embedding ready) are considered.
    -- When include_comments=false the embedding was stored without comments,
    -- so the stored vector already reflects the "no-comment" space.
    SET @rank_sql = CONCAT(
        'INSERT INTO temp_ranked_tables (schema_name, table_name, distance) ',
        'SELECT schema_name, table_name, ',
        '  DISTANCE(embedding, STRING_TO_VECTOR(''', v_vector_string, '''), ''COSINE'') AS distance ',
        'FROM mysql.schema_embeddings ',
        'WHERE status = 1 ',
        '  AND embedding IS NOT NULL '
    );

    -- Apply filter: schemas
    IF v_schema_filter != '' THEN
        SET @rank_sql = CONCAT(@rank_sql,
            'AND schema_name IN (', v_schema_filter, ') '
        );
    END IF;

    -- Apply filter: specific (schema, table) pairs
    IF v_table_filter != '' THEN
        SET @rank_sql = CONCAT(@rank_sql,
            'AND (schema_name, table_name) IN (', v_table_filter, ') '
        );
    END IF;

    SET @rank_sql = CONCAT(@rank_sql,
        'ORDER BY distance ASC ',
        'LIMIT ', CAST(v_n_results AS CHAR)
    );

    PREPARE rank_stmt FROM @rank_sql;
    EXECUTE rank_stmt;
    DEALLOCATE PREPARE rank_stmt;

    -- Cursor: iterate ranked tables, build abridged CREATE TABLE
    OPEN result_cursor;

    result_loop: LOOP
        FETCH result_cursor INTO v_curr_schema, v_curr_table;
        IF v_done THEN
            LEAVE result_loop;
        END IF;

        -- Column definitions
        -- Abridged: name + type only; comments are optional.
        SELECT GROUP_CONCAT(
            CONCAT(
                '  `', COLUMN_NAME, '` ', COLUMN_TYPE,
                IF(
                    v_include_comments
                        AND COLUMN_COMMENT IS NOT NULL
                        AND COLUMN_COMMENT != '',
                    CONCAT(
                        ' COMMENT ''',
                        REPLACE(COLUMN_COMMENT, '''', ''''''),
                        ''''
                    ),
                    ''
                )
            )
            ORDER BY ORDINAL_POSITION
            SEPARATOR ',\n'
        )
        INTO v_col_def
        FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = v_curr_schema
          AND TABLE_NAME   = v_curr_table;

        -- Foreign-key definitions
        SELECT GROUP_CONCAT(
            CONCAT(
                '  FOREIGN KEY (`', kcu.COLUMN_NAME, '`) ',
                'REFERENCES `', kcu.REFERENCED_TABLE_SCHEMA,
                '`.`', kcu.REFERENCED_TABLE_NAME,
                '`(`', kcu.REFERENCED_COLUMN_NAME, '`)'
            )
            ORDER BY kcu.CONSTRAINT_NAME, kcu.ORDINAL_POSITION
            SEPARATOR ',\n'
        )
        INTO v_fk_def
        FROM information_schema.KEY_COLUMN_USAGE kcu
        WHERE kcu.TABLE_SCHEMA          = v_curr_schema
          AND kcu.TABLE_NAME            = v_curr_table
          AND kcu.REFERENCED_TABLE_NAME IS NOT NULL;

        -- Table-level comment (only when include_comments=true)
        SET v_table_comment = NULL;
        IF v_include_comments THEN
            SELECT TABLE_COMMENT
            INTO   v_table_comment
            FROM   information_schema.TABLES
            WHERE  TABLE_SCHEMA = v_curr_schema
              AND  TABLE_NAME   = v_curr_table;
        END IF;

        -- Assemble abridged CREATE TABLE
        SET v_ddl = CONCAT(
            'CREATE TABLE `', v_curr_schema, '`.`', v_curr_table, '`(\n',
            COALESCE(v_col_def, ''),
            IF(v_fk_def IS NOT NULL,
               CONCAT(',\n', v_fk_def),
               ''
            ),
            '\n)',
            IF(
                v_include_comments
                    AND v_table_comment IS NOT NULL
                    AND v_table_comment != '',
                CONCAT(
                    ' COMMENT ''',
                    REPLACE(v_table_comment, '''', ''''''),
                    ''''
                ),
                ''
            ),
            ';\n\n'
        );

        SET v_output = CONCAT(v_output, v_ddl);
    END LOOP;

    CLOSE result_cursor;

    -- Output
    SET out_output = IF(v_output = '',
        '/* No relevant tables found */',
        v_output
    );

    -- Cleanup
    DROP TEMPORARY TABLE IF EXISTS temp_ranked_tables;

END$$

DELIMITER ;