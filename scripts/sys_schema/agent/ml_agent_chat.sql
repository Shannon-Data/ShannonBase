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
-- 
-- javascript stored function must be follow the pattern:
-- newline
-- CREATE DEFINER='mysql.sys'@'localhost' FUNCTION shannon_chat(
--    user_message     TEXT,
--    conversation_id  VARCHAR(64)
-- ) RETURNS TEXT SQL SECURITY INVOKER LANGUAGE JAVASCRIPT 
-- AS $$
-- 
-- function shannon_agent_run(user_message, conversation_id) {
--  // JavaScript function body
-- }
-- 
-- $$;
-- newline

DROP FUNCTION IF EXISTS shannon_agent_default;

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION shannon_agent_default(
    user_message     TEXT,
    conversation_id  VARCHAR(64)
) RETURNS LONGTEXT SQL SECURITY INVOKER LANGUAGE JAVASCRIPT
AS $$

function shannon_agent_run(user_message, conversation_id) {

  /*
   * Language detection  –  MUST run before any t() call.
   * Any CJK Unified Ideograph in the user message → 'zh'.
   * Everything else (Latin, Arabic, Cyrillic …) → 'en'.
   * */
  function detect_lang(text) {
    return /[\u4e00-\u9fff\u3400-\u4dbf]/.test(String(text || '')) ? 'zh' : 'en';
  }
  var lang = detect_lang(user_message);

  /* Inline translation helper – reads `lang` via closure at call-time */
  function t(zh, en) { return lang === 'zh' ? zh : en; }

  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/\\/g, '\\\\')
      .replace(/'/g,  "\\'");
  }

  function query(sql) {
    try   { return JSON.parse(sys.exec_sql(sql)); }
    catch (e) { return { error: String(e) }; }
  }

  function rows_to_text(rows, limit) {
    limit = limit || 150;
    if (!Array.isArray(rows)) {
      if (rows && rows.error)
        return t('执行出错：', 'Error: ') + rows.error;
      if (rows && rows.affected_rows !== undefined) {
        if (rows.columns) {
          return t('（查询结果为空）', '(No results)');
        }
        return t('执行成功，影响行数：', 'Success, rows affected: ') + rows.affected_rows;
      }
      return JSON.stringify(rows);
    }
    if (rows.length === 0) return t('（查询结果为空）', '(No results)');

    var cols = Object.keys(rows[0]);
    var cap  = Math.min(rows.length, limit);
    var out  = [t('共 ', 'Total ') + rows.length + t(' 条：', ' rows:')];
    for (var i = 0; i < cap; i++) {
      out.push(t('行', 'Row') + (i + 1) + ':');
      for (var c = 0; c < cols.length; c++) {
        var v = rows[i][cols[c]];
        out.push('  ' + cols[c] + ': ' + (v === null ? 'NULL' : String(v)));
      }
    }
    if (rows.length > cap)
      out.push(t('（仅展示前 ', '(Showing first ') + cap + t(' 条）', ' rows)'));
    return out.join('\n');
  }

  function compress(text, max_chars) {
    max_chars = max_chars || 700;
    if (!text || text.length <= max_chars) return text;
    return text.substring(0, max_chars) +
           t('\n…[截断，原长 ', '\n…[truncated, original ') +
           text.length +
           t(' 字符]', ' chars]');
  }

  function think_suffix() {
    return _last_think ? '\n[think]\n' + _last_think : '';
  }

  function strip_think_tags(text) {
    if (!text) return text;
    return text.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
  }

  function est_tok(s) { return Math.ceil((s || '').length / 3); }
  function gen_query_id() { return Math.random().toString(36).substring(2, 10); }
  function save_secret_api_key(real_key) {
    if (!real_key || real_key === '***') return;
    try {
      sys.exec_sql("SET @chat_api_key = '" + esc(real_key) + "'");
    } catch (e) {}
  }

  function load_secret_api_key() {
    try {
      var rows = query("SELECT @chat_api_key AS k");
      if (Array.isArray(rows) && rows.length && rows[0].k) return String(rows[0].k);
    } catch (e) {}
    return '';
  }

  var _cached_chat_opt = null;
  function get_chat_options() {
    if (_cached_chat_opt) return _cached_chat_opt;
    try {
      var rows = query("SELECT @chat_options AS opt");
      if (!rows || !Array.isArray(rows) || !rows.length || !rows[0].opt) return {};
      _cached_chat_opt = JSON.parse(rows[0].opt);
      if (_cached_chat_opt.model_options) {
        var cur_key = _cached_chat_opt.model_options.api_key;
        if (!cur_key || cur_key === '***') {
          var real_key = load_secret_api_key();
          if (real_key) _cached_chat_opt.model_options.api_key = real_key;
        }
      }      
      return _cached_chat_opt;
    } catch(e) { return {}; }
  }

  function cfg(key, default_val) {
    var co = get_chat_options();
    if (!co || co[key] === undefined || co[key] === null) return default_val;
    var v = Number(co[key]);
    return (isFinite(v) && v > 0) ? v : default_val;
  }

  /* ML helpers */
  function ml_generate(prompt, extra) {
    var chat_opt   = get_chat_options();
    var model_opts = (chat_opt && chat_opt.model_options) ? chat_opt.model_options : {};
    /* `language` defaults to auto-detected lang; caller may override via extra */
    var o = Object.assign({
      task: 'generation', model_id: 'Qwen3.5-2B-ONNX',
      language: lang,
      temperature: 0.25, max_tokens: 1200,
      top_p: 0.95, repeat_penalty: 1.1,
      frequency_penalty: 0.0, presence_penalty: 0.0
    }, model_opts, extra || {});

    var sql =
      "SELECT sys.ML_GENERATE('" + esc(prompt) + "'," +
      "JSON_OBJECT(" +
      "'task','"             + esc(o.task)             + "'," +
      "'model_id','"         + esc(o.model_id)         + "'," +
      "'language','"         + esc(o.language)         + "'," +
      "'temperature',"       + Number(o.temperature)   + "," +
      "'max_tokens',"        + Number(o.max_tokens)    + "," +
      "'top_p',"             + Number(o.top_p)         + "," +
      "'repeat_penalty',"    + Number(o.repeat_penalty)    + "," +
      "'frequency_penalty'," + Number(o.frequency_penalty) + "," +
      "'presence_penalty',"  + Number(o.presence_penalty);

    if (o.provider)           sql += ",'provider','"           + esc(o.provider)           + "'";
    if (o.endpoint)           sql += ",'endpoint','"           + esc(o.endpoint)           + "'";
    if (o.api_key)            sql += ",'api_key','"            + esc(o.api_key)            + "'";
    if (o.workspace_id)       sql += ",'workspace_id','"       + esc(o.workspace_id)       + "'";
    if (o.region)             sql += ",'region','"             + esc(o.region)             + "'";
    if (o.api_config)         sql += ",'api_config','"         + esc(o.api_config)         + "'";

    /* DeepSeek thinking mode */
    if (o.deepseek_thinking !== undefined && o.deepseek_thinking !== '')
      sql += ",'deepseek_thinking','" + esc(String(o.deepseek_thinking)) + "'";
    if (o.reasoning_effort)
      sql += ",'reasoning_effort','" + esc(o.reasoning_effort) + "'";

    /* timeout */
    if (o.timeout_ms)
      sql += ",'timeout_ms'," + Number(o.timeout_ms);

    sql += ")) AS result";

    var rows = query(sql);
    var raw = (rows && Array.isArray(rows) && rows.length && rows[0].result != null)
          ? String(rows[0].result) : '';
    var think_m = raw.match(/<think>([\s\S]*?)<\/think>/i);
    _last_think = think_m ? think_m[1].trim() : '';
    return raw.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
  }

  function ml_rag(question, topK, opt_override) {
    topK = topK || 6;
    var opt = Object.assign(
      { n_citations: topK, distance_metric: 'COSINE', skip_generate: 1 },
      opt_override || {}
    );
    try {
      sys.exec_sql("SET @_rag_out = NULL");
      sys.exec_sql(
        "CALL sys.ML_RAG(" +
        "'" + esc(question) + "'," +
        "@_rag_out," +
        "'" + esc(JSON.stringify(opt)) + "')"
      );
    } catch (e) { return ''; }
    var rows = query("SELECT @_rag_out AS result");
    if (!rows || !Array.isArray(rows) || !rows.length || rows[0].result == null) return '';
    try {
      var obj = JSON.parse(rows[0].result);
      return obj.text || String(rows[0].result);
    } catch(e) { return String(rows[0].result); }
  }

  /* Schema / embedding helpers */
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

  function build_schema_context(db, available_tokens, chat_opt) {
    var n = Math.min(12, Math.max(5, Math.floor((available_tokens || 800) / 200)));
    var extra = {};
    if (chat_opt && chat_opt.schema_name) extra.schemas = [chat_opt.schema_name];
    if (check_schema_embeddings_ready(db)) {
      var result = call_schema_metadata(user_message, db, n, extra);
      if (result && result.length > 0)
        return t('【相关表 DDL（语义排序，FOREIGN KEY 可用于 JOIN）】\n',
                 '[Relevant Table DDL (semantic order, FOREIGN KEY usable for JOIN)]\n') + result;
    }
    var budget = Math.max(400, (available_tokens || 800) * 3);
    return build_schema_context_fallback(db, budget);
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

  /* Chat history & agent memory */
  function save_chat_options(chat_opt) {
    _cached_chat_opt = chat_opt;
    try {
      /* save api_key with mask key into agent_memory */
      if (chat_opt.model_options && chat_opt.model_options.api_key) {
        save_secret_api_key(chat_opt.model_options.api_key);
      }

      var safe = JSON.parse(JSON.stringify(chat_opt));
      if (safe.model_options && safe.model_options.api_key)
        safe.model_options.api_key = '***';
      sys.exec_sql("SET @chat_options = '" + esc(JSON.stringify(safe)) + "'");
    } catch(e) {}
  }

  function update_chat_history(chat_opt, user_msg, bot_msg) {
    if (!Array.isArray(chat_opt.chat_history)) chat_opt.chat_history = [];
    var entry = {
      user_message: user_msg, chat_bot_message: bot_msg,
      chat_query_id: chat_opt.chat_query_id || gen_query_id()
    };
    if (chat_opt.re_run && chat_opt.chat_history.length > 0)
      chat_opt.chat_history[chat_opt.chat_history.length - 1] = entry;
    else
      chat_opt.chat_history.push(entry);
    var max_len = (chat_opt.history_length >= 0) ? chat_opt.history_length : 3;
    if (chat_opt.chat_history.length > max_len)
      chat_opt.chat_history = chat_opt.chat_history.slice(-max_len);
    chat_opt.re_run = false;
    chat_opt.chat_query_id = gen_query_id();
    return chat_opt;
  }

  function chat_history_to_text(chat_opt) {
    if (!Array.isArray(chat_opt.chat_history) || !chat_opt.chat_history.length) return '';
    return chat_opt.chat_history.map(function(h) {
      return t('用户：', 'User: ')      + (h.user_message    || '') + '\n' +
             t('助手：', 'Assistant: ') + (h.chat_bot_message || '');
    }).join('\n');
  }

  function get_history(conv_id, n) {
    n = n || 8;
    var rows = query(
      "SELECT role, content FROM (" +
      "  SELECT role, content, created_at FROM mysql.agent_memory" +
      "  WHERE conversation_id='" + esc(conv_id) + "'" +
      "  ORDER BY created_at DESC LIMIT " + n +
      ") t ORDER BY t.created_at ASC"
    );
    if (!Array.isArray(rows) || !rows.length) return '';
    return rows.map(function(r) {
      return (r.role === 'user' ? t('用户', 'User') : t('助手', 'Assistant')) +
             '：' + r.content;
    }).join('\n');
  }

  /*
  * Rebuild chat_history array from mysql.agent_memory for cross-session recovery.
  * Pairs user/assistant rows in chronological order.
  * Returns [] if no records found or on error.
  */
  function recover_chat_history_from_memory(conv_id, max_turns) {
    max_turns = max_turns || 3;
    try {
      /* Fetch last max_turns*2 rows DESC, then reverse to ASC for pairing */
      var rows = query(
        "SELECT role, content FROM (" +
        "  SELECT role, content, created_at FROM mysql.agent_memory" +
        "  WHERE conversation_id='" + esc(conv_id) + "'" +
        "  ORDER BY created_at DESC LIMIT " + (max_turns * 2) +
        ") t ORDER BY t.created_at ASC"
      );
      if (!Array.isArray(rows) || !rows.length) return [];

      /* Pair consecutive user → assistant rows into chat_history entries */
      var history = [];
      var i = 0;
      while (i < rows.length && history.length < max_turns) {
        if (rows[i].role !== 'user') { i++; continue; }   /* skip orphan assistant row */

        var user_msg = rows[i].content || '';
        var bot_msg  = '';
        if (i + 1 < rows.length && rows[i + 1].role === 'assistant') {
          bot_msg = rows[i + 1].content || '';
          i += 2;
        } else {
          i += 1;   /* user turn without assistant reply yet */
        }

        history.push({
          user_message:    user_msg,
          chat_bot_message: bot_msg,
          chat_query_id:   gen_query_id()
        });
      }
      return history;
    } catch(e) { return []; }
  }

  function save_memory(conv_id, role, content, thought, with_embedding) {
    with_embedding = (with_embedding === true);
    var EMBED_SAFE_LIMIT = 1800;
    var safe_content = String(content || '').substring(0, EMBED_SAFE_LIMIT);

    if (with_embedding) {
      try {
        sys.exec_sql(
          "INSERT INTO mysql.agent_memory(conversation_id, role, content, thought, embedding) " +
          "SELECT '" + esc(conv_id) + "','" + esc(role) + "','" + esc(content) + "','" +
          esc(thought || '') + "', " +
          "sys.ML_EMBED_ROW('" + esc(safe_content) + "', " +
          "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))"
        );
        return;
      } catch (e) {}
    }
    sys.exec_sql(
      "INSERT INTO mysql.agent_memory(conversation_id, role, content, thought, embedding) " +
      "VALUES('" + esc(conv_id) + "','" + esc(role) + "','" + esc(content) + "','" +
      esc(thought || '') + "', NULL)"
    );
  }

  function persist_turn(conv_id, user_msg, bot_msg, thought) {
    try { save_memory(conv_id, 'user',      user_msg, '',      true); } catch (e) {}
    try { save_memory(conv_id, 'assistant', bot_msg,  thought, true); } catch (e) {}
  }

  function retrieve_few_shot(question, topK) {
    topK = topK || 3;
    try {
      var emb_rows = query(
        "SELECT sys.ML_EMBED_ROW('" + esc(question) + "'," +
        "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true)) AS emb"
      );
      if (!emb_rows || !emb_rows.length || !emb_rows[0].emb) return '';
      var emb_val  = emb_rows[0].emb;
      var sim_rows = query(
        "SELECT content, thought FROM mysql.agent_memory" +
        " WHERE role='assistant' AND thought LIKE '%query_db%'" +
        "   AND conversation_id != '" + esc(conversation_id) + "'" +
        "   AND embedding IS NOT NULL" +
        " ORDER BY DISTANCE(embedding," +
        "   (SELECT sys.ML_EMBED_ROW('" + esc(question) + "'," +
        "    JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true)))," +
        "   'cosine') ASC LIMIT " + topK
      );
      if (!Array.isArray(sim_rows) || !sim_rows.length) return '';
      var lines = [t('【Few-Shot 参考（向量检索）】', '[Few-Shot References (vector search)]')];
      for (var i = 0; i < sim_rows.length; i++) {
        var m = (sim_rows[i].thought || '').match(/"sql"\s*:\s*"([^"]{10,200})"/);
        if (m) {
          lines.push('Q' + (i+1) + ': ' + sim_rows[i].content.substring(0, 80));
          lines.push('SQL: ' + m[1].substring(0, 150));
        }
      }
      return lines.length > 1 ? lines.join('\n') : '';
    } catch(e) { return ''; }
  }

  /* RAG / knowledge dispatch */
  function discover_vector_tables(chat_opt) {
    if (Array.isArray(chat_opt.tables) && chat_opt.tables.length > 0) return chat_opt.tables;
    var seg_col = 'segment', emb_col = 'segment_embedding';
    var schema_filter = chat_opt.schema_name
      ? " AND c.TABLE_SCHEMA='" + esc(chat_opt.schema_name) + "'" : '';
    var rows = query(
      "SELECT c.TABLE_SCHEMA, c.TABLE_NAME FROM information_schema.COLUMNS c" +
      " WHERE c.COLUMN_NAME='" + esc(emb_col) + "' AND c.DATA_TYPE='vector'" +
      schema_filter +
      "   AND EXISTS (SELECT 1 FROM information_schema.COLUMNS c2" +
      "     WHERE c2.TABLE_SCHEMA=c.TABLE_SCHEMA AND c2.TABLE_NAME=c.TABLE_NAME" +
      "       AND c2.COLUMN_NAME='" + esc(seg_col) + "')" +
      " ORDER BY c.TABLE_SCHEMA, c.TABLE_NAME"
    );
    if (!Array.isArray(rows) || !rows.length) return [];
    return rows.map(function(r) {
      return { schema_name: r.TABLE_SCHEMA, table_name: r.TABLE_NAME };
    });
  }

  function is_knowledge_query(text) {
    /*
     * Exclusion list – schema/DDL introspection MUST NOT go to RAG;
     * it must fall through to rule_planner (ROUTE B) instead.
     * Patterns cover both Chinese and English phrasing.
     */
    var schema_pat = new RegExp(
      '有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables|' +
      '有哪些数据库|所有数据库|列出.*数据库|show.?databases|list.*databases|what.*databases|' +
      '表.*结构|结构.*表|字段|列信息|column.*info|table.*structure|describe.*table|' +
      '关联关系|外键|foreign.?key|join.*关系|table.*relation|related.*tables|' +
      '索引|index.*info|index.*analysis|missing.*index|covering.*index|' +
      '数据量|行数|row.?count|record.?count|table.*size|数据大小', 'i'
    );
    if (schema_pat.test(text)) return false;

    var knowledge_pat = new RegExp(
      '(什么是|what is|how does|解释|explain|介绍|describe|为什么|why|' +
      'what are|有哪些|如何理解|原理|architecture|概念|区别|对比|compare|' +
      'shannonbase|heatwave|innodb|mysql.*特性|raft|mvcc|lsm)', 'i'
    );
    var sql_pat = /SELECT|INSERT|UPDATE|DELETE|查询|查找|统计|修改|删除|插入|导出/i;
    return knowledge_pat.test(text.trim()) && !sql_pat.test(text);
  }

  function heatwave_dispatch(user_msg, chat_opt, vector_tables) {
    var u_pre = t('用户：', 'User: ');
    var a_suf = t('\n助手：', '\nAssistant: ');

    if (!vector_tables || vector_tables.length === 0) {
      if (chat_opt.skip_generate)
        return { mode:'EMPTY',
                 response: t('未找到向量知识库。', 'No vector knowledge base found.'),
                 tables: [] };
      var hist_ctx0 = chat_history_to_text(chat_opt);
      return {
        mode: 'GENERATE',
        response: ml_generate(
          (hist_ctx0 ? hist_ctx0 + '\n\n' : '') + u_pre + user_msg + a_suf,
          chat_opt.model_options || {}),
        tables: []
      };
    }

    var ret_opt = {};
    if (chat_opt.retrieval_options) {
      try {
        ret_opt = typeof chat_opt.retrieval_options === 'string'
                  ? JSON.parse(chat_opt.retrieval_options)
                  : chat_opt.retrieval_options;
      } catch(e) {}
    }
    var rag_extra = {};
    if (chat_opt.embed_model_id)   rag_extra.embed_model_id  = chat_opt.embed_model_id;
    if (ret_opt.max_distance)      rag_extra.max_distance    = ret_opt.max_distance;
    if (ret_opt.segment_overlap)   rag_extra.segment_overlap = ret_opt.segment_overlap;

    var hist_ctx     = chat_history_to_text(chat_opt);
    var rag_question = hist_ctx ? hist_ctx + '\n' + u_pre + user_msg : user_msg;
    var rag_res      = ml_rag(rag_question, chat_opt.retrieve_top_k || 6, rag_extra);

    if (rag_res && rag_res.length >= 30)
      return { mode: 'RAG', response: rag_res, tables: vector_tables };

    if (!chat_opt.skip_generate) {
      var fb_prompt = (hist_ctx ? hist_ctx + '\n\n' : '') + u_pre + user_msg + a_suf;
      return { mode:'GENERATE',
               response: ml_generate(fb_prompt, chat_opt.model_options || {}),
               tables: vector_tables };
    }
    return { mode:'EMPTY',
             response: t('知识库中未找到相关内容。',
                         'No relevant content found in knowledge base.'),
             tables: vector_tables };
  }

  /* 
   * System query catalog
   *
   * Built inside catalog_match() so t() executes at call-time
   * (after `lang` is set) rather than at function-definition time.
   * Patterns cover both Chinese and English phrasing.
   * */
  function catalog_match(text) {
    var catalog = [
      { pattern: /锁[\s\S]{0,15}情况|当前.*持有.*锁|data.?locks|current.*locks?|show.*locks?/i,
        sql: 'SELECT ENGINE_TRANSACTION_ID,OBJECT_SCHEMA,OBJECT_NAME,LOCK_TYPE,LOCK_MODE,' +
             'LOCK_STATUS,LOCK_DATA FROM performance_schema.data_locks LIMIT 50',
        desc: t('当前持有的行锁', 'Current row locks held') },

      { pattern: /锁等待|lock.?wait|谁在等|被谁锁|who.*block|blocking.*tr(x|ansaction)/i,
        sql: 'SELECT r.trx_id AS waiting_trx,r.trx_mysql_thread_id AS waiting_thread,' +
             'r.trx_query AS waiting_query,b.trx_id AS blocking_trx,' +
             'b.trx_mysql_thread_id AS blocking_thread,b.trx_query AS blocking_query,' +
             'w.BLOCKING_ENGINE_LOCK_ID FROM information_schema.INNODB_TRX r ' +
             'JOIN performance_schema.data_lock_waits w ' +
             '  ON r.trx_id=w.REQUESTING_ENGINE_TRANSACTION_ID ' +
             'JOIN information_schema.INNODB_TRX b ' +
             '  ON b.trx_id=w.BLOCKING_ENGINE_TRANSACTION_ID',
        desc: t('锁等待关系', 'Lock wait relationships') },

      { pattern: /死锁|deadlock/i,
        sql: 'SHOW ENGINE INNODB STATUS',
        desc: t('InnoDB 死锁详情', 'InnoDB deadlock details') },

      { pattern: /活跃事务|active.?tr(x|ansaction)|innodb.*trx|长事务|long.?tr(x|ansaction)/i,
        sql: 'SELECT trx_id,trx_state,trx_started,trx_mysql_thread_id,' +
             'LEFT(trx_query,100) AS trx_query,trx_rows_modified,trx_rows_locked ' +
             'FROM information_schema.INNODB_TRX ORDER BY trx_started',
        desc: t('当前活跃 InnoDB 事务', 'Active InnoDB transactions') },

      { pattern: /processlist|进程列表|正在执行|running.?quer|active.*queries/i,
        sql: "SELECT ID,USER,HOST,DB,COMMAND,TIME,STATE,LEFT(INFO,120) AS QUERY " +
             "FROM information_schema.PROCESSLIST WHERE COMMAND!='Sleep' ORDER BY TIME DESC",
        desc: t('当前活跃进程列表', 'Active process list') },

      { pattern: /连接数|thread.?connect|Threads_connected|connection.?count/i,
        sql: 'SHOW STATUS LIKE "Threads%"',
        desc: t('线程连接数统计', 'Thread / connection statistics') },

      { pattern: /慢查询|slow.?quer|top.*慢|耗时最长|slowest.?quer/i,
        sql: 'SELECT DIGEST_TEXT,COUNT_STAR,' +
             'ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec,' +
             'ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec,' +
             'SUM_ROWS_EXAMINED,SUM_ROWS_SENT ' +
             'FROM performance_schema.events_statements_summary_by_digest ' +
             'ORDER BY AVG_TIMER_WAIT DESC LIMIT 20',
        desc: t('慢查询 TOP20', 'Slow query TOP20') },

      { pattern: /buffer.?pool|缓冲池/i,
        sql: 'SELECT POOL_ID,POOL_SIZE,FREE_BUFFERS,DATABASE_PAGES,HIT_RATE,' +
             'PAGES_MADE_YOUNG FROM information_schema.INNODB_BUFFER_POOL_STATS',
        desc: t('InnoDB Buffer Pool 状态', 'InnoDB Buffer Pool status') },

      { pattern: /redo.?log|redo日志|log.?capacity/i,
        sql: 'SHOW STATUS LIKE "Innodb_redo%"',
        desc: t('InnoDB Redo Log 状态', 'InnoDB Redo Log status') },

      { pattern: /表空间|tablespace/i,
        sql: 'SELECT SPACE_ID,NAME,ROW_FORMAT,PAGE_SIZE,ZIP_PAGE_SIZE ' +
             'FROM information_schema.INNODB_TABLESPACES ORDER BY NAME LIMIT 50',
        desc: t('InnoDB 表空间信息', 'InnoDB tablespace info') },

      { pattern: /系统变量|global.?var|show.?variable/i,
        sql: 'SHOW VARIABLES LIKE "innodb%"',
        desc: t('InnoDB 相关系统变量', 'InnoDB system variables') }
    ];

    for (var i = 0; i < catalog.length; i++)
      if (catalog[i].pattern.test(text)) return catalog[i];
    return null;
  }

  /* Table name fallback / auto-recovery */
  var TABLE_FALLBACK = {
    'INNODB_LOCKS':       'performance_schema.data_locks',
    'INNODB_LOCK_WAITS':  'performance_schema.data_lock_waits',
    'USER_STATISTICS':    'performance_schema.accounts',
    'INDEX_STATISTICS':   'information_schema.STATISTICS',
    'QUERY_CACHE_INFO':   'performance_schema.events_statements_summary_by_digest',
    'GLOBAL_STATUS':      'performance_schema.global_status',
    'GLOBAL_VARIABLES':   'performance_schema.global_variables'
  };

  function try_recover_unknown_table(result, original_sql) {
    if (!result || result.indexOf('Unknown table') === -1) return null;
    var m = result.match(/Unknown table\s+'?(?:[\w]+\.)?([\w]+)'?/i);
    if (!m) return null;
    var bad = m[1].toUpperCase(), replacement = TABLE_FALLBACK[bad];
    if (!replacement) return null;
    var new_sql = original_sql.replace(
      new RegExp('(?:information_schema\\.|performance_schema\\.)?' + bad, 'gi'), replacement
    );
    return { sql: new_sql,
             desc: t('⚡自动恢复：', '⚡Auto-recovered: ') + bad + ' → ' + replacement };
  }

  /* Query complexity / logical plan */
  function estimate_complexity(text) {
    var signals = [
      /\bGROUP\s+BY\b|按.{1,8}分组/i,
      /\bJOIN\b|多表.*关联|关联.*多表/i,
      /子查询|\bIN\s*\(\s*SELECT/i,
      /同比|环比|趋势|占比|排名.*前\s*\d|TOP\s*\d/i,
      /先.*再.*(?:然后|最后)|第一步.*第二步/,
      /\bUNION\b|\bWITH\b.*\bAS\b/i
    ];
    var score = 0;
    for (var i = 0; i < signals.length; i++) if (signals[i].test(text)) score++;
    return score;
  }

  function decompose_query(text) {
    if (estimate_complexity(text) < 2) return [{ op: 'scan', note: text }];
    var tasks = [{ op: 'scan', note: t('主表扫描', 'main table scan') }];
    if (/最近|过去\s*\d+|大于|小于|等于|筛选|过滤|WHERE|filter|recent|last\s+\d/i.test(text))
      tasks.push({ op:'filter', note: t('条件过滤', 'filter condition') });
    if (/关联|联合|join/i.test(text))
      tasks.push({ op:'join', note: t('多表关联', 'multi-table join') });
    if (/聚合|汇总|统计|SUM|AVG|COUNT|aggregate|average/i.test(text))
      tasks.push({ op:'agg', note: t('聚合计算', 'aggregation') });
    if (/排名|排序|最高|最低|前\s*\d+|rank|sort|top\s*\d/i.test(text))
      tasks.push({ op:'sort', note: t('排序限行', 'sort + limit') });
    tasks.push({ op: 'explain', note: t('执行计划验证', 'explain plan check') });
    return tasks;
  }

  function logical_plan_to_hint(tasks, schema_from_embeddings) {
    if (!tasks || tasks.length <= 1) return '';
    var op_hints = {
      scan:    t('确定主表，必要时先用 query_db 检查表结构',
                 'Identify main table; use query_db to inspect schema if needed'),
      filter:  t('构造 WHERE 条件，确认谓词列有索引',
                 'Build WHERE clause; confirm predicate columns are indexed'),
      join:    schema_from_embeddings
               ? t('按上方 DDL 中的 FOREIGN KEY 子句拼写 JOIN 条件',
                   'Use FOREIGN KEY clauses from the DDL above to write JOIN conditions')
               : t('按【推荐JOIN路径】拼写 JOIN 子句',
                   'Use [Recommended JOIN paths] to write JOIN clauses'),
      agg:     t('构造 GROUP BY + 聚合函数，注意 HAVING 与 WHERE 顺序',
                 'Build GROUP BY + aggregate functions; note HAVING vs WHERE ordering'),
      sort:    t('添加 ORDER BY；LIMIT 避免大结果集',
                 'Add ORDER BY; use LIMIT to avoid large result sets'),
      explain: t('调用 explain_sql 验证；⚠全表扫描时先改写 SQL',
                 'Call explain_sql to verify; ⚠ rewrite SQL if full table scan detected')
    };
    var lines = [t('【逻辑计划（按序执行，不得跳步）】',
                   '[Logical Plan (execute in order, no skipping)]')];
    for (var i = 0; i < tasks.length; i++)
      lines.push('Step' + (i+1) + ' [' + tasks[i].op.toUpperCase() + ']: ' +
                 (op_hints[tasks[i].op] || tasks[i].op) + '  ← ' + tasks[i].note);
    return lines.join('\n');
  }

  /* Rule planner bilingual patterns, translated descs  */
  function rule_planner(msg, db) {

    /* Mode 0: list databases/schemas (instance-level, not table-level) */
    if (/有哪些(数据库|schema)|所有数据库|列出.*数据库|show.?databases|show.?schemas|list.*databases/i.test(msg)) {
      return [
        { sql: "SELECT SCHEMA_NAME AS database_name" +
              " FROM information_schema.SCHEMATA" +
              " ORDER BY SCHEMA_NAME",
          desc: t('当前实例所有数据库', 'All databases in current instance') }
      ];
    }

    /* Mode 1: list tables */
    if (/有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables/i.test(msg) &&
        !/结构|schema|column|字段|列|create|structure|definition/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,TABLE_ROWS,TABLE_COMMENT" +
               " FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
               " ORDER BY TABLE_NAME",
          desc: t('获取所有表概览', 'Get all tables overview') }
      ];
    }

    /* Mode 2: table structure / columns */
    if (/(表.*结构|结构.*表|字段|列信息|column.*info|table.*structure|describe.*table)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
               "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
               "  TABLE_COMMENT" +
               " FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
               " ORDER BY TABLE_NAME",
          desc: t('所有表概览（行数 / 大小 / 注释）',
                  'All tables overview (rows / size / comment)') },
          { sql: "SELECT TABLE_NAME," +
                "  GROUP_CONCAT(" +
                "    CONCAT(COLUMN_NAME,' ',COLUMN_TYPE," +
                "      IF(COLUMN_KEY<>'',CONCAT(' ',COLUMN_KEY),'')," +
                "      IF(IS_NULLABLE='NO',' NOT NULL','')" +
                "    ) ORDER BY ORDINAL_POSITION SEPARATOR ', '" +
                "  ) AS columns" +
                " FROM information_schema.COLUMNS" +
                " WHERE TABLE_SCHEMA=DATABASE()" +
                " GROUP BY TABLE_NAME" +
                " ORDER BY TABLE_NAME",
            desc: t('所有表的列定义（每表一行紧凑格式）',
                    'Column definitions for all tables (one compact row per table)') },
         { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
                " FROM information_schema.KEY_COLUMN_USAGE" +
                " WHERE CONSTRAINT_SCHEMA=DATABASE()" +
                "   AND REFERENCED_TABLE_NAME IS NOT NULL",
           desc: t('外键关系（JOIN 路径）', 'Foreign key relationships (JOIN paths)') }
      ];
    }

    /* Mode 3: FK / JOIN / table relationships */
    if (/(外键|foreign.?key|join.*关系|关联关系|table.*relation|related.*table)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
               " FROM information_schema.KEY_COLUMN_USAGE" +
               " WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL",
          desc: t('所有外键关系（JOIN 路径）', 'All foreign key relationships (JOIN paths)') }
      ];
    }

    /* Mode 4: index analysis */
    if (/(索引|index.*分析|缺少.*索引|覆盖.*索引|index.*analysis|missing.*index|covering.*index)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX," +
               "COLUMN_NAME,CARDINALITY,INDEX_TYPE" +
               " FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()" +
               " ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX",
          desc: t('当前库所有索引详情', 'All index details for current database') },
        { sql: "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE" +
               " FROM information_schema.COLUMNS" +
               " WHERE TABLE_SCHEMA=DATABASE()" +
               "   AND COLUMN_NAME NOT IN (" +
               "     SELECT DISTINCT COLUMN_NAME FROM information_schema.STATISTICS" +
               "     WHERE TABLE_SCHEMA=DATABASE())" +
               " ORDER BY TABLE_NAME,ORDINAL_POSITION",
          desc: t('未被索引覆盖的列（候选索引）', 'Unindexed columns (index candidates)') }
      ];
    }

    /* Mode 5: data volume */
    if (/(数据量|行数|record.?count|row.?count|表.*大小|数据大小|table.*size)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
               "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
               "  DATA_FREE" +
               " FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
               " ORDER BY DATA_LENGTH+INDEX_LENGTH DESC",
          desc: t('各表数据量与磁盘占用', 'Table row counts and disk usage') }
      ];
    }

    /* Mode 6: query performance */
    if (/(性能.*top|top.*性能|最慢.*查询|query.*perf|slow.*queries|performance.*top)/i.test(msg)) {
      return [
        { sql: "SELECT DIGEST_TEXT,COUNT_STAR," +
               "  ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec," +
               "  ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec," +
               "  SUM_ROWS_EXAMINED,SUM_ROWS_SENT" +
               " FROM performance_schema.events_statements_summary_by_digest" +
               " ORDER BY SUM_TIMER_WAIT DESC LIMIT 20",
          desc: t('SQL 性能 TOP20', 'SQL performance TOP20') }
      ];
    }

    return null; /* fall through to LLM Agent */
  }

  /* Plan execution */
  var MAX_PLAN_STEPS = 15;

  function execute_plan(steps, db) {
    var results = [];
    var last_result_text = '';
    var per_step_limit = cfg('plan_step_max_tokens', 4000);

    for (var i = 0; i < Math.min(steps.length, MAX_PLAN_STEPS); i++) {
      var step = steps[i];
      var sql  = replace_ph(String(step.sql || ''), db);
      if (sql.indexOf('__LAST_RESULT__') !== -1)
        sql = sql.replace(/__LAST_RESULT__/g, esc(last_result_text.substring(0, 200)));

      var upper = sql.trim().toUpperCase().split(/\s+/)[0];
      var RO = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
      var result_text;
      if (RO[upper]) {
        var raw = query(sql);
        result_text = compress(rows_to_text(raw), per_step_limit);
        if (result_text.indexOf('Unknown table') !== -1) {
          var recovery = try_recover_unknown_table(result_text, sql);
          if (recovery)
            result_text = recovery.desc + '\n' + compress(rows_to_text(query(recovery.sql)), per_step_limit);
        }
      } else {
        result_text = t('跳过非只读 SQL（plan_sql 中仅执行 SELECT/SHOW/DESC）：',
                        'Skipped non-read-only SQL: ') + sql.substring(0, 80);
      }
      last_result_text = result_text;
      results.push({ step: i + 1, desc: step.desc || ('Step ' + (i+1)),
                    sql: sql, result: result_text });
    }
    return results;
  }

  /* Tool validation & execution */
  function validate_tool_call(tool_obj) {
    if (!tool_obj || typeof tool_obj.tool !== 'string')
      return t('工具调用格式错误：缺少 tool 字段',
               'Tool call format error: missing "tool" field');
    var args = tool_obj.args || {};
    switch (tool_obj.tool) {
      case 'query_db':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 3)
          return t(
            'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...',
            'query_db missing valid sql argument. Provide a complete SQL, e.g.: SHOW TABLES or SELECT ...'
          );
        break;
      case 'explain_sql':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
          return t('explain_sql 缺少有效 sql 参数', 'explain_sql missing valid sql argument');
        break;
      case 'update_data':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
          return t('update_data 缺少有效 sql 参数', 'update_data missing valid sql argument');
        break;
      case 'plan_sql':
        if (!Array.isArray(args.steps) || args.steps.length === 0)
          return t('plan_sql 缺少 steps 数组，或 steps 为空',
                   'plan_sql missing steps array or steps is empty');
        for (var vi = 0; vi < args.steps.length; vi++) {
          if (!args.steps[vi].sql || typeof args.steps[vi].sql !== 'string')
            return t('plan_sql steps[', 'plan_sql steps[') + vi +
                   t('] 缺少 sql 字段', '] missing sql field');
        }
        break;
      case 'ml_rag':
        if (!args.question || typeof args.question !== 'string')
          return t('ml_rag 缺少 question 参数', 'ml_rag missing question argument');
        break;
      case 'generate_text':
        if (!args.prompt || typeof args.prompt !== 'string')
          return t('generate_text 缺少 prompt 参数', 'generate_text missing prompt argument');
        break;
      case 'begin_tx': case 'commit_tx': case 'rollback_tx':
        break;
      default:
        return t('未知工具：', 'Unknown tool: ') + tool_obj.tool;
    }
    return null;
  }

  var TX_STATE = { active: false };

  function replace_ph(sql, db) {
    if (!db) return sql;
    return sql.replace(
      /your_database_name|your_db_name|<database_name>|\[database_name\]|\{database_name\}|your_schema/gi,
      db
    );
  }

  function parse_explain(json_str) {
    try {
      var plan = JSON.parse(json_str), out = [];
      function walk(node) {
        if (!node || typeof node !== 'object') return;
        if (node.table_name)             out.push(t('表=', 'table=')   + node.table_name);
        if (node.access_type) {
          out.push(t('访问=', 'access=') + node.access_type);
          if (node.access_type === 'ALL') out.push(t('⚠全表扫描', '⚠full table scan'));
        }
        if (node.rows_examined_per_scan) out.push(t('扫描行≈', 'rows≈') + node.rows_examined_per_scan);
        if (node.using_filesort)  out.push(t('⚠需filesort', '⚠filesort required'));
        if (node.using_temporary) out.push(t('⚠临时表',     '⚠temp table'));
        if (node.key) out.push(t('索引=', 'key=') + node.key);
        ['nested_loop','attached_subqueries','query_block',
         'ordering_operation','grouping_operation'].forEach(function(k) {
          if (!node[k]) return;
          if (Array.isArray(node[k])) node[k].forEach(function(c){ walk(c.table || c); });
          else walk(node[k]);
        });
      }
      walk(plan.query_block || plan);
      return out.length ? out.join(' ') : json_str.substring(0, 150);
    } catch(e) { return json_str.substring(0, 150); }
  }

  function execute_tool(tool, args, db) {
    var sql, upper, first;

    if (tool === 'query_db') {
      sql = replace_ph(String(args.sql || ''), db);
      first = sql.trim().toUpperCase().split(/\s+/)[0];
      var RO2 = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
      if (!RO2[first])
        return t('拒绝：query_db 只允许只读语句（SELECT/SHOW/DESC/EXPLAIN/WITH）。',
                 'Rejected: query_db only allows read-only statements (SELECT/SHOW/DESC/EXPLAIN/WITH).');
      var result = compress(rows_to_text(query(sql)), 900);
      if (result.indexOf('Unknown table') !== -1) {
        var recovery2 = try_recover_unknown_table(result, sql);
        if (recovery2)
          return recovery2.desc + '\n' + compress(rows_to_text(query(recovery2.sql)), 900);
      }
      return result;
    }

    if (tool === 'explain_sql') {
      sql = replace_ph(String(args.sql || ''), db);
      var ex = query("EXPLAIN FORMAT=JSON " + sql);
      if (!ex || !ex.length) return t('EXPLAIN 执行失败', 'EXPLAIN execution failed');
      var raw = ex[0]['EXPLAIN'] || ex[0]['explain'] || JSON.stringify(ex[0]);
      return t('执行计划：', 'Execution plan: ') + parse_explain(String(raw));
    }

    if (tool === 'plan_sql') {
      var steps2      = args.steps || [];
      var plan_res    = execute_plan(steps2, db);
      var out_lines   = [t('【plan_sql 多步执行结果】', '[plan_sql multi-step results]')];
      for (var pi = 0; pi < plan_res.length; pi++) {
        var pr = plan_res[pi];
        out_lines.push('--- Step ' + pr.step + ': ' + pr.desc + ' ---');
        out_lines.push(pr.result);
      }
      return compress(out_lines.join('\n'), cfg('plan_log_max_tokens', 4000));
    }

    if (tool === 'begin_tx') {
      if (TX_STATE.active)
        return t('警告：事务已活跃，禁止重复 begin_tx。',
                 'Warning: transaction already active; duplicate begin_tx forbidden.');
      sys.exec_sql('START TRANSACTION');
      TX_STATE.active = true;
      return t('事务已开启', 'Transaction started');
    }

    if (tool === 'update_data') {
      if (!TX_STATE.active)
        return t('拒绝：写操作必须在事务内，请先 begin_tx。',
                 'Rejected: write operations must be inside a transaction; call begin_tx first.');
      sql   = replace_ph(String(args.sql || ''), db);
      upper = sql.trim().toUpperCase();
      first = upper.split(/\s+/)[0];
      if ((first==='UPDATE'||first==='DELETE') && !/\bWHERE\b/.test(upper))
        return t('拒绝：', 'Rejected: ') + first +
               t(' 必须含 WHERE 条件。', ' must include a WHERE clause.');
      sys.exec_sql(sql);
      return t('执行成功', 'Success');
    }

    if (tool === 'commit_tx') {
      if (!TX_STATE.active)
        return t('警告：当前无活跃事务。', 'Warning: no active transaction.');
      sys.exec_sql('COMMIT');
      TX_STATE.active = false;
      return t('事务已提交', 'Transaction committed');
    }

    if (tool === 'rollback_tx') {
      if (TX_STATE.active) { sys.exec_sql('ROLLBACK'); TX_STATE.active = false; }
      return t('事务已回滚', 'Transaction rolled back');
    }

    if (tool === 'ml_rag')
      return compress(ml_rag(String(args.question || user_message), args.top_k || 6), 800);
    if (tool === 'generate_text')
      return ml_generate(String(args.prompt || ''), args.options || {});

    return t('错误：未知工具 "', 'Error: unknown tool "') + tool + '"';
  }

  function parse_tool_call(raw) {
    if (!raw) return null;
    var s = raw.replace(/```json|```/g, '').trim();
    var depth = 0, start = -1, end = -1;
    for (var i = 0; i < s.length; i++) {
      if      (s[i]==='{') { if (depth++ === 0) start = i; }
      else if (s[i]==='}') { if (--depth === 0) { end = i; break; } }
    }
    if (start === -1 || end === -1) return null;
    try {
      var obj = JSON.parse(s.substring(start, end + 1));
      return (obj && typeof obj.tool === 'string') ? obj : null;
    } catch(e) { return null; }
  }

  /* Query classification & system prompt */
  function classify_query(text) {
    var sys_pat = new RegExp(
      '进程|processlist|线程|thread|连接数|connection|慢查询|slow.?quer|变量|variable|' +
      '系统状态|status|存储引擎|engine|权限|grant|表空间|tablespace|redo|buffer.?pool|' +
      '锁|lock|死锁|deadlock|活跃.*事务|active.*tr[xa]|innodb.*trx|' +
      'information.?schema|performance.?schema|当前.*用户|current.*user|版本|version', 'i'
    );
    var olap_pat = new RegExp(
      'GROUP\\s+BY|HAVING|SUM\\s*\\(|AVG\\s*\\(|COUNT\\s*\\(|OVER\\s*\\(|' +
      'PARTITION\\s+BY|ROLLUP|CUBE|聚合|汇总|统计|分析|趋势|占比|排名|同比|环比', 'i'
    );
    if (sys_pat.test(text)) return 'system';
    if (olap_pat.test(text)) return 'olap';
    return 'oltp';
  }

  function get_workload_hint(qtype) {
    if (qtype === 'system') return t(
      '⚠ 系统元数据查询 — 必须使用正确表：\n' +
      '  行锁 → performance_schema.data_locks\n' +
      '  锁等待 → performance_schema.data_lock_waits\n' +
      '  活跃事务 → information_schema.INNODB_TRX\n' +
      '  死锁 → SHOW ENGINE INNODB STATUS\n' +
      '  ⛔ 禁止：INNODB_LOCKS / INNODB_LOCK_WAITS（MySQL 8.0 已删除）\n' +
      '  慢查询 → performance_schema.events_statements_summary_by_digest\n' +
      '  ⛔ 报 Unknown table → 立即换正确表，禁止重试同一表名。',
      '⚠ System metadata query — use the correct tables:\n' +
      '  Row locks    → performance_schema.data_locks\n' +
      '  Lock waits   → performance_schema.data_lock_waits\n' +
      '  Active trx   → information_schema.INNODB_TRX\n' +
      '  Deadlocks    → SHOW ENGINE INNODB STATUS\n' +
      '  ⛔ Forbidden: INNODB_LOCKS / INNODB_LOCK_WAITS (removed in MySQL 8.0)\n' +
      '  Slow queries → performance_schema.events_statements_summary_by_digest\n' +
      '  ⛔ On Unknown table error → switch to correct table, do not retry.'
    );
    if (qtype === 'olap') return t(
      '⚠ OLAP：优先列存；GROUP BY 列需索引；大结果集主键偏移分页。',
      '⚠ OLAP: prefer columnar storage; GROUP BY columns need indexes; use keyset pagination for large result sets.'
    );
    return t(
      'OLTP：主键命中优先；UPDATE/DELETE 必须带 WHERE；写操作走 begin_tx 流程。',
      'OLTP: prefer primary key lookups; UPDATE/DELETE must include WHERE; wrap writes with begin_tx.'
    );
  }

  function build_system_prompt(db, schema_ctx, join_hint, plan_hint,
                               few_shot, history, hw_history) {
    var qtype        = classify_query(user_message);
    var hist_section = hw_history || history || t('（无历史）', '(No history)');

    var now_rows = query("SELECT DATE_FORMAT(NOW(),'%Y-%m-%d') AS today, CAST(YEAR(NOW()) AS CHAR) AS yr");
    var today    = (Array.isArray(now_rows) && now_rows.length) ? (now_rows[0].today || '') : '';
    var cur_year = (Array.isArray(now_rows) && now_rows.length) ? (now_rows[0].yr    || '') : '';
    var current_time_info = t(
        /* Chinese */ '【当前时间】\n今天日期：' + today + '，当前年份：' + cur_year + '\n\n',
        /* English */ '[Current Time]\nToday: ' + today + ', Current Year: ' + cur_year + '\n\n'
    );

    /* Inline few-shot – language-specific Q&A examples */
    var inline_few_shot = t(
      /* Chinese */
      '【Few-Shot 强制示例（必须照此格式）】\n' +
      'Q: 有哪些表？\n' +
      'A: {"thought":"列出当前库所有表","tool":"query_db","args":{"sql":"SHOW TABLES"}}\n\n' +
      'Q: 每张表的结构是什么？\n' +
      'A: {"thought":"plan_sql 先列表再查列定义","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"SHOW TABLES","desc":"枚举所有表"},\n' +
      '     {"sql":"SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION","desc":"所有列定义"}\n' +
      '   ]}}\n\n' +
      'Q: 统计每张表的行数\n' +
      'A: {"thought":"查 information_schema.TABLES","tool":"query_db","args":{"sql":"SELECT TABLE_NAME,TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE=\'BASE TABLE\'"}}\n\n' +
      'Q: 查看 orders 表最近 7 天的数据\n' +
      'A: {"thought":"滑动窗口过滤，用 NOW()-INTERVAL，不指定固定年份","tool":"query_db","args":{"sql":"SELECT * FROM orders WHERE created_at >= NOW()-INTERVAL 7 DAY LIMIT 100"}}\n\n' +
      'Q: 分析订单金额按月汇总\n' +
      'A: {"thought":"多步：先看结构再聚合","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"DESC orders","desc":"确认列名"},\n' +
      '     {"sql":"SELECT DATE_FORMAT(created_at,\'%Y-%m\') AS month,SUM(amount) AS total FROM orders GROUP BY month ORDER BY month","desc":"按月汇总"}\n' +
      '   ]}}\n\n' +
      'Q: 分析2024年每个月的收入\n' +
      'A: {"thought":"用户指定固定年份2024，必须用 YEAR()=N，禁止用 NOW()-INTERVAL 滑动窗口","tool":"query_db","args":{"sql":"SELECT DATE_FORMAT(tx_date,\'%Y-%m\') AS month,SUM(amount) AS total FROM revenue_items WHERE YEAR(tx_date)=2024 GROUP BY month ORDER BY month"}}\n\n' +
      'Q: 统计2023年各部门费用合计\n' +
      'A: {"thought":"固定年份过滤 + GROUP BY dept_name，用 YEAR()=N","tool":"query_db","args":{"sql":"SELECT dept_name,SUM(amount) AS total FROM expense_items WHERE YEAR(tx_date)=2023 GROUP BY dept_name ORDER BY total DESC"}}\n',
      'Q: 生成2024年1-6月每月利润表（收入/成本/毛利润/期间费用/净利润）\n' +
      'A: {"thought":"需要一个完整的1-6月月份序列，用单个派生表生成月份号，再分别LEFT JOIN各表按月聚合的结果；⛔禁止用两个平级派生表互相引用对方别名（如 (SELECT...) months JOIN (SELECT ... months.m ...) m ON 1=1），MySQL不支持子查询引用同级FROM子句中其它表的列","tool":"query_db","args":{"sql":"SELECT CONCAT(\'2024-\',LPAD(m.mo,2,\'0\')) AS month_label, COALESCE(r.revenue,0) AS revenue, COALESCE(c.cost,0) AS cost FROM (SELECT 1 AS mo UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6) m LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(amount) AS revenue FROM revenue_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' GROUP BY MONTH(tx_date)) r ON m.mo=r.mo LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(cogs_amount) AS cost FROM cost_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' AND cost_type=\'cogs\' GROUP BY MONTH(tx_date)) c ON m.mo=c.mo ORDER BY m.mo"}}\n' +
      /* English */
      '[Few-Shot Examples (required format)]\n' +
      'Q: What tables are in this database?\n' +
      'A: {"thought":"List all tables in current database","tool":"query_db","args":{"sql":"SHOW TABLES"}}\n\n' +
      'Q: What is the structure of each table?\n' +
      'A: {"thought":"Use plan_sql to enumerate tables then get column definitions","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"SHOW TABLES","desc":"List all tables"},\n' +
      '     {"sql":"SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION","desc":"All column definitions"}\n' +
      '   ]}}\n\n' +
      'Q: Count rows in each table\n' +
      'A: {"thought":"Query information_schema.TABLES","tool":"query_db","args":{"sql":"SELECT TABLE_NAME,TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE=\'BASE TABLE\'"}}\n\n' +
      'Q: Show orders from the last 7 days\n' +
      'A: {"thought":"Sliding window filter using NOW()-INTERVAL, not a fixed year","tool":"query_db","args":{"sql":"SELECT * FROM orders WHERE created_at >= NOW()-INTERVAL 7 DAY LIMIT 100"}}\n\n' +
      'Q: Analyze monthly order amount summary\n' +
      'A: {"thought":"Multi-step: inspect schema then aggregate","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"DESC orders","desc":"Confirm column names"},\n' +
      '     {"sql":"SELECT DATE_FORMAT(created_at,\'%Y-%m\') AS month,SUM(amount) AS total FROM orders GROUP BY month ORDER BY month","desc":"Monthly aggregation"}\n' +
      '   ]}}\n\n' +
      'Q: Analyze monthly revenue for 2024\n' +
      'A: {"thought":"User specified fixed year 2024; must use YEAR()=N, never NOW()-INTERVAL sliding window","tool":"query_db","args":{"sql":"SELECT DATE_FORMAT(tx_date,\'%Y-%m\') AS month,SUM(amount) AS total FROM revenue_items WHERE YEAR(tx_date)=2024 GROUP BY month ORDER BY month"}}\n\n' +
      'Q: Summarize department expenses for 2023\n' +
      'A: {"thought":"Fixed year filter + GROUP BY dept_name using YEAR()=N","tool":"query_db","args":{"sql":"SELECT dept_name,SUM(amount) AS total FROM expense_items WHERE YEAR(tx_date)=2023 GROUP BY dept_name ORDER BY total DESC"}}\n\n' +
      'Q: Generate a monthly profit & loss statement for Jan-Jun 2024 (revenue/cost/gross profit/period expenses/net profit)\n' +
      'A: {"thought":"Need a complete month sequence (1-6) from a single derived table, then LEFT JOIN separately-aggregated per-table results onto it; ⛔ never let one derived table\'s subquery reference another sibling derived table\'s alias/column in the same FROM clause (e.g. (SELECT...) months JOIN (SELECT ... months.m ...) m ON 1=1) — MySQL does not support a subquery referencing a sibling table in the same FROM clause, this raises Unknown table","tool":"query_db","args":{"sql":"SELECT CONCAT(\'2024-\',LPAD(m.mo,2,\'0\')) AS month_label, COALESCE(r.revenue,0) AS revenue, COALESCE(c.cost,0) AS cost FROM (SELECT 1 AS mo UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6) m LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(amount) AS revenue FROM revenue_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' GROUP BY MONTH(tx_date)) r ON m.mo=r.mo LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(cogs_amount) AS cost FROM cost_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' AND cost_type=\'cogs\' GROUP BY MONTH(tx_date)) c ON m.mo=c.mo ORDER BY m.mo"}}\n'
    );

    if (lang === 'zh') {
      return (
        '你是 ShannonBase Agent v4，内置于 ShannonBase 数据库的智能 SQL 助手。\n' +
        '当前数据库：' + db + '（SQL 中直接使用该库名，禁止任何占位符）\n\n' +
        current_time_info +
        get_workload_hint(qtype) + '\n\n' +
        (schema_ctx ? schema_ctx + '\n\n' : '') +
        (join_hint  ? join_hint  + '\n\n' : '') +
        (plan_hint  ? plan_hint  + '\n\n' : '') +
        '【可用工具】每次只输出一个合法 JSON，禁止在 JSON 前后添加任何文字：\n' +
        '1. {"thought":"...","tool":"query_db","args":{"sql":"SELECT ..."}}\n' +
        '   → 执行单条只读 SQL（SELECT/SHOW/DESC/EXPLAIN/WITH）\n' +
        '2. {"thought":"...","tool":"explain_sql","args":{"sql":"SELECT ..."}}\n' +
        '   → 分析执行计划，⚠ 结果中出现全表扫描时必须先改写 SQL\n' +
        '3. {"thought":"...","tool":"plan_sql","args":{"steps":[{"sql":"...","desc":"..."},...]}}\n' +
        '   → 【多步执行】一次提交有序 SQL 列表，引擎顺序执行并返回汇总结果\n' +
        '   → 适用：需要先 SHOW TABLES 再 SHOW CREATE、先探查结构再聚合等场景\n' +
        '4. {"thought":"...","tool":"begin_tx","args":{}}\n' +
        '5. {"thought":"...","tool":"update_data","args":{"sql":"INSERT/UPDATE/DELETE ..."}}\n' +
        '6. {"thought":"...","tool":"commit_tx","args":{}}\n' +
        '7. {"thought":"...","tool":"rollback_tx","args":{}}\n' +
        '8. {"thought":"...","tool":"ml_rag","args":{"question":"...","top_k":6}}\n' +
        '9. {"thought":"...","tool":"generate_text","args":{"prompt":"..."}}\n\n' +
        '【args 严格约束 - 违反视为错误】\n' +
        '  ① query_db / explain_sql / update_data：args.sql 必须是完整可执行 SQL，禁止为空或省略\n' +
        '  ② plan_sql：args.steps 必须是非空数组，每个元素含 sql 字段\n' +
        '  ③ 不确定 SQL 时：先用 query_db/plan_sql 探查 schema，再构造目标 SQL\n' +
        '  ④ 禁止输出 {"tool":"query_db","args":{}} 这类空 args\n\n' +
        '【关键约束】列名/表名必须来自上方 DDL；⛔ 禁用废弃系统表；' +
        'JOIN 优先使用 DDL 中的 FOREIGN KEY 子句；explain_sql 含⚠时先改写；' +
        '无需工具时直接输出自然语言；' +
        '⛔ 生成月份/日期序列时只能用单个派生表（如 SELECT 1 UNION ALL SELECT 2 ...），' +
        '禁止让一个派生表的子查询引用同级 FROM 子句中另一个派生表的别名/列' +
        '（MySQL 不支持，会报 Unknown table）；' +
        '⛔ 上方【相关表 DDL】中可能附带少量示例数据或检索片段，这些仅用于帮助理解表结构，' +
        '不代表该表实际的数据覆盖范围（时间区间、行数等）；遇到统计/汇总/聚合类问题时，' +
        '必须通过 query_db/plan_sql 执行真实的 SUM/COUNT/GROUP BY 等聚合 SQL 来得到结论，' +
        '禁止仅凭检索到的少量示例行就判断"数据不足""月份不全"而拒绝查询或直接给出免责声明。\n\n' +
        inline_few_shot + '\n\n' +
        (few_shot ? few_shot + '\n\n' : '') +
        '【历史对话】\n' + hist_section + '\n\n' +
        '【用户问题】\n' + user_message + '\n\n【助手】\n'
      );
    } else {
      return (
        'You are ShannonBase Agent v4, an intelligent SQL assistant embedded in ShannonBase.\n' +
        'Current database: ' + db + ' (use this name directly in SQL; no placeholders allowed)\n\n' +
        current_time_info +
        get_workload_hint(qtype) + '\n\n' +
        (schema_ctx ? schema_ctx + '\n\n' : '') +
        (join_hint  ? join_hint  + '\n\n' : '') +
        (plan_hint  ? plan_hint  + '\n\n' : '') +
        '[Available Tools] Output exactly one valid JSON per turn; no surrounding text:\n' +
        '1. {"thought":"...","tool":"query_db","args":{"sql":"SELECT ..."}}\n' +
        '   → Execute a single read-only SQL (SELECT/SHOW/DESC/EXPLAIN/WITH)\n' +
        '2. {"thought":"...","tool":"explain_sql","args":{"sql":"SELECT ..."}}\n' +
        '   → Analyze execution plan; ⚠ rewrite SQL if full table scan is detected\n' +
        '3. {"thought":"...","tool":"plan_sql","args":{"steps":[{"sql":"...","desc":"..."},...]}}\n' +
        '   → [Multi-step] Submit an ordered SQL list executed sequentially\n' +
        '   → Use for: SHOW TABLES then inspect columns, check schema then aggregate, etc.\n' +
        '4. {"thought":"...","tool":"begin_tx","args":{}}\n' +
        '5. {"thought":"...","tool":"update_data","args":{"sql":"INSERT/UPDATE/DELETE ..."}}\n' +
        '6. {"thought":"...","tool":"commit_tx","args":{}}\n' +
        '7. {"thought":"...","tool":"rollback_tx","args":{}}\n' +
        '8. {"thought":"...","tool":"ml_rag","args":{"question":"...","top_k":6}}\n' +
        '9. {"thought":"...","tool":"generate_text","args":{"prompt":"..."}}\n\n' +
        '[args Strict Constraints – violations are errors]\n' +
        '  ① query_db / explain_sql / update_data: args.sql must be a complete executable SQL; cannot be empty\n' +
        '  ② plan_sql: args.steps must be a non-empty array; each element must have a sql field\n' +
        '  ③ When SQL is uncertain: use query_db/plan_sql to explore schema first\n' +
        '  ④ Forbidden: {"tool":"query_db","args":{}} style empty args\n\n' +
        '[Key Constraints] Column/table names must come from the DDL above; ⛔ no deprecated system tables; ' +
        'prefer FOREIGN KEY clauses from DDL for JOINs; rewrite SQL when explain_sql shows ⚠; ' +
        'output natural language directly when no tool is needed; ' +
        '⛔ When generating a month/date sequence, use only a single derived table ' +
        '(e.g. SELECT 1 UNION ALL SELECT 2 ...) — never let one derived table\'s subquery ' +
        'reference another sibling derived table\'s alias/column in the same FROM clause ' +
        '(MySQL does not support this and will raise "Unknown table"); ' +
        '⛔ The [Relevant Table DDL] section above may include a few sample rows or retrieval ' +
        'snippets meant only to illustrate table structure — they do NOT represent the table\'s ' +
        'actual data coverage (date range, row count, etc.); for any statistical/aggregation ' +
        'question you MUST run a real SUM/COUNT/GROUP BY query via query_db/plan_sql to get the ' +
        'answer, never conclude "insufficient data" or "incomplete months" just from a handful of ' +
        'retrieved sample rows and refuse to query.\n\n' +
        inline_few_shot + '\n\n' +
        (few_shot ? few_shot + '\n\n' : '') +
        '[Conversation History]\n' + hist_section + '\n\n' +
        '[User Question]\n' + user_message + '\n\n[Assistant]\n'
      );
    }
  }

  function final_summary(system_prompt_base, tool_log) {
    return ml_generate(
      system_prompt_base +
      t('\n\n【已执行工具及结果】\n', '\n\n[Tool Execution Results]\n') +
      compress(tool_log, cfg('plan_log_max_tokens', 4000)) +
      t('\n\n请根据以上工具结果用清晰专业的中文直接回答用户问题。' +
        '禁止输出 JSON，禁止逐行复述原始数据，只输出结论和分析；' +
        '⛔ 表名/列名必须与工具结果中出现的原始名称完全一致，禁止编造未出现过的字段名：\n',
        '\n\nBased on the above results, answer the user\'s question clearly and professionally. ' +
        'Do not output JSON, do not repeat raw data row by row; output only conclusions and analysis. ' +
        '⛔ Table/column names must exactly match what appeared in the tool results — never fabricate ' +
        'a field name that did not actually appear:\n'),
      { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) }
    );
  }

  /* 
   * Main routing
   *
   *  A: catalog_match  – system monitoring (locks / processes …)
   *  B: rule_planner   – schema / DDL introspection          ← before RAG
   *  C: is_knowledge   – RAG vector retrieval
   *  D: LLM Agent loop – fallback for all other queries
   * */
  var MAX_TURNS        = 10;
  var PROMPT_TOK_LIMIT = 2800;
  var MAX_ERRORS       = 3;
  var _last_think = '';

  var chat_opt   = get_chat_options();

  if (conversation_id &&
      (!Array.isArray(chat_opt.chat_history) || chat_opt.chat_history.length === 0)) {

    var max_turns  = (chat_opt.history_length >= 0) ? chat_opt.history_length : 3;
    var recovered  = recover_chat_history_from_memory(conversation_id, max_turns);

    if (recovered.length > 0) {
      chat_opt.chat_history = recovered;
      save_chat_options(chat_opt);   /* writes to cache + @chat_options */
    }
  }
  var db_rows    = query("SELECT CAST(DATABASE() AS CHAR) AS db");
  var current_db = (db_rows && Array.isArray(db_rows) && db_rows.length && db_rows[0].db)
                   ? db_rows[0].db : '';
  var agent_response = '';

  /* ROUTE A: CATALOG */
  var cat = catalog_match(user_message);
  if (cat) {
    agent_response = t('【', '[') + cat.desc + t('】\n', ']\n') +
                     rows_to_text(query(cat.sql));
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, 'catalog:' + cat.sql);
    return agent_response;
  }

  /* ROUTE B: RULE PLANNER (schema / DDL introspection) */
  var rule_steps = rule_planner(user_message, current_db);
  if (rule_steps !== null) {
    var plan_results = execute_plan(rule_steps, current_db);
    var plan_log = '';
    for (var pri = 0; pri < plan_results.length; pri++) {
      plan_log += '\n--- Step ' + plan_results[pri].step + ': ' +
                  plan_results[pri].desc + ' ---\n' +
                  plan_results[pri].result + '\n';
    }

    var rule_summary_prompt =
      t('你是 ShannonBase 数据库助手。\n', 'You are ShannonBase database assistant.\n') +
      t('当前库：', 'Current database: ') + current_db + '\n\n' +
      t('用户问题：', 'User question: ')  + user_message + '\n\n' +
      t('已执行查询结果：\n', 'Query results:\n') +
      compress(plan_log, cfg('plan_log_max_tokens', 4000)) + '\n\n' +
      t('请用清晰的中文直接回答用户问题，不要输出 JSON，不要逐行复述原始数据。' +
        '⛔ 严格要求：表名、列名、数据类型必须与上方查询结果中出现的原始文字完全一致，' +
        '禁止凭经验编造或"补全"任何未出现在查询结果里的列名/字段（如常见命名习惯的 ' +
        'customer_name、revenue_date 等）；如果查询结果信息不足以回答某个细节，' +
        '直接说明信息不足，不要猜测填补：\n',
        'Answer the user\'s question clearly and directly. Do not output JSON; do not repeat raw data. ' +
        '⛔ Strict requirement: table/column names and data types must exactly match what appears in the ' +
        'query results above — never fabricate or "fill in" column names from common naming conventions ' +
        '(e.g. customer_name, revenue_date) that did not actually appear in the results; if the results ' +
        'are insufficient to answer some detail, say so explicitly rather than guessing:\n');

    agent_response = ml_generate(rule_summary_prompt, { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) });
    if (!agent_response || agent_response.trim().length < 5)
      agent_response = compress(plan_log, cfg('plan_log_max_tokens', 4000));

    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, plan_log + think_suffix());
    return agent_response;
  }

  /* ROUTE C: RAG / knowledge base */
  if (is_knowledge_query(user_message)) {
    var vector_tables = discover_vector_tables(chat_opt);
    var hw_result     = heatwave_dispatch(user_message, chat_opt, vector_tables);
    agent_response    = hw_result.response || '';
    if (hw_result.tables && hw_result.tables.length > 0)
      chat_opt.tables = hw_result.tables;
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, 'hw_mode:' + hw_result.mode + think_suffix());
    return agent_response;
  }

  /* ROUTE D: LLM Agent Loop */
  var history      = get_history(conversation_id, 8);
  var few_shot     = retrieve_few_shot(user_message, 3);
  var hw_hist_text = chat_history_to_text(chat_opt);

  var schema_embeddings_ready = check_schema_embeddings_ready(current_db);
  var logical_tasks           = decompose_query(user_message);

  var fixed_cost       = est_tok(hw_hist_text || history) + est_tok(few_shot) + 1000;
  var available_tokens = Math.max(400, PROMPT_TOK_LIMIT - fixed_cost);

  var schema_ctx = build_schema_context(current_db, available_tokens, chat_opt);

  var join_hint = '';
  /* NOTE: loop callback uses `task` to avoid shadowing the t() helper */
  if (!schema_embeddings_ready &&
      logical_tasks.some(function(task){ return task.op === 'join'; })) {
    var schema_graph = build_schema_graph(current_db);
    var tbl_m = schema_ctx.match(/^(\w+)\(/gm);
    if (tbl_m && tbl_m.length >= 2) {
      var candidates = tbl_m.slice(0, 4).map(function(s){ return s.replace('(', ''); });
      join_hint = suggest_joins(schema_graph, candidates);
    }
  }

  var plan_hint = logical_plan_to_hint(logical_tasks, schema_embeddings_ready);

  var system_prompt_base = build_system_prompt(
    current_db, schema_ctx, join_hint, plan_hint, few_shot, history, hw_hist_text
  );
  var full_prompt   = system_prompt_base;
  var prompt_tokens = est_tok(full_prompt);

  var tool_log = '', last_result = '', need_summary = false, error_count = 0;

  var last_tool_sig = ''; 
  for (var turn = 0; turn < MAX_TURNS; turn++) {
    var llm_out = ml_generate(full_prompt, {});
    var tool_obj = parse_tool_call(llm_out);

    if (!tool_obj) {
      agent_response = llm_out.trim() || last_result;
      need_summary   = false;
      break;
    }

    var cur_sig = tool_obj.tool + '|' + JSON.stringify(tool_obj.args || {});
    if (cur_sig === last_tool_sig) {
      if (last_result && last_result.length > 0) {
        agent_response = last_result;
        need_summary = false;
      } else {
        tool_log += '\n' + t('[重复检测] 模型输出与上轮相同，终止循环: ',
                            '[Loop detected] Same tool call as last turn, breaking: ') +
                    cur_sig.substring(0, 80);
        need_summary = true;
      }
      break;
    }
    last_tool_sig = cur_sig;    

    var validation_error = validate_tool_call(tool_obj);
    if (validation_error) {
      var err_hint =
        '\n' + t('[校验错误] ', '[Validation error] ') + validation_error +
        t('\n请重新输出合法 JSON（args.sql 不能为空）：\n【助手】\n',
          '\nPlease re-output a valid JSON (args.sql cannot be empty):\n[Assistant]\n');
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 工具校验失败次数过多：', '[Aborted] Too many validation failures: ') +
          validation_error;
        need_summary = true;
        break;
      }
      if (prompt_tokens + est_tok(err_hint) <= PROMPT_TOK_LIMIT) {
        full_prompt   += llm_out.trim() + err_hint;
        prompt_tokens += est_tok(err_hint);
      }
      continue;
    }

    var result;
    try {
      result = execute_tool(tool_obj.tool, tool_obj.args || {}, current_db);
    } catch (e) {
      result = t('工具执行异常：', 'Tool execution exception: ') + String(e);
    }
    last_result = result;
    need_summary = true;

    if (tool_obj.tool === 'plan_sql') {
      tool_log += '\n[Step ' + (turn+1) + '] tool=plan_sql' +
                  ' thought=' + (tool_obj.thought || '') +
                  '\nresult=' + compress(result, cfg('plan_log_max_tokens', 4000)) + '\n';
      need_summary = true;
      break;
    }

    /* error detection - check both zh and en prefixes produced by rows_to_text */
    if (result.indexOf('执行出错') !== -1 ||
        result.indexOf('Error: ') !== -1  ||
        result.indexOf('Unknown table') !== -1) {
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 连续错误，强制摘要。', '[Aborted] Consecutive errors, forcing summary.');
        break;
      }
    } else { error_count = 0; }

    tool_log += '\n[Step ' + (turn+1) + '] tool=' + tool_obj.tool +
                ' thought=' + (tool_obj.thought || '') +
                '\nresult=' + compress(result, 600) + '\n';

    if (tool_obj.tool === 'commit_tx' || tool_obj.tool === 'rollback_tx') {
      need_summary = true; break;
    }
    if (tool_obj.tool === 'generate_text') {
      agent_response = result; need_summary = false; break;
    }

    var append =
      '\n' + t('工具结果：', 'Tool result: ') +
      compress(result, 600) + '\n' +
      t('【助手】\n', '[Assistant]\n');
    if (prompt_tokens + est_tok(append) > PROMPT_TOK_LIMIT) { need_summary = true; break; }
    full_prompt   += llm_out.trim() + append;
    prompt_tokens += est_tok(append);
  }

  /* Safety net: force-rollback any uncommitted transaction */
  if (TX_STATE.active) {
    try { sys.exec_sql('ROLLBACK'); } catch(e) {}
    TX_STATE.active = false;
    tool_log +=
      '\n' + t('[安全网] 未提交事务已强制回滚。',
               '[Safety net] Uncommitted transaction force-rolled back.');
    if (need_summary)
      last_result += t('\n（事务已被强制回滚）', '\n(Transaction force-rolled back)');
  }

  if (need_summary && tool_log.length > 0) {
    var summary = final_summary(system_prompt_base, tool_log);
    agent_response = (summary && summary.trim().length > 0) ? summary.trim() : last_result;
  }

  if (!agent_response || agent_response.length === 0)
    agent_response = last_result.length > 0
      ? last_result
      : t('抱歉，未能生成有效回答，请重试。',
          'Sorry, unable to generate a valid response. Please try again.');

  if (!agent_response || agent_response.length === 0 || 
      (agent_response.trim().charAt(0) === '{' && agent_response.indexOf('"tool"') !== -1)) {
    if (tool_log.length > 0) {
      agent_response = final_summary(system_prompt_base, tool_log);
    } else {
      agent_response = t('抱歉，未能生成有效回答，请重试。',
                        'Sorry, unable to generate a valid response. Please try again.');
    }
  }

  chat_opt = update_chat_history(chat_opt, user_message, agent_response);
  chat_opt.response = agent_response; chat_opt.request_completed = true;
  save_chat_options(chat_opt);

  persist_turn(conversation_id, user_message, agent_response, tool_log + think_suffix());
  return agent_response;
}

