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

DROP PROCEDURE IF EXISTS ml_rag;

DELIMITER $$

CREATE DEFINER='mysql.sys'@'localhost' PROCEDURE ml_rag (
    IN  in_query_text  TEXT,
    OUT out_output     JSON,
    IN  in_options     JSON
)
COMMENT '
Description
-----------
The ML_RAG routine performs retrieval-augmented generation (RAG) by:
- Taking a natural-language query
- Retrieving context from relevant documents using semantic search
- Generating a response that integrates information from the retrieved documents

This routine aims to provide detailed, accurate, and contextually relevant answers 
by augmenting a generative model with information retrieved from a comprehensive knowledge base.

Parameters
-----------
in_query_text (TEXT):
  The natural-language query to process

out_output (JSON):
  Stores the generated output containing:
  - text: the generated text-based response
  - citations: array of retrieved segments with details (segment, distance, document_name, vector_store)

in_options (JSON):
  Specifies optional parameters as key-value pairs:
  - vector_store: JSON array of vector store table names to use
  - schema: JSON array of schema names to search
  - n_citations: number of segments for context retrieval (default: 3, range: 0-100)
  - distance_metric: COSINE|DOT|EUCLIDEAN (default: COSINE)
  - document_name: JSON array of specific documents to use
  - skip_generate: true|false (default: false)
  - model_options: additional options for text generation
  - exclude_vector_store: JSON array of vector stores to exclude
  - exclude_document_name: JSON array of documents to exclude
  - retrieval_options: context retrieval parameters
  - vector_store_columns: column name mappings
  - embed_model_id: embedding model to use (default: multilingual-e5-small)
  - query_embedding: pre-computed query embedding

