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
    exec_sql("SET @chat_options = '" + esc(JSON.stringify(safe)) + "'");
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

  var max_len = Number(get_memory_options(chat_opt).short_term.recent_turns);
  if (!isFinite(max_len) || max_len < 0) max_len = 3;

  if (chat_opt.chat_history.length > max_len)
    chat_opt.chat_history =
      chat_opt.chat_history.slice(chat_opt.chat_history.length - max_len);
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

// Legacy accessor, kept because plugins and older call sites use it.
function get_history(conv_id, n) {
  var mo = get_memory_options(get_chat_options());
  if (n) {
    mo = mem_merge_defaults(mo, { short_term: { recent_turns: Math.ceil(Number(n) / 2) } });
  }
  var loaded = mem_short_load(conv_id, mo);
  return mem_turns_to_text(loaded.turns);
}

// Rebuild @chat_options.chat_history from the server-side store.
function recover_chat_history_from_memory(conv_id, max_turns) {
  max_turns = max_turns || 3;
  try {

    var rows = query(
      "SELECT role, content, turn_no, seq FROM mysql.agent_memory" +
      " WHERE conversation_id='" + esc(conv_id) + "'" +
      "   AND turn_no IN (SELECT t FROM (" +
      "         SELECT DISTINCT turn_no AS t FROM mysql.agent_memory" +
      "          WHERE conversation_id='" + esc(conv_id) + "'" +
      "          ORDER BY turn_no DESC LIMIT " + Number(max_turns) +
      "       ) w)" +
      " ORDER BY seq ASC"
    );
    if (!Array.isArray(rows) || !rows.length) return [];

    // Grouped by turn_no, but a group only ever holds one row per role.
    var by_turn = {}, order = [], groups = {};
    for (var i = 0; i < rows.length; i++) {
      var tn  = String(rows[i].turn_no || 0);
      var key = groups[tn];
      var role_field = (rows[i].role === 'user') ? 'user_message' : 'chat_bot_message';
      if (key === undefined || by_turn[key][role_field]) {
        key = tn + '#' + order.length;
        groups[tn] = key;
        by_turn[key] = { user_message: '', chat_bot_message: '',
                         chat_query_id: gen_query_id() };
        order.push(key);
      }
      by_turn[key][role_field] = rows[i].content || '';
    }

    var history = [];
    for (var o = 0; o < order.length && history.length < max_turns; o++) {
      var entry = by_turn[order[o]];
      if (entry.user_message) history.push(entry);
    }
    return history;
  } catch(e) { return []; }
}

// Persist one turn.
// One id per invocation, minted on first use.
function current_turn_id() {
  if (A.execution && A.execution.id) return A.execution.id;
  if (!A.turn_id) A.turn_id = gen_query_id() + gen_query_id();
  return A.turn_id;
}

// What did this turn cost?
function log_turn_cost(route) {
  var c = A.cost;
  if (!c || !c.llm_calls) return;
  mem_log_audit('L1', 'cost', 'ml_generate',
                'turn=' + current_turn_id() + ' route=' + String(route || '') +
                ' prompt_tokens=' + c.prompt_tokens +
                ' completion_tokens=' + c.completion_tokens +
                ' mem_block_tokens=' + Number(A.mem_block_tokens || 0),
                c.llm_calls, c.llm_ms, '');
}

function persist_turn(conv_id, user_msg, bot_msg, thought, route) {
  if (!route) {
    var th = String(thought || '');
    if      (th.indexOf('review:')  === 0) route = 'review';
    else if (th.indexOf('catalog:') === 0) route = 'catalog';
    else if (th.indexOf('hw_mode:') === 0) route = 'rag';
    else                                   route = 'agent_loop';
  }
  MEM.short.append_turn(conv_id, user_msg, bot_msg, thought, { route: route });
  log_turn_cost(route);

  if (A.memory_degraded) {
    try {
      exec_sql(
        "SET @chat_options = JSON_SET(COALESCE(@chat_options, JSON_OBJECT())," +
        " '$.memory_degraded', TRUE," +
        " '$.memory_degraded_reason', '" + esc(A.memory_degraded_reason || '') + "')"
      );
    } catch (e) {}
  }
}

