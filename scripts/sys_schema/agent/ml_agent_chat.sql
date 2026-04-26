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
) RETURNS TEXT SQL SECURITY INVOKER LANGUAGE JAVASCRIPT 
AS $$

function shannon_agent_run(user_message, conversation_id) {

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
      if (rows && rows.error)                       return '执行出错：' + rows.error;
      if (rows && rows.affected_rows !== undefined) return '执行成功，影响行数：' + rows.affected_rows;
      return JSON.stringify(rows);
    }
    if (rows.length === 0) return '（查询结果为空）';
    var cols = Object.keys(rows[0]);
    var cap  = Math.min(rows.length, limit);
    var out  = ['共 ' + rows.length + ' 条：'];
    for (var i = 0; i < cap; i++) {
      out.push('行' + (i + 1) + ':');
      for (var c = 0; c < cols.length; c++) {
        var v = rows[i][cols[c]];
        out.push('  ' + cols[c] + ': ' + (v === null ? 'NULL' : String(v)));
      }
    }
    if (rows.length > cap) out.push('（仅展示前 ' + cap + ' 条）');
    return out.join('\n');
  }
 
  function compress(text, max_chars) {
    max_chars = max_chars || 700;
    if (!text || text.length <= max_chars) return text;
    return text.substring(0, max_chars) +
           '\n…[截断，原长 ' + text.length + ' 字符]';
  }
 
  function est_tok(s) { return Math.ceil((s || '').length / 3); }
  function gen_query_id() { return Math.random().toString(36).substring(2, 10); }
 
  var _cached_chat_opt = null;
  function get_chat_options() {
    if (_cached_chat_opt) return _cached_chat_opt;
    try {
      var rows = query("SELECT @chat_options AS opt");
      if (!rows || !Array.isArray(rows) || !rows.length || !rows[0].opt) return {};
      _cached_chat_opt = JSON.parse(rows[0].opt);
      return _cached_chat_opt;
    } catch(e) { return {}; }
  }
 
  function ml_generate(prompt, extra) {
    var chat_opt  = get_chat_options();
    var model_opts = (chat_opt && chat_opt.model_options) ? chat_opt.model_options : {};
    var o = Object.assign({
      task: 'generation', model_id: 'Qwen3.5-2B-ONNX', language: 'zh',
      temperature: 0.25, max_tokens: 1200, top_p: 0.95,
      repeat_penalty: 1.1, frequency_penalty: 0.0, presence_penalty: 0.0
    }, model_opts, extra || {});
    var sql =
      "SELECT sys.ML_GENERATE('" + esc(prompt) + "'," +
      "JSON_OBJECT('task','"             + esc(o.task)             + "'," +
                  "'model_id','"         + esc(o.model_id)         + "'," +
                  "'language','"         + esc(o.language)         + "'," +
                  "'temperature',"       + Number(o.temperature)   + "," +
                  "'max_tokens',"        + Number(o.max_tokens)    + "," +
                  "'top_p',"             + Number(o.top_p)         + "," +
                  "'repeat_penalty',"    + Number(o.repeat_penalty)    + "," +
                  "'frequency_penalty'," + Number(o.frequency_penalty) + "," +
                  "'presence_penalty',"  + Number(o.presence_penalty)  +
      ")) AS result";
    var rows = query(sql);
    return (rows && Array.isArray(rows) && rows.length && rows[0].result != null)
           ? String(rows[0].result) : '';
  }
 
  function ml_rag(question, topK, opt_override) {
    topK = topK || 6;
    var opt = Object.assign({ n_citations: topK, distance_metric: 'COSINE' }, opt_override || {});
    var rows = query(
      "SELECT sys.ML_RAG('" + esc(question) + "','" + esc(JSON.stringify(opt)) + "') AS result"
    );
    if (!rows || !Array.isArray(rows) || !rows.length || rows[0].result == null) return '';
    try {
      var obj = JSON.parse(rows[0].result);
      return obj.text || String(rows[0].result);
    } catch(e) { return String(rows[0].result); }
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
    var opts = Object.assign(
      { n_results: n_results, include_comments: true, embed_model_id: 'all-MiniLM-L12-v2' },
      extra_opts || {}
    );
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
      function compact_type(t) {
        return t.replace(/varchar\(\d+\)/i,'VC').replace(/char\(\d+\)/i,'CH')
                .replace(/bigint\(\d+\) unsigned/i,'UBIGINT').replace(/bigint\(\d+\)/i,'BIGINT')
                .replace(/int\(\d+\) unsigned/i,'UINT').replace(/int\(\d+\)/i,'INT')
                .replace(/tinyint\(1\)/i,'BOOL').replace(/tinyint\(\d+\)/i,'TINT')
                .replace(/decimal\([\d,]+\)/i,'DEC').replace(/timestamp/i,'TS')
                .replace(/datetime/i,'DT').replace(/(long|medium|tiny)?text/i,'TEXT')
                .toUpperCase();
      }
 
      var lines = ['【Schema（fallback）: ' + db + '】'];
      var total = 0;
      if (tbl_rows.length > MAX_TABLES) {
        lines.push('表共 ' + tbl_rows.length + ' 张: ' +
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
            lines.push('（剩余: ' +
              tbl_rows.slice(ti).map(function(r){return r.TABLE_NAME;}).join(',') + '）');
            break;
          }
          lines.push(line);
        }
      }
      if (fk_str.length > 0) lines.push('【JOIN路径】' + fk_str);
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
        return '【相关表 DDL（语义排序，FOREIGN KEY 可用于 JOIN）】\n' + result;
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
    return hints.length > 0 ? '【推荐JOIN路径（BFS）】\n' + hints.join('\n') : '';
  }
 
  function save_chat_options(chat_opt) {
    _cached_chat_opt = chat_opt;
    try { sys.exec_sql("SET @chat_options = '" + esc(JSON.stringify(chat_opt)) + "'"); }
    catch(e) {}
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
      return '用户：' + (h.user_message || '') + '\n助手：' + (h.chat_bot_message || '');
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
      return (r.role === 'user' ? '用户' : '助手') + '：' + r.content;
    }).join('\n');
  }
 
  function save_memory(conv_id, role, content, thought, with_embedding) {
    with_embedding = (with_embedding === true);
    if (with_embedding) {
      sys.exec_sql(
        "INSERT INTO mysql.agent_memory(conversation_id, role, content, thought, embedding) " +
        "SELECT '" + esc(conv_id) + "','" + esc(role) + "','" + esc(content) + "','" +
        esc(thought || '') + "', " +
        "sys.ML_EMBED_ROW('" + esc(content) + "', JSON_OBJECT('model_id','all-MiniLM-L12-v2','truncate',true))"
      );
    } else {
      sys.exec_sql(
        "INSERT INTO mysql.agent_memory(conversation_id, role, content, thought, embedding) " +
        "VALUES('" + esc(conv_id) + "','" + esc(role) + "','" + esc(content) + "','" +
        esc(thought || '') + "', NULL)"
      );
    }
  }
 
  function retrieve_few_shot(question, topK) {
    topK = topK || 3;
    try {
      var emb_rows = query(
        "SELECT sys.ML_EMBED_ROW('" + esc(question) + "'," +
        "JSON_OBJECT('model_id','all-MiniLM-L12-v2','truncate',true)) AS emb"
      );
      if (!emb_rows || !emb_rows.length || !emb_rows[0].emb) return '';
      var emb_val  = emb_rows[0].emb;
      var sim_rows = query(
        "SELECT content, thought FROM mysql.agent_memory" +
        " WHERE role='assistant' AND thought LIKE '%query_db%'" +
        "   AND conversation_id != '" + esc(conversation_id) + "'" +
        "   AND embedding IS NOT NULL" +
        " ORDER BY VEC_DISTANCE_COSINE(embedding,'" + esc(emb_val) + "') ASC LIMIT " + topK
      );
      if (!Array.isArray(sim_rows) || !sim_rows.length) return '';
      var lines = ['【Few-Shot 参考（向量检索）】'];
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
    var pat = new RegExp(
      '^(什么是|what is|how does|解释|explain|介绍|describe|为什么|why|' +
      'what are|有哪些|如何理解|原理|architecture|概念|区别|对比|compare|' +
      'shannonbase|heatwave|innodb|mysql.*特性|raft|mvcc|lsm)', 'i'
    );
    var sql_pat = /SELECT|INSERT|UPDATE|DELETE|查询|查找|统计|修改|删除|插入|导出/i;
    return pat.test(text.trim()) && !sql_pat.test(text);
  }
 
  function heatwave_dispatch(user_msg, chat_opt, vector_tables) {
    if (!vector_tables || vector_tables.length === 0) {
      if (chat_opt.skip_generate) return { mode:'EMPTY', response:'未找到向量知识库。', tables:[] };
      var hist_ctx = chat_history_to_text(chat_opt);
      return {
        mode: 'GENERATE',
        response: ml_generate((hist_ctx ? hist_ctx + '\n\n' : '') + '用户：' + user_msg + '\n助手：',
                              chat_opt.model_options || {}),
        tables: []
      };
    }
    var ret_opt = {};
    if (chat_opt.retrieval_options) {
      try { ret_opt = typeof chat_opt.retrieval_options === 'string'
                      ? JSON.parse(chat_opt.retrieval_options) : chat_opt.retrieval_options; }
      catch(e) {}
    }
    var rag_extra = {};
    if (chat_opt.embed_model_id)   rag_extra.model_id       = chat_opt.embed_model_id;
    if (ret_opt.max_distance)      rag_extra.max_distance    = ret_opt.max_distance;
    if (ret_opt.segment_overlap)   rag_extra.segment_overlap = ret_opt.segment_overlap;
    var hist_ctx     = chat_history_to_text(chat_opt);
    var rag_question = hist_ctx ? hist_ctx + '\n用户：' + user_msg : user_msg;
    var rag_res = ml_rag(rag_question, chat_opt.retrieve_top_k || 6, rag_extra);
    if (rag_res && rag_res.length >= 30)
      return { mode: 'RAG', response: rag_res, tables: vector_tables };
    if (!chat_opt.skip_generate) {
      var fb_prompt = (hist_ctx ? hist_ctx + '\n\n' : '') + '用户：' + user_msg + '\n助手：';
      return { mode:'GENERATE', response: ml_generate(fb_prompt, chat_opt.model_options || {}), tables: vector_tables };
    }
    return { mode:'EMPTY', response:'知识库中未找到相关内容。', tables: vector_tables };
  }
 
  var SYSTEM_QUERY_CATALOG = [
    { pattern: /锁[\s\S]{0,15}情况|当前.*持有.*锁|data.?locks/i,
      sql: 'SELECT ENGINE_TRANSACTION_ID,OBJECT_SCHEMA,OBJECT_NAME,LOCK_TYPE,LOCK_MODE,LOCK_STATUS,LOCK_DATA FROM performance_schema.data_locks LIMIT 50',
      desc: '当前持有的行锁' },
    { pattern: /锁等待|lock.?wait|谁在等|被谁锁/i,
      sql: 'SELECT r.trx_id AS waiting_trx,r.trx_mysql_thread_id AS waiting_thread,r.trx_query AS waiting_query,b.trx_id AS blocking_trx,b.trx_mysql_thread_id AS blocking_thread,b.trx_query AS blocking_query,w.BLOCKING_ENGINE_LOCK_ID FROM information_schema.INNODB_TRX r JOIN performance_schema.data_lock_waits w ON r.trx_id=w.REQUESTING_ENGINE_TRANSACTION_ID JOIN information_schema.INNODB_TRX b ON b.trx_id=w.BLOCKING_ENGINE_TRANSACTION_ID',
      desc: '锁等待关系' },
    { pattern: /死锁|deadlock/i,  sql: 'SHOW ENGINE INNODB STATUS', desc: 'InnoDB 死锁详情' },
    { pattern: /活跃事务|active.?tr(x|ansaction)|innodb.*trx|长事务/i,
      sql: 'SELECT trx_id,trx_state,trx_started,trx_mysql_thread_id,LEFT(trx_query,100) AS trx_query,trx_rows_modified,trx_rows_locked FROM information_schema.INNODB_TRX ORDER BY trx_started',
      desc: '当前活跃 InnoDB 事务' },
    { pattern: /processlist|进程列表|正在执行|running.?quer/i,
      sql: "SELECT ID,USER,HOST,DB,COMMAND,TIME,STATE,LEFT(INFO,120) AS QUERY FROM information_schema.PROCESSLIST WHERE COMMAND!='Sleep' ORDER BY TIME DESC",
      desc: '当前活跃进程列表' },
    { pattern: /连接数|thread.?connect|Threads_connected/i,
      sql: 'SHOW STATUS LIKE "Threads%"', desc: '线程连接数统计' },
    { pattern: /慢查询|slow.?quer|top.*慢|耗时最长/i,
      sql: 'SELECT DIGEST_TEXT,COUNT_STAR,ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec,ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec,SUM_ROWS_EXAMINED,SUM_ROWS_SENT FROM performance_schema.events_statements_summary_by_digest ORDER BY AVG_TIMER_WAIT DESC LIMIT 20',
      desc: '慢查询 TOP20' },
    { pattern: /buffer.?pool|缓冲池/i,
      sql: 'SELECT POOL_ID,POOL_SIZE,FREE_BUFFERS,DATABASE_PAGES,HIT_RATE,PAGES_MADE_YOUNG FROM information_schema.INNODB_BUFFER_POOL_STATS',
      desc: 'InnoDB Buffer Pool 状态' },
    { pattern: /redo.?log|redo日志|log.?capacity/i,
      sql: 'SHOW STATUS LIKE "Innodb_redo%"', desc: 'InnoDB Redo Log 状态' },
    { pattern: /表空间|tablespace/i,
      sql: 'SELECT SPACE_ID,NAME,ROW_FORMAT,PAGE_SIZE,ZIP_PAGE_SIZE FROM information_schema.INNODB_TABLESPACES ORDER BY NAME LIMIT 50',
      desc: 'InnoDB 表空间信息' },
    { pattern: /系统变量|global.?var|show.?variable/i,
      sql: 'SHOW VARIABLES LIKE "innodb%"', desc: 'InnoDB 相关系统变量' }
  ];
 
  function catalog_match(text) {
    for (var i = 0; i < SYSTEM_QUERY_CATALOG.length; i++)
      if (SYSTEM_QUERY_CATALOG[i].pattern.test(text)) return SYSTEM_QUERY_CATALOG[i];
    return null;
  }
 
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
    return { sql: new_sql, desc: '⚡自动恢复：' + bad + ' → ' + replacement };
  }
 
  function estimate_complexity(text) {
    var signals = [/并且|同时|以及|另外|还要|而且/,/按.*分组|group\s+by/i,/关联|联合|join/i,
                   /子查询|in\s*\(select/i,/分析|统计|趋势|占比|同比|环比/,
                   /先.*再.*然后|第一步.*第二步/,/最高.*同时|最低.*并且|聚合.*过滤/];
    var score = 0;
    for (var i = 0; i < signals.length; i++) if (signals[i].test(text)) score++;
    return score;
  }
 
  function decompose_query(text) {
    if (estimate_complexity(text) < 3) return [{ op: 'scan', note: text }];
    var tasks = [{ op: 'scan', note: '主表扫描' }];
    if (/最近|过去\s*\d+|大于|小于|等于|筛选|过滤|WHERE/i.test(text)) tasks.push({ op:'filter', note:'条件过滤' });
    if (/关联|联合|join/i.test(text))     tasks.push({ op:'join',   note:'多表关联' });
    if (/聚合|汇总|统计|SUM|AVG|COUNT/i.test(text)) tasks.push({ op:'agg', note:'聚合计算' });
    if (/排名|排序|最高|最低|前\s*\d+/i.test(text)) tasks.push({ op:'sort', note:'排序限行' });
    tasks.push({ op: 'explain', note: '执行计划验证' });
    return tasks;
  }
 
  function logical_plan_to_hint(tasks, schema_from_embeddings) {
    if (!tasks || tasks.length <= 1) return '';
    var op_hints = {
      scan:    '确定主表，必要时先用 query_db 检查表结构',
      filter:  '构造 WHERE 条件，确认谓词列有索引',
      join:    schema_from_embeddings
               ? '按上方 DDL 中的 FOREIGN KEY 子句拼写 JOIN 条件'
               : '按【推荐JOIN路径】拼写 JOIN 子句',
      agg:     '构造 GROUP BY + 聚合函数，注意 HAVING 与 WHERE 顺序',
      sort:    '添加 ORDER BY；LIMIT 避免大结果集',
      explain: '调用 explain_sql 验证；⚠全表扫描时先改写 SQL'
    };
    var lines = ['【逻辑计划（按序执行，不得跳步）】'];
    for (var i = 0; i < tasks.length; i++)
      lines.push('Step' + (i+1) + ' [' + tasks[i].op.toUpperCase() + ']: ' +
                 (op_hints[tasks[i].op] || tasks[i].op) + '  ← ' + tasks[i].note);
    return lines.join('\n');
  }
 
  function rule_planner(msg, db) {
    /* 返回 [{sql, desc}] 或 null（表示不命中，走 LLM Agent） */
 
    var lo = msg.toLowerCase();
 
    /* ---- 模式 1：列出所有表 ---- */
    if (/有哪些表|所有表|列出.*表|show.?tables/i.test(msg) &&
        !/结构|schema|column|字段|列|create/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME, TABLE_ROWS, TABLE_COMMENT" +
               " FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
               " ORDER BY TABLE_NAME",
          desc: '获取所有表概览' }
      ];
    }
 
    /* ---- 模式 2：表结构（含列信息） ---- */
    if (/(表.*结构|结构.*表|字段|列信息|schema|column.*info)/i.test(msg)) {
      /* 先拿表名，再逐表拿列定义 */
      var tbl_rows = query(
        "SELECT TABLE_NAME FROM information_schema.TABLES" +
        " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME"
      );
      var steps = [
        { sql: "SELECT TABLE_NAME FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME",
          desc: '枚举所有表名' }
      ];
      /* 对每张表追加一个 SHOW CREATE TABLE */
      if (Array.isArray(tbl_rows)) {
        for (var ti = 0; ti < Math.min(tbl_rows.length, 20); ti++) {
          var tn = tbl_rows[ti].TABLE_NAME;
          steps.push({
            sql: 'SHOW CREATE TABLE `' + esc(db) + '`.`' + esc(tn) + '`',
            desc: '表 ' + tn + ' 的 DDL'
          });
        }
      } else {
        /* 兜底：用 information_schema 列信息 */
        steps.push({
          sql: "SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY,COLUMN_DEFAULT,IS_NULLABLE,COLUMN_COMMENT" +
               " FROM information_schema.COLUMNS" +
               " WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION",
          desc: '所有表的列定义'
        });
      }
      return steps;
    }
 
    /* ---- 模式 3：外键 / JOIN 关系 ---- */
    if (/(外键|foreign.?key|join.*关系|关联关系)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
               " FROM information_schema.KEY_COLUMN_USAGE" +
               " WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL",
          desc: '所有外键关系（JOIN 路径）' }
      ];
    }
 
    /* ---- 模式 4：索引分析 ---- */
    if (/(索引|index.*分析|缺少.*索引|覆盖.*索引)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME,INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX,COLUMN_NAME,CARDINALITY,INDEX_TYPE" +
               " FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()" +
               " ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX",
          desc: '当前库所有索引详情' },
        { sql: "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE" +
               " FROM information_schema.COLUMNS" +
               " WHERE TABLE_SCHEMA=DATABASE()" +
               "   AND COLUMN_NAME NOT IN (" +
               "     SELECT DISTINCT COLUMN_NAME FROM information_schema.STATISTICS" +
               "     WHERE TABLE_SCHEMA=DATABASE())" +
               " ORDER BY TABLE_NAME,ORDINAL_POSITION",
          desc: '未被索引覆盖的列（候选索引）' }
      ];
    }
 
    /* ---- 模式 5：数据量统计 ---- */
    if (/(数据量|行数|record.?count|row.?count|表.*大小|数据大小)/i.test(msg)) {
      return [
        { sql: "SELECT TABLE_NAME," +
               "  TABLE_ROWS," +
               "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024, 2) AS size_mb," +
               "  DATA_FREE" +
               " FROM information_schema.TABLES" +
               " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
               " ORDER BY DATA_LENGTH+INDEX_LENGTH DESC",
          desc: '各表数据量与磁盘占用' }
      ];
    }
 
    /* ---- 模式 6：查询性能 TOP N ---- */
    if (/(性能.*top|top.*性能|最慢.*查询|query.*perf)/i.test(msg)) {
      return [
        { sql: "SELECT DIGEST_TEXT,COUNT_STAR," +
               "  ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec," +
               "  ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec," +
               "  SUM_ROWS_EXAMINED, SUM_ROWS_SENT" +
               " FROM performance_schema.events_statements_summary_by_digest" +
               " ORDER BY SUM_TIMER_WAIT DESC LIMIT 20",
          desc: 'SQL 性能 TOP20' }
      ];
    }
 
    /* ---- 其他：无命中 → 返回 null → 走 LLM Agent ---- */
    return null;
  }
 
  var MAX_PLAN_STEPS = 15;
 
  function execute_plan(steps, db) {
    var results = [];
    var last_result_text = '';
 
    for (var i = 0; i < Math.min(steps.length, MAX_PLAN_STEPS); i++) {
      var step = steps[i];
      var sql  = replace_ph(String(step.sql || ''), db);
 
      /* 支持上步结果注入（仅截断前 200 字符） */
      if (sql.indexOf('__LAST_RESULT__') !== -1)
        sql = sql.replace(/__LAST_RESULT__/g, esc(last_result_text.substring(0, 200)));
 
      var upper = sql.trim().toUpperCase().split(/\s+/)[0];
      var RO = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
 
      var result_text;
      if (RO[upper]) {
        var raw = query(sql);
        result_text = compress(rows_to_text(raw), 800);
        /* 自动恢复 Unknown table */
        if (result_text.indexOf('Unknown table') !== -1) {
          var recovery = try_recover_unknown_table(result_text, sql);
          if (recovery) {
            result_text = recovery.desc + '\n' + compress(rows_to_text(query(recovery.sql)), 800);
          }
        }
      } else {
        result_text = '跳过非只读 SQL（plan_sql 中仅执行 SELECT/SHOW/DESC）：' + sql.substring(0, 80);
      }
 
      last_result_text = result_text;
      results.push({
        step: i + 1,
        desc: step.desc || ('Step ' + (i+1)),
        sql:  sql,
        result: result_text
      });
    }
 
    return results;
  }
 
  function validate_tool_call(tool_obj) {
    if (!tool_obj || typeof tool_obj.tool !== 'string') return '工具调用格式错误：缺少 tool 字段';
 
    var args = tool_obj.args || {};
 
    switch (tool_obj.tool) {
      case 'query_db':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 3)
          return 'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...';
        break;
 
      case 'explain_sql':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
          return 'explain_sql 缺少有效 sql 参数';
        break;
 
      case 'update_data':
        if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
          return 'update_data 缺少有效 sql 参数';
        break;
 
      case 'plan_sql':
        if (!Array.isArray(args.steps) || args.steps.length === 0)
          return 'plan_sql 缺少 steps 数组，或 steps 为空';
        for (var i = 0; i < args.steps.length; i++) {
          if (!args.steps[i].sql || typeof args.steps[i].sql !== 'string')
            return 'plan_sql steps[' + i + '] 缺少 sql 字段';
        }
        break;
 
      case 'ml_rag':
        if (!args.question || typeof args.question !== 'string')
          return 'ml_rag 缺少 question 参数';
        break;
 
      case 'generate_text':
        if (!args.prompt || typeof args.prompt !== 'string')
          return 'generate_text 缺少 prompt 参数';
        break;
 
      case 'begin_tx':
      case 'commit_tx':
      case 'rollback_tx':
        break; /* args 可为空 */
 
      default:
        return '未知工具：' + tool_obj.tool;
    }
    return null; /* 合法 */
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
        if (node.table_name)             out.push('表=' + node.table_name);
        if (node.access_type) {
          out.push('访问=' + node.access_type);
          if (node.access_type === 'ALL') out.push('⚠全表扫描');
        }
        if (node.rows_examined_per_scan) out.push('扫描行≈' + node.rows_examined_per_scan);
        if (node.using_filesort)  out.push('⚠需filesort');
        if (node.using_temporary) out.push('⚠临时表');
        if (node.key) out.push('索引=' + node.key);
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
 
    /* ---- query_db ---- */
    if (tool === 'query_db') {
      sql = replace_ph(String(args.sql || ''), db);
      first = sql.trim().toUpperCase().split(/\s+/)[0];
      var RO = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
      if (!RO[first]) return '拒绝：query_db 只允许只读语句（SELECT/SHOW/DESC/EXPLAIN/WITH）。';
      var result = compress(rows_to_text(query(sql)), 900);
      if (result.indexOf('Unknown table') !== -1) {
        var recovery = try_recover_unknown_table(result, sql);
        if (recovery) return recovery.desc + '\n' + compress(rows_to_text(query(recovery.sql)), 900);
      }
      return result;
    }
 
    /* ---- explain_sql ---- */
    if (tool === 'explain_sql') {
      sql = replace_ph(String(args.sql || ''), db);
      var ex = query("EXPLAIN FORMAT=JSON " + sql);
      if (!ex || !ex.length) return 'EXPLAIN 执行失败';
      var raw = ex[0]['EXPLAIN'] || ex[0]['explain'] || JSON.stringify(ex[0]);
      return '执行计划：' + parse_explain(String(raw));
    }
 
    /* ---- NEW: plan_sql ---- */
    if (tool === 'plan_sql') {
      var steps   = args.steps || [];
      var plan_results = execute_plan(steps, db);
      /* 将多步结果拼成可读文本返回给 LLM 做汇总 */
      var out_lines = ['【plan_sql 多步执行结果】'];
      for (var pi = 0; pi < plan_results.length; pi++) {
        var pr = plan_results[pi];
        out_lines.push('--- Step ' + pr.step + ': ' + pr.desc + ' ---');
        out_lines.push(pr.result);
      }
      return compress(out_lines.join('\n'), 2000);
    }
 
    /* ---- 事务工具 ---- */
    if (tool === 'begin_tx') {
      if (TX_STATE.active) return '警告：事务已活跃，禁止重复 begin_tx。';
      sys.exec_sql('START TRANSACTION'); TX_STATE.active = true; return '事务已开启';
    }
    if (tool === 'update_data') {
      if (!TX_STATE.active) return '拒绝：写操作必须在事务内，请先 begin_tx。';
      sql = replace_ph(String(args.sql || ''), db);
      upper = sql.trim().toUpperCase();
      first = upper.split(/\s+/)[0];
      if ((first==='UPDATE'||first==='DELETE') && !/\bWHERE\b/.test(upper))
        return '拒绝：' + first + ' 必须含 WHERE 条件。';
      sys.exec_sql(sql); return '执行成功';
    }
    if (tool === 'commit_tx') {
      if (!TX_STATE.active) return '警告：当前无活跃事务。';
      sys.exec_sql('COMMIT'); TX_STATE.active = false; return '事务已提交';
    }
    if (tool === 'rollback_tx') {
      if (TX_STATE.active) { sys.exec_sql('ROLLBACK'); TX_STATE.active = false; }
      return '事务已回滚';
    }
 
    /* ---- ML 工具 ---- */
    if (tool === 'ml_rag')       return compress(ml_rag(String(args.question || user_message), args.top_k || 6), 800);
    if (tool === 'generate_text') return ml_generate(String(args.prompt || ''), args.options || {});
 
    return '错误：未知工具 "' + tool + '"。';
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
    if (qtype === 'system') return (
      '⚠ 系统元数据查询 — 必须使用正确表：\n' +
      '  行锁 → performance_schema.data_locks\n' +
      '  锁等待 → performance_schema.data_lock_waits\n' +
      '  活跃事务 → information_schema.INNODB_TRX\n' +
      '  死锁 → SHOW ENGINE INNODB STATUS\n' +
      '  ⛔ 禁止：INNODB_LOCKS / INNODB_LOCK_WAITS（MySQL 8.0 已删除）\n' +
      '  慢查询 → performance_schema.events_statements_summary_by_digest\n' +
      '  ⛔ 报 Unknown table → 立即换正确表，禁止重试同一表名。'
    );
    if (qtype === 'olap') return '⚠ OLAP：优先列存；GROUP BY 列需索引；大结果集主键偏移分页。';
    return 'OLTP：主键命中优先；UPDATE/DELETE 必须带 WHERE；写操作走 begin_tx 流程。';
  }
 
  function build_system_prompt(db, schema_ctx, join_hint, plan_hint,
                               few_shot, history, hw_history) {
    var qtype = classify_query(user_message);
    var hist_section = hw_history || history || '（无历史）';
 
    /* --- 内联 few-shot（固定，覆盖最高频失败场景） --- */
    var inline_few_shot =
      '【Few-Shot 强制示例（必须照此格式）】\n' +
      'Q: 有哪些表？\n' +
      'A: {"thought":"列出当前库所有表","tool":"query_db","args":{"sql":"SHOW TABLES"}}\n\n' +
      'Q: 每张表的结构是什么？\n' +
      'A: {"thought":"使用 plan_sql 先列表再逐表 SHOW CREATE","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"SHOW TABLES","desc":"枚举所有表"},\n' +
      '     {"sql":"SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION","desc":"所有列定义"}\n' +
      '   ]}}\n\n' +
      'Q: 统计每张表的行数\n' +
      'A: {"thought":"查 information_schema.TABLES","tool":"query_db","args":{"sql":"SELECT TABLE_NAME,TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE=\'BASE TABLE\'"}}\n\n' +
      'Q: 查看 orders 表最近 7 天的数据\n' +
      'A: {"thought":"按日期过滤","tool":"query_db","args":{"sql":"SELECT * FROM orders WHERE created_at >= NOW()-INTERVAL 7 DAY LIMIT 100"}}\n\n' +
      'Q: 分析订单金额按月汇总\n' +
      'A: {"thought":"多步：先看结构再聚合","tool":"plan_sql","args":{"steps":[\n' +
      '     {"sql":"DESC orders","desc":"确认列名"},\n' +
      '     {"sql":"SELECT DATE_FORMAT(created_at,\'%Y-%m\') AS month,SUM(amount) AS total FROM orders GROUP BY month ORDER BY month","desc":"按月汇总"}\n' +
      '   ]}}\n';
 
    return (
      '你是 ShannonBase Agent v4，内置于 ShannonBase 数据库的智能 SQL 助手。\n' +
      '当前数据库：' + db + '（SQL 中直接使用该库名，禁止任何占位符）\n\n' +
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
      '无需工具时直接输出自然语言。\n\n' +
 
      inline_few_shot + '\n\n' +
      (few_shot ? few_shot + '\n\n' : '') +
 
      '【历史对话】\n' + hist_section + '\n\n' +
      '【用户问题】\n' + user_message + '\n\n【助手】\n'
    );
  }
 
  function final_summary(system_prompt_base, tool_log) {
    return ml_generate(
      system_prompt_base + '\n\n【已执行工具及结果】\n' + compress(tool_log, 1500) +
      '\n\n请根据以上工具结果用清晰专业的中文直接回答用户问题。' +
      '禁止输出 JSON，禁止逐行复述原始数据，只输出结论和分析：\n',
      { temperature: 0.3, max_tokens: 1500 }
    );
  }
 
  var MAX_TURNS        = 10;
  var PROMPT_TOK_LIMIT = 2800;
  var MAX_ERRORS       = 3;
 
  /* Step 0: @chat_options */
  var chat_opt = get_chat_options();
 
  /* Step 1: 当前库 */
  var db_rows    = query("SELECT CAST(DATABASE() AS CHAR) AS db");
  var current_db = (db_rows && Array.isArray(db_rows) && db_rows.length && db_rows[0].db)
                   ? db_rows[0].db : '';
 
  var agent_response = '';
 
  /* ── ROUTE A: CATALOG ── */
  var cat = catalog_match(user_message);
  if (cat) {
    agent_response = '【' + cat.desc + '】\n' + rows_to_text(query(cat.sql));
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    save_memory(conversation_id, 'user', user_message, '', false);
    save_memory(conversation_id, 'assistant', agent_response, 'catalog:' + cat.sql, false);
    save_memory(conversation_id, 'user', user_message, '', true);
    save_memory(conversation_id, 'assistant', agent_response, 'catalog:' + cat.sql, true);
    return agent_response;
  }
 
  /* ── ROUTE B: HEATWAVE DISPATCH ── */
  var hw_triggered  = false;
  var vector_tables = [];
  if (is_knowledge_query(user_message)) {
    hw_triggered = true; vector_tables = discover_vector_tables(chat_opt);
  } else if (Array.isArray(chat_opt.tables) && chat_opt.tables.length > 0) {
    hw_triggered = true; vector_tables = chat_opt.tables;
  }
  if (hw_triggered) {
    var hw_result = heatwave_dispatch(user_message, chat_opt, vector_tables);
    agent_response = hw_result.response || '';
    if (hw_result.tables && hw_result.tables.length > 0) chat_opt.tables = hw_result.tables;
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    save_memory(conversation_id, 'user', user_message, '', false);
    save_memory(conversation_id, 'assistant', agent_response, 'hw_mode:' + hw_result.mode, false);
    save_memory(conversation_id, 'user', user_message, '', true);
    save_memory(conversation_id, 'assistant', agent_response, 'hw_mode:' + hw_result.mode, true);
    return agent_response;
  }
 
  /* ── ROUTE C: NEW v4 - Rule Planner（优先于 LLM Agent） ── */
  var rule_steps = rule_planner(user_message, current_db);
  if (rule_steps !== null) {
    var plan_results = execute_plan(rule_steps, current_db);
 
    /* 构造可读汇总供 LLM 总结 */
    var plan_log = '';
    for (var pri = 0; pri < plan_results.length; pri++) {
      plan_log += '\n--- Step ' + plan_results[pri].step + ': ' +
                  plan_results[pri].desc + ' ---\n' +
                  plan_results[pri].result + '\n';
    }
 
    /* 用 LLM 对 rule plan 结果做自然语言总结 */
    var rule_summary_prompt =
      '你是 ShannonBase 数据库助手。\n' +
      '当前库：' + current_db + '\n\n' +
      '用户问题：' + user_message + '\n\n' +
      '已执行查询结果：\n' + compress(plan_log, 2000) + '\n\n' +
      '请用清晰的中文直接回答用户问题，不要输出 JSON，不要逐行复述原始数据：\n';
    agent_response = ml_generate(rule_summary_prompt, { temperature: 0.3, max_tokens: 1200 });
 
    if (!agent_response || agent_response.trim().length < 5)
      agent_response = compress(plan_log, 3000);
 
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    save_memory(conversation_id, 'user', user_message, '', false);
    save_memory(conversation_id, 'assistant', agent_response, plan_log, false);
    save_memory(conversation_id, 'user', user_message, '', true);
    save_memory(conversation_id, 'assistant', agent_response, plan_log, true);
    return agent_response;
  }
 
  /* ── ROUTE D: LLM Agent Loop（v3.2 升级版，含 Tool 校验） ── */
 
  var history      = get_history(conversation_id, 8);
  var few_shot     = retrieve_few_shot(user_message, 3);
  var hw_hist_text = chat_history_to_text(chat_opt);
 
  var schema_embeddings_ready = check_schema_embeddings_ready(current_db);
  var logical_tasks           = decompose_query(user_message);
 
  var fixed_cost       = est_tok(hw_hist_text || history) + est_tok(few_shot) + 800;
  var available_tokens = Math.max(400, PROMPT_TOK_LIMIT - fixed_cost);
 
  var schema_ctx = build_schema_context(current_db, available_tokens, chat_opt);
 
  var join_hint = '';
  if (!schema_embeddings_ready &&
      logical_tasks.some(function(t){ return t.op === 'join'; })) {
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
 
  for (var turn = 0; turn < MAX_TURNS; turn++) {
    var llm_out  = ml_generate(full_prompt, { max_tokens: 600 }); /* v4: 略增 max_tokens */
    var tool_obj = parse_tool_call(llm_out);
 
    if (!tool_obj) {
      /* 模型直接输出自然语言 → 结束 */
      agent_response = llm_out.trim() || last_result;
      need_summary = false;
      break;
    }
 
    /* ▼▼▼ NEW v4: 工具校验（核心修复） ▼▼▼ */
    var validation_error = validate_tool_call(tool_obj);
    if (validation_error) {
      /* 把错误注入 prompt，让模型有机会自我纠正 */
      var err_hint = '\n[校验错误] ' + validation_error +
                     '\n请重新输出合法 JSON（args.sql 不能为空）：\n【助手】\n';
      if (++error_count >= MAX_ERRORS) {
        tool_log += '\n[终止] 工具校验失败次数过多：' + validation_error;
        need_summary = true;
        break;
      }
      if (prompt_tokens + est_tok(err_hint) <= PROMPT_TOK_LIMIT) {
        full_prompt += llm_out.trim() + err_hint;
        prompt_tokens += est_tok(err_hint);
      }
      continue;
    }
    /* ▲▲▲ END 工具校验 ▲▲▲ */
 
    var result  = execute_tool(tool_obj.tool, tool_obj.args || {}, current_db);
    last_result = result;
    need_summary = true;
 
    /* plan_sql 的结果已经是多步汇总，一般可以直接用作 final_summary 输入 */
    if (tool_obj.tool === 'plan_sql') {
      tool_log += '\n[Step ' + (turn+1) + '] tool=plan_sql' +
                  ' thought=' + (tool_obj.thought || '') +
                  '\nresult=' + compress(result, 1200) + '\n';
      need_summary = true;
      break; /* plan_sql 通常是终结性操作，直接进 summary */
    }
 
    if (result.indexOf('执行出错') !== -1 || result.indexOf('Unknown table') !== -1) {
      if (++error_count >= MAX_ERRORS) { tool_log += '\n[终止] 连续错误，强制摘要。'; break; }
    } else { error_count = 0; }
 
    tool_log += '\n[Step ' + (turn+1) + '] tool=' + tool_obj.tool +
                ' thought=' + (tool_obj.thought || '') +
                '\nresult=' + compress(result, 600) + '\n';
 
    if (tool_obj.tool === 'commit_tx' || tool_obj.tool === 'rollback_tx') { need_summary = true; break; }
    if (tool_obj.tool === 'generate_text') { agent_response = result; need_summary = false; break; }
 
    var append = '\n工具结果：' + compress(result, 600) + '\n【助手】\n';
    if (prompt_tokens + est_tok(append) > PROMPT_TOK_LIMIT) { need_summary = true; break; }
    full_prompt   += llm_out.trim() + append;
    prompt_tokens += est_tok(append);
  }
 
  /* 安全网：未提交事务强制回滚 */
  if (TX_STATE.active) {
    try { sys.exec_sql('ROLLBACK'); } catch(e) {}
    TX_STATE.active = false;
    tool_log += '\n[安全网] 未提交事务已强制回滚。';
    if (need_summary) last_result += '\n（事务已被强制回滚）';
  }
 
  if (need_summary && tool_log.length > 0) {
    var summary = final_summary(system_prompt_base, tool_log);
    agent_response = (summary && summary.trim().length > 0) ? summary.trim() : last_result;
  }
 
  if (!agent_response || agent_response.length === 0)
    agent_response = last_result.length > 0 ? last_result : '抱歉，未能生成有效回答，请重试。';
 
  chat_opt = update_chat_history(chat_opt, user_message, agent_response);
  chat_opt.response = agent_response; chat_opt.request_completed = true;
  save_chat_options(chat_opt);
 
  save_memory(conversation_id, 'user',      user_message,   '', false);
  save_memory(conversation_id, 'assistant', agent_response, tool_log, false);
  save_memory(conversation_id, 'user',      user_message,   '', true);
  save_memory(conversation_id, 'assistant', agent_response, tool_log, true);
 
  return agent_response;
}
 
