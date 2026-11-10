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
  // ML_RETRIEVE_SCHEMA_METADATA returns relevance-ranked abridged CREATE TABLE statements.
  var opts = { include_comments: true };
  extra_opts = extra_opts || {};
  if (Array.isArray(extra_opts.tables) && extra_opts.tables.length) {
    opts.tables = extra_opts.tables;
  } else if (Array.isArray(extra_opts.schemas) && extra_opts.schemas.length) {
    opts.schemas = extra_opts.schemas;
  } else if (db) {
    opts.schemas = [db];
  }

  try {
    exec_sql("SET @_sm_out = NULL");
    exec_sql(
      "CALL sys.ML_RETRIEVE_SCHEMA_METADATA(" +
      "'" + esc(user_msg) + "'," +
      "@_sm_out," +
      "CAST('" + esc(JSON.stringify(opts)) + "' AS JSON))"
    );
    var rows = query_checked("SELECT @_sm_out AS result");
    if (!rows || !Array.isArray(rows) || !rows.length) return '';
    var result = String(rows[0].result || '');
    if (result.indexOf('No relevant tables found') !== -1) return '';
    return result;
  } catch(e) { return ''; }
}

// Total schema context, in characters.
var MAX_SCHEMA_CHARS_DEFAULT = 4000;

function max_schema_chars() {
  try { return Math.max(MAX_SCHEMA_CHARS_DEFAULT, schema_char_budget()); }
  catch (e) { return MAX_SCHEMA_CHARS_DEFAULT; }
}
var MAX_COLS           = 16;   // Absolute ceiling on columns per table (dynamic budget applies below this)
var MIN_COLS           = 4;    // Never show fewer than this many columns for a Tier1 table
var MAX_CANDIDATE_DDL  = 10;   // Max tables for which we output full column DDL
var SCHEMA_CACHE_TTL_MS = 5 * 60 * 1000; // 5 min: catalog cache freshness window

var LOW_VALUE_COLUMN_PATTERN = new RegExp(
  '^(created_at|create_time|gmt_create|inserted_at|create_by|created_by|' +
  'updated_at|update_time|gmt_modified|gmt_update|last_modified|modified_at|updated_by|update_by|' +
  'deleted_at|delete_time|is_deleted|del_flag|is_del|deleted|' +
  'version|row_version|_version|extra|remark|remarks|memo)$', 'i'
);

function is_low_value_column(name) {
  return LOW_VALUE_COLUMN_PATTERN.test(String(name || '').trim());
}

// Backtick-quote an identifier, reusing the shared escaping.
function ident(s) {
  return '`' + esc_ident(s) + '`';
}

// "schema.table" → { schema, table }; the table part is everything after the first dot.
function split_table_ref(name) {
  var s   = String(name == null ? '' : name).trim();
  var dot = s.indexOf('.');
  if (dot < 0) return { schema: '', table: s, qualified: false };
  return { schema: s.substring(0, dot), table: s.substring(dot + 1), qualified: true };
}

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

// Render sample rows as a compact `(v1, v2, v3) | (v1, v2, v3)` string.
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

// Whether to attach sample rows to Tier1 tables at all.
function sample_rows_enabled() {
  var co = get_chat_options();
  return !(co && co.schema_sample_rows_enabled === false);
}

function is_opaque_identifier(name, kind) {
  var n = String(name || '').trim();
  if (!n) return true;
  var low = n.toLowerCase();

  if (/^(?:table|tbl|tab|t)[_-]?\d+$/.test(low)) return true;
  if (/^(?:col|column|field|fld|f|c)[_-]?\d+$/.test(low)) return true;
  if (/^[tc][_-]?\d+$/.test(low)) return true;
  if (/^(?:tmp|data|item|value|val)[_-]?\d+$/.test(low)) return true;

  if (n.length <= 3 && /[0-9]/.test(n) && /^[a-zA-Z][a-zA-Z0-9_]*$/.test(n))
    return true;
  return false;
}