function log_sql_trace(conv_id, turn_no, step_no, route, tool, sql, desc, result, execution_ok) {
  try {
    var stmt = classify_statement(sql);
    var is_write = stmt.is_write ? 1 : 0;

    var result_str = String(result || '');
    var has_error =
      result_str.indexOf('执行出错')     !== -1 ||
      result_str.indexOf('Error: ')      !== -1 ||
      result_str.indexOf('Unknown table') !== -1;

    /* The vector is derived by the queue, not here: an ONNX call per step put
     * the embedding on the user's critical path for a retrieval that only reads
     * *other* conversations. */
    var should_embed =
      tool === 'query_db' && !is_write && execution_ok === true && !has_error &&
      String(desc || '').trim().length > 0;

    // turn_id, not just turn_no.
    var inserted = exec_sql(
      "INSERT INTO mysql.agent_sql_trace " +
      "(conversation_id, turn_no, step_no, route, tool, sql_text, desc_text, " +
      " result_preview, is_write, embedding, embed_eligible, turn_id) " +
      "VALUES ('" + esc(conv_id) + "'," + Number(turn_no) + "," + Number(step_no) + "," +
      "'" + esc(route) + "','" + esc(tool || '') + "'," +
      "'" + esc(String(sql || '')) + "','" + esc(String(desc || '')) + "'," +
      "'" + esc(result_str.substring(0, 1200)) + "'," + is_write + ",NULL," + (should_embed ? 1 : 0) + ",'" +
      esc(current_turn_id()) + "')"
    );
    /* A rejected INSERT leaves LAST_INSERT_ID() pointing at some earlier row,
       which would enqueue the wrong one to be embedded. */
    if (!inserted || inserted.error) return;

    var mo = get_memory_options(get_chat_options());
    if (should_embed && mo.enabled !== false && mo.derive && mo.derive.enabled !== false) {
      var id_rows = query("SELECT LAST_INSERT_ID() AS id");
      var trace_id = (Array.isArray(id_rows) && id_rows.length) ? id_rows[0].id : null;
      var prefix   = mem_principal_prefix();
      if (trace_id && prefix) {
        exec_sql(
          "INSERT IGNORE INTO mysql.agent_derive_queue" +
          " (principal_prefix, kind, target_table, target_id, payload)" +
          " VALUES ('" + esc(prefix) + "','embed_trace','agent_sql_trace'," +
          Number(trace_id) + ", JSON_OBJECT('source_column','desc_text'," +
          "'target_column','embedding'))"
        );
      }
    }
  } catch (e) {}
}

var MEM_EMBED_VAR = '@shannon_mem_qvec';

function mem_embed_question_var(question, varname) {
  var v    = varname || MEM_EMBED_VAR;
  var text = String(question == null ? '' : question).substring(0, 1800);

  var set_res = query("SET " + v + " := " + mem_embed_expr(text));
  if (set_res && !Array.isArray(set_res) && set_res.error) return null;

  var chk = query("SELECT " + v + " IS NULL AS embed_is_null");
  if (!Array.isArray(chk) || !chk.length || Number(chk[0].embed_is_null) === 1) return null;

  return v;
}

// L2b procedural recall.
function retrieve_few_shot(question, topK) {
  topK = topK || 3;
  try {
    var principal_prefix = scoped_principal_prefix(A.conversation_id);
    if (!principal_prefix) return '';
    /* Embed once, not once per trace row -- see mem_embed_question_var(). */
    var embed_expr = mem_embed_question_var(question);
    if (!embed_expr) return '';
    var lambda = mem_ranking_options(get_memory_options(get_chat_options())).lambda;
    var fetch_k = mem_candidate_limit(topK,lambda);
    var sim_rows = query(
      "SELECT desc_text, sql_text FROM mysql.agent_sql_trace" +
      " WHERE tool='query_db' AND is_write=0" +
      "   AND conversation_id LIKE '" + esc(principal_prefix) + ":%'" +
      "   AND conversation_id != '" + esc(A.conversation_id) + "'" +
      "   AND embedding IS NOT NULL AND embed_eligible=1" +
      " ORDER BY DISTANCE(embedding, " + embed_expr + ", 'cosine') ASC LIMIT " + fetch_k
    );
    if (!Array.isArray(sim_rows) || !sim_rows.length) return '';
    for (var ri = 0; ri < sim_rows.length; ri++) sim_rows[ri].rank_rel = 1 / (1 + ri);
    sim_rows = mem_select_candidates(sim_rows,topK,lambda,
      function(r) { return String(r.desc_text || '') + ' ' + String(r.sql_text || ''); },
      function(r) { return r.rank_rel; });
    var lines = [t('【Few-Shot 参考（向量检索）】', '[Few-Shot References (vector search)]')];
    for (var i = 0; i < sim_rows.length; i++) {
      var desc = String(sim_rows[i].desc_text || '').trim();
      var sql  = String(sim_rows[i].sql_text  || '').trim();
      if (!sql) continue;
      lines.push('Q' + (i+1) + ': ' + (desc || t('（无描述）', '(no description)')).substring(0, 80));
      lines.push('SQL: ' + sql.substring(0, 150));
    }
    return lines.length > 1 ? lines.join('\n') : '';
  } catch(e) { return ''; }
}