return shannon_agent_run(user_message, conversation_id);
$$;

DELIMITER ;

-- shannon_chat: main entry function for agent.

DROP FUNCTION IF EXISTS sys.shannon_chat;

CREATE DEFINER='mysql.sys'@'localhost' FUNCTION sys.shannon_chat(
    user_message     TEXT,
    conversation_id  VARCHAR(64)
) RETURNS TEXT SQL SECURITY INVOKER LANGUAGE JAVASCRIPT 
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
    if (!rows || rows.error) return { ok: false, error: String(rows && rows.error || 'call failed') };
    return { ok: true, result: scalar(rows, 'result') || '' };
  }
 
  /* ── check function existence(information_schema.ROUTINES)── */
  function func_exists(schema, func) {
    var rows = query(
      "SELECT 1 AS found FROM information_schema.ROUTINES" +
      " WHERE ROUTINE_TYPE='FUNCTION'" +
      "   AND ROUTINE_SCHEMA='"  + esc(schema) + "'" +
      "   AND ROUTINE_NAME='"    + esc(func)   + "'" +
      " LIMIT 1"
    );
    return Array.isArray(rows) && rows.length > 0;
  }
 
  function current_db() {
    var rows = query("SELECT CAST(DATABASE() AS CHAR) AS db");
    return scalar(rows, 'db') || '';
  }
 
  /*
   * L1: session var  @shannon_agent_plugin
   *     format: 'schema_name.function_name'
   *     example: SET @shannon_agent_plugin = 'mydb.my_agent';
   */
  var l1_rows = query("SELECT @shannon_agent_plugin AS plugin");
  var l1_val  = scalar(l1_rows, 'plugin');
 
  if (l1_val && l1_val.indexOf('.') !== -1) {
    var l1_parts = l1_val.split('.');
    var l1_schema = l1_parts[0].trim();
    var l1_func   = l1_parts.slice(1).join('.').trim();
 
    if (func_exists(l1_schema, l1_func)) {
      var l1_res = call_plugin(l1_schema, l1_func, user_message, conversation_id);
      if (l1_res.ok) return '[L1:session] ' + l1_res.result;
      /* fall through */
    }
  }
 
  /* 
   * L2: registered plugin:  mysql.shannon_agent_plugins
   *     get: enabled=1 and priority minimum record.
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
      if (!func_exists(l2_schema, l2_func)) continue; /* unregistered → skip */
      var l2_res = call_plugin(l2_schema, l2_func, user_message, conversation_id);
      if (l2_res.ok) return '[L2:registry] ' + l2_res.result;
    }
  }
 
  /* 
   * L3: pattern  {current_db}.shannon_agent()
   */
  var db = current_db();
  if (db && func_exists(db, 'shannon_agent')) {
    var l3_res = call_plugin(db, 'shannon_agent', user_message, conversation_id);
    if (l3_res.ok) return '[L3:convention] ' + l3_res.result;
  }
 
  /*
   * L4: system default  sys.shannon_agent_default()
   */
  var l4_rows = query(
    "SELECT sys.shannon_agent_default('" +
    esc(user_message) + "','" + esc(conversation_id) + "') AS result"
  );
  return scalar(l4_rows, 'result') || 'system default engine failed, pls check sys.shannon_agent_default.';
}
 
return dispatcher(user_message, conversation_id);
$$;

DELIMITER ;
