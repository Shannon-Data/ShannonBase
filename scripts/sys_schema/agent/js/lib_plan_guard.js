// Optimizer estimates guard execution; they are not actual row counts.
function read_plan_guard(sql, stmt) {
  if (stmt.first_keyword !== 'SELECT' && stmt.first_keyword !== 'WITH') return null;
  if (sql_lex_info(sql).top_tokens.indexOf('FROM') < 0 && stmt.first_keyword !== 'WITH') return null;
  var base = get_operator_policy();
  var row_cap = _combine_min(base, 'read_plan_rows_max', 1000000, 1);
  var cost_cap = _combine_min(base, 'read_plan_cost_max', 1000000, 1);
  try {
    var rows = query_checked('EXPLAIN FORMAT=JSON ' + sql);
    if (!rows || !rows.length) throw new Error('empty EXPLAIN');
    var plan = JSON.parse(String(rows[0].EXPLAIN || rows[0].explain || ''));
    var info = { rows: 0, cost: 0, full_scan: false, temporary: false, filesort: false };
    function visit(node, depth) {
      if (!node || typeof node !== 'object' || depth > 32) return;
      info.rows = Math.max(info.rows, Number(node.rows_examined_per_scan || 0),
                           Number(node.rows_produced_per_join || 0));
      if (node.cost_info) info.cost = Math.max(info.cost, Number(node.cost_info.query_cost || 0));
      if (node.access_type === 'ALL') info.full_scan = true;
      if (node.using_temporary_table) info.temporary = true;
      if (node.using_filesort) info.filesort = true;
      for (var k in node) if (Object.prototype.hasOwnProperty.call(node, k)) visit(node[k], depth + 1);
    }
    visit(plan, 0);
    if (info.rows <= row_cap && info.cost <= cost_cap) return null;
    return { ok: false, error: 'read_plan_requires_rewrite', plan: info,
      response: 'Read plan exceeds the execution ceiling. Add selective indexed predicates or rewrite the query. ' + JSON.stringify(info) };
  } catch (e) {
    return { ok: false, error: 'read_plan_unverified', response: 'Cannot verify the read plan: ' + String(e) };
  }
}

function ddl_plan_guard(sql, db) {
  var lex = sql_lex_info(sql);
  if (lex.first_keyword !== 'ALTER' || lex.top_tokens[1] !== 'TABLE') return null;
  var tables = infer_affected_tables(sql);
  if (!tables.length) return { ok: false, error: 'ddl_table_unknown', response: 'Cannot identify the ALTER target.' };
  var parts = String(tables[0]).replace(/`/g, '').split('.');
  var schema = parts.length > 1 ? parts[0] : db;
  var table = parts.length > 1 ? parts[1] : parts[0];
  var rows;
  try {
    rows = query_checked("SELECT TABLE_ROWS,COALESCE(DATA_LENGTH,0)+COALESCE(INDEX_LENGTH,0) AS bytes" +
      " FROM information_schema.TABLES WHERE TABLE_SCHEMA='" + esc(schema) + "' AND TABLE_NAME='" + esc(table) + "'");
  } catch (e) { return { ok: false, error: 'ddl_metadata_unverified', response: String(e) }; }
  if (!rows || !rows.length) return { ok: false, error: 'ddl_metadata_unverified', response: 'ALTER target metadata is unavailable.' };
  var base = get_operator_policy();
  var large = Number(rows[0].bytes) > _combine_min(base, 'ddl_table_bytes_max', 64 * 1024 * 1024, 1) ||
              Number(rows[0].TABLE_ROWS) > _combine_min(base, 'ddl_table_rows_max', 100000, 1);
  var top = lex.top_tokens;
  var special = top.indexOf('SECONDARY_LOAD') >= 0 || top.indexOf('SECONDARY_UNLOAD') >= 0 ||
                top.indexOf('SECONDARY_ENGINE') >= 0;
  var algorithm = top.indexOf('ALGORITHM'), lock = top.indexOf('LOCK');
  var online = algorithm >= 0 && ['INSTANT','INPLACE'].indexOf(top[algorithm + 1]) >= 0 &&
               lock >= 0 && top[lock + 1] === 'NONE';
  if (!large || online || (special && A.approved_args)) return null;
  if (_pol_bool(base.allow_blocking_ddl) && A.approved_args) return null;
  return { ok: false, error: 'ddl_online_clauses_required', plan: rows[0],
    response: 'Large-table ALTER requires explicit ALGORITHM=INSTANT/INPLACE and LOCK=NONE. Unsupported clauses must fail instead of falling back to a blocking copy. ' + JSON.stringify(rows[0]) };
}