Example
-----------
mysql> SET @options = JSON_OBJECT("vector_store", JSON_ARRAY("demo_db.demo_embeddings"));
mysql> CALL sys.ML_RAG("What is AutoML", @output, @options);
mysql> SELECT @output;
'
SQL SECURITY INVOKER
READS SQL DATA
BEGIN
    DECLARE v_vector_store       JSON;
    DECLARE v_schema             JSON;
    DECLARE v_n_citations        INT            DEFAULT 3;
    DECLARE v_distance_metric    VARCHAR(32)    DEFAULT 'COSINE';
    DECLARE v_document_name      JSON;
    DECLARE v_skip_generate      BOOLEAN        DEFAULT FALSE;
    DECLARE v_model_options      JSON;
    DECLARE v_embed_model_id     VARCHAR(255)   DEFAULT 'multilingual-e5-small';
    DECLARE v_query_embedding    VECTOR(384);
    DECLARE v_max_distance       DECIMAL(10,6)  DEFAULT 0.6;
    DECLARE v_percentage_distance DECIMAL(5,2)  DEFAULT 20.0;
    DECLARE v_segment_overlap    INT            DEFAULT 1;

    DECLARE v_generated_text     LONGTEXT       DEFAULT '';
    DECLARE v_citations          JSON           DEFAULT JSON_ARRAY();
    DECLARE v_error_msg          TEXT           DEFAULT '';
    DECLARE v_store_count        INT            DEFAULT 0;
    DECLARE v_current_store      VARCHAR(255);
    DECLARE v_curr_schema        VARCHAR(64);
    DECLARE v_curr_table         VARCHAR(64);
    DECLARE v_vector_string      TEXT;
    DECLARE v_min_distance       DECIMAL(10,6);
    DECLARE v_max_allowed_distance DECIMAL(10,6);

    DECLARE v_doc_filter         TEXT           DEFAULT '';
    DECLARE v_doc_count          INT            DEFAULT 0;
    DECLARE v_doc_name           TEXT;

    -- Column name mapping (overridable via vector_store_columns option)
    DECLARE v_col_segment            VARCHAR(255) DEFAULT 'segment';
    DECLARE v_col_segment_embedding  VARCHAR(255) DEFAULT 'segment_embedding';
    DECLARE v_col_document_name      VARCHAR(255) DEFAULT 'document_name';
    DECLARE v_col_document_id        VARCHAR(255) DEFAULT 'document_id';
    DECLARE v_col_metadata           VARCHAR(255) DEFAULT 'metadata';
    DECLARE v_col_segment_number     VARCHAR(255) DEFAULT 'segment_number';

    DECLARE v_has_document_name  TINYINT(1)    DEFAULT 1;
    DECLARE v_has_segment_number TINYINT(1)    DEFAULT 1;
    DECLARE v_has_metadata       TINYINT(1)    DEFAULT 1;

    -- Cursor over enriched vector-store registry
    DECLARE done INT DEFAULT FALSE;
    DECLARE store_cursor CURSOR FOR
        SELECT store_name, schema_name, table_name,
               has_document_name, has_segment_number, has_metadata
        FROM temp_vector_stores;
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = TRUE;

    -- Exception handler – clean up temp tables and surface the error
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        GET DIAGNOSTICS CONDITION 1 v_error_msg = MESSAGE_TEXT;
        DROP TEMPORARY TABLE IF EXISTS temp_vector_stores;
        DROP TEMPORARY TABLE IF EXISTS temp_citations_base;
        DROP TEMPORARY TABLE IF EXISTS temp_citations_overlap;
        DROP TEMPORARY TABLE IF EXISTS temp_citations_final;
        SET out_output = JSON_OBJECT(
            'error', CONCAT('RAG processing failed: ', v_error_msg)
        );
        RESIGNAL;
    END;

    -- Input validation
    IF in_query_text IS NULL OR TRIM(in_query_text) = '' THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'Query text cannot be null or empty';
    END IF;

    -- Parse options
    IF JSON_VALID(in_options) AND in_options IS NOT NULL THEN
        SET v_vector_store    = JSON_EXTRACT(in_options, '$.vector_store');
        SET v_schema          = JSON_EXTRACT(in_options, '$.schema');
        SET v_n_citations     = COALESCE(
            CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.n_citations')) AS UNSIGNED), 3);
        SET v_distance_metric = COALESCE(
            JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.distance_metric')), 'COSINE');
        SET v_document_name   = JSON_EXTRACT(in_options, '$.document_name');
        SET v_skip_generate   = COALESCE(
            CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.skip_generate')) AS UNSIGNED), FALSE);
        SET v_model_options   = JSON_EXTRACT(in_options, '$.model_options');
        SET v_embed_model_id  = COALESCE(
            JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.embed_model_id')),
            'multilingual-e5-small');

        -- Column name overrides
        IF JSON_EXTRACT(in_options, '$.vector_store_columns') IS NOT NULL THEN
            SET v_col_segment = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.segment')),
                v_col_segment);
            SET v_col_segment_embedding = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.segment_embedding')),
                v_col_segment_embedding);
            SET v_col_document_name = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.document_name')),
                v_col_document_name);
            SET v_col_document_id = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.document_id')),
                v_col_document_id);
            SET v_col_metadata = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.metadata')),
                v_col_metadata);
            SET v_col_segment_number = COALESCE(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.vector_store_columns.segment_number')),
                v_col_segment_number);
        END IF;

        -- Retrieval tuning
        IF JSON_EXTRACT(in_options, '$.retrieval_options') IS NOT NULL THEN
            SET v_max_distance = COALESCE(
                CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.retrieval_options.max_distance'))
                     AS DECIMAL(10,6)), 0.6);
            SET v_percentage_distance = COALESCE(
                CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.retrieval_options.percentage_distance'))
                     AS DECIMAL(5,2)), 20.0);
            SET v_segment_overlap = COALESCE(
                CAST(JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.retrieval_options.segment_overlap'))
                     AS UNSIGNED), 1);
        END IF;

        -- Pre-computed embedding shortcut
        IF JSON_EXTRACT(in_options, '$.query_embedding') IS NOT NULL THEN
            SET v_query_embedding = STRING_TO_VECTOR(
                JSON_UNQUOTE(JSON_EXTRACT(in_options, '$.query_embedding')));
        END IF;
    END IF;

    -- Parameter validation
    IF v_n_citations < 0 OR v_n_citations > 100 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'n_citations must be between 0 and 100';
    END IF;

    IF v_distance_metric NOT IN ('COSINE', 'DOT', 'EUCLIDEAN', 'L2') THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'distance_metric must be COSINE, DOT, EUCLIDEAN, or L2';
    END IF;

    -- Generate query embedding (if not pre-supplied)
    IF v_query_embedding IS NULL THEN
        SELECT ML_MODEL_EMBED_ROW(
            in_query_text,
            JSON_OBJECT('model_id', v_embed_model_id, 'truncate', true)
        ) INTO v_query_embedding;

        IF v_query_embedding IS NULL THEN
            SIGNAL SQLSTATE '45000'
                SET MESSAGE_TEXT = 'Failed to generate query embedding';
        END IF;
    END IF;

    SET v_vector_string = REPLACE(VECTOR_TO_STRING(v_query_embedding), '''', '''''');

    -- Build temp_vector_stores with column-existence flags
    DROP TEMPORARY TABLE IF EXISTS temp_vector_stores;
    CREATE TEMPORARY TABLE temp_vector_stores (
        store_name        VARCHAR(255) COLLATE utf8mb4_0900_ai_ci NOT NULL,
        schema_name       VARCHAR(64)  COLLATE utf8mb4_0900_ai_ci,
        table_name        VARCHAR(64)  COLLATE utf8mb4_0900_ai_ci,
        has_document_name TINYINT(1)   NOT NULL DEFAULT 1,
        has_segment_number TINYINT(1)  NOT NULL DEFAULT 1,
        has_metadata      TINYINT(1)   NOT NULL DEFAULT 1,
        PRIMARY KEY (store_name)
    ) ENGINE=InnoDB;

    IF v_vector_store IS NOT NULL AND JSON_LENGTH(v_vector_store) > 0 THEN
        SET v_store_count = JSON_LENGTH(v_vector_store);
        WHILE v_store_count > 0 DO
            SET v_store_count    = v_store_count - 1;
            SET v_current_store  = JSON_UNQUOTE(
                JSON_EXTRACT(v_vector_store, CONCAT('$[', v_store_count, ']')));

            IF LOCATE('.', v_current_store) > 0 THEN
                INSERT IGNORE INTO temp_vector_stores
                    (store_name, schema_name, table_name)
                VALUES (
                    v_current_store,
                    SUBSTRING_INDEX(v_current_store, '.', 1),
                    SUBSTRING_INDEX(v_current_store, '.', -1)
                );
            ELSE
                INSERT IGNORE INTO temp_vector_stores
                    (store_name, schema_name, table_name)
                VALUES (
                    CONCAT(DATABASE(), '.', v_current_store),
                    DATABASE(),
                    v_current_store
                );
            END IF;
        END WHILE;
    ELSE
        -- Auto-discover: find tables that have the configured embedding column
        INSERT INTO temp_vector_stores (store_name, schema_name, table_name)
        SELECT CONCAT(c.TABLE_SCHEMA, '.', c.TABLE_NAME),
               c.TABLE_SCHEMA,
               c.TABLE_NAME
        FROM information_schema.COLUMNS c
        WHERE c.DATA_TYPE   = 'vector'
          AND c.COLUMN_NAME = v_col_segment_embedding
          AND c.TABLE_SCHEMA NOT IN (
              'information_schema', 'mysql', 'performance_schema', 'sys')
        GROUP BY c.TABLE_SCHEMA, c.TABLE_NAME;

        IF v_schema IS NOT NULL AND JSON_LENGTH(v_schema) > 0 THEN
            DELETE FROM temp_vector_stores
            WHERE schema_name NOT IN (
                SELECT JSON_UNQUOTE(j.value)
                FROM JSON_TABLE(v_schema, '$[*]' COLUMNS (value TEXT PATH '$')) j
            );
        END IF;
    END IF;

    -- Probe column existence for every registered store in one pass.
    -- We use a correlated EXISTS against information_schema.COLUMNS.
    UPDATE temp_vector_stores tvs
    SET
        has_document_name  = EXISTS (
            SELECT 1 FROM information_schema.COLUMNS ic
            WHERE ic.TABLE_SCHEMA = tvs.schema_name
              AND ic.TABLE_NAME   = tvs.table_name
              AND ic.COLUMN_NAME  = v_col_document_name),
        has_segment_number = EXISTS (
            SELECT 1 FROM information_schema.COLUMNS ic
            WHERE ic.TABLE_SCHEMA = tvs.schema_name
              AND ic.TABLE_NAME   = tvs.table_name
              AND ic.COLUMN_NAME  = v_col_segment_number),
        has_metadata       = EXISTS (
            SELECT 1 FROM information_schema.COLUMNS ic
            WHERE ic.TABLE_SCHEMA = tvs.schema_name
              AND ic.TABLE_NAME   = tvs.table_name
              AND ic.COLUMN_NAME  = v_col_metadata);

    -- 1. Base retrieval
    DROP TEMPORARY TABLE IF EXISTS temp_citations_base;
    CREATE TEMPORARY TABLE temp_citations_base (
        segment       LONGTEXT     COLLATE utf8mb4_0900_ai_ci,
        distance      DECIMAL(10,6),
        document_name VARCHAR(255) COLLATE utf8mb4_0900_ai_ci,
        vector_store  VARCHAR(255) COLLATE utf8mb4_0900_ai_ci,
        segment_number INT,
        metadata      JSON,
        INDEX idx_distance (distance)
    ) ENGINE=InnoDB;

    OPEN store_cursor;
    base_loop: LOOP
        FETCH store_cursor
            INTO v_current_store, v_curr_schema, v_curr_table,
                 v_has_document_name, v_has_segment_number, v_has_metadata;
        IF done THEN
            LEAVE base_loop;
        END IF;

        SET @sql = CONCAT(
            'INSERT INTO temp_citations_base'
            ' (segment, distance, document_name, vector_store, segment_number, metadata) ',
            'SELECT ',
            -- segment
            'COALESCE(CAST(`', v_col_segment, '` AS CHAR CHARACTER SET utf8mb4)'
                ' COLLATE utf8mb4_0900_ai_ci, '''') AS segment, ',
            -- distance
            'DISTANCE(`', v_col_segment_embedding, '`,'
                ' STRING_TO_VECTOR(''', v_vector_string, '''), ''', v_distance_metric, ''')'
                ' AS distance, '
        );

        -- document_name column
        IF v_has_document_name THEN
            SET @sql = CONCAT(@sql,
                'COALESCE(',
                    'CAST(`', v_col_document_name, '` AS CHAR CHARACTER SET utf8mb4)'
                        ' COLLATE utf8mb4_0900_ai_ci,',
                    ' ''default_document_', v_curr_table, '''',
                ') AS document_name, ');
        ELSE
            SET @sql = CONCAT(@sql,
                '''default_document_', v_curr_table, ''' AS document_name, ');
        END IF;

        -- vector_store literal
        SET @sql = CONCAT(@sql,
            '''', REPLACE(v_current_store, '''', ''''''), ''' AS vector_store, ');

        -- segment_number
        IF v_has_segment_number THEN
            SET @sql = CONCAT(@sql,
                'COALESCE(`', v_col_segment_number, '`, 0) AS segment_number, ');
        ELSE
            SET @sql = CONCAT(@sql, '0 AS segment_number, ');
        END IF;

        -- metadata
        IF v_has_metadata THEN
            SET @sql = CONCAT(@sql,
                'COALESCE(`', v_col_metadata, '`, JSON_OBJECT()) AS metadata ');
        ELSE
            SET @sql = CONCAT(@sql, 'JSON_OBJECT() AS metadata ');
        END IF;

        -- FROM / WHERE / ORDER / LIMIT clauses
        SET @sql = CONCAT(@sql,
            'FROM `', v_curr_schema, '`.`', v_curr_table, '` ',
            'WHERE `', v_col_segment, '` IS NOT NULL ',
            'AND CAST(`', v_col_segment, '` AS CHAR CHARACTER SET utf8mb4)'
                ' COLLATE utf8mb4_0900_ai_ci <> '''' ',
            'AND `', v_col_segment_embedding, '` IS NOT NULL '
        );

        -- Optional document-name filter (skip when column absent)
        IF v_document_name IS NOT NULL
           AND JSON_LENGTH(v_document_name) > 0
           AND v_has_document_name THEN

            SET v_doc_count  = JSON_LENGTH(v_document_name);
            SET v_doc_filter = '';
            WHILE v_doc_count > 0 DO
                SET v_doc_count = v_doc_count - 1;
                SET v_doc_name  = REPLACE(
                    JSON_UNQUOTE(JSON_EXTRACT(v_document_name,
                        CONCAT('$[', v_doc_count, ']'))),
                    '''', '''''');
                SET v_doc_filter = CONCAT(
                    v_doc_filter,
                    IF(v_doc_filter = '', '', ', '),
                    '''', v_doc_name, '''');
            END WHILE;
            SET @sql = CONCAT(@sql,
                'AND CAST(`', v_col_document_name, '` AS CHAR CHARACTER SET utf8mb4)'
                    ' COLLATE utf8mb4_0900_ai_ci IN (', v_doc_filter, ') ');
        END IF;

        SET @sql = CONCAT(@sql,
            'ORDER BY distance ASC LIMIT ', CAST(v_n_citations * 2 AS CHAR));

        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

    END LOOP;
    CLOSE store_cursor;

    -- 2. Segment overlap expansion
    IF v_segment_overlap > 0 THEN
        DROP TEMPORARY TABLE IF EXISTS temp_citations_overlap;
        CREATE TEMPORARY TABLE temp_citations_overlap (
            segment       LONGTEXT,
            distance      DECIMAL(10,6),
            document_name VARCHAR(255),
            vector_store  VARCHAR(255),
            segment_number INT,
            metadata      JSON,
            INDEX idx_distance (distance)
        ) ENGINE=InnoDB;

        SET done = FALSE;
        OPEN store_cursor;
        overlap_loop: LOOP
            FETCH store_cursor
                INTO v_current_store, v_curr_schema, v_curr_table,
                     v_has_document_name, v_has_segment_number, v_has_metadata;
            IF done THEN
                LEAVE overlap_loop;
            END IF;

            IF NOT v_has_segment_number THEN
                ITERATE overlap_loop;
            END IF;

            -- overlap SELECT list
            SET @overlap_sql = CONCAT(
                'INSERT INTO temp_citations_overlap'
                ' (segment, distance, document_name, vector_store, segment_number, metadata) ',
                'SELECT ',
                'CAST(t2.`', v_col_segment, '` AS CHAR CHARACTER SET utf8mb4)'
                    ' COLLATE utf8mb4_0900_ai_ci, ',
                '(base.distance + 0.01) AS distance, '
            );

            SET @overlap_sql = CONCAT(@overlap_sql, 'base.document_name, ');

            -- vector_store from base
            SET @overlap_sql = CONCAT(@overlap_sql, 'base.vector_store, ');

            -- segment_number (v_has_segment_number is TRUE here)
            SET @overlap_sql = CONCAT(@overlap_sql,
                't2.`', v_col_segment_number, '` AS segment_number, ');

            -- metadata
            IF v_has_metadata THEN
                SET @overlap_sql = CONCAT(@overlap_sql,
                    'COALESCE(t2.`', v_col_metadata, '`, JSON_OBJECT()) AS metadata ');
            ELSE
                SET @overlap_sql = CONCAT(@overlap_sql, 'JSON_OBJECT() AS metadata ');
            END IF;

            -- FROM + JOIN
            SET @overlap_sql = CONCAT(@overlap_sql,
                'FROM `', v_curr_schema, '`.`', v_curr_table, '` t2 ',
                'INNER JOIN temp_citations_base base ON ',
                    'base.vector_store COLLATE utf8mb4_0900_ai_ci = ''',
                        REPLACE(v_current_store, '''', ''''''), ''' '
            );

            -- document join: match on actual column when available,
            -- otherwise match on the synthetic default_document_<table> literal
            IF v_has_document_name THEN
                SET @overlap_sql = CONCAT(@overlap_sql,
                    'AND CAST(t2.`', v_col_document_name, '`'
                        ' AS CHAR CHARACTER SET utf8mb4) COLLATE utf8mb4_0900_ai_ci'
                        ' = base.document_name COLLATE utf8mb4_0900_ai_ci ');
            ELSE
                SET @overlap_sql = CONCAT(@overlap_sql,
                    'AND ''default_document_', v_curr_table, ''''
                        ' COLLATE utf8mb4_0900_ai_ci'
                        ' = base.document_name COLLATE utf8mb4_0900_ai_ci ');
            END IF;

            -- adjacency condition on segment_number
            SET @overlap_sql = CONCAT(@overlap_sql,
                'AND ABS(t2.`', v_col_segment_number,
                    '` - base.segment_number) <= ', CAST(v_segment_overlap AS CHAR), ' ',
                'AND t2.`', v_col_segment_number, '` <> base.segment_number '
            );

            -- WHERE: non-empty segment
            SET @overlap_sql = CONCAT(@overlap_sql,
                'WHERE t2.`', v_col_segment, '` IS NOT NULL ',
                'AND CAST(t2.`', v_col_segment, '` AS CHAR CHARACTER SET utf8mb4)'
                    ' COLLATE utf8mb4_0900_ai_ci <> '''' '
            );

            PREPARE stmt2 FROM @overlap_sql;
            EXECUTE stmt2;
            DEALLOCATE PREPARE stmt2;

        END LOOP;
        CLOSE store_cursor;

        -- Merge: base rows first, then overlap rows not already in base
        DROP TEMPORARY TABLE IF EXISTS temp_citations_final;
        CREATE TEMPORARY TABLE temp_citations_final (
            segment       LONGTEXT,
            distance      DECIMAL(10,6),
            document_name VARCHAR(255),
            vector_store  VARCHAR(255),
            segment_number INT,
            metadata      JSON,
            INDEX idx_distance (distance)
        ) ENGINE=InnoDB;

        INSERT INTO temp_citations_final SELECT * FROM temp_citations_base;

        INSERT INTO temp_citations_final
        SELECT o.* FROM temp_citations_overlap o
        WHERE NOT EXISTS (
            SELECT 1 FROM temp_citations_base b
            WHERE b.document_name  = o.document_name
              AND b.segment_number = o.segment_number
        );

        DROP TEMPORARY TABLE temp_citations_base;
        DROP TEMPORARY TABLE temp_citations_overlap;

    ELSE
        -- No overlap requested: promote base table directly
        DROP TEMPORARY TABLE IF EXISTS temp_citations_final;
        RENAME TABLE temp_citations_base TO temp_citations_final;
    END IF;

    -- 3. Distance filtering
    SELECT MIN(distance) INTO v_min_distance FROM temp_citations_final;
    IF v_min_distance IS NULL THEN
        SET v_min_distance = v_max_distance;
    END IF;
    SET v_max_allowed_distance = LEAST(
        v_max_distance,
        v_min_distance + (v_min_distance * v_percentage_distance / 100.0)
    );
    DELETE FROM temp_citations_final WHERE distance > v_max_allowed_distance;

    -- 4. Build citations JSON
    SELECT JSON_ARRAYAGG(
        JSON_OBJECT(
            'segment',        segment,
            'distance',       distance,
            'document_name',  document_name,
            'vector_store',   vector_store,
            'segment_number', segment_number,
            'metadata',       metadata
        )
    ) INTO v_citations
    FROM (
        SELECT * FROM temp_citations_final
        ORDER BY distance ASC
        LIMIT v_n_citations
    ) ordered_citations;

    IF v_citations IS NULL THEN
        SET v_citations = JSON_ARRAY();
    END IF;

    -- 5. Generate response (or return raw context)
    SELECT COALESCE(GROUP_CONCAT(segment SEPARATOR '\n\n'), '')
    INTO @context
    FROM (
        SELECT segment FROM temp_citations_final
        ORDER BY distance ASC
        LIMIT v_n_citations
    ) ctx;

    IF NOT v_skip_generate THEN
        SET @prompt = CONCAT(
            'Based on the following context, answer the question: ',
            COALESCE(in_query_text, ''),
            '\n\nContext:\n',
            COALESCE(@context, ''),
            '\n\nAnswer:'
        );

        IF v_model_options IS NULL THEN
            SET v_model_options = JSON_OBJECT(
                'model_id',    'Llama-3.2-3B-Instruct',
                'max_tokens',  1000,
                'temperature', 0.7
            );
        END IF;

        SELECT ML_MODEL_GENERATE(@prompt, v_model_options)
        INTO v_generated_text;

        IF v_generated_text IS NULL OR v_generated_text = '' THEN
            SET v_generated_text =
                'Unable to generate response based on the provided context.';
        END IF;
    ELSE
        -- skip_generate=1: return concatenated segments as-is
        SET v_generated_text = COALESCE(@context, '');
    END IF;

    -- 6. Build output JSON
    SELECT COUNT(*) INTO @total_found FROM temp_citations_final;
    SET out_output = JSON_OBJECT(
        'text',       v_generated_text,
        'citations',  COALESCE(v_citations, JSON_ARRAY()),
        'query_embedding', v_vector_string,
        'processing_info', JSON_OBJECT(
            'total_citations_found',  @total_found,
            'citations_returned',     LEAST(v_n_citations, @total_found),
            'min_distance',           v_min_distance,
            'max_allowed_distance',   v_max_allowed_distance,
            'vector_stores_queried',  (SELECT COUNT(*) FROM temp_vector_stores)
        )
    );

    DROP TEMPORARY TABLE IF EXISTS temp_vector_stores;
    DROP TEMPORARY TABLE IF EXISTS temp_citations_final;
END$$

DELIMITER ;
