//@include shannon_agent_run.js
// Real-session checks use only their labelled fixtures.
function shannon_runtime_selfcheck(op, label) {
  var out = [];
  if (op !== 'state' && op !== 'entry' && op !== 'timeout' && op !== 'mode' && !selfcheck_writes_allowed())
    return [['error', 'runtime writes require the self-check write opt-in']];
  function row(k, v) { out.push([String(k), String(v)]); }
  var prefix = mem_principal_prefix();
  A.conversation_id = principal_scope_conversation_id('runtime_' + String(label || 'test'));
  if (op === 'state') {
    var state = get_session_tx_state(), ctx = get_tx_context();
    row('known', state.known); row('active', state.active); row('owner', ctx.owner);
  } else if (op === 'retry') {
    var opt = { idempotency_key: String(label) };
    var first = execution_begin('original request', opt);
    var execution = A.execution;
    A.execution = null;
    var pending = execution_begin('original request', opt);
    var changed = execution_begin('changed request', opt);
    A.execution = execution;
    execution_finish('saved response', false);
    A.execution = null;
    var replay = execution_begin('original request', opt);
    row('admitted', first.ok && !!execution);
    row('pending_blocked', pending.error === 'execution_outcome_uncertain');
    row('changed_blocked', changed.error === 'idempotency_key_conflict');
    row('replayed', replay.replay && replay.response === 'saved response');
    query_checked("DELETE FROM mysql.agent_execution WHERE execution_id='" + esc(opt.execution_id) +
                  "' AND principal_prefix='" + esc(prefix) + "'");
  } else if (op === 'mode') {
    var saved_drain = MEM.derive.drain, saved_script = A.eval_script;
    var calls = 0;
    try {
      MEM.derive.drain = function () { calls++; };
      A.eval_script = { turns: [{ text: 'must not run' }], calls: 0, pos: 0 };
      var rejected = shannon_agent_run('mode probe', 'mode_probe');
      row('mode_rejected_before_work', rejected.indexOf('NO_BACKSLASH_ESCAPES') >= 0 &&
          calls === 0 && A.eval_script.calls === 0);
    } finally { MEM.derive.drain = saved_drain; A.eval_script = saved_script; }
  } else if (op === 'timeout') {
    var start = Date.now();
    sys.exec_sql('SELECT SLEEP(2)', 20, true);
    row('bounded', Date.now() - start < 1500);
  } else if (op === 'primary') {
    A.runtime_active = true; A.turn_deadline_at = Date.now() + 30000;
    var r = query_checked('SELECT COUNT(*) AS n FROM test.agent_runtime_ryw WHERE id=2');
    A.runtime_active = false;
    row('own_write_visible', Number(r[0].n) === 1);
    row('setting_preserved', query_checked('SELECT @@use_secondary_engine AS v')[0].v === 'FORCED');
  } else if (op === 'pool_open') {
    row('began', impl_begin_tx({}, {}).ok);
    query_checked('INSERT INTO test.agent_runtime_ryw VALUES(2)');
  } else if (op === 'pool_recover') {
    row('recovered', recover_agent_entry().ok);
    row('closed', !get_session_tx_state().active);
    row('old_write_rolled_back', Number(query_checked('SELECT COUNT(*) AS n FROM test.agent_runtime_ryw WHERE id=2')[0].n) === 0);
  } else if (op === 'tx_budget' || op === 'tx_model') {
    var began = impl_begin_tx({}, {});
    row('began', began.ok);
    if (!began.ok) return out;
    var saved_policy = A.operator_policy;
    try {
      A.runtime_active = true; A.turn_deadline_at = Date.now() + 30000;
      if (op === 'tx_budget') {
        query_checked('INSERT INTO test.agent_runtime_ryw VALUES(2)');
        A.operator_policy = { tx_max_open_ms: '1' };
        A.agent_tx_limit_ms = tx_max_open_ms();
        A.agent_tx_started_at = Date.now() - 10;
        row('expired_commit_blocked', execute_tool('commit_tx', {}, 'test').error === 'agent_deadline_exceeded');
      } else {
        A.eval_script = { turns: [{ text: 'must not run' }], calls: 0, pos: 0 };
        ml_generate_call('', 'probe', null);
        row('inference_blocked', A.last_llm_status.kind === 'transaction_open' && A.eval_script.calls === 0);
      }
      row('rolled_back', rollback_agent_transaction('selfcheck').ok);
    } finally {
      A.runtime_active = false; A.operator_policy = saved_policy; A.eval_script = null;
      rollback_agent_transaction('selfcheck_finally');
    }
    row('no_write_survived', Number(query_checked('SELECT COUNT(*) AS n FROM test.agent_runtime_ryw WHERE id=2')[0].n) === 0);
  } else if (op === 'entry') {
    var saved = { ctx: get_tx_context, rollback: rollback_agent_transaction,
                  drain: MEM.derive.drain, script: A.eval_script };
    try {
      get_tx_context = function () { return { known: true, active: true, owner: TX_OWNER_AGENT }; };
      rollback_agent_transaction = function () { return { ok: false, error: 'rollback_failed' }; };
      row('failed_recovery_blocked', !recover_agent_entry().ok);
      var drained = 0;
      MEM.derive.drain = function () { drained++; };
      A.eval_script = { turns: [{ text: 'must not run' }], calls: 0, pos: 0 };
      var refused = shannon_agent_run('recovery probe', 'recovery_probe');
      row('no_new_work', refused.indexOf('[transaction_recovery_unverified]') === 0 &&
          drained === 0 && A.eval_script.calls === 0);
      get_tx_context = function () { return { known: false, active: true, owner: TX_OWNER_UNKNOWN }; };
      row('unknown_owner_blocked', !recover_agent_entry().ok);
      get_tx_context = function () { return { known: true, active: true, owner: TX_OWNER_CALLER }; };
      row('caller_preserved', recover_agent_entry().ok);
    } finally {
      get_tx_context = saved.ctx; rollback_agent_transaction = saved.rollback;
      MEM.derive.drain = saved.drain; A.eval_script = saved.script;
    }
  }
  return out;
}
