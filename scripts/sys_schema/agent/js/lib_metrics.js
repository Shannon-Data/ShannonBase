//@include lib_lang.js
// Metrics need SQL and principal helpers, not the agent dispatcher.
function runtime_metrics_sql(days) {
  var n = Math.max(1, Math.min(90, Math.floor(Number(days) || 7)));
  var prefix = mem_principal_prefix();
  if (!prefix) throw new Error('missing_isolation_key');
  return "WITH samples AS (SELECT DATE(created_at) AS day,elapsed_ms," +
    " CAST(JSON_UNQUOTE(JSON_EXTRACT(detail,'$.prompt_tokens')) AS UNSIGNED) AS prompt_tokens," +
    " CAST(JSON_UNQUOTE(JSON_EXTRACT(detail,'$.completion_tokens')) AS UNSIGNED) AS completion_tokens" +
    " FROM mysql.agent_memory_audit WHERE principal_prefix='" + esc(prefix) + "'" +
    " AND tier='runtime' AND op='turn' AND JSON_VALID(detail)" +
    " AND created_at >= NOW() - INTERVAL " + n + " DAY)," +
    " ranked AS (SELECT *,ROW_NUMBER() OVER(PARTITION BY day ORDER BY elapsed_ms) AS rn," +
    " COUNT(*) OVER(PARTITION BY day) AS n FROM samples)" +
    " SELECT day,COUNT(*) AS turns,SUM(prompt_tokens) AS prompt_tokens," +
    " SUM(completion_tokens) AS completion_tokens," +
    " MIN(CASE WHEN rn>=CEIL(n*0.50) THEN elapsed_ms END) AS p50_ms," +
    " MIN(CASE WHEN rn>=CEIL(n*0.95) THEN elapsed_ms END) AS p95_ms" +
    " FROM ranked GROUP BY day ORDER BY day";
}