return shannon_agent_run(user_message, conversation_id);
$$;

DELIMITER ;

-- sys.shannon_chat - dispatcher / plugin resolution
DROP FUNCTION IF EXISTS sys.shannon_chat;

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION sys.shannon_chat(
    user_message     TEXT,
    conversation_id  VARCHAR(64)
) RETURNS LONGTEXT  SQL SECURITY INVOKER LANGUAGE JAVASCRIPT
COMMENT '
Description
-----------
ShannonBase Agent - intelligent SQL assistant embedded in ShannonBase.
Accepts a natural-language message and a conversation UUID; returns a
text answer.

Parameters
-----------
user_message    TEXT         Natural-language question or instruction.
conversation_id VARCHAR(64)  UUID identifying the conversation session.
                             Use UUID() to start a new session.

Session configuration  (@chat_options JSON)
-----------
Set @chat_options before calling to customise behaviour.
All keys are optional; defaults are shown in parentheses.

Model settings:
  model_options.model_id          VARCHAR   LLM model ("Qwen3.5-2B-ONNX")
  model_options.temperature       FLOAT     Sampling temperature (0.25)
  model_options.max_tokens        INT       Max generation tokens (1200)
  model_options.top_p             FLOAT     Top-p sampling (0.95)
  model_options.repeat_penalty    FLOAT     Repeat penalty (1.1)
  model_options.frequency_penalty FLOAT     Frequency penalty (0.0)
  model_options.presence_penalty  FLOAT     Presence penalty (0.0)