function semantic_opacity_score(table_name, table_comment, cols) {
  cols = Array.isArray(cols) ? cols : [];
  var table_score = is_opaque_identifier(table_name, 'table') ? 1 : 0;
  if (table_comment && String(table_comment).trim()) table_score *= 0.25;

  if (!cols.length) return table_score;
  var opaque_cols = 0, useful_comments = 0;
  for (var i = 0; i < cols.length; i++) {
    if (is_opaque_identifier(cols[i].COLUMN_NAME, 'column')) opaque_cols++;
    if (cols[i].COLUMN_COMMENT && String(cols[i].COLUMN_COMMENT).trim()) useful_comments++;
  }
  var col_score = opaque_cols / cols.length;
  if (useful_comments > 0) col_score *= Math.max(0.25, 1 - useful_comments / cols.length);
  return 0.35 * table_score + 0.65 * col_score;
}

function table_needs_semantic_sampling(table_name, table_comment, cols) {
  var threshold = Number(cfg('schema_semantic_opacity_threshold', 0.55));
  return semantic_opacity_score(table_name, table_comment, cols) >= threshold;
}

function extract_schema_metadata_table_names(text, db) {
  var out = [], seen = {};
  var re = /CREATE\s+TABLE\s+(?:`([^`]+)`\.)?`([^`]+)`/gi, m;
  while ((m = re.exec(String(text || '')))) {
    if (m[1] && db && String(m[1]).toLowerCase() !== String(db).toLowerCase()) continue;
    var tn = m[2];
    if (!seen[tn]) { seen[tn] = true; out.push(tn); }
  }
  return out;
}

function build_semantic_sample_context(db, preferred_tables, char_budget) {
  if (!db || !sample_rows_enabled()) return '';
  char_budget = Math.max(200, Number(char_budget || 1200));

  var catalog;
  try { catalog = get_schema_catalog(db); } catch (e) { return ''; }
  var tbl_rows = catalog.tables || [], col_map = catalog.col_map || {};
  var table_by_name = {};
  for (var i = 0; i < tbl_rows.length; i++) table_by_name[tbl_rows[i].TABLE_NAME] = tbl_rows[i];

  var ordered = [], seen = {};
  function add_table(tn) {
    if (!tn || seen[tn] || !table_by_name[tn]) return;
    seen[tn] = true;
    ordered.push(tn);
  }
  (preferred_tables || []).forEach(add_table);

  // If retrieval has little lexical signal (table1/col1-style schema), add opaque tables too.
  var opaque_cap = Math.max(1, Number(cfg('schema_opaque_sample_max_tables', 12)));
  var opaque_all_threshold = Math.max(1, Number(cfg('schema_opaque_sample_all_tables_threshold', 16)));
  if (tbl_rows.length <= opaque_all_threshold) opaque_cap = Math.min(tbl_rows.length, 16);
  for (var j = 0; j < tbl_rows.length && ordered.length < opaque_cap; j++) {
    var tr = tbl_rows[j], cols0 = col_map[tr.TABLE_NAME] || [];
    if (table_needs_semantic_sampling(tr.TABLE_NAME, tr.TABLE_COMMENT || '', cols0))
      add_table(tr.TABLE_NAME);
  }

  var row_limit = Math.max(1, Number(cfg('schema_opaque_sample_rows_limit', 3)));
  var col_limit = Math.max(2, Number(cfg('schema_opaque_sample_max_cols', 8)));
  var lines = [];
  var used = 0, sampled = 0;

  for (var k = 0; k < ordered.length && sampled < opaque_cap; k++) {
    var tn = ordered[k], tr2 = table_by_name[tn], cols = col_map[tn] || [];
    if (!table_needs_semantic_sampling(tn, tr2.TABLE_COMMENT || '', cols)) continue;

    var names = cols.slice(0, col_limit).map(function(c) { return c.COLUMN_NAME; });
    if (!names.length) continue;
    var rows = get_sample_rows(db, tn, names, row_limit);
    if (!rows || !rows.length) continue;

    var line = tn + '(' + names.join(',') + ')\n  ' +
      t('语义样本：', 'semantic samples: ') + format_sample_rows(rows, names, 24);
    if (used + line.length > char_budget) break;
    lines.push(line);
    used += line.length + 1;
    sampled++;
  }

  if (!lines.length) return '';
  return t('【弱语义 Schema 的真实数据样本】\n'
           + '以下值仅用于推断 table1/col1 等无语义标识符的可能含义；属于概率性线索，不是权威 schema 定义，写操作不得仅凭该推断执行。\n',
           '[Real samples for semantically opaque schema]\n'
           + 'These values are probabilistic hints for understanding opaque identifiers such as table1/col1, not authoritative schema definitions; writes must not rely on these inferences alone.\n') +
         lines.join('\n');
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
      if (cached && cached.db === db && cached.fp === fingerprint &&
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
    var serialized = JSON.stringify({ db: db, fp: fingerprint, ts: Date.now(), data: data });

    if (serialized.length < 2 * 1024 * 1024) {
      exec_sql("SET " + cache_var + " = '" + esc(serialized) + "'");
    }
  } catch (e) { /* caching is best-effort; ignore failures */ }

  return data;
}

function fmt_row_count(nr) {
  nr = Number(nr) || 0;
  return nr >= 1000000 ? (nr/1000000).toFixed(1)+'M' :
         nr >= 1000    ? (nr/1000).toFixed(1)+'K'    : String(nr);
}

// Tiered schema context builder.
function build_schema_context_fallback(db, budget, candidate_tables) {
  if (!db) return '';
  budget = Math.min(budget || 1200, max_schema_chars());
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

    var score_map = {};
    if (Array.isArray(candidate_tables) && candidate_tables.length > 0) {
      for (var ct = 0; ct < candidate_tables.length; ct++) {
        var c = candidate_tables[ct];
        var cname = typeof c === 'string' ? c : c.table;
        var cscore = typeof c === 'string' ? 1 : c.score;
        if (cname) score_map[String(cname).toLowerCase()] = cscore;
      }
    }

    var opaque_promote_cap = Math.max(1, Number(cfg('schema_opaque_sample_max_tables', 12)));
    var opaque_all_threshold = Math.max(1, Number(cfg('schema_opaque_sample_all_tables_threshold', 16)));
    if (tbl_rows.length <= opaque_all_threshold) opaque_promote_cap = Math.min(tbl_rows.length, 16);
    var existing_candidate_count = Object.keys(score_map).length;
    if (existing_candidate_count === 0 || tbl_rows.length <= opaque_promote_cap) {
      var promoted = 0;
      for (var opi = 0; opi < tbl_rows.length && promoted < opaque_promote_cap; opi++) {
        var opn = tbl_rows[opi].TABLE_NAME;
        var opc = col_map[opn] || [];
        if (table_needs_semantic_sampling(opn, tbl_rows[opi].TABLE_COMMENT || '', opc)) {
          if (!score_map.hasOwnProperty(String(opn).toLowerCase())) {
            score_map[String(opn).toLowerCase()] = 0.25;
            promoted++;
          }
        }
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

    tier1.sort(function(a, b) { return b.score - a.score; });

    var lines = [t('【Schema: ', '[Schema: ') + db +
                 t('】（候选表含完整列定义，其余表仅列名。需要某表详情时用 query_db 执行 DESC/ SHOW CREATE TABLE，' +
                   '或 SHOW TABLES LIKE \'%关键词%\' 按名称检索）',
                   '] (Candidate tables have full column definitions; others are name-only. ' +
                   'Use query_db with DESC / SHOW CREATE TABLE for details on other tables, ' +
                   'or SHOW TABLES LIKE \'%keyword%\' to search by name.)')];
    var total = 0;

    var ddl_count = 0;
    var ddl_budget = Math.floor(budget * 0.7); // 70% of budget for DDL
    // Normalize against the *highest* score in Tier1, not the score sum.
    var max_score = 0.1;
    for (var si = 0; si < tier1.length; si++) max_score = Math.max(max_score, tier1[si].score);

    var sample_max_tables = cfg('schema_sample_rows_max_tables', 2);
    var sample_row_limit  = cfg('schema_sample_rows_limit', 2);
    var opaque_row_limit  = cfg('schema_opaque_sample_rows_limit', 3);
    var opaque_sample_cap = Math.max(1, Number(cfg('schema_opaque_sample_max_tables', 12)));
    if (tbl_rows.length <= opaque_all_threshold) opaque_sample_cap = Math.min(tbl_rows.length, 16);
    var opaque_sampled    = 0;
    var samples_on        = sample_rows_enabled();

    var candidate_ddl_limit = Math.max(
      MAX_CANDIDATE_DDL,
      Math.min(Math.max(1, Number(opaque_sample_cap)), 16)
    );

    for (var t1 = 0; t1 < tier1.length && ddl_count < candidate_ddl_limit; t1++) {
      var tn   = tier1[t1].row.TABLE_NAME;
      var tc   = tier1[t1].row.TABLE_COMMENT || '';
      var cols = (col_map[tn] || []).slice();
      cols.sort(function(a,b){
        var pa = col_prio(a.COLUMN_KEY), pb = col_prio(b.COLUMN_KEY);
        if (pa !== pb) return pa - pb;

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

      var rows_approx = Number(tier1[t1].row.TABLE_ROWS || 0);
      if (rows_approx > 0) line += ' ~' + fmt_row_count(rows_approx) + ' rows';

      var sample_line = '';
      var opaque_table = table_needs_semantic_sampling(tn, tc, cols);
      var should_sample = samples_on && show > 0 &&
        ((opaque_table && opaque_sampled < opaque_sample_cap) || t1 < sample_max_tables);
      if (should_sample) {
        var this_limit = opaque_table ? opaque_row_limit : sample_row_limit;
        var sample_rows = get_sample_rows(db, tn, shown_names, this_limit);
        if (sample_rows && sample_rows.length) {
          sample_line = '  ' +
            (opaque_table ? t('语义样本：', 'semantic samples: ') : t('示例：', 'e.g. ')) +
            format_sample_rows(sample_rows, shown_names, opaque_table ? 24 : 20);
          if (opaque_table) opaque_sampled++;
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

  var n = Math.min(10, Math.max(4, Math.floor((available_tokens || 800) / 200)));
  var extra = {};
  if (chat_opt && chat_opt.schema_name) extra.schemas = [chat_opt.schema_name];
  var effective_intent = intent || analyze_intent(user_msg || A.user_message);

  var result;
  var used_metadata_retrieval = false;
  if (db && embeddings_ready) {
    result = call_schema_metadata(A.user_message, db, n, extra);
    if (result && result.length > 0) {
      used_metadata_retrieval = true;
      var retrieved_tables = extract_schema_metadata_table_names(result, db);
      var semantic_samples = build_semantic_sample_context(
        db, retrieved_tables, Math.floor(max_schema_chars() * 0.35)
      );
      result = t('【相关表 DDL（语义排序，FOREIGN KEY 可用于 JOIN）】\n',
                 '[Relevant Table DDL (semantic order, FOREIGN KEY usable for JOIN)]\n') +
               result +
               (semantic_samples ? '\n' + semantic_samples : '');
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

  // Hard safety net: schema context must never exceed the budget.
  var schema_cap = max_schema_chars();
  if (result && result.length > schema_cap) {
    var cut = result.lastIndexOf('\n', schema_cap - 80);
    if (cut < schema_cap * 0.5) cut = schema_cap - 60;
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

// Replaces the inline schema dump for large schemas (see build_schema_context).
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

// Tool: list_tables — the "directory" half of the on-demand schema tool pair.
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

// Tool: describe_table — the "detail" half of the pair.
function tool_describe_table(db, args) {
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

  /* A qualified name resolves without USE; only a bare one needs the session db. */
  var needs_db = false;
  for (var nb = 0; nb < names.length; nb++)
    if (names[nb].indexOf('.') === -1) needs_db = true;
  if (!db && needs_db)
    return { ok: false, response: t('未选择数据库。', 'No database selected.'), error: 'no_db' };

  var catalog = get_schema_catalog(db);
  var by_name_lc = {};
  for (var i = 0; i < catalog.tables.length; i++)
    by_name_lc[String(catalog.tables[i].TABLE_NAME).toLowerCase()] = catalog.tables[i];

  var out = [];
  for (var ni = 0; ni < names.length; ni++) {
    var ref          = split_table_ref(names[ni]);
    var schema_name  = ref.schema;
    var bare_name    = ref.table;
    var lookup_key   = bare_name.toLowerCase();

    var match      = by_name_lc[lookup_key];
    var cross_cols = null;  // columns fetched on-demand for cross-db tables

    /* Cross-database reference: catalog doesn't cover it — query I_S directly. */
    if (!match && schema_name && schema_name.toLowerCase() !== db.toLowerCase()) {
      var xrows = query(
        "SELECT TABLE_NAME, TABLE_ROWS, TABLE_COMMENT FROM information_schema.TABLES" +
        " WHERE TABLE_SCHEMA='" + esc(schema_name) + "' AND TABLE_NAME='" + esc(bare_name) + "'" +
        " AND TABLE_TYPE='BASE TABLE'"
      );
      if (Array.isArray(xrows) && xrows.length) {
        match = xrows[0];
        cross_cols = query(
          "SELECT COLUMN_NAME, COLUMN_TYPE, COLUMN_KEY, COLUMN_COMMENT" +
          " FROM information_schema.COLUMNS" +
          " WHERE TABLE_SCHEMA='" + esc(schema_name) + "' AND TABLE_NAME='" + esc(bare_name) + "'" +
          " ORDER BY ORDINAL_POSITION"
        );
      }
    }

    if (!match) {
      var search_for = schema_name ? bare_name : names[ni];
      var suggestions = [];
      for (var tk in by_name_lc) {
        if (!by_name_lc.hasOwnProperty(tk)) continue;
        if (tk.indexOf(search_for.toLowerCase()) !== -1 || search_for.toLowerCase().indexOf(tk) !== -1)
          suggestions.push(by_name_lc[tk].TABLE_NAME);
      }
      var display = schema_name ? schema_name + '.' + bare_name : names[ni];
      out.push(
        t('表 "', 'Table "') + display + t('" 不存在。', '" does not exist.') +
        (suggestions.length
          ? t(' 相似表名：', ' Similar table names: ') + suggestions.slice(0, 5).join(', ')
          : t(' 请用 list_tables 检索正确表名。', ' Use list_tables to search for the correct name.'))
      );
      continue;
    }

    var tn   = match.TABLE_NAME;
    var cols = (cross_cols && Array.isArray(cross_cols))
      ? cross_cols.slice()
      : (catalog.col_map[tn] || []).slice();
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

    // FK edges touching this table specifically (either side).
    if (!cross_cols && catalog.fk_str) {
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

// Tool: check_secondary_load — RAPID engine load state, qualified names only.
function tool_check_secondary_load(args) {
  var cs_name = String((args && args.table_name) || '').trim();
  var ref     = split_table_ref(cs_name);
  if (!ref.qualified || !ref.schema || !ref.table || ref.table.indexOf('.') !== -1)
    return { ok: false,
             response: t('table_name 必须是 schema.table 格式，例如 shannon_ml.census_train',
                         'table_name must be schema.table format, e.g. shannon_ml.census_train'),
             error: 'invalid_table_format' };

  var cs_sql = "SELECT rt.LOAD_STATUS, rt.NROWS, rt.LOAD_PROGRESS, rt.SIZE_BYTES, " +
               "rt.LOAD_START_TIMESTAMP, rt.LOAD_END_TIMESTAMP, rt.LOAD_TYPE " +
               "FROM performance_schema.rpd_tables rt " +
               "JOIN performance_schema.rpd_table_id rti ON rt.ID = rti.ID " +
               "WHERE rti.SCHEMA_NAME = '" + esc(ref.schema) + "' " +
               "AND rti.TABLE_NAME = '" + esc(ref.table) + "'";
  try {
    var cs_res = query_checked(cs_sql);
    if (Array.isArray(cs_res) && cs_res.length > 0) {
      return { ok: true, loaded: true,
               response: t('✅ 表 ' + cs_name + ' 已加载到 RAPID 引擎：\n',
                           '✅ Table ' + cs_name + ' is loaded into RAPID engine:\n') +
                         compress(rows_to_text(cs_res), 1500),
               sql: cs_sql };
    }
    return { ok: true, loaded: false,
             response: t('❌ 表 ' + cs_name + ' 尚未加载到 RAPID 引擎。\n请先执行：\n' +
                         '  ALTER TABLE ' + cs_name + ' SECONDARY_LOAD;\n' +
                         '加载完成后再重试 ml_train。',
                         '❌ Table ' + cs_name + ' is NOT loaded into RAPID engine.\nPlease run:\n' +
                         '  ALTER TABLE ' + cs_name + ' SECONDARY_LOAD;\n' +
                         'Then retry ml_train after loading completes.'),
             sql: cs_sql };
  } catch (e) {
    return { ok: false,
             response: t('check_secondary_load 执行失败：', 'check_secondary_load failed: ') + String(e),
             error: 'check_secondary_load_failed' };
  }
}