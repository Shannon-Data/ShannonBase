//@include lib_lang.js
//@include shannon_agent_run.js

function resolve_conversation_id(explicit_conv_id) {
  if (explicit_conv_id) return explicit_conv_id;

  var opt = {};
  try {
    var rows = query("SELECT @chat_options AS opt");
    if (rows && Array.isArray(rows) && rows.length && rows[0].opt) {
      opt = JSON.parse(rows[0].opt);
    }
  } catch (e) {}

  if (opt.conversation_id === 'new') {
    return gen_fresh_conversation_id();
  }
  if (opt.conversation_id) {
    return String(opt.conversation_id);
  }

  try {
    var last = query("SELECT @_shannon_last_conv_id AS id");
    if (last && Array.isArray(last) && last.length && last[0].id)
      return String(last[0].id);
  } catch (e) {}

  return gen_fresh_conversation_id();
}

function gen_fresh_conversation_id() {
  try {
    var rows = query("SELECT UUID() AS id");
    if (Array.isArray(rows) && rows.length && rows[0].id) return String(rows[0].id);
  } catch (e) {}
  return gen_query_id();
}

function dispatcher(user_message, conversation_id) {

  function scalar(rows, col) {
    if (!Array.isArray(rows) || !rows.length) return null;
    var v = rows[0][col];
    return (v === undefined || v === null) ? null : String(v);
  }

  function call_plugin(schema, func, msg, conv_id) {
    var sql =
      "SELECT `" + esc_ident(schema) + "`.`" + esc_ident(func) + "`(" +
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

  A.request_intent = analyze_intent(user_message);

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
   * L4: the built-in agent (default fallback).
   *
   * Invoked in-process rather than through SELECT sys.shannon_agent_default(),
   * because sys.shannon_chat is a PROCEDURE and a procedure is not a
   * sub-statement.  The agent can therefore own a real transaction here --
   * begin_tx / update_data / commit_tx work, and finalize_tx_safety_net() can
   * actually roll one back.  Inside the stored FUNCTION none of that is
   * possible: THD::restore_sub_statement_state() restores option_bits
   * wholesale when a function returns, so a transaction opened there could
   * never survive (see the note in sql/sp_head.cc).
   *
   * sys.shannon_agent_default is still published, both for callers that
   * invoke it directly and as the signature plugins mirror; it simply is no
   * longer how this dispatcher reaches the built-in agent.
   */
  try {
    var l4_answer = shannon_agent_run(user_message, conversation_id);
    if (l4_answer !== undefined && l4_answer !== null && String(l4_answer).length > 0)
      return String(l4_answer);
  } catch (e) {
    return 'L4 internal error: ' + String(e);
  }
  return 'system default engine failed, please check sys.shannon_agent_default.';
}