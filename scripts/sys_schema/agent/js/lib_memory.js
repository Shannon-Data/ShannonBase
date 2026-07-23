//@include lib_ml.js

function save_chat_options(chat_opt) {
  A.cached_chat_opt = chat_opt;
  try {
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

function recover_chat_history_from_memory(conv_id, max_turns) {
  max_turns = max_turns || 3;
  try {
    var rows = query(
      "SELECT role, content FROM (" +
      "  SELECT role, content, created_at FROM mysql.agent_memory" +
      "  WHERE conversation_id='" + esc(conv_id) + "'" +
      "  ORDER BY created_at DESC LIMIT " + (max_turns * 2) +
      ") t ORDER BY t.created_at ASC"
    );
    if (!Array.isArray(rows) || !rows.length) return [];

    var history = [];
    var i = 0;
    while (i < rows.length && history.length < max_turns) {
      if (rows[i].role !== 'user') { i++; continue; }

      var user_msg = rows[i].content || '';
      var bot_msg  = '';
      if (i + 1 < rows.length && rows[i + 1].role === 'assistant') {
        bot_msg = rows[i + 1].content || '';
        i += 2;
      } else {
        i += 1;
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
  var intent = analyze_intent(user_msg);
  var user_meta = JSON.stringify({
    intent: intent.kind,
    need_join: intent.need_join,
    need_time_filter: intent.need_time_filter,
    need_agg: intent.need_agg
  });
  try { save_memory(conv_id, 'user', user_msg, user_meta, true); } catch (e) {}
  try { save_memory(conv_id, 'assistant', bot_msg, thought, true); } catch (e) {}
}

/**
 * Persist one executed SQL step to mysql.agent_sql_trace, and — for
 * successful, read-only query_db steps only — also embed `desc` (the
 * model's stated reason/intent for that query, the closest thing we have
 * to "what question was this SQL answering") into a new `embedding` column
 * on this same row.
 *
 * This table (not mysql.agent_memory) is now the single source of truth
 * for retrieve_few_shot()'s vector search: agent_memory.thought never
 * actually contained the raw SQL text (see tool_log's `thought=<free text>`
 * format in shannon_agent_run.js), so a prior version of retrieve_few_shot
 * regex-matched against a field that could never contain what it was
 * looking for and silently always returned ''. Keeping (desc, sql) + their
 * embedding together on one row, in the one table that already stores the
 * real sql_text, removes that cross-table/format mismatch entirely.
 *
 * Embedding is intentionally gated to `tool === 'query_db' && !is_write`
 * with a non-empty desc and no visible error in `result`:
 *   - write/DDL/ML/tx-control steps aren't useful text-to-SQL examples and
 *     would just add embedding cost with no few-shot value;
 *   - a failed query is actively harmful as a "learned" example, since it
 *     would teach the model to reproduce SQL that didn't work.
 * This keeps embedding calls bounded to roughly the same steps the old
 * `thought LIKE '%query_db%'` filter was trying (and failing) to select.
 */
function log_sql_trace(conv_id, turn_no, step_no, route, tool, sql, desc, result) {
  try {
    var stmt = classify_statement(sql);
    var is_write = stmt.is_write ? 1 : 0;

    var result_str = String(result || '');
    var has_error =
      result_str.indexOf('执行出错')     !== -1 ||
      result_str.indexOf('Error: ')      !== -1 ||
      result_str.indexOf('Unknown table') !== -1;

    var should_embed =
      tool === 'query_db' && !is_write && !has_error &&
      String(desc || '').trim().length > 0;

    var embed_col_sql = 'NULL';
    if (should_embed) {
      var safe_desc = String(desc).trim().substring(0, 1800);
      embed_col_sql =
        "sys.ML_EMBED_ROW('" + esc(safe_desc) + "'," +
        "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
    }

    sys.exec_sql(
      "INSERT INTO mysql.agent_sql_trace " +
      "(conversation_id, turn_no, step_no, route, tool, sql_text, desc_text, result_preview, is_write, embedding) " +
      "VALUES ('" + esc(conv_id) + "'," + Number(turn_no) + "," + Number(step_no) + "," +
      "'" + esc(route) + "','" + esc(tool || '') + "'," +
      "'" + esc(String(sql || '')) + "','" + esc(String(desc || '')) + "'," +
      "'" + esc(result_str.substring(0, 1200)) + "'," + is_write + "," +
      embed_col_sql + ")"
    );
  } catch (e) {}
}

/**
 * Vector-retrieve a small number of past (intent -> working SQL) examples
 * to use as few-shot grounding for the current question.
 *
 * Queries mysql.agent_sql_trace directly (see log_sql_trace) rather than
 * mysql.agent_memory: sql_text/desc_text/embedding all live on the same row
 * there, for exactly the query_db steps that succeeded, so there is no
 * text-format assumption to keep in sync with how shannon_agent_run.js
 * happens to build tool_log/thought this month.
 */
function retrieve_few_shot(question, topK) {
  topK = topK || 3;
  try {
    var embed_expr =
      "sys.ML_EMBED_ROW('" + esc(question) + "'," +
      "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
    var sim_rows = query(
      "SELECT desc_text, sql_text FROM mysql.agent_sql_trace" +
      " WHERE tool='query_db' AND is_write=0" +
      "   AND conversation_id != '" + esc(A.conversation_id) + "'" +
      "   AND embedding IS NOT NULL" +
      " ORDER BY DISTANCE(embedding, " + embed_expr + ", 'cosine') ASC LIMIT " + topK
    );
    if (!Array.isArray(sim_rows) || !sim_rows.length) return '';
    var lines = [t('【Few-Shot 参考（向量检索）】', '[Few-Shot References (vector search)]')];
    for (var i = 0; i < sim_rows.length; i++) {
      var desc = String(sim_rows[i].desc_text || '').trim();
      var sql  = String(sim_rows[i].sql_text  || '').trim();
      if (!sql) continue; // defensive: should never happen given the WHERE/embedding gate, but never emit a bare Q with no SQL
      lines.push('Q' + (i+1) + ': ' + (desc || t('（无描述）', '(no description)')).substring(0, 80));
      lines.push('SQL: ' + sql.substring(0, 150));
    }
    return lines.length > 1 ? lines.join('\n') : '';
  } catch(e) { return ''; }
}