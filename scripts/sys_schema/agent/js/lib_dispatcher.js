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