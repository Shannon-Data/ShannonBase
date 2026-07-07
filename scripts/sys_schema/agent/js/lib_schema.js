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

function build_schema_context_fallback(db, budget) {
  if (!db) return '';
  budget = budget || 1200;
  var MAX_TABLES = 25, MAX_COLS = 10;
  try {
    var tbl_rows = query(
      "SELECT TABLE_NAME, TABLE_COMMENT FROM information_schema.TABLES" +
      " WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE'" +
      " ORDER BY TABLE_NAME"
    );
    if (!Array.isArray(tbl_rows) || !tbl_rows.length) return '';

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

    function col_prio(k) { return k==='PRI'?0:k==='UNI'?1:k==='MUL'?2:3; }
    function compact_type(tp) {
      return tp.replace(/varchar\(\d+\)/i,'VC').replace(/char\(\d+\)/i,'CH')
               .replace(/bigint\(\d+\) unsigned/i,'UBIGINT').replace(/bigint\(\d+\)/i,'BIGINT')
               .replace(/int\(\d+\) unsigned/i,'UINT').replace(/int\(\d+\)/i,'INT')
               .replace(/tinyint\(1\)/i,'BOOL').replace(/tinyint\(\d+\)/i,'TINT')
               .replace(/decimal\([\d,]+\)/i,'DEC').replace(/timestamp/i,'TS')
               .replace(/datetime/i,'DT').replace(/(long|medium|tiny)?text/i,'TEXT')
               .toUpperCase();
    }

    var lines = [t('【Schema（fallback）: ', '[Schema(fallback): ') + db + t('】', ']')];
    var total = 0;
    if (tbl_rows.length > MAX_TABLES) {
      lines.push(t('表共 ', 'Tables(') + tbl_rows.length + t(' 张: ', '): ') +
                 tbl_rows.map(function(r){ return r.TABLE_NAME; }).join(', '));
    } else {
      for (var ti = 0; ti < tbl_rows.length; ti++) {
        var tn   = tbl_rows[ti].TABLE_NAME;
        var tc   = tbl_rows[ti].TABLE_COMMENT || '';
        var cols = (col_map[tn] || []).slice();
        cols.sort(function(a,b){ return col_prio(a.COLUMN_KEY)-col_prio(b.COLUMN_KEY); });
        var descs = [], show = Math.min(cols.length, MAX_COLS);
        for (var ci2 = 0; ci2 < show; ci2++) {
          var col  = cols[ci2];
          var desc = col.COLUMN_NAME + ' ' + compact_type(col.COLUMN_TYPE);
          if (col.COLUMN_KEY==='PRI')      desc += ' PK';
          else if (col.COLUMN_KEY==='UNI') desc += ' UQ';
          else if (col.COLUMN_KEY==='MUL') desc += ' IDX';
          if (col.COLUMN_COMMENT) desc += '/*' + col.COLUMN_COMMENT.substring(0,30) + '*/';
          descs.push(desc);
        }
        if (cols.length > MAX_COLS) descs.push('…+' + (cols.length-MAX_COLS) + 'cols');
        var line = tn + '(' + descs.join(',') + ')';
        if (tc && tc.length > 0) line += '--' + tc.substring(0,30);
        total += line.length;
        if (total > budget) {
          lines.push(t('（剩余: ', '(Remaining: ') +
                     tbl_rows.slice(ti).map(function(x){ return x.TABLE_NAME; }).join(',') +
                     t('）', ')'));
          break;
        }
        lines.push(line);
      }
    }
    if (fk_str.length > 0) lines.push(t('【JOIN路径】', '[FK/JOIN paths]') + fk_str);
    return lines.join('\n');
  } catch(e) { return ''; }
}

function build_schema_context(db, available_tokens, chat_opt, intent, user_msg) {
  var n = Math.min(12, Math.max(5, Math.floor((available_tokens || 800) / 200)));
  var extra = {};
  if (chat_opt && chat_opt.schema_name) extra.schemas = [chat_opt.schema_name];
  var effective_intent = intent || analyze_intent(user_msg || A.user_message);
  var candidate_tables = [];
  if (db) candidate_tables = infer_candidate_tables(db, effective_intent, user_msg || A.user_message);
  var context_prefix = '';
  if (candidate_tables.length > 0) {
    context_prefix += t('【候选表】', '[Candidate tables]') + ' ' + candidate_tables.join(', ') + '\n';
  }
  if (check_schema_embeddings_ready(db)) {
    var result = call_schema_metadata(A.user_message, db, n, extra);
    if (result && result.length > 0)
      return context_prefix + t('【相关表 DDL（语义排序，FOREIGN KEY 可用于 JOIN）】\n',
                                '[Relevant Table DDL (semantic order, FOREIGN KEY usable for JOIN)]\n') + result;
  }
  var budget = Math.max(400, (available_tokens || 800) * 3);
  var fallback = build_schema_context_fallback(db, budget);
  if (candidate_tables.length > 0) fallback = context_prefix + fallback;
  if (effective_intent && effective_intent.need_join && candidate_tables.length > 1) {
    var schema_graph = build_schema_graph(db);
    var join_hint = suggest_joins(schema_graph, candidate_tables.slice(0, 4));
    if (join_hint) fallback += '\n' + join_hint;
  }
  return fallback;
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