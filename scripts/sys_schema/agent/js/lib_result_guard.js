// Check only identities present in the same result row. No second read races
// with concurrent writes, and no SQL expression is evaluated in JavaScript.
function aggregate_result_check(sql, rows) {
  var result = { checked: 0, ok: true, errors: [] };
  if (!Array.isArray(rows) || !/^\s*SELECT\b/i.test(sql)) return result;
  // Restrict recognition to simple, explicitly aliased aggregate projections.
  // Complex expressions, DISTINCT, windows and scalar subqueries stay unchecked.
  var projection = String(sql).match(/^\s*SELECT\s+([\s\S]*?)\s+FROM\b/i);
  if (!projection || /\b(?:SELECT|OVER|DISTINCT)\b/i.test(projection[1])) return result;
  var aggs = [], re = /(?:^|,)\s*(COUNT|SUM|AVG)\s*\(\s*(\*|(?:`[^`]+`|[A-Za-z_][A-Za-z0-9_]*)(?:\.(?:`[^`]+`|[A-Za-z_][A-Za-z0-9_]*))?)\s*\)\s+AS\s+(`[^`]+`|[A-Za-z_][A-Za-z0-9_]*)\s*(?=,|$)/gi;
  var m;
  while ((m = re.exec(projection[1])) !== null)
    aggs.push({ fn: m[1].toUpperCase(), expr: m[2].replace(/`/g, '').toLowerCase(), key: m[3].replace(/`/g, '') });
  function error(code) { result.ok = false; if (result.errors.indexOf(code) < 0) result.errors.push(code); }
  function number(v) {
    if (v === null || v === undefined || v === '') return null;
    var n = Number(v);
    return isFinite(n) && Math.abs(n) <= 9007199254740991 ? n : null;
  }
  for (var r = 0; r < rows.length; r++) {
    var all = null, by_expr = {};
    for (var a = 0; a < aggs.length; a++) {
      var agg = aggs[a], val = rows[r][agg.key], n = number(val);
      if (agg.fn === 'COUNT' && n !== null) {
        result.checked++;
        if (n < 0 || Math.floor(n) !== n) error('invalid_count');
        if (agg.expr === '*') all = n;
      }
      if (!by_expr[agg.expr]) by_expr[agg.expr] = {};
      by_expr[agg.expr][agg.fn] = { value: val, number: n };
    }
    for (var expr in by_expr) {
      if (!Object.prototype.hasOwnProperty.call(by_expr, expr)) continue;
      var set = by_expr[expr], count = set.COUNT && set.COUNT.number;
      if (count !== undefined && count !== null && all !== null && expr !== '*') {
        result.checked++; if (count > all) error('nonnull_count_exceeds_rows');
      }
      if (count === 0 && (set.SUM || set.AVG)) {
        result.checked++;
        if ((set.SUM && set.SUM.value !== null) || (set.AVG && set.AVG.value !== null)) error('empty_aggregate_not_null');
      }
      if (count > 0 && set.SUM && set.AVG && set.SUM.number !== null && set.AVG.number !== null) {
        // MySQL rounds AVG(DECIMAL) to a bounded scale. This tolerance is a
        // sanity check, not a replacement for exact decimal arithmetic.
        result.checked++;
        var expected = set.SUM.number / count;
        if (Math.abs(expected - set.AVG.number) > Math.max(0.0001, Math.abs(expected) * 0.000001))
          error('sum_count_avg_mismatch');
      }
    }
  }
  return result;
}

function read_result_reference(rows, sql) {
  var limit = read_sql_limit_info(sql_lex_info(sql));
  return { source_sql: sql, row_count: Array.isArray(rows) ? rows.length : 0,
    truncated: rows_truncated(rows),
    // A full LIMIT page proves only a bounded result, not whole-table coverage.
    limit_reached: !!(limit.has_limit && limit.value !== null && rows.length >= limit.value),
    consistency: aggregate_result_check(sql, rows) };
}

// One envelope for success, rejection, hook replacements and handler failures.
function tool_result_envelope(result) {
  if (!result || typeof result !== 'object')
    result = { ok: false, error: 'invalid_tool_result', response: 'Tool returned no structured result.' };
  result.trace_id = current_turn_id(); result.span_id = A.current_span_id || '';
  result.parent_span_id = A.parent_span_id || '';
  result.content = [{ type: 'text', text: String(result.response || '') }];
  result.citations = Array.isArray(result.citations) ? result.citations : [];
  if (result.result) {
    var ref = result.result;
    if (ref.truncated || ref.preview_only || ref.limit_reached) A.truncated_evidence = true;
    if (ref.artifact_id) result.artifact_id = ref.artifact_id;
    result.content.push({ type: 'json', data: ref });
    if (!result.citations.length && (ref.source_sql || ref.artifact_id)) result.citations.push(ref);
  }
  A.result_citations = A.result_citations || [];
  for (var i = 0; i < result.citations.length && A.result_citations.length < 8; i++) {
    var citation = result.citations[i];
    A.result_citations.push({ artifact_id: citation.artifact_id || null,
      content_hash: citation.content_hash || null, row_count: citation.row_count,
      truncated: !!citation.truncated, limit_reached: !!citation.limit_reached,
      preview_only: !!citation.preview_only, source_sql: citation.source_sql || result.sql || '',
      trace_id: result.trace_id, span_id: result.span_id });
  }
  return result;
}
