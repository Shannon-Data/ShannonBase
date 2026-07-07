//@include lib_ml.js

function get_embed_model_id(opts) {
  if (opts && opts.embed_model_id) return opts.embed_model_id;
  var co = get_chat_options();
  if (co && co.embed_model_id) return co.embed_model_id;
  return 'multilingual-e5-small';
}

function check_schema_embeddings_ready(db) {
  if (!db) return false;
  try {
    var rows = query(
      "SELECT COUNT(*) AS cnt FROM mysql.schema_embeddings" +
      " WHERE schema_name='" + esc(db) + "' AND status=1"
    );
    return (Array.isArray(rows) && rows.length && rows[0].cnt > 0);
  } catch(e) { return false; }
}

function call_schema_metadata(user_msg, db, n_results, extra_opts) {
  n_results = n_results || 8;
  var opts = Object.assign({ n_results: n_results, include_comments: true }, extra_opts || {});
  if (!opts.embed_model_id) opts.embed_model_id = get_embed_model_id(extra_opts);
  if (!opts.schemas) opts.schemas = [db];
  try {
    sys.exec_sql("SET @_sm_out = NULL");
    sys.exec_sql(
      "CALL sys.ML_RETRIEVE_SCHEMA_METADATA(" +
      "'" + esc(user_msg) + "'," +
      "@_sm_out," +
      "'" + esc(JSON.stringify(opts)) + "')"
    );
    var rows = query("SELECT @_sm_out AS result");
    if (!rows || !Array.isArray(rows) || !rows.length) return '';
    var result = String(rows[0].result || '');
    if (result.indexOf('No relevant tables found') !== -1) return '';
    return result;
  } catch(e) { return ''; }
}

var MAX_SCHEMA_CHARS   = 4000;  // Hard cap: total schema context in chars
var MAX_COLS           = 16;   // Absolute ceiling on columns per table (dynamic budget applies below this)
var MIN_COLS           = 4;    // Never show fewer than this many columns for a Tier1 table
var MAX_CANDIDATE_DDL  = 10;   // Max tables for which we output full column DDL
var SCHEMA_CACHE_TTL_MS = 5 * 60 * 1000; // 5 min: catalog cache freshness window

/**
 * Column names that are near-universal in transactional schemas (audit
 * timestamps, soft-delete flags, row-version counters, free-text scratch
 * fields) and carry almost no signal for text-to-SQL / analytics — they're
 * structurally necessary in the table but rarely part of what the user is
 * actually asking to SELECT/WHERE/JOIN on. Matched against the *whole*
 * column name (case-insensitive) so it never misfires on a business column
 * that merely contains one of these words as a substring (e.g. a genuine
 * `deleted_reason` column is left alone).
 *
 * This is only used to push such columns toward the *tail* of the non-key
 * ordering within a table (see cols.sort in build_schema_context_fallback),
 * so that when the per-table column budget (dyn_max_cols) is tight, a
 * business column doesn't get silently truncated to make room for
 * `updated_at`. It never overrides key priority: a column that happens to
 * be both indexed/PK and named e.g. `deleted_at` keeps its structural
 * priority regardless of this list.
 */
var LOW_VALUE_COLUMN_PATTERN = new RegExp(
  '^(created_at|create_time|gmt_create|inserted_at|create_by|created_by|' +
  'updated_at|update_time|gmt_modified|gmt_update|last_modified|modified_at|updated_by|update_by|' +
  'deleted_at|delete_time|is_deleted|del_flag|is_del|deleted|' +
  'version|row_version|_version|extra|remark|remarks|memo)$', 'i'
);

function is_low_value_column(name) {
  return LOW_VALUE_COLUMN_PATTERN.test(String(name || '').trim());
}

/**
 * Backtick-quote an identifier (table/column/schema name), doubling any
 * embedded backticks. Table/column names used here always originate from
 * information_schema (i.e. they name real existing objects), but this is
 * cheap insurance against exotic names containing a backtick.
 */
