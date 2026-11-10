//@include lib_metrics.js
// Runtime admission, deadlines and durable keyed retries.
function runtime_sql_timeout() {
  if (A.runtime_cleanup) return 2000;
  if (A.cancelled) return 0;
  var left = Number(A.turn_deadline_at || 0) - Date.now();
  if (A.tool_deadline_at) left = Math.min(left, A.tool_deadline_at - Date.now());
  if (A.agent_tx_started_at)
    left = Math.min(left, A.agent_tx_started_at + Number(A.agent_tx_limit_ms || 5000) - Date.now());
  return Math.max(0, Math.floor(left));
}

function tx_max_open_ms() {
  return _combine_min(get_operator_policy(), 'tx_max_open_ms', 5000, 1);
}

function recover_agent_entry() {
  var ctx = get_tx_context();
  if (!ctx.known || ctx.owner === TX_OWNER_UNKNOWN)
    return { ok: false, error: 'transaction_recovery_unverified' };
  if (ctx.active && ctx.owner === TX_OWNER_AGENT)
    return rollback_agent_transaction('entry_recovery');
  var marker = get_agent_tx_event_marker();
  if (ctx.active && marker) {
    if (!ctx.event_id) return { ok: false, error: 'transaction_identity_unknown' };
    if (Number(marker) === Number(ctx.event_id)) {
      var rows = query_checked('SELECT @_shannon_agent_tx_conversation_id AS id');
      var previous = rows && rows[0] && rows[0].id;
      if (!previous) return { ok: false, error: 'transaction_owner_unknown' };
      var current = A.conversation_id;
      try {
        A.conversation_id = String(previous);
        return rollback_agent_transaction('pooled_connection_recovery');
      } finally { A.conversation_id = current; }
    }
  }
  return { ok: true };
}

function runtime_maintenance() {
  if (get_tx_context().active) return;
  var tasks = [MEM.long.purge_expired, MEM.long.purge_logs, ARTIFACT.purge_expired,
               MEM.derive.drain];
  for (var i = 0; i < tasks.length && runtime_sql_timeout() > 0; i++) tasks[i]();
}

function execution_begin(message, opt) {
  var key = opt.idempotency_key;
  if (key !== undefined && (typeof key !== 'string' || !key.length || key.length > 128))
    return { ok: false, error: 'invalid_idempotency_key' };
  var state = sys.transaction_state();
  if (!state.known) return { ok: false, error: 'transaction_state_unknown' };
  if (state.multi_statement || !state.autocommit) {
    if (key) return { ok: false, error: 'idempotency_requires_autocommit' };
    return { ok: true };
  }
  var token = query_checked('SELECT UUID() AS id')[0].id;
  if (!key) key = token;
  var prefix = mem_principal_prefix();
  var ids = query_checked("SELECT SHA2(CONCAT('" + esc(prefix) + ":','" + esc(key) +
    "'),256) AS id,SHA2(CONCAT('" + esc(A.conversation_id) + "',CHAR(0),'" +
    esc(message) + "'),256) AS fingerprint")[0];
  query_checked("INSERT INTO mysql.agent_execution" +
    " (execution_id,principal_prefix,conversation_id,request_hash,run_token,status) VALUES ('" +
    esc(ids.id) + "','" + esc(prefix) + "','" + esc(A.conversation_id) + "','" +
    esc(ids.fingerprint) + "','" + esc(token) + "','RUNNING')" +
    " ON DUPLICATE KEY UPDATE execution_id=execution_id");
  var row = query_checked("SELECT request_hash,run_token,status,response FROM mysql.agent_execution" +
    " WHERE execution_id='" + esc(ids.id) + "' AND principal_prefix='" + esc(prefix) + "'")[0];
  if (!row || row.request_hash !== ids.fingerprint)
    return { ok: false, error: 'idempotency_key_conflict' };
  opt.execution_id = ids.id;
  if (row.run_token !== token) {
    if (row.status === 'SUCCEEDED')
      return { ok: true, replay: true, response: String(row.response || '') };
    return { ok: false, error: 'execution_outcome_uncertain', execution_id: ids.id };
  }
  A.execution = { id: ids.id, token: token };
  return { ok: true };
}

function execution_finish(response, failed) {
  if (!A.execution) return response;
  var state = sys.transaction_state();
  var status = (!state.known || state.multi_statement || (failed && A.execution_effect))
    ? 'RECOVERING' : (failed ? 'FAILED' : 'SUCCEEDED');
  if (state.multi_statement || !state.known)
    return String(response || '') + '\n[execution_outcome_uncertain] ' + A.execution.id;
  var r = query_checked("UPDATE mysql.agent_execution SET status='" + status +
    "',response='" + esc(String(response || '')) + "' WHERE execution_id='" +
    esc(A.execution.id) + "' AND run_token='" + esc(A.execution.token) + "' AND status='RUNNING'");
  if (!r || Number(r.affected_rows) !== 1)
    throw new Error('execution_completion_unverified: ' + A.execution.id);
  return response;
}

function shannon_agent_run(user_message, conversation_id) {
  A.runtime_active = true;
  A.runtime_cleanup = false;
  A.cancelled = false;
  A.turn_started_ms = Date.now();
  A.turn_deadline_at = A.turn_started_ms + TURN_DEADLINE_MS_DEFAULT;
  A.agent_tx_started_at = 0;
  A.agent_tx_limit_ms = 5000;
  A.tool_deadline_at = 0;
  A.execution = null;
  A.execution_effect = false;
  A.cost = null;
  A.tool_calls = 0;
  A.result_citations = [];
  A.rag_discovery = null;
  A.rag_context = null;
  A.truncated_evidence = false;
  try {
    var response = shannon_agent_run_body(user_message, conversation_id);
    A.runtime_cleanup = true;
    return execution_finish(response, false);
  } catch (e) {
    A.runtime_cleanup = true;
    try { execution_finish(String(e), true); } catch (ignored) {}
    throw e;
  } finally {
    A.runtime_cleanup = true;
    runtime_record_metrics();
    A.runtime_active = false;
    A.runtime_cleanup = false;
  }
}

// Raw per-turn samples retain the distribution; daily sums cannot yield p95.
function runtime_record_metrics() {
  var cost = A.cost || {};
  mem_log_audit('runtime', 'turn', 'agent', JSON.stringify({
    trace_id: current_turn_id(), route: A.route || 'tool_loop',
    stop_reason: A.stop_reason || '', prompt_tokens: Number(cost.prompt_tokens || 0),
    completion_tokens: Number(cost.completion_tokens || 0), llm_ms: Number(cost.llm_ms || 0),
    llm_calls: Number(cost.llm_calls || 0), tool_calls: Number(A.tool_calls || 0)
  }), 1, Math.max(0, Date.now() - Number(A.turn_started_ms || Date.now())), '');
}