Budget / quality:
  plan_log_max_tokens INT   Max chars of query results passed to LLM for
                            summarisation in ROUTE B and final_summary. (4000)
                            Increase for wide schemas or many tables.
  summary_max_tokens  INT   Max tokens for rule-summary and final-summary
                            LLM calls. (2000)
                            Increase when answers are truncated.

RAG settings:
  retrieve_top_k      INT   Number of RAG citations to retrieve. (6)
  retrieval_options   JSON  {max_distance FLOAT, percentage_distance FLOAT,
                             segment_overlap INT}
  embed_model_id      VARCHAR  Embedding model. ("multilingual-e5-small")
  tables              JSON  Cached vector-store list: [{schema_name, table_name}].
                            Populated automatically on first RAG call;
                            cleared to force re-discovery.

History:
  history_length      INT   Number of conversation turns to retain. (3)
  schema_name         VARCHAR  Restrict schema-metadata lookups to one schema.

Example
-----------
-- Minimal (all defaults)
SET @s1 = UUID();
SELECT sys.shannon_chat("当前库有哪些表？", @s1) AS answer;

-- Custom model + wider budget
SET @chat_options = JSON_OBJECT(
  "model_options",     JSON_OBJECT(
                         "model_id",    "Qwen2.5-0.5B-Instruct",
                         "temperature", 0,
                         "max_tokens",  5000),
  "plan_log_max_tokens", 8000,
  "summary_max_tokens", 3000,
  "retrieve_top_k",     8,
  "history_length",     5
);
SET @s1 = UUID();
SELECT sys.shannon_chat("list all tables and their relationships", @s1) AS answer;
'
AS $$
function dispatcher(user_message, conversation_id) {

  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/\\/g, '\\\\')
      .replace(/'/g,  "\\'");
  }

  function query(sql) {
    try   { return JSON.parse(sys.exec_sql(sql)); }
    catch (e) { return { error: String(e) }; }
  }

  function scalar(rows, col) {
    if (!Array.isArray(rows) || !rows.length) return null;
    var v = rows[0][col];
    return (v === undefined || v === null) ? null : String(v);
  }

  function call_plugin(schema, func, msg, conv_id) {
    var sql =
      "SELECT `" + esc(schema) + "`.`" + esc(func) + "`(" +
      "'" + esc(msg) + "','" + esc(conv_id) + "') AS result";
    var rows = query(sql);
    if (!rows || rows.error)
      return { ok: false, error: String(rows && rows.error || 'call failed') };
    return { ok: true, result: scalar(rows, 'result') || '' };
  }

  function func_exists(schema, func) {
    var rows = query(
      "SELECT 1 AS found FROM information_schema.ROUTINES" +
      " WHERE ROUTINE_TYPE='FUNCTION'" +
      "   AND ROUTINE_SCHEMA='" + esc(schema) + "'" +
      "   AND ROUTINE_NAME='"   + esc(func)   + "'" +
      " LIMIT 1"
    );
    return Array.isArray(rows) && rows.length > 0;
  }

  function current_db() {
    var rows = query("SELECT CAST(DATABASE() AS CHAR) AS db");
    return scalar(rows, 'db') || '';
  }

  /*
   * L1: session variable  @shannon_agent_plugin
   *     format: 'schema_name.function_name'
   *     SET @shannon_agent_plugin = 'mydb.my_agent';
   */
  var l1_rows = query("SELECT @shannon_agent_plugin AS plugin");
  var l1_val  = scalar(l1_rows, 'plugin');

  if (l1_val && l1_val.indexOf('.') !== -1) {
    var l1_parts  = l1_val.split('.');
    var l1_schema = l1_parts[0].trim();
    var l1_func   = l1_parts.slice(1).join('.').trim();
    if (func_exists(l1_schema, l1_func)) {
      var l1_res = call_plugin(l1_schema, l1_func, user_message, conversation_id);
      if (l1_res.ok) return l1_res.result;
    }
  }

  /*
   * L2: mysql.shannon_agent_plugins  (enabled=1, ordered by priority)
   */
  var l2_rows = query(
    "SELECT schema_name, function_name FROM mysql.shannon_agent_plugins" +
    " WHERE enabled = 1 ORDER BY priority ASC, created_at ASC LIMIT 10"
  );
  if (Array.isArray(l2_rows)) {
    for (var i = 0; i < l2_rows.length; i++) {
      var l2_schema = l2_rows[i].schema_name;
      var l2_func   = l2_rows[i].function_name;
      if (!l2_schema || !l2_func) continue;
      if (!func_exists(l2_schema, l2_func)) continue;
      var l2_res = call_plugin(l2_schema, l2_func, user_message, conversation_id);
      if (l2_res.ok) return l2_res.result;
    }
  }

  /*
   * L3: {current_db}.shannon_agent()
   */
  var db = current_db();
  if (db && func_exists(db, 'shannon_agent')) {
    var l3_res = call_plugin(db, 'shannon_agent', user_message, conversation_id);
    if (l3_res.ok) return l3_res.result;
  }

  /*
   * L4: sys.shannon_agent_default()  (built-in fallback)
   */
  var l4_rows = query(
    "SELECT sys.shannon_agent_default('" +
    esc(user_message) + "','" + esc(conversation_id) + "') AS result"
  );
  return scalar(l4_rows, 'result') ||
         'system default engine failed, please check sys.shannon_agent_default.';
}

return dispatcher(user_message, conversation_id);
$$;

DELIMITER ;