function ident(s) {
  return '`' + String(s || '').replace(/`/g, '``') + '`';
}

/**
 * Fetch a small sample of real row values for a table, limited to the
 * columns actually being shown in the Tier1 DDL line (not SELECT *) to
 * keep both the query and the resulting text cheap. Best-effort: any
 * failure (missing table, permission issue, etc.) yields null rather than
 * surfacing an error into schema context.
 */
function get_sample_rows(db, table, col_names, limit) {
  limit = limit || 2;
  if (!db || !table || !col_names || !col_names.length) return null;
  try {
    var col_list = col_names.map(ident).join(',');
    var rows = query(
      "SELECT " + col_list + " FROM " + ident(db) + "." + ident(table) +
      " LIMIT " + Number(limit)
    );
    return (Array.isArray(rows) && rows.length) ? rows : null;
  } catch (e) { return null; }
}

/**
 * Render sample rows as a compact `(v1, v2, v3) | (v1, v2, v3)` string.
 * Long values (free text, JSON blobs) are truncated hard so one wide TEXT
 * column can't blow up the schema-context budget on its own; NULL is
 * rendered explicitly since it's often meaningful (e.g. optional FK).
 */
function format_sample_rows(rows, col_names, max_val_len) {
  max_val_len = max_val_len || 20;
  var lines = [];
  for (var i = 0; i < rows.length; i++) {
    var vals = col_names.map(function(c) {
      var v = rows[i][c];
      if (v === null || v === undefined) return 'NULL';
      var s = String(v);
      return s.length > max_val_len ? s.substring(0, max_val_len) + '…' : s;
    });
    lines.push('(' + vals.join(', ') + ')');
  }
  return lines.join(' | ');
}

/**
 * Whether to attach sample rows to Tier1 tables at all. Read directly from
 * chat_opt (not via cfg()) because cfg() treats 0 as "unset" and falls back
 * to its default — an operator explicitly opting out with
 * `schema_sample_rows_enabled: false` must actually disable it.
 */
function sample_rows_enabled() {
  var co = get_chat_options();
  return !(co && co.schema_sample_rows_enabled === false);
}

function get_table_count(db) {
  if (!db) return 0;
  try {
    var rows = query(
      "SELECT COUNT(*) AS cnt FROM information_schema.TABLES" +
      " WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE'"
    );
    return (Array.isArray(rows) && rows.length) ? (Number(rows[0].cnt) || 0) : 0;
  } catch (e) { return 0; }
}

/**
 * Fetch (and session-cache) the raw catalog for a schema: table list, the
 * full column map, and FK edges.  information_schema.COLUMNS scans get
 * expensive once a schema has hundreds of wide tables, and this was
 * previously re-run on *every single turn* of every conversation touching
 * that schema.
 *
 * Cache key is the schema name; invalidation is TTL-based plus a cheap
 * fingerprint check (COUNT(*) of tables + COUNT(*) of columns) so a DDL
 * change (table/column added or dropped) invalidates the cache well before
 * the TTL expires, without paying the cost of a full COLUMNS scan just to
 * validate the cache. Stored in a session user variable, same pattern as
 * @chat_options / @_rag_out elsewhere in this codebase — persists for the
 * connection's lifetime, rebuilt on demand when stale.
 */
function get_schema_catalog(db) {
  if (!db) return { tables: [], col_map: {}, fk_str: '' };

  var fp_rows = query(
    "SELECT " +
    " (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE') AS tcnt," +
    " (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + esc(db) + "') AS ccnt"
  );
  var fingerprint = (Array.isArray(fp_rows) && fp_rows.length) ?
    (String(fp_rows[0].tcnt) + ':' + String(fp_rows[0].ccnt)) : '0:0';

  var cache_var = '@schema_catalog_cache_' + db.replace(/[^A-Za-z0-9_]/g, '_');
  try {
    var cached_rows = query("SELECT " + cache_var + " AS c");
    if (Array.isArray(cached_rows) && cached_rows.length && cached_rows[0].c) {
      var cached = JSON.parse(cached_rows[0].c);
      if (cached && cached.fp === fingerprint &&
          (Date.now() - (cached.ts || 0)) < SCHEMA_CACHE_TTL_MS) {
        return cached.data;
      }
    }
  } catch (e) { /* fall through to rebuild */ }

  var tbl_rows = query(
    "SELECT TABLE_NAME, TABLE_ROWS, TABLE_COMMENT FROM information_schema.TABLES" +
    " WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE'" +
    " ORDER BY TABLE_NAME"
  );
  var col_rows = query(
    "SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY,COLUMN_COMMENT" +
    " FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + esc(db) + "'" +
    " ORDER BY TABLE_NAME,ORDINAL_POSITION"
  );
  var col_map = {};
  if (Array.isArray(col_rows)) {
    for (var ci = 0; ci < col_rows.length; ci++) {
      var r = col_rows[ci];
      if (!col_map[r.TABLE_NAME]) col_map[r.TABLE_NAME] = [];
      col_map[r.TABLE_NAME].push(r);
    }
  }
  var fk_rows = query(
    "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
    " FROM information_schema.KEY_COLUMN_USAGE" +
    " WHERE CONSTRAINT_SCHEMA='" + esc(db) + "' AND REFERENCED_TABLE_NAME IS NOT NULL"
  );
  var fk_str = Array.isArray(fk_rows) ? fk_rows.map(function(f) {
    return f.TABLE_NAME + '.' + f.COLUMN_NAME +
           '→' + f.REFERENCED_TABLE_NAME + '.' + f.REFERENCED_COLUMN_NAME;
  }).join(' | ') : '';

  var data = {
    tables: Array.isArray(tbl_rows) ? tbl_rows : [],
    col_map: col_map,
    fk_str: fk_str
  };

  try {
    var serialized = JSON.stringify({ fp: fingerprint, ts: Date.now(), data: data });
    /* Guard against blowing max_allowed_packet on pathologically wide schemas
     * (tens of thousands of columns). Caching is a pure optimization — if it's
     * too big to cache cheaply, just skip it and eat the full scan next turn. */
    if (serialized.length < 2 * 1024 * 1024) {
      sys.exec_sql("SET " + cache_var + " = '" + esc(serialized) + "'");
    }
  } catch (e) { /* caching is best-effort; ignore failures */ }

  return data;
}

function fmt_row_count(nr) {
  nr = Number(nr) || 0;
  return nr >= 1000000 ? (nr/1000000).toFixed(1)+'M' :
         nr >= 1000    ? (nr/1000).toFixed(1)+'K'    : String(nr);
}

/**
 * Tiered schema context builder.
 *
 * Strategy: accuracy requires that the LLM knows (a) which tables exist and
 * (b) full column details for the most relevant ones.  Dumping full DDL for
 * hundreds of tables blows up the prompt, but showing only names loses
 * precision.  The compromise:
 *
 *   Tier 1 [Full DDL] — candidate tables (matched by user-message keywords
 *     against column names or table names) get full column DDL so the LLM
 *     can write accurate SQL for the likely-relevant tables.
 *
 *   Tier 2 [Compact]  — all *other* tables are listed as
 *     `table_name -- comment` so the LLM knows they exist and can use
 *     `list_tables` / `describe_table` to pull details on demand.
 *
 * Within Tier 1, non-key columns are additionally sorted so that low-signal
 * audit/system columns (created_at, is_deleted, version, ...) sit at the
 * tail — see is_low_value_column — so they're the first ones dropped when
 * dyn_max_cols truncates, instead of a business column losing its slot to
 * `updated_at` purely by scan-order luck. The top-ranked table(s) also get
 * a couple of real sample rows appended (see get_sample_rows) for the exact
 * columns being shown, since column name/type/comment often doesn't convey
 * what the actual values look like (encoded status ints, date formats,
 * free-text categories) — this is bounded to a handful of tables since each
 * one is an extra query round trip.
 *
 * Note: this whole function is only reached for schemas at or below
 * cfg('schema_stuff_table_threshold', ...) tables — see build_schema_context.
 * Past that size, schema context is never inlined into the system prompt at
 * all; the model is pointed at the list_tables/describe_table tools instead
 * (build_catalog_pointer below).
 */
function build_schema_context_fallback(db, budget, candidate_tables) {
  if (!db) return '';
  budget = Math.min(budget || 1200, MAX_SCHEMA_CHARS);
  try {
    var catalog  = get_schema_catalog(db);
    var tbl_rows = catalog.tables;
    var col_map  = catalog.col_map;
    var fk_str   = catalog.fk_str;
    if (!Array.isArray(tbl_rows) || !tbl_rows.length) return '';

    function col_prio(k) { return k==='PRI'?0:k==='UNI'?1:k==='MUL'?2:3; }
    function compact_type(tp) {
      return tp.replace(/varchar\((\d+)\)/i,'VC($1)').replace(/char\((\d+)\)/i,'CH($1)')
               .replace(/bigint\(\d+\) unsigned/i,'UBIGINT').replace(/bigint\(\d+\)/i,'BIGINT')
               .replace(/int\(\d+\) unsigned/i,'UINT').replace(/int\(\d+\)/i,'INT')
               .replace(/tinyint\(1\)/i,'BOOL').replace(/tinyint\(\d+\)/i,'TINT')
               .replace(/decimal\((\d+),(\d+)\)/i,'DEC($1,$2)').replace(/timestamp/i,'TS')
               .replace(/datetime/i,'DT').replace(/(long|medium|tiny)?text/i,'TEXT')
               .toUpperCase();
    }

    /* --- candidate_tables is now a score-ranked list of {table, score, name_hit}
     * from infer_candidate_tables (IDF-weighted). Build a lookup by lowercased
     * name; tables not in this map fall to Tier2 regardless of any weak
     * substring coincidence — the scoring pass already absorbed that logic. */
    var score_map = {};
    if (Array.isArray(candidate_tables) && candidate_tables.length > 0) {
      for (var ct = 0; ct < candidate_tables.length; ct++) {
        var c = candidate_tables[ct];
        var cname = typeof c === 'string' ? c : c.table;
        var cscore = typeof c === 'string' ? 1 : c.score;
        if (cname) score_map[String(cname).toLowerCase()] = cscore;
      }
    }

    /* --- Partition into tiers, carrying score for Tier1 ranking --- */
    var tier1 = [], tier2 = [];
    for (var ti3 = 0; ti3 < tbl_rows.length; ti3++) {
      var tkey = String(tbl_rows[ti3].TABLE_NAME).toLowerCase();
      if (score_map.hasOwnProperty(tkey)) {
        tier1.push({ row: tbl_rows[ti3], score: score_map[tkey] });
      } else {
        tier2.push(tbl_rows[ti3]);
      }
    }
    /* Highest-relevance tables first, so if budget/MAX_CANDIDATE_DDL forces a
     * cutoff, we drop the *least* relevant candidates rather than whichever
     * happened to sort last alphabetically. */
    tier1.sort(function(a, b) { return b.score - a.score; });

    var lines = [t('【Schema: ', '[Schema: ') + db +
                 t('】（候选表含完整列定义，其余表仅列名。需要某表详情时用 query_db 执行 DESC/ SHOW CREATE TABLE，' +
                   '或 SHOW TABLES LIKE \'%关键词%\' 按名称检索）',
                   '] (Candidate tables have full column definitions; others are name-only. ' +
                   'Use query_db with DESC / SHOW CREATE TABLE for details on other tables, ' +
                   'or SHOW TABLES LIKE \'%keyword%\' to search by name.)')];
    var total = 0;

    /* ---- Tier 1: Full column DDL for candidate tables, score-ranked ----
     * Per-table column budget is now proportional to that table's relevance
     * score rather than a flat MAX_COLS=10 for every candidate: a table that
     * matched on its own name (name_hit / high score) gets more room than one
     * that only scraped in on a single low-idf column match. */
    var ddl_count = 0;
    var ddl_budget = Math.floor(budget * 0.7); // 70% of budget for DDL
    /* Normalize against the *highest* score in Tier1, not the score sum.
     * The previous formula (share * tier1.length, where share = score /
     * score_sum) averages to ~1 for an average-relevance table regardless
     * of how many candidates there are — Σshare is 1 by construction, so
     * share * length averages to 1 — which meant any table at or above
     * average relevance saturated at MAX_COLS after clamping. In practice
     * that collapsed the "proportional budget" into a near-binary
     * above/below-average cutoff instead of a smooth curve. Scaling by
     * relative distance from the top score instead gives an actual
     * proportional spread: the top-ranked table gets MAX_COLS, and lower-
     * ranked tables scale down toward MIN_COLS in proportion to how far
     * behind the leader they are — independent of how many candidates
     * happen to be in Tier1. */
    var max_score = 0.1;
    for (var si = 0; si < tier1.length; si++) max_score = Math.max(max_score, tier1[si].score);

    /* Sample rows: attach `SELECT <shown cols> LIMIT n` output to only the
     * top-ranked candidates (not every Tier1 table) — each one is an extra
     * round trip, and the top 1-2 tables are almost always the ones the
     * query is actually about. Column names/types alone often don't convey
     * what a column's *values* look like (encoded status ints, date
     * formats, free-text categories); a couple of real rows fixes that far
     * more cheaply, token-wise, than prose explanation would. */
    var sample_max_tables = cfg('schema_sample_rows_max_tables', 2);
    var sample_row_limit  = cfg('schema_sample_rows_limit', 2);
    var samples_on        = sample_rows_enabled();

    for (var t1 = 0; t1 < tier1.length && ddl_count < MAX_CANDIDATE_DDL; t1++) {
      var tn   = tier1[t1].row.TABLE_NAME;
      var tc   = tier1[t1].row.TABLE_COMMENT || '';
      var cols = (col_map[tn] || []).slice();
      cols.sort(function(a,b){
        var pa = col_prio(a.COLUMN_KEY), pb = col_prio(b.COLUMN_KEY);
        if (pa !== pb) return pa - pb;
        /* Within the non-key tier, push low-signal audit/system columns
         * (created_at, is_deleted, version, ...) toward the tail so they're
         * the first to fall off when dyn_max_cols truncates — a business
         * column should never lose its slot to `updated_at`. */
        if (pa === 3) {
          var la = is_low_value_column(a.COLUMN_NAME) ? 1 : 0;
          var lb = is_low_value_column(b.COLUMN_NAME) ? 1 : 0;
          if (la !== lb) return la - lb;
        }
        return 0;
      });

      var share = Math.max(tier1[t1].score, 0) / max_score; // 1.0 for the top table, ->0 for weak matches
      var dyn_max_cols = Math.round(MIN_COLS + share * (MAX_COLS - MIN_COLS));
      dyn_max_cols = Math.max(MIN_COLS, Math.min(MAX_COLS, dyn_max_cols));

      var descs = [], show = Math.min(cols.length, dyn_max_cols);
      var shown_names = [];
      for (var ci3 = 0; ci3 < show; ci3++) {
        var col  = cols[ci3];
        var desc = col.COLUMN_NAME + ' ' + compact_type(col.COLUMN_TYPE);
        if (col.COLUMN_KEY==='PRI')      desc += ' PK';
        else if (col.COLUMN_KEY==='UNI') desc += ' UQ';
        else if (col.COLUMN_KEY==='MUL') desc += ' IDX';
        if (col.COLUMN_COMMENT) desc += '/*' + col.COLUMN_COMMENT.substring(0,30) + '*/';
        descs.push(desc);
        shown_names.push(col.COLUMN_NAME);
      }
      if (cols.length > dyn_max_cols) descs.push('…+' + (cols.length-dyn_max_cols) + 'cols');
      var line = tn + '(' + descs.join(',') + ')';
      if (tc && tc.length > 0) line += ' --' + tc.substring(0,40);
      /* Append approximate row count so the LLM can decide JOIN order
       * (small driving table first, large probe table second). */
      var rows_approx = Number(tier1[t1].row.TABLE_ROWS || 0);
      if (rows_approx > 0) line += ' ~' + fmt_row_count(rows_approx) + ' rows';

      /* Only the top-ranked handful of tables get a sample-rows line, and
       * only using the columns we're already showing (never a separate
       * SELECT * — that would both cost more and could leak columns the
       * budget deliberately left out). */
      var sample_line = '';
      if (samples_on && t1 < sample_max_tables && show > 0) {
        var sample_rows = get_sample_rows(db, tn, shown_names, sample_row_limit);
        if (sample_rows && sample_rows.length) {
          sample_line = '  ' + t('示例：', 'e.g. ') + format_sample_rows(sample_rows, shown_names, 20);
        }
      }

      total += line.length + (sample_line ? sample_line.length + 1 : 0);
      if (total > ddl_budget) break;
      lines.push(line);
      if (sample_line) lines.push(sample_line);
      ddl_count++;
    }
    if (tier1.length > ddl_count) {
      lines.push(t('（+', '(+') + (tier1.length - ddl_count) +
                 t(' 张候选表因预算限制省略，相关性低于以上表；如需要请让 Agent 用 query_db 单独查询）',
                   ' more candidate tables omitted (lower relevance than those above); ' +
                   'ask the agent to query_db them individually if needed)'));
    }

    /* ---- Tier 2: Compact name list for all other tables ----
     * Even when the full list can't fit, the truncation message always states
     * the omitted count and explicitly names the escape hatch (query_db /
     * SHOW TABLES LIKE), so a target table sorted late is never silently
     * dropped with no trace it exists. */
    if (tier2.length > 0) {
      var other_budget = Math.floor((budget - total) * 0.8);
      if (other_budget < 60) other_budget = 60; // At least room for a few names
      lines.push(t('【其他表 ', '[Other tables (') + tier2.length + t(' 张）】', ')]'));
      var compact_names = [];
      var ctotal = 0;
      for (var t2 = 0; t2 < tier2.length; t2++) {
        var nt = tier2[t2].TABLE_NAME;
        var nc = tier2[t2].TABLE_COMMENT || '';
        var nr = Number(tier2[t2].TABLE_ROWS || 0);
        var entry = nt;
        if (nr > 0) entry += '~' + fmt_row_count(nr);
        if (nc) entry += '(' + nc.substring(0, 20) + ')';
        ctotal += entry.length + 2; // +2 for ", "
        if (ctotal > other_budget) {
          compact_names.push(t('…（+', '…(+') + (tier2.length - t2) +
                            t(' 张表未列出，表名可能不在此列表中；用 query_db 执行 ' +
                              "SHOW TABLES LIKE '%关键词%' 按名称检索，或 SHOW TABLES 获取完整列表）",
                              ' tables not shown, target table name may not appear in this list; ' +
                              "use query_db with SHOW TABLES LIKE '%keyword%' to search by name, " +
                              'or SHOW TABLES for the full list)'));
          break;
        }
        compact_names.push(entry);
      }
      lines.push(compact_names.join(', '));
    }

    if (fk_str.length > 0) lines.push(t('【JOIN路径】', '[FK/JOIN paths]') + fk_str);
    return lines.join('\n');
  } catch(e) { return ''; }
}

function build_schema_context(db, available_tokens, chat_opt, intent, user_msg, embeddings_ready) {
  /* Embeddings-based retrieval: semantic search already filters to the most
   * relevant tables, so we can be generous.  4–10 tables of full DDL is
   * usually fine for LLM context. */
  var n = Math.min(10, Math.max(4, Math.floor((available_tokens || 800) / 200)));
  var extra = {};
  if (chat_opt && chat_opt.schema_name) extra.schemas = [chat_opt.schema_name];
  var effective_intent = intent || analyze_intent(user_msg || A.user_message);

  var result;
  if (db && embeddings_ready) {
    result = call_schema_metadata(A.user_message, db, n, extra);
    if (result && result.length > 0) {
      result = t('【相关表 DDL（语义排序，FOREIGN KEY 可用于 JOIN）】\n',
                 '[Relevant Table DDL (semantic order, FOREIGN KEY usable for JOIN)]\n') + result;
    }
  }

  if (!result) {
    var candidate_tables = db ? infer_candidate_tables(db, effective_intent, user_msg || A.user_message) : [];
    var candidate_names = candidate_tables.map(function(c) { return c.table; });

    var stuff_threshold = cfg('schema_stuff_table_threshold', 40);
    if (db) {
      var table_count = get_table_count(db);
      if (table_count > stuff_threshold) {
        return build_catalog_pointer(db, candidate_names, table_count);
      }
    }

    var budget = Math.max(400, (available_tokens || 800) * 3);
    result = build_schema_context_fallback(db, budget, candidate_tables);
    if (effective_intent && effective_intent.need_join && candidate_names.length > 1) {
      var schema_graph = build_schema_graph(db);
      var join_hint = suggest_joins(schema_graph, candidate_names.slice(0, 4));
      if (join_hint) result += '\n' + join_hint;
    }
  }

  /* Hard safety net: schema context must never exceed MAX_SCHEMA_CHARS.
   * Even with the tiered approach above, edge cases (e.g. very wide tables,
   * long comments, large candidate match) can still blow up the prompt. */
  if (result && result.length > MAX_SCHEMA_CHARS) {
    var cut = result.lastIndexOf('\n', MAX_SCHEMA_CHARS - 80);
    if (cut < MAX_SCHEMA_CHARS * 0.5) cut = MAX_SCHEMA_CHARS - 60;
    result = result.substring(0, cut) +
             t('\n…[Schema 截断，原长 ', '\n…[Schema truncated, original ') +
             result.length +
             t(' 字符]', ' chars]');
  }

  return result || '';
}

function build_schema_graph(db) {
  if (!db) return {};
  try {
    var fk_rows = query(
      "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
      " FROM information_schema.KEY_COLUMN_USAGE" +
      " WHERE CONSTRAINT_SCHEMA='" + esc(db) + "' AND REFERENCED_TABLE_NAME IS NOT NULL"
    );
    var graph = {};
    if (!Array.isArray(fk_rows)) return graph;
    for (var i = 0; i < fk_rows.length; i++) {
      var f = fk_rows[i];
      if (!graph[f.TABLE_NAME])            graph[f.TABLE_NAME]            = [];
      if (!graph[f.REFERENCED_TABLE_NAME]) graph[f.REFERENCED_TABLE_NAME] = [];
      graph[f.TABLE_NAME].push({
        from: f.TABLE_NAME, from_col: f.COLUMN_NAME,
        to: f.REFERENCED_TABLE_NAME, to_col: f.REFERENCED_COLUMN_NAME
      });
      graph[f.REFERENCED_TABLE_NAME].push({
        from: f.REFERENCED_TABLE_NAME, from_col: f.REFERENCED_COLUMN_NAME,
        to: f.TABLE_NAME, to_col: f.COLUMN_NAME
      });
    }
    return graph;
  } catch(e) { return {}; }
}

function find_join_path(graph, src, dst) {
  if (src === dst) return [];
  var visited = {}, queue = [[src, []]];
  visited[src] = true;
  while (queue.length > 0) {
    var item = queue.shift(), node = item[0], edges = graph[node] || [];
    for (var i = 0; i < edges.length; i++) {
      var e = edges[i];
      if (visited[e.to]) continue;
      var path = item[1].concat([e]);
      if (e.to === dst) return path;
      visited[e.to] = true;
      queue.push([e.to, path]);
    }
  }
  return null;
}

function suggest_joins(graph, tables) {
  if (!tables || tables.length < 2) return '';
  var hints = [];
  for (var i = 1; i < tables.length; i++) {
    var path = find_join_path(graph, tables[0], tables[i]);
    if (path && path.length > 0)
      hints.push(path.map(function(e) {
        return 'JOIN ' + e.to + ' ON ' + e.from + '.' + e.from_col +
               ' = ' + e.to + '.' + e.to_col;
      }).join(' '));
  }
  return hints.length > 0
    ? t('【推荐JOIN路径（BFS）】\n', '[Recommended JOIN paths (BFS)]\n') + hints.join('\n')
    : '';
}

/**
 * Replaces the inline schema dump for large schemas (see build_schema_context).
 * Instead of trying to fit N hundred tables into the prompt at any level of
 * compression, this just states the table count, offers a short IDF-ranked
 * warm-start list (if the user message produced any candidates), and tells
 * the model to use list_tables / describe_table for everything else. This
 * is the "directory, not a phone book" approach — constant prompt cost
 * regardless of schema size, at the expense of 1-2 extra ReAct turns for
 * queries that need schema exploration.
 */
function build_catalog_pointer(db, candidate_names, total_tables) {
  var lines = [
    t('【Schema: ', '[Schema: ') + db + t('】共 ', '] has ') + total_tables +
    t(' 张表，数量较多，未在此处展开完整 DDL（避免撑爆上下文）。',
      ' tables — too many to inline full DDL here (would blow up the context window).')
  ];
  if (candidate_names && candidate_names.length > 0) {
    lines.push(
      t('【初步候选表（按关键词相关性排序，仅供参考，非最终答案）】',
        '[Preliminary candidate tables (ranked by keyword relevance — a starting point, not the final answer)]') +
      ' ' + candidate_names.slice(0, 8).join(', ')
    );
  }
  lines.push(t(
    '请先调用 list_tables（可选 keyword 参数，用于按关键词缩小范围）定位相关表，' +
    '再调用 describe_table 获取该表完整列定义；⛔ 在拿到 describe_table 结果之前，' +
    '禁止凭猜测编写涉及该表列名的 SQL。',
    'Call list_tables first (optionally with a keyword to narrow down) to locate relevant tables, ' +
    'then call describe_table to get that table\'s full column definitions; ⛔ never guess column ' +
    'names for a table you have not yet called describe_table on.'
  ));
  return lines.join('\n');
}

/**
 * Tool: list_tables — the "directory" half of the on-demand schema tool
 * pair. With a keyword, reuses the same IDF-weighted scoring as the inline
 * Tier1 builder (infer_candidate_tables) so results are consistent whether
 * a table shows up via the warm-start hint or an explicit tool call.
 * Without a keyword, returns a capped slice of the full catalog and tells
 * the model to narrow down rather than silently truncating with no
 * explanation.
 */
function tool_list_tables(db, args) {
  if (!db) return { ok: false, response: t('未选择数据库。', 'No database selected.'), error: 'no_db' };
  var keyword = String((args && args.keyword) || '').trim();
  var top_k   = Math.max(1, Math.min(50, Number(args && args.top_k) || 20));
  var catalog = get_schema_catalog(db);

  if (!catalog.tables.length)
    return { ok: true, response: t('当前库没有表。', 'No tables in current database.') };

  var by_name = {};
  for (var i = 0; i < catalog.tables.length; i++) by_name[catalog.tables[i].TABLE_NAME] = catalog.tables[i];

  if (keyword) {
    var ranked = infer_candidate_tables(db, null, keyword, top_k);
    if (!ranked.length) {
      return { ok: true, response:
        t('未找到与 "', 'No tables matched "') + keyword +
        t('" 相关的表。可尝试更换关键词，或不带 keyword 调用 list_tables 查看全部表。',
          '". Try a different keyword, or call list_tables without a keyword to browse all tables.') };
    }
    var lines = [t('【表检索: "', '[Table search: "') + keyword + t('"】共 ', '"] ') +
                 ranked.length + t(' 个候选（按相关性排序）：', ' candidates (ranked by relevance):')];
    for (var r = 0; r < ranked.length; r++) {
      var row = by_name[ranked[r].table] || {};
      var nr  = Number(row.TABLE_ROWS || 0);
      var line = (r + 1) + '. ' + ranked[r].table + (nr > 0 ? ' ~' + fmt_row_count(nr) + ' rows' : '');
      if (row.TABLE_COMMENT) line += ' -- ' + String(row.TABLE_COMMENT).substring(0, 60);
      lines.push(line);
    }
    lines.push(t('用 describe_table 获取具体表的完整列定义。',
                 'Use describe_table to get full column definitions for a specific table.'));
    return { ok: true, response: lines.join('\n') };
  }

  var total_n = catalog.tables.length;
  var show_n  = Math.min(total_n, top_k);
  var lines2  = [t('【当前库共 ', '[Current database has ') + total_n + t(' 张表】', ' tables]')];
  for (var j = 0; j < show_n; j++) {
    var tb = catalog.tables[j];
    var nr2 = Number(tb.TABLE_ROWS || 0);
    var l2  = tb.TABLE_NAME + (nr2 > 0 ? ' ~' + fmt_row_count(nr2) + ' rows' : '');
    if (tb.TABLE_COMMENT) l2 += ' -- ' + String(tb.TABLE_COMMENT).substring(0, 60);
    lines2.push(l2);
  }
  if (total_n > show_n) {
    lines2.push(t('…（+' + (total_n - show_n) + ' 张表未显示；建议带 keyword 参数缩小范围）',
                  '…(+' + (total_n - show_n) + ' more tables not shown; pass a keyword to narrow down)'));
  }
  return { ok: true, response: lines2.join('\n') };
}

/**
 * Tool: describe_table — the "detail" half of the pair. Explicitly
 * requested, so unlike the inline Tier1 DDL builder this does NOT apply
 * MAX_COLS/dyn_max_cols truncation: a single table's full column list is a
 * bounded, known-small cost, and truncating it would just force another
 * round trip. Accepts either a single table_name or a small table_names
 * batch (capped at 5) so the model can pull a couple of join partners in
 * one call instead of one tool call per table.
 */
function tool_describe_table(db, args) {
  if (!db) return { ok: false, response: t('未选择数据库。', 'No database selected.'), error: 'no_db' };

  var names = [];
  if (Array.isArray(args && args.table_names)) names = args.table_names;
  else if (args && args.table_name) names = [args.table_name];
  names = names.map(function(n) { return String(n || '').trim(); }).filter(function(n) { return n; });
  if (!names.length) {
    return { ok: false,
             response: t('describe_table 缺少 table_name 参数。', 'describe_table missing table_name argument.'),
             error: 'missing_table_name' };
  }
  if (names.length > 5) names = names.slice(0, 5);

  var catalog = get_schema_catalog(db);
  var by_name_lc = {};
  for (var i = 0; i < catalog.tables.length; i++)
    by_name_lc[String(catalog.tables[i].TABLE_NAME).toLowerCase()] = catalog.tables[i];

  var out = [];
  for (var ni = 0; ni < names.length; ni++) {
    var want  = names[ni];
    var match = by_name_lc[want.toLowerCase()];
    if (!match) {
      var suggestions = [];
      for (var tk in by_name_lc) {
        if (!by_name_lc.hasOwnProperty(tk)) continue;
        if (tk.indexOf(want.toLowerCase()) !== -1 || want.toLowerCase().indexOf(tk) !== -1)
          suggestions.push(by_name_lc[tk].TABLE_NAME);
      }
      out.push(
        t('表 "', 'Table "') + want + t('" 不存在。', '" does not exist.') +
        (suggestions.length
          ? t(' 相似表名：', ' Similar table names: ') + suggestions.slice(0, 5).join(', ')
          : t(' 请用 list_tables 检索正确表名。', ' Use list_tables to search for the correct name.'))
      );
      continue;
    }

    var tn   = match.TABLE_NAME;
    var cols = (catalog.col_map[tn] || []).slice();
    var descs = [];
    for (var ci = 0; ci < cols.length; ci++) {
      var col = cols[ci];
      var d = col.COLUMN_NAME + ' ' + col.COLUMN_TYPE;
      if (col.COLUMN_KEY === 'PRI')      d += ' PRIMARY KEY';
      else if (col.COLUMN_KEY === 'UNI') d += ' UNIQUE';
      else if (col.COLUMN_KEY === 'MUL') d += ' INDEX';
      if (col.COLUMN_COMMENT) d += " COMMENT '" + String(col.COLUMN_COMMENT).substring(0, 60) + "'";
      descs.push('  ' + d);
    }

    var header = tn +
      (match.TABLE_COMMENT ? ' -- ' + match.TABLE_COMMENT : '') +
      (Number(match.TABLE_ROWS || 0) > 0 ? ' (~' + fmt_row_count(Number(match.TABLE_ROWS)) + ' rows)' : '');
    var block = [header, descs.join('\n')];

    /* FK edges touching this table specifically (either side). */
    if (catalog.fk_str) {
      var fk_lines = [];
      var edges = catalog.fk_str.split(' | ');
      for (var fe = 0; fe < edges.length; fe++) {
        if (edges[fe].indexOf(tn + '.') === 0 || edges[fe].indexOf('→' + tn + '.') !== -1)
          fk_lines.push(edges[fe]);
      }
      if (fk_lines.length) block.push(t('FK: ', 'FK: ') + fk_lines.join(' | '));
    }

    out.push(block.join('\n'));
  }

  return { ok: true, response: out.join('\n\n') };
}