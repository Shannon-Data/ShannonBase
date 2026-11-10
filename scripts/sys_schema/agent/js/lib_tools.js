//@include lib_router.js
//@include lib_plan_guard.js
//@include lib_result_guard.js
// Tool specs own their own registration.
//@include lib_tool_specs_core.js
//@include lib_tool_specs_schema.js
//@include lib_tool_specs_ml.js
//@include lib_tool_specs_memory.js
//@include lib_trusted_tools.js
var TABLE_FALLBACK = {
  'INNODB_LOCKS':       'performance_schema.data_locks',
  'INNODB_LOCK_WAITS':  'performance_schema.data_lock_waits',
  'USER_STATISTICS':    'performance_schema.accounts',
  'INDEX_STATISTICS':   'information_schema.STATISTICS',
  'QUERY_CACHE_INFO':   'performance_schema.events_statements_summary_by_digest',
  'GLOBAL_STATUS':      'performance_schema.global_status',
  'GLOBAL_VARIABLES':   'performance_schema.global_variables'
};

// Transaction ownership helpers.
var TX_OWNER_NONE    = 'none';
var TX_OWNER_CALLER  = 'caller';
var TX_OWNER_AGENT   = 'agent';
var TX_OWNER_UNKNOWN = 'unknown';

function tx_active_for(convo) {
  A._tx_sessions = A._tx_sessions || {};
  var k = String(convo || A.conversation_id || 'global');
  return !!A._tx_sessions[k];
}
function set_tx_active_for(convo, val) {
  A._tx_sessions = A._tx_sessions || {};
  var k = String(convo || A.conversation_id || 'global');
  if (val) A._tx_sessions[k] = true;
  else delete A._tx_sessions[k];
}

function agent_tx_lease_owned(conv_id) {
  try {
    var rows = query_checked(
      "SELECT 1 FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(conv_id || A.conversation_id) + "' " +
      "AND session_conn_id = CONNECTION_ID() LIMIT 1"
    );
    return Array.isArray(rows) && rows.length > 0;
  } catch (e) { return null; }
}

// Detect the physical transaction on THIS connection.
function get_session_tx_state() {
  var native_state;
  try { native_state = sys.transaction_state(); } catch (e) {}
  if (native_state && native_state.known && !native_state.active && !native_state.multi_statement)
    return { known: true, active: false, event_id: 0, source: 'kernel' };
  try {
    var inst = query_checked(
      "SELECT COUNT(*) AS enabled FROM performance_schema.setup_consumers WHERE NAME IN" +
      " ('global_instrumentation','thread_instrumentation','events_transactions_current') AND ENABLED='YES'");
    var instrument = query_checked("SELECT ENABLED FROM performance_schema.setup_instruments WHERE NAME='transaction'");
    if (!Array.isArray(inst) || !inst.length || Number(inst[0].enabled) !== 3 ||
        !Array.isArray(instrument) || !instrument.length || instrument[0].ENABLED !== 'YES')
      throw new Error('transaction_instrumentation_disabled');
    var rows = query_checked(
      "SELECT et.ACCESS_MODE,et.EVENT_ID FROM performance_schema.events_transactions_current et" +
      " JOIN performance_schema.threads th ON th.THREAD_ID=et.THREAD_ID" +
      " WHERE th.PROCESSLIST_ID=CONNECTION_ID() AND et.STATE='ACTIVE' AND et.AUTOCOMMIT='NO' LIMIT 1");
    var active = Array.isArray(rows) && rows.length > 0;
    return { known: true, active: !!active || !!(native_state && native_state.multi_statement),
             event_id: active ? Number(rows[0].EVENT_ID || 0) : 0,
             access_mode: active ? String(rows[0].ACCESS_MODE || '') : '', source: 'kernel+pfs' };
  } catch (e) {
    return { known: !!(native_state && native_state.known),
             active: !!(native_state && (native_state.active || native_state.multi_statement)),
             event_id: 0, source: 'kernel', error: String(e) };
  }
}

function get_agent_tx_event_marker() {
  try {
    var rows = query_checked("SELECT @_shannon_agent_tx_event_id AS event_id");
    return (Array.isArray(rows) && rows.length) ? Number(rows[0].event_id || 0) : 0;
  } catch (e) { return 0; }
}

function set_agent_tx_event_marker(event_id) {
  try {
    query_checked("SET @_shannon_agent_tx_event_id=" + (event_id ? Number(event_id) : 'NULL') +
      ",@_shannon_agent_tx_conversation_id=" + (event_id ? "'" + esc(A.conversation_id) + "'" : 'NULL'));
    return get_agent_tx_event_marker() === Number(event_id || 0);
  } catch (e) { return false; }
}

function get_tx_context() {
  var physical = get_session_tx_state();
  var lease_owned = agent_tx_lease_owned(A.conversation_id);
  var agent_event_marker = get_agent_tx_event_marker();

  if (physical.known) {
    if (!physical.active) {
      if (lease_owned) clear_tx_lease(A.conversation_id);
      set_agent_tx_event_marker(0);
      set_tx_active_for(A.conversation_id, false);
      return { active: false, owner: TX_OWNER_NONE, known: true,
               access_mode: physical.access_mode, event_id: 0, source: physical.source };
    }

    if (lease_owned === null)
      return { active: true, owner: TX_OWNER_UNKNOWN, known: false, source: physical.source };

    if (lease_owned && agent_event_marker &&
        Number(physical.event_id) === Number(agent_event_marker)) {
      set_tx_active_for(A.conversation_id, true);
      return { active: true, owner: TX_OWNER_AGENT, known: true,
               access_mode: physical.access_mode, event_id: physical.event_id, source: physical.source };
    }

    if (lease_owned && (!physical.event_id || !agent_event_marker))
      return { active: true, owner: TX_OWNER_UNKNOWN, known: false, source: physical.source };
    if (lease_owned) clear_tx_lease(A.conversation_id);
    set_tx_active_for(A.conversation_id, false);
    return { active: true, owner: TX_OWNER_CALLER, known: true,
             access_mode: physical.access_mode, event_id: physical.event_id, source: physical.source };
  }

  // With P_S unavailable, only the same-invocation JS mirror can prove ownership.
  if (tx_active_for(A.conversation_id) && lease_owned) {
    return { active: true, owner: TX_OWNER_AGENT, known: false,
             access_mode: '', event_id: agent_event_marker, source: physical.source };
  }
  return { active: false, owner: TX_OWNER_UNKNOWN, known: false,
           access_mode: '', event_id: 0, source: physical.source };
}

/* Audit must never change control flow. */
function audit_tx_failure(op, target, err) {
  try {
    mem_log_audit('tx', op, String(target || ''), String(err).substring(0, 400), 0, 0, '');
  } catch (e) {}
}

function begin_tx_lease(conv_id, plan_id, timeout_minutes) {
  timeout_minutes = timeout_minutes || 30;

  try {
    query_checked(
      "INSERT INTO mysql.agent_tx_lease(conversation_id, session_conn_id, plan_id, expires_at) " +
      "VALUES ('" + esc(conv_id) + "', CONNECTION_ID(), '" + esc(plan_id || '') + "', " +
      "DATE_ADD(NOW(), INTERVAL " + timeout_minutes + " MINUTE)) " +
      "ON DUPLICATE KEY UPDATE " +
      "session_conn_id = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(session_conn_id), session_conn_id), " +
      "plan_id         = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(plan_id), plan_id), " +
      "created_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), NOW(), created_at), " +
      "expires_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(expires_at), expires_at)"
    );
  } catch (e) {
    /* No lease was written, so none may be assumed. */
    audit_tx_failure('begin_tx_lease_failed', conv_id, e);
    return false;
  }

  return agent_tx_lease_owned(conv_id);
}

/** Remove this session's lease row; false on failure, never throws. */
function clear_tx_lease(conv_id) {
  try {
    query_checked(
      "DELETE FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(conv_id) + "' " +
      "AND session_conn_id = CONNECTION_ID()"
    );
    return true;
  } catch (e) {
    audit_tx_failure('clear_tx_lease_failed', conv_id, e);
    return false;
  }
}

/* The one ROLLBACK path: state is dropped only once the tx is confirmed gone. */
function rollback_agent_transaction(reason) {
  var saved_cleanup = A.runtime_cleanup;
  A.runtime_cleanup = true;
  try {
    var ctx = get_tx_context();
    if (!ctx.active) return { ok: true, already_closed: true };
    if (ctx.owner !== TX_OWNER_AGENT)
      return { ok: false, error: 'transaction_not_owned', owner: ctx.owner };

    try {
      query_checked('ROLLBACK');
    } catch (e) {
      audit_tx_failure('rollback_failed', A.conversation_id, e);
      return { ok: false, error: 'rollback_failed', detail: String(e) };
    }

    var after = get_session_tx_state();
    if (!after.known || after.active) {
      audit_tx_failure('rollback_not_verified', A.conversation_id, reason || '');
      return { ok: false, error: 'rollback_not_verified' };
    }
    set_tx_active_for(A.conversation_id, false);
    A.agent_tx_started_at = 0;
    set_agent_tx_event_marker(0);
    if (!clear_tx_lease(A.conversation_id))
      return { ok: true, lease_retained: true };
    return { ok: true };
  } finally { A.runtime_cleanup = saved_cleanup; }
}

// Is the whole process list visible to this session?
function processlist_fully_visible() {
  if (A._pl_visible !== undefined) return A._pl_visible;
  A._pl_visible = false;
  try {
    var rows = query_checked(
      "SELECT (SELECT COUNT(*) FROM performance_schema.threads" +
      "         WHERE PROCESSLIST_ID IS NOT NULL) AS seen," +
      "       (SELECT VARIABLE_VALUE FROM performance_schema.global_status" +
      "         WHERE VARIABLE_NAME='Threads_connected') AS total");
    if (Array.isArray(rows) && rows.length) {
      var seen  = Number(rows[0].seen  || 0);
      var total = Number(rows[0].total || 0);
      A._pl_visible = (seen > 0 && total > 0 && seen >= total);
    }
  } catch (e) { A._pl_visible = false; }
  return A._pl_visible;
}

// Leases whose owning connection no longer exists.
function reap_dead_tx_leases() {
  if (!processlist_fully_visible()) return 0;
  try {
    var r = query_checked(
      "DELETE l FROM mysql.agent_tx_lease l" +
      " WHERE l.session_conn_id <> CONNECTION_ID()" +
      "   AND NOT EXISTS (SELECT 1 FROM performance_schema.threads th" +
      "                    WHERE th.PROCESSLIST_ID = l.session_conn_id)");
    return (r && r.affected_rows) ? Number(r.affected_rows) : 0;
  } catch (e) { return 0; }
}

// Clean expired AGENT leases owned by this connection.
function cleanup_expired_tx_leases() {
  try {
    var expired = query(
      "SELECT conversation_id, plan_id, TIMESTAMPDIFF(SECOND, created_at, NOW()) AS age_sec " +
      "FROM mysql.agent_tx_lease WHERE expires_at < NOW() AND session_conn_id = CONNECTION_ID()"
    );
    if (!Array.isArray(expired) || !expired.length) return [];

    // The lease is already expired, so get_tx_context() deliberately will not trust it.
    var physical = get_session_tx_state();
    var marker = get_agent_tx_event_marker();
    if (physical.known && physical.active) {
      /* Never discard ownership evidence for an unverified active tx. */
      if (!marker || Number(physical.event_id) !== Number(marker)) return [];
      if (!rollback_agent_transaction('lease_expired').ok) return [];
    } else if (!physical.known) {
      return []; /* Do not erase a lease when physical state is unknown. */
    }
    for (var i = 0; i < expired.length; i++) {
      clear_tx_lease(expired[i].conversation_id);
      log_rollback_event(expired[i].conversation_id, expired[i].plan_id, 0, '',
                         'expired', t('超时自动回滚：', 'Timeout auto-rollback: ') + expired[i].age_sec + 's');
    }
    return expired;
  } catch (e) { return []; }
}

// Final safety net is allowed to rollback only AGENT-owned transactions.
function finalize_tx_safety_net() {
  var ctx = get_tx_context();
  if (!ctx.active || ctx.owner !== TX_OWNER_AGENT) return '';

  var rb = rollback_agent_transaction('safety_net');
  if (!rb.ok) {
    if (rb.error === 'rollback_failed')
      return '\n' + t('[安全网] 回滚失败，事务状态未确认；保留事务租约。',
                      '[Safety net] Rollback failed; transaction state unverified, lease retained.');
    return '\n' + t('[安全网] ROLLBACK 已发送，但无法确认事务结束；保留事务租约。',
                    '[Safety net] ROLLBACK issued but closure unverified; lease retained.');
  }
  if (rb.lease_retained)
    return '\n' + t('[安全网] Agent 未提交事务已强制回滚，但事务租约清理失败。',
                    '[Safety net] Agent-owned transaction rolled back, but its lease could not be cleared.');
  return '\n' + t('[安全网] Agent 未提交事务已强制回滚。',
                  '[Safety net] Uncommitted agent-owned transaction force-rolled back.');
}

// The sql_mode the policy gate assumes, checked once per turn.
var GATE_INCOMPATIBLE_SQL_MODES = ['NO_BACKSLASH_ESCAPES', 'ANSI_QUOTES', 'ANSI'];

function sql_mode_gate_check() {
  var rows = query("SELECT @@session.sql_mode AS m");
  if (!Array.isArray(rows) || !rows.length) {
    /* Could not read it.  Fail closed: the gate's whole job is to be sure. */
    return { ok: false, modes: [],
             message: t('无法读取当前会话的 sql_mode，出于安全考虑拒绝本次请求。',
                        'Could not read this session\'s sql_mode; refusing the request rather than '
                        + 'parsing SQL under an unknown quoting dialect.') };
  }
  var mode  = String(rows[0].m || '').toUpperCase();
  var parts = mode ? mode.split(',') : [];
  var bad   = [];
  for (var i = 0; i < parts.length; i++) {
    if (GATE_INCOMPATIBLE_SQL_MODES.indexOf(parts[i].trim()) !== -1) bad.push(parts[i].trim());
  }
  if (!bad.length) return { ok: true, modes: [] };

  return {
    ok: false,
    modes: bad,
    message: t('当前会话的 sql_mode 包含 ' + bad.join(', ') +
               '，Agent 的 SQL 安全检查无法在该模式下正确解析语句，因此拒绝执行。'
               + '请在该连接上执行 SET SESSION sql_mode = \'\' （或移除这些模式）后重试。',
               'This session\'s sql_mode contains ' + bad.join(', ') + '. The agent\'s SQL policy '
               + 'checks cannot parse statements correctly under those modes, so the request is '
               + 'refused. Run SET SESSION sql_mode = \'\' (or drop those modes) on this '
               + 'connection and retry.')
  };
}

function sql_lex_info(sql) {
  var s = String(sql || '');
  var tokens = [];
  var token = '';
  var depth = 0;
  var in_single = false, in_double = false, in_backtick = false;
  var in_line_comment = false, in_block_comment = false;
  var escaped = false;
  var top_level_semicolons = 0;
  var executable_comment = false;

  function flush() {
    if (token) {
      tokens.push({ text: token.toUpperCase(), depth: depth });
      token = '';
    }
  }

  for (var i = 0; i < s.length; i++) {
    var ch = s[i], nx = (i + 1 < s.length) ? s[i + 1] : '';

    if (in_line_comment) {
      if (ch === '\n' || ch === '\r') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (ch === '*' && nx === '/') { in_block_comment = false; i++; }
      continue;
    }

    if (in_single || in_double || in_backtick) {
      if (escaped) { escaped = false; continue; }
      if (!in_backtick && ch === '\\') { escaped = true; continue; }

      if (in_single && ch === "'") {
        if (nx === "'") { i++; continue; }
        in_single = false;
      } else if (in_double && ch === '"') {
        if (nx === '"') { i++; continue; }
        in_double = false;
      } else if (in_backtick && ch === '`') {
        if (nx === '`') { i++; continue; }
        in_backtick = false;
      }
      continue;
    }

    if (ch === '-' && nx === '-' &&
        (i + 2 >= s.length || /\s/.test(s[i + 2]))) {
      flush(); in_line_comment = true; i++; continue;
    }
    if (ch === '#') { flush(); in_line_comment = true; continue; }
    if (ch === '/' && nx === '*') {
      if (s[i + 2] === '!') executable_comment = true;
      flush(); in_block_comment = true; i++; continue;
    }
    if (ch === "'") { flush(); in_single = true; continue; }
    if (ch === '"') { flush(); in_double = true; continue; }
    if (ch === '`') { flush(); in_backtick = true; continue; }

    if (ch === '(') {

      var pending_call = !!token;
      flush();
      if (pending_call) tokens[tokens.length - 1].call = true;
      depth++;
      continue;
    }
    if (ch === ')') { flush(); if (depth > 0) depth--; continue; }
    if (ch === ';') {
      flush();
      if (depth === 0) top_level_semicolons++;
      continue;
    }

    if (/[A-Za-z0-9_$]/.test(ch)) token += ch;
    else flush();
  }
  flush();

  // top: every top-level token.
  var top = [], top_calls = [];
  for (var ti = 0; ti < tokens.length; ti++) {
    if (tokens[ti].depth !== 0) continue;
    top.push(tokens[ti].text);
    if (tokens[ti].call) top_calls.push(tokens[ti].text);
  }
  var first = top.length ? top[0] : '';
  var dml = '';
  if (['INSERT','UPDATE','DELETE','REPLACE'].indexOf(first) !== -1) {
    dml = first;
  } else if (first === 'WITH') {
    for (var j = 1; j < top.length; j++) {
      if (['INSERT','UPDATE','DELETE','REPLACE'].indexOf(top[j]) !== -1) {
        dml = top[j];
        break;
      }
      if (top[j] === 'SELECT') break;
    }
  }

  var trimmed = s.trim();
  var trailing_only = top_level_semicolons === 1 && /;\s*$/.test(trimmed);
  return {
    executable_comment: executable_comment,
    file_io: tokens.some(function(t, i) {
      return t.text === 'LOAD_FILE' || (t.text === 'INTO' && tokens[i + 1] &&
        (tokens[i + 1].text === 'OUTFILE' || tokens[i + 1].text === 'DUMPFILE'));
    }),
    first_keyword: first,
    dml_keyword: dml,
    top_tokens: top,
    top_call_tokens: top_calls,
    has_top_level_where: top.indexOf('WHERE') !== -1,
    multiple_statements: top_level_semicolons > (trailing_only ? 1 : 0)
  };
}

var READ_ROW_LIMIT_MAX_DEFAULT = 1000;
var REVIEW_STEP_STUCK_TTL_DEFAULT = 120;

function read_row_limit_max() {
  return _combine_min(get_operator_policy(), 'read_row_limit_max',
                      READ_ROW_LIMIT_MAX_DEFAULT, 1);
}

function read_sql_limit_info(lex) {
  var top = lex.top_tokens || [];
  var idx = top.indexOf('LIMIT');
  if (idx === -1) return { has_limit: false, value: null };

  var nums = [];
  for (var i = idx + 1; i < top.length && nums.length < 2; i++) {
    if (/^[0-9]+$/.test(top[i])) nums.push(Number(top[i]));
    else break;
  }
  if (!nums.length) return { has_limit: true, value: null };
  return { has_limit: true, value: (nums.length === 2) ? nums[1] : nums[0] };
}

var _AGG_FUNCS = ['COUNT','SUM','AVG','MIN','MAX','GROUP_CONCAT','STDDEV',
                  'STDDEV_POP','STDDEV_SAMP','VARIANCE','VAR_POP','VAR_SAMP',
                  'BIT_AND','BIT_OR','BIT_XOR','JSON_ARRAYAGG','JSON_OBJECTAGG'];

function read_sql_is_single_row(lex) {
  var top = lex.top_tokens || [];
  if (top.indexOf('GROUP') !== -1) return false;
  /* A set operation re-opens the row count even if each leg aggregates. */
  if (top.indexOf('UNION') !== -1 || top.indexOf('INTERSECT') !== -1 ||
      top.indexOf('EXCEPT') !== -1) return false;
  // Call tokens only.
  var calls = lex.top_call_tokens || [];
  for (var i = 0; i < calls.length; i++)
    if (_AGG_FUNCS.indexOf(calls[i]) !== -1) return true;
  return false;
}

// Bound an unbounded read by capping it, rather than refusing it.
function apply_read_ceiling(sql, stmt) {
  if (!stmt || !stmt.is_read || stmt.multiple_statements) return sql;
  if (stmt.first_keyword !== 'SELECT' && stmt.first_keyword !== 'WITH') return sql;

  var lex = sql_lex_info(sql);
  if (read_sql_is_single_row(lex)) return sql;
  if (read_sql_limit_info(lex).has_limit) return sql;

  var trimmed = String(sql).replace(/\s*;\s*$/, '');
  return trimmed + '\nLIMIT ' + read_row_limit_max();
}

// Returns null when the statement may run as written, or a tool-shaped rejection when it may not.
function guard_read_sql(sql, stmt) {
  if (!stmt || !stmt.is_read) return null;

  if (stmt.first_keyword !== 'SELECT' && stmt.first_keyword !== 'WITH')
    return null;

  var lex = sql_lex_info(sql);
  if (read_sql_is_single_row(lex)) return null;

  var lim = read_sql_limit_info(lex);
  var row_max = read_row_limit_max();
  if (!lim.has_limit) {
    return { ok: false, error: 'read_limit_required',
             response: t(
      '拒绝执行：该 SELECT 没有 LIMIT。结果集会被读进 JavaScript 引擎堆（每线程 ' +
      Math.round(engine_heap_bytes() / 1024) + 'KB，且由构建期决定），' +
      '无 LIMIT 的查询可能耗尽它。请加上 LIMIT（最大 ' + row_max + '），' +
      '或改写为聚合查询（例如 SELECT COUNT(*) / SUM() / GROUP BY ... LIMIT n）后重试。',
      'Rejected: this SELECT has no LIMIT. Results are read into the JavaScript engine heap ' +
      '(' + Math.round(engine_heap_bytes() / 1024) + 'KB per thread, fixed at build time), ' +
      'which an unbounded query can exhaust. Add a LIMIT ' +
      '(at most ' + row_max + '), or rewrite it as an aggregate ' +
      '(SELECT COUNT(*) / SUM() / GROUP BY ... LIMIT n) and retry.') };
  }
  if (lim.value !== null && lim.value > row_max) {
    return { ok: false, error: 'read_limit_too_large',
             response: t(
      '拒绝执行：LIMIT ' + lim.value + ' 超过单次读取上限 ' + row_max + ' 行。' +
      '请降低 LIMIT，或改用聚合把计算下推到数据库（模型不需要看到每一行才能得出结论）。',
      'Rejected: LIMIT ' + lim.value + ' exceeds the ' + row_max + '-row ceiling for a ' +
      'single read. Lower the LIMIT, or push the computation into the database with an aggregate ' +
      '-- answering the question rarely requires seeing every row.') };
  }
  return null;
}

// Default statement wall clock for agent reads, in milliseconds.
var READ_TIMEOUT_MS_DEFAULT = 30000;

function read_timeout_ms() {
  return _combine_min(get_operator_policy(), 'read_timeout_ms',
                      READ_TIMEOUT_MS_DEFAULT, 1000);
}

var MAX_TURNS_DEFAULT        = 10;
var TURN_DEADLINE_MS_DEFAULT = 10 * 60 * 1000;

function max_turns() {
  var base = get_operator_policy();
  var ceiling = Number(base.max_turns);
  var fallback = isFinite(ceiling) && ceiling > 0 ? ceiling : MAX_TURNS_DEFAULT;
  return Math.floor(_combine_min(base, 'max_turns', cfg('max_turns', fallback), 1));
}

function turn_deadline_ms() {
  return _combine_min(get_operator_policy(), 'turn_deadline_ms',
                      TURN_DEADLINE_MS_DEFAULT, 10000);
}

// Prefix telling the model it is looking at a prefix, not the answer.
function truncation_note(rows) {
  var total = rows_total(rows);
  var got   = Array.isArray(rows) ? rows.length : 0;
  return t(
    '⚠ 结果被截断：仅返回前 ' + got + ' 行（共 ' + total + ' 行），因为结果超出了单次读取的内存上限。' +
    '不要基于这些行计算总计或做全量结论；请改用聚合查询（COUNT/SUM/GROUP BY）让数据库完成计算。\n',
    '⚠ Truncated: only the first ' + got + ' of ' + total + ' rows were returned, because the ' +
    'result exceeded the per-read memory ceiling. Do not total these rows or draw whole-population ' +
    'conclusions from them; use an aggregate (COUNT/SUM/GROUP BY) and let the database compute it.\n');
}

// The statement-level wall clock.
function with_read_timeout(sql, stmt, ms) {
  ms = Number(ms) || 0;
  if (ms <= 0 || !stmt || stmt.first_keyword !== 'SELECT') return sql;
  var ceiling = Math.round(ms);
  var hint = 'MAX_EXECUTION_TIME(' + ceiling + ')';

  if (/^\s*SELECT\s+\/\*\+/i.test(sql)) {
    // Only the leading hint block is ours to rewrite.
    var blk = /^(\s*SELECT\s+\/\*\+)([\s\S]*?)(\*\/)/i.exec(sql);

    if (!blk) return sql.replace(/(\/\*\+)/, '$1 ' + hint);

    var body = /MAX_EXECUTION_TIME/i.test(blk[2])
             ? clamp_exec_time_hint(blk[2], ceiling)
             : ' ' + hint + blk[2];
    return blk[1] + body + blk[3] + sql.substring(blk[0].length);
  }
  return sql.replace(/^(\s*SELECT)(\s)/i, '$1 /*+ ' + hint + ' */$2');
}

// Rewrite every MAX_EXECUTION_TIME(n) in a hint block down to `ceiling` unless it is already stricter.
function clamp_exec_time_hint(sql, ceiling) {
  return sql.replace(/MAX_EXECUTION_TIME\s*\(\s*([0-9]*)\s*\)/gi,
                     function (whole, digits) {
                       var n = parseInt(digits, 10);
                       if (isFinite(n) && n > 0 && n <= ceiling) return whole;
                       return 'MAX_EXECUTION_TIME(' + ceiling + ')';
                     });
}

function classify_statement(sql) {
  var lex = sql_lex_info(sql);
  var first = lex.first_keyword;

  var _READ  = ['SELECT','SHOW','DESCRIBE','DESC','EXPLAIN','WITH'];
  var _DDL   = ['CREATE','ALTER','DROP','TRUNCATE','RENAME'];
  var _TCL   = ['BEGIN','START','COMMIT','ROLLBACK','SAVEPOINT'];

  var external_effects = lex.file_io || lex.executable_comment || first === 'LOAD' || first === 'CALL';
  var is_write = !!lex.dml_keyword || external_effects;
  var is_read  = _READ.indexOf(first) !== -1 && !is_write;
  var is_ddl   = _DDL.indexOf(first) !== -1;
  var is_tcl   = _TCL.indexOf(first) !== -1;

  var risk = external_effects ? 'high' : 'low';
  if (is_ddl) {
    risk = 'high';
  } else if (is_write) {
    risk = ((lex.dml_keyword === 'DELETE' || lex.dml_keyword === 'UPDATE') &&
            !lex.has_top_level_where) ? 'high' : 'medium';
  }

  if (external_effects) risk = 'high';

  /* Only meaningful for DDL; a SELECT has no object class to speak of. */
  var act = is_ddl ? ddl_action_class(sql, lex)
                   : { action_class: null, ddl_object: null, is_js_routine: false };

  var kind;
  if (is_read)              kind = first;
  else if (is_write)        kind = 'DML';
  else if (is_ddl)          kind = 'DDL';
  else if (is_tcl)          kind = 'TCL';
  else if (first === 'SET') kind = 'SET';
  else                      kind = 'OTHER';

  return {
    external_side_effects: !!external_effects,
    kind:          kind,
    first_keyword: first,
    dml_keyword:   lex.dml_keyword,
    is_read:       is_read,
    is_write:      is_write,
    is_ddl:        is_ddl,
    is_tcl:        is_tcl,
    risk:          risk,
    action_class:  act.action_class,
    ddl_object:    act.ddl_object,
    is_js_routine: act.is_js_routine,
    has_top_level_where: lex.has_top_level_where,
    multiple_statements: lex.multiple_statements
  };
}

function check_ddl_action_policy(sql, policy) {
  var act = ddl_action_class(sql);
  if (act.action_class === 'account' && !(policy && policy.allow_account_ddl))
    return { reason: 'account_ddl_not_allowed',
             message: t('该语句创建或修改账户/角色（' + (act.ddl_object || '') + '），' +
                        'agent 默认不执行；如确需执行，请在 @chat_options 中设置 ' +
                        'allow_account_ddl=true。',
                        'This statement creates or alters an account or role (' +
                        (act.ddl_object || '') + '). The agent will not run it unless ' +
                        'allow_account_ddl=true is set in @chat_options.') };
  if (act.action_class === 'code' && !(policy && policy.allow_code_ddl))
    return { reason: 'code_ddl_not_allowed',
             message: act.is_js_routine
               ? t('该语句创建 LANGUAGE JAVASCRIPT 例程，即在库内写入新的 agent 代码，' +
                   'agent 默认不执行；如确需执行，请在 @chat_options 中设置 allow_code_ddl=true。',
                   'This statement creates a LANGUAGE JAVASCRIPT routine -- new agent code ' +
                   'inside the database. The agent will not run it unless allow_code_ddl=true ' +
                   'is set in @chat_options.')
               : t('该语句创建或修改存储程序/触发器/事件（' + (act.ddl_object || '') + '），' +
                   '其效果会超出本次会话，agent 默认不执行；如确需执行，请在 @chat_options 中' +
                   '设置 allow_code_ddl=true。',
                   'This statement creates or alters stored code (' + (act.ddl_object || '') +
                   '), whose effect outlives this conversation. The agent will not run it ' +
                   'unless allow_code_ddl=true is set in @chat_options.') };
  if (act.action_class === 'instance' && !(policy && policy.allow_instance_ddl))
    return { reason: 'instance_ddl_not_allowed',
             message: t('该语句改变服务器实例级状态（' + (act.ddl_object || '') + '），' +
                        '影响范围超出当前库，agent 默认不执行；如确需执行，请在 @chat_options ' +
                        '中设置 allow_instance_ddl=true。',
                        'This statement changes server-wide instance state (' +
                        (act.ddl_object || '') + '), beyond the scope of any one schema. ' +
                        'The agent will not run it unless allow_instance_ddl=true is set in ' +
                        '@chat_options.') };
  return null;
}

// Schemas the agent must not write to without an explicit opt-in.
var SYSTEM_SCHEMAS = {
  mysql: 1, sys: 1, performance_schema: 1, information_schema: 1
};

function is_system_schema(name) {
  return Object.prototype.hasOwnProperty.call(
    SYSTEM_SCHEMAS, String(name || '').toLowerCase().replace(/`/g, ''));
}

/* The schema a table reference names, or '' when it is unqualified. */
function ref_schema(ref) {
  var m = String(ref || '').replace(/`/g, '').match(/^([A-Za-z0-9_$]+)\s*\./);
  return m ? m[1] : '';
}

// One table reference as SQL spells it: `db` .
var _SQL_REF = '`?[A-Za-z0-9_$]+`?(?:\\s*\\.\\s*`?[A-Za-z0-9_$]+`?)?';
var _RE_INSERT  = new RegExp('\\binsert\\s+(?:low_priority\\s+|delayed\\s+|high_priority\\s+|' +
                             'ignore\\s+)*(?:into\\s+)?(' + _SQL_REF + ')', 'i');
var _RE_REPLACE = new RegExp('\\breplace\\s+(?:low_priority\\s+|delayed\\s+)*(?:into\\s+)?(' +
                             _SQL_REF + ')', 'i');
var _RE_DDL_OBJ = new RegExp('\\b(?:table|view|index|trigger|event|database|schema|tablespace)\\s+' +
                             '(?:if\\s+(?:not\\s+)?exists\\s+)?(' + _SQL_REF + ')', 'i');
var _RE_DDL_ON  = new RegExp('\\bon\\s+(' + _SQL_REF + ')', 'i');

var _RE_RENAME_TO = new RegExp('\\bto\\s+(' + _SQL_REF + ')', 'gi');
// ALTER TABLE t RENAME [TO|AS] new.
var _RE_ALTER_RENAME = new RegExp('\\brename\\s+(?:to\\s+|as\\s+)?' +
                                  '(?!table\\b|column\\b|index\\b|key\\b)(' + _SQL_REF + ')', 'i');

// Which tables does this statement WRITE to?
function sql_write_targets(sql) {
  var s = String(sql || '');
  var out = [], seen = {};

  function add(ref) {

    var name = String(ref || '').trim().replace(/[`;,()]/g, '').replace(/\s*\.\s*/g, '.');
    if (!name || seen[name]) return;
    /* Aliases, keywords and derived tables are not write targets. */
    if (!/^[A-Za-z0-9_$]+(\.[A-Za-z0-9_$]+)?$/.test(name)) return;
    seen[name] = true;
    out.push(name);
  }

  function add_refs(segment) {

    var items = String(segment || '').split(/,|\bjoin\b|\binner\b|\bleft\b|\bright\b|\bcross\b/i);
    for (var i = 0; i < items.length; i++) {
      var m = String(items[i]).trim().match(/^`?([A-Za-z0-9_$]+`?(?:\s*\.\s*`?[A-Za-z0-9_$]+`?)?)/);
      if (m) add(m[1].replace(/\s+/g, ''));
    }
  }

  var m;
  if ((m = s.match(_RE_INSERT)))  add(m[1]);
  if ((m = s.match(_RE_REPLACE))) add(m[1]);
  if ((m = s.match(/\bupdate\s+(?:low_priority\s+)?(?:ignore\s+)?([\s\S]*?)\bset\b/i)))             add_refs(m[1]);
  if ((m = s.match(/\bdelete\s+(?:low_priority\s+|quick\s+|ignore\s+)*([\s\S]*?)\bfrom\b([\s\S]*?)(?:\bwhere\b|\busing\b|$)/i))) {
    add_refs(m[1]);
    add_refs(m[2]);
  }

  // DDL names its object after the object keyword.
  var first = String((s.match(/^\s*(\w+)/) || ['', ''])[1]).toUpperCase();
  if (['CREATE','ALTER','DROP','TRUNCATE','RENAME'].indexOf(first) !== -1) {
    if ((m = s.match(_RE_DDL_OBJ))) add(m[1]);

    if (/\b(?:index|trigger)\b/i.test(s) && (m = s.match(_RE_DDL_ON))) add(m[1]);

    if (/^\s*rename\s+table\b/i.test(s)) {
      _RE_RENAME_TO.lastIndex = 0;
      while ((m = _RE_RENAME_TO.exec(s))) add(m[1]);
    } else if ((m = s.match(_RE_ALTER_RENAME))) {
      add(m[1]);
    }
  }
  return out;
}

// Refuse a write whose target is in a system schema.
function check_system_schema_policy(sql, policy, db) {
  if (policy && policy.allow_system_schema_writes) return null;

  var targets = sql_write_targets(sql);
  if (!targets.length) return null;

  var hit = null;
  for (var i = 0; i < targets.length && !hit; i++) {
    var schema = ref_schema(targets[i]);
    /* DROP DATABASE mysql: the "table" is the schema. */
    if (!schema && is_system_schema(targets[i]) && /\b(?:database|schema)\b/i.test(sql))
      hit = targets[i];
    else if (schema && is_system_schema(schema)) hit = targets[i];
    else if (!schema && is_system_schema(db))    hit = db + '.' + targets[i];
  }
  if (!hit) return null;

  return { reason: 'system_schema_write_not_allowed',
           target:  hit,
           message: t('拒绝：该语句会写入系统库（' + hit + '）。系统库里放的是权限表、' +
                      'agent 自己的记忆与审批记录，以及 agent 自身的存储程序，' +
                      'agent 默认不写；如确需执行，请在 @chat_options 中设置 ' +
                      'allow_system_schema_writes=true，或直接由 DBA 手工执行。',
                      'Rejected: this statement writes to a system schema (' + hit + '). ' +
                      'Those schemas hold the grant tables, the agent\'s own memory and ' +
                      'approval records, and the agent\'s own stored program. The agent will ' +
                      'not write to them unless allow_system_schema_writes=true is set in ' +
                      '@chat_options -- otherwise run it yourself as a DBA.') };
}

// Does this DDL statement destroy data or schema objects?
function is_destructive_ddl(sql) {
  var lex = sql_lex_info(sql);
  if (lex.first_keyword === 'TRUNCATE') return true;
  return lex.top_tokens.indexOf('DROP') !== -1;
}

// What KIND of thing does this DDL act on?
var _DDL_OBJECT_CLASS = {
  USER: 'account', ROLE: 'account',
  FUNCTION: 'code', PROCEDURE: 'code', TRIGGER: 'code', EVENT: 'code',

  INSTANCE: 'instance', RESOURCE: 'instance',
  TABLE: 'schema', INDEX: 'schema', VIEW: 'schema', DATABASE: 'schema',
  SCHEMA: 'schema', TABLESPACE: 'schema', SERVER: 'schema', SEQUENCE: 'schema',

  LOGFILE: 'schema', SPATIAL: 'schema'
};

function ddl_action_class(sql, lex) {
  lex = lex || sql_lex_info(sql);
  var top = lex.top_tokens;
  var klass = null, object = null;
  for (var i = 0; i < top.length; i++) {
    if (Object.prototype.hasOwnProperty.call(_DDL_OBJECT_CLASS, top[i])) {
      object = top[i];
      klass  = _DDL_OBJECT_CLASS[top[i]];
      break;
    }
  }
  return {

    action_class: klass || 'schema',
    ddl_object:   object,

    is_js_routine: klass === 'code' && top.indexOf('JAVASCRIPT') !== -1
  };
}

function try_recover_unknown_table(result, original_sql) {
  if (!result || result.indexOf('Unknown table') === -1) return null;
  var m = result.match(/Unknown table\s+'?(?:[\w]+\.)?([\w]+)'?/i);
  if (!m) return null;
  var bad = m[1].toUpperCase(), replacement = TABLE_FALLBACK[bad];
  if (!replacement) return null;
  var pattern = new RegExp('((?:FROM|JOIN)\\s+)(?:information_schema\\.|performance_schema\\.)?' + bad + '\\b', 'gi');
  var new_sql = original_sql.replace(pattern, function(match, prefix) {
    return prefix + replacement;
  });
  return { sql: new_sql,
           desc: t('⚡自动恢复：', '⚡Auto-recovered: ') + bad + ' → ' + replacement };
}

function estimate_complexity(text) {
  var signals = [
    /\bGROUP\s+BY\b|按.{1,8}分组/i,
    /\bJOIN\b|多表.*关联|关联.*多表/i,
    /子查询|\bIN\s*\(\s*SELECT/i,
    /同比|环比|趋势|占比|排名.*前\s*\d|TOP\s*\d/i,
    /先.*再.*(?:然后|最后)|第一步.*第二步/,
    /\bUNION\b|\bWITH\b.*\bAS\b/i
  ];
  var score = 0;
  for (var i = 0; i < signals.length; i++) if (signals[i].test(text)) score++;
  return score;
}

function decompose_query(text) {
  if (estimate_complexity(text) < 2) return [{ op: 'scan', note: text }];
  var tasks = [{ op: 'scan', note: t('主表扫描', 'main table scan') }];
  if (/最近|过去\s*\d+|大于|小于|等于|筛选|过滤|WHERE|filter|recent|last\s+\d/i.test(text))
    tasks.push({ op:'filter', note: t('条件过滤', 'filter condition') });
  if (/关联|联合|join/i.test(text))
    tasks.push({ op:'join', note: t('多表关联', 'multi-table join') });
  if (/聚合|汇总|统计|SUM|AVG|COUNT|aggregate|average/i.test(text))
    tasks.push({ op:'agg', note: t('聚合计算', 'aggregation') });
  if (/排名|排序|最高|最低|前\s*\d+|rank|sort|top\s*\d/i.test(text))
    tasks.push({ op:'sort', note: t('排序限行', 'sort + limit') });
  tasks.push({ op: 'explain', note: t('执行计划验证', 'explain plan check') });
  return tasks;
}

function logical_plan_to_hint(tasks, schema_from_embeddings) {
  if (!tasks || tasks.length <= 1) return '';
  var op_hints = {
    scan:    t('确定主表，必要时先用 query_db 检查表结构',
               'Identify main table; use query_db to inspect schema if needed'),
    filter:  t('构造 WHERE 条件，确认谓词列有索引',
               'Build WHERE clause; confirm predicate columns are indexed'),
    join:    schema_from_embeddings
             ? t('按上方 DDL 中的 FOREIGN KEY 子句拼写 JOIN 条件',
                 'Use FOREIGN KEY clauses from the DDL above to write JOIN conditions')
             : t('按【推荐JOIN路径】拼写 JOIN 子句',
                 'Use [Recommended JOIN paths] to write JOIN clauses'),
    agg:     t('构造 GROUP BY + 聚合函数，注意 HAVING 与 WHERE 顺序',
               'Build GROUP BY + aggregate functions; note HAVING vs WHERE ordering'),
    sort:    t('添加 ORDER BY；LIMIT 避免大结果集',
               'Add ORDER BY; use LIMIT to avoid large result sets'),
    explain: t('调用 explain_sql 验证；⚠全表扫描时先改写 SQL',
               'Call explain_sql to verify; ⚠ rewrite SQL if full table scan detected')
  };
  var lines = [t('【逻辑计划（按序执行，不得跳步）】',
                 '[Logical Plan (execute in order, no skipping)]')];
  for (var i = 0; i < tasks.length; i++)
    lines.push('Step' + (i+1) + ' [' + tasks[i].op.toUpperCase() + ']: ' +
               (op_hints[tasks[i].op] || tasks[i].op) + '  ← ' + tasks[i].note);
  return lines.join('\n');
}

var MAX_PLAN_STEPS = 15;

function execute_plan(steps, db) {
  var results = [];
  var last_result_text = '';
  var per_step_limit = cfg('plan_step_max_tokens', 4000);

  for (var i = 0; i < Math.min(steps.length, MAX_PLAN_STEPS); i++) {
    var step = steps[i];
    var sql  = replace_ph(String(step.sql || ''), db);
    if (sql.indexOf('__LAST_RESULT__') !== -1)
      sql = sql.replace(/__LAST_RESULT__/g, esc(last_result_text.substring(0, 200)));

    var stmt = classify_statement(sql);
    sql = apply_read_ceiling(sql, stmt);
    var result_text, step_ok = true;
    var guard = guard_read_sql(sql, stmt);
    if (guard) {
      /* Same ceiling as query_db: a plan step is not a way around it. */
      step_ok = false;
      result_text = guard.response;
      last_result_text = result_text;
      results.push({ step: i + 1, desc: step.desc || ('Step ' + (i+1)),
                     sql: sql, result: result_text, ok: false });
      continue;
    }
    if (stmt.is_read && !stmt.multiple_statements) {
      var planned = execute_tool('query_db', { sql: sql }, db);
      result_text = compress(planned.response || planned.error || '', per_step_limit);
      step_ok = !!planned.ok;
    } else {
      step_ok = false;
      result_text = 'Skipped non-read-only or multi-statement SQL: ' + sql.substring(0, 80);
    }
    last_result_text = result_text;
    results.push({ step: i + 1, desc: step.desc || ('Step ' + (i+1)),
                  sql: sql, result: result_text, ok: step_ok });
  }

  for (var ri = 0; ri < results.length; ri++) {
    log_sql_trace(A.conversation_id, 0, results[ri].step, 'plan_sql',
                  'plan_sql', results[ri].sql, results[ri].desc, results[ri].result);
  }

  return results;
}

// Registry-driven validation.
function validate_tool_call(tool_obj, policy) {
  if (!tool_obj || typeof tool_obj.tool !== 'string')
    return t('工具调用格式错误：缺少 tool 字段',
             'Tool call format error: missing "tool" field');

  var spec = get_tool_spec(tool_obj.tool);
  if (!spec)
    return t('未知工具：', 'Unknown tool: ') + tool_obj.tool;

  var args = tool_obj.args || {};
  var err  = ts_validate(spec.args, args, '');
  if (err) return ts_error_message(spec, err);

  /* Cross-field / policy-dependent checks that a schema cannot express. */
  if (spec.validate) {
    var msg = spec.validate(args, policy, spec);
    if (msg) return msg;
  }
  return null;
}

function replace_ph(sql, db) {
  if (!db) return sql;
  return sql.replace(
    /your_database_name|your_db_name|<database_name>|\[database_name\]|\{database_name\}|your_schema/gi,
    db
  );
}

// Shared EXPLAIN FORMAT=JSON tree walker.
function walk_explain_tree(node, visitor) {
  if (!node || typeof node !== 'object') return;
  visitor(node);
  ['nested_loop','attached_subqueries','query_block',
   'ordering_operation','grouping_operation'].forEach(function(k) {
    if (!node[k]) return;
    if (Array.isArray(node[k])) node[k].forEach(function(c){ walk_explain_tree(c.table || c, visitor); });
    else walk_explain_tree(node[k], visitor);
  });
}

function parse_explain(json_str) {
  try {
    var plan = JSON.parse(json_str), out = [];
    walk_explain_tree(plan.query_block || plan, function(node) {
      if (node.table_name)             out.push(t('表=', 'table=')   + node.table_name);
      if (node.access_type) {
        out.push(t('访问=', 'access=') + node.access_type);
        if (node.access_type === 'ALL') out.push(t('⚠全表扫描', '⚠full table scan'));
      }
      if (node.rows_examined_per_scan) out.push(t('扫描行≈', 'rows≈') + node.rows_examined_per_scan);
      if (node.using_filesort)  out.push(t('⚠需filesort', '⚠filesort required'));
      if (node.using_temporary) out.push(t('⚠临时表',     '⚠temp table'));
      if (node.key) out.push(t('索引=', 'key=') + node.key);
    });
    return out.length ? out.join(' ') : json_str.substring(0, 150);
  } catch(e) { return json_str.substring(0, 150); }
}

// Numeric row estimate extracted from the same EXPLAIN JSON tree.
function sum_explain_rows(json_str) {
  try {
    var plan = JSON.parse(json_str);
    var found = false, max_single = 0;
    walk_explain_tree(plan.query_block || plan, function(node) {
      if (node.rows_examined_per_scan) {
        var n = Number(node.rows_examined_per_scan) || 0;
        if (n > max_single) max_single = n;
        found = true;
      }
    });
    return found ? max_single : null;
  } catch (e) { return null; }
}

// EXPLAIN-based best-effort row estimate for a write statement.
function estimate_affected_rows(sql) {
  try {
    var ex = query("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !Array.isArray(ex) || !ex.length) return null;
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'];
    if (!raw) return null;
    return sum_explain_rows(String(raw));
  } catch (e) { return null; }
}

function tool_time_budget(spec) {
  var base = get_operator_policy();
  var fallback = Number(spec.timeout_ms) || (spec.category === 'ml' ? 120000 : 30000);
  var cap = _combine_min(base, 'tool_timeout_ms', cfg('tool_timeout_ms', fallback), 1);
  if (spec.category === 'ml')
    cap = Math.min(cap, _combine_min(base, 'ml_tool_timeout_ms', cfg('ml_tool_timeout_ms', fallback), 1));
  if (Number(spec.timeout_ms) > 0) cap = Math.min(cap, Number(spec.timeout_ms));
  return cap;
}

// Registry-driven dispatch.
function execute_tool(tool, args, db) {
  var spec = get_tool_spec(tool);
  if (!spec || !spec.handler) {
    return { ok: false,
             response: t('错误：未知工具 "', 'Error: unknown tool "') + tool + '"',
             error: 'unknown_tool' };
  }

  if (A.runtime_active && tool !== 'rollback_tx' && runtime_sql_timeout() <= 0)
    return { ok: false, error: 'agent_deadline_exceeded', response: 'Agent deadline exceeded.' };
  if (spec.write || spec.ddl || tool === 'begin_tx') A.execution_effect = true;

  var span_started = Date.now(), parent_span = A.current_span_id;
  A.current_span_id = gen_query_id() + gen_query_id();
  var saved_parent = A.parent_span_id;
  A.parent_span_id = parent_span || '';
  var saved_deadline = A.tool_deadline_at;
  var cap = tool_time_budget(spec);
  A.tool_deadline_at = Math.min(saved_deadline || Infinity, Date.now() + cap);
  try {
  A.tool_calls = Number(A.tool_calls || 0) + 1;
  var chat_opt = get_chat_options();
  var ctx = {
    tool:            String(tool),
    db:              db,
    chat_opt:        chat_opt,
    policy:          get_review_policy(chat_opt),
    conversation_id: A.conversation_id
  };

  var pre = run_pre_tool_hooks(ctx.tool, args || {}, ctx);
  if (pre.deny) return (result = tool_result_envelope(run_post_tool_hooks(ctx.tool, args || {}, ctx, pre.result)));

  /* A hook can rewrite args in place, so validate the final shape again. */
  var effective_args = pre.args || {};
  var validation_error = validate_tool_call(
    { tool: ctx.tool, args: effective_args }, ctx.policy);
  if (validation_error) {
    return (result = tool_result_envelope(run_post_tool_hooks(ctx.tool, effective_args, ctx,
      { ok: false, error: 'hook_args_invalid', response: validation_error })));
  }
  var admission = spec.trusted ? null : trusted_tool_admission(ctx.tool,spec);
  if (admission) return (result = tool_result_envelope(admission));
  var required_tx = null;
  if (spec.requiresTx) {
    if (spec.transactional === false)
      return (result = tool_result_envelope({ok:false,error:'transaction_contract_invalid',response:'requiresTx conflicts with transactional:false.'}));
    required_tx = get_tx_context();
    if (!required_tx.known || !required_tx.active ||
        [TX_OWNER_AGENT,TX_OWNER_CALLER].indexOf(required_tx.owner) < 0)
      return (result = tool_result_envelope({ok:false,error:'transaction_required',response:'This tool requires a known active transaction.'}));
  }
  if (A.approved_args && A.approved_tool === ctx.tool && JSON.stringify(effective_args) !== A.approved_args)
    return (result = tool_result_envelope({ ok: false, error: 'approval_arguments_changed', response: 'Hook changed approved arguments; review again.' }));
  if (spec.category !== 'tx') {
    // Share the contract built after hooks with the handler for this call only.
    var policy_step = build_review_step({ tool: ctx.tool }, effective_args, db, false, ctx);
    var decision = evaluate_step_policy(policy_step, ctx.policy);
    if (decision.action === 'reject' || (decision.action === 'pause' &&
        !(A.approved_tool === ctx.tool && A.approved_args)))
      return (result = tool_result_envelope({ ok: false, error: decision.action === 'pause' ? 'approval_required' : decision.reason,
               response: decision.message || 'This tool requires explicit review.' }));
    if (ctx.trusted_plan && A.approved_sql !== ctx.trusted_plan.preview)
      return (result = tool_result_envelope({ok:false,error:'trusted_template_changed',response:'Trusted template, target or version changed. Request a fresh approval.'}));
  }
  var result = spec.handler(effective_args, ctx);
  result = run_post_tool_hooks(ctx.tool, effective_args, ctx, result);
  if (required_tx && result && result.ok) {
    var after_tx = get_tx_context();
    if (!after_tx.known || !after_tx.active || after_tx.owner !== required_tx.owner ||
        (required_tx.event_id && after_tx.event_id !== required_tx.event_id))
      result = {ok:false,error:'transaction_contract_violated',response:'Tool changed or closed its required transaction.'};
  }
  return (result = tool_result_envelope(result));
  } catch (tool_error) {
    return (result = tool_result_envelope({ ok: false, error: 'tool_exception', response: String(tool_error) }));
  } finally {
    // Writes may create a knowledge table (e.g. ML_EMBED_TABLE) in this turn.
    if ((spec.write || spec.ddl) && !tool_is_memory_scoped(tool)) A.rag_discovery = null;
    A.tool_deadline_at = saved_deadline;
    mem_log_audit('tool', 'span', String(tool), JSON.stringify({
      trace_id: current_turn_id(), span_id: A.current_span_id, parent_span_id: parent_span || '',
      route: A.route || 'tool_loop', ok: !!(result && result.ok),
      plan_id:A.current_plan_id || '',step_no:Number(A.current_step_id || 0),
      execution_id:A.execution ? A.execution.id : '',
      contract_hash:result && result.contract_hash || '',error:result && result.error || ''
    }), 1, Date.now() - span_started, '');
    A.current_span_id = parent_span;
    A.parent_span_id = saved_parent;
  }
}

function get_operator_policy() {
  if (A.operator_policy) return A.operator_policy;
  var base = null;

  try {
    var trows = query("SELECT policy_key, policy_value FROM mysql.agent_policy");
    if (Array.isArray(trows)) {
      base = {};
      for (var i = 0; i < trows.length; i++) {
        var k = String(trows[i].policy_key || '').trim();
        if (k) base[k] = String(trows[i].policy_value === null ? '' : trows[i].policy_value).trim();
      }
    }
  } catch (e) { base = null; }

  if (base === null) base = _restrictive_policy();
  A.operator_policy = base;
  return base;
}

function _restrictive_policy() {
  return {
    auto_execute_read_only:        'false',
    require_approval_for_write:    'true',
    require_approval_for_ddl:      'true',
    require_approval_for_risky_sql:'true',
    allow_destructive_ddl:         'false',
    allow_account_ddl:             'false',
    allow_code_ddl:                'false',
    allow_instance_ddl:            'false',
    allow_system_schema_writes:    'false',

    allow_endpoint_override:       'false'
  };
}

function _pol_bool(v) {
  var x = String(v === undefined || v === null ? '' : v).toLowerCase();
  return (x === '1' || x === 'true' || x === 'on' || x === 'yes');
}
function _pol_has(base, key) {
  return Object.prototype.hasOwnProperty.call(base, key);
}

// Combine one boolean option.
function _combine_bool(base, key, session_value, safe_value) {
  if (session_value === safe_value) return safe_value;
  if (_pol_has(base, key) && _pol_bool(base[key]) === safe_value) return safe_value;
  return session_value;
}

/* Combine one numeric ceiling: the smaller number is the tighter one. */
function _combine_min(base, key, session_value, floor_value) {
  var v = Number(session_value);
  if (!isFinite(v)) v = floor_value;
  if (_pol_has(base, key)) {
    var b = Number(base[key]);
    if (isFinite(b) && b > 0 && b < v) v = b;
  }
  return Math.max(floor_value, v);
}

function get_review_policy(chat_opt) {
  var cfg = (chat_opt && typeof chat_opt === 'object') ? chat_opt : {};
  var base = get_operator_policy();
  var mode = String(cfg.review_mode || '').toLowerCase();
  var session_review = (mode === 'review' || mode === 'true' || mode === '1');

  var effective_review = session_review ||
      (_pol_has(base, 'review_mode') &&
       String(base.review_mode).toLowerCase() === 'review');
  return {
    review_mode: effective_review ? 'review' : 'off',
    auto_execute_read_only:
      _combine_bool(base, 'auto_execute_read_only', cfg.auto_execute_read_only !== false, false),
    require_approval_for_write:
      _combine_bool(base, 'require_approval_for_write', cfg.require_approval_for_write !== false, true),
    require_approval_for_ddl:
      _combine_bool(base, 'require_approval_for_ddl', cfg.require_approval_for_ddl !== false, true),

    allow_destructive_ddl:
      _combine_bool(base, 'allow_destructive_ddl', cfg.allow_destructive_ddl === true, false),

    allow_account_ddl:
      _combine_bool(base, 'allow_account_ddl', cfg.allow_account_ddl === true, false),
    allow_code_ddl:
      _combine_bool(base, 'allow_code_ddl', cfg.allow_code_ddl === true, false),
    allow_instance_ddl:
      _combine_bool(base, 'allow_instance_ddl', cfg.allow_instance_ddl === true, false),

    allow_system_schema_writes:
      _combine_bool(base, 'allow_system_schema_writes',
                    cfg.allow_system_schema_writes === true, false),
    require_approval_for_risky_sql:
      _combine_bool(base, 'require_approval_for_risky_sql',
                    cfg.require_approval_for_risky_sql !== false, true),
    max_pending_steps: _combine_min(base, 'max_pending_steps',
                                    Number(cfg.max_pending_steps || 3), 1),

    review_plan_ttl_minutes: _combine_min(base, 'review_plan_ttl_minutes',
                                          Number(cfg.review_plan_ttl_minutes || 30), 1),
    // How long a step may sit in `executing` before it is treated as interrupted.
    review_step_stuck_ttl_minutes:
      _combine_min(base, 'review_step_stuck_ttl_minutes',
                   REVIEW_STEP_STUCK_TTL_DEFAULT, 5)
  };
}

function normalize_review_command(text) {
  var s = String(text || '').trim().toLowerCase();
  if (!s) return null;
  if (s === 'approve' || s === 'yes' || s === 'y' || s.indexOf('approve') === 0)
    return 'approve';
  if (s === 'reject' || s === 'no' || s === 'n' || s.indexOf('reject') === 0)
    return 'reject';
  if (s.indexOf('modify:') === 0 || s.indexOf('modify ') === 0 ||
      s.indexOf('修改:') === 0 || s.indexOf('修改 ') === 0)
    return 'modify';
  return null;
}

function parse_review_approval(text) {
  var m = String(text || '').trim().match(/^approve\s+([A-Za-z0-9_-]+)\s+(\d+)\s+([a-f0-9]{64})$/i);
  return m ? { plan_id: m[1], step_id: Number(m[2]), digest: m[3].toLowerCase() } : null;
}

function parse_review_modify(text) {
  var s = String(text || '').trim();
  // [\s\S] rather than .
  var m = s.match(/^(?:modify|修改)\s*[:：]?\s*([\s\S]+)$/i);
  return m && m[1] ? m[1].trim() : null;
}

function infer_affected_tables(sql) {
  var tables = [];
  var seen = {};
  var patterns = [
    /\bfrom\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bjoin\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\binto\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bupdate\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bdelete\s+from\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\btable\s+`?([a-zA-Z0-9_$.]+)`?/gi
  ];
  for (var i = 0; i < patterns.length; i++) {
    var re = patterns[i];
    var m;
    while ((m = re.exec(sql))) {
      var table = String(m[1] || '').replace(/^`|`$/g, '').replace(/^([^.]+)\./, '');
      if (!table || seen[table]) continue;
      seen[table] = true;
      tables.push(table);
    }
  }
  return tables;
}

var RISK_RANK = { low: 0, medium: 1, high: 2 };

function risk_max(a, b) {
  var ra = RISK_RANK[a] === undefined ? 0 : RISK_RANK[a];
  var rb = RISK_RANK[b] === undefined ? 0 : RISK_RANK[b];
  return ra >= rb ? (a || 'low') : (b || 'low');
}

function infer_step_risk(sql, tool) {
  var stmt = classify_statement(sql);
  if (stmt.risk !== 'low') return stmt.risk;
  if (tool === 'update_data') return 'medium';
  return 'low';
}

function ml_display_sql(tool_name, args) {
  var h = String(args.model_handle || '');
  var at_h = h ? '@' + h : '@model';
  var tbl = String(args.table_name || '');
  var tgt = String(args.target_column || '');
  var opt_json = args.options ? JSON.stringify(args.options) : null;

  switch (tool_name) {
    case 'ml_train':
      return "CALL sys.ML_TRAIN('" + tbl + "', '" + tgt + "', " +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ", " + at_h + ")";

    case 'ml_predict_row':
      return "SELECT sys.ML_PREDICT_ROW(" +
             "CAST('" + esc(JSON.stringify(args.data || {})) + "' AS JSON), " +
             at_h + ", " + (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ") AS prediction";

    case 'ml_predict_table':
      return "CALL sys.ML_PREDICT_TABLE('" + tbl + "', " + at_h + ", '" +
             esc(String(args.output_table || '')) + "', " +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ")";

    case 'ml_explain':
      return "CALL sys.ML_EXPLAIN('" + tbl + "', '" + tgt + "', " + at_h + ", " +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ")";

    case 'ml_explain_row':
      return "SELECT sys.ML_EXPLAIN_ROW(" +
             "CAST('" + esc(JSON.stringify(args.data || {})) + "' AS JSON), " +
             at_h + ", " + (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ") AS explanation";

    case 'ml_explain_table':
      return "CALL sys.ML_EXPLAIN_TABLE('" + tbl + "', " + at_h + ", '" +
             esc(String(args.output_table || '')) + "', " +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ")";

    case 'ml_score':
      return "CALL sys.ML_SCORE('" + tbl + "', '" + tgt + "', " + at_h + ", '" +
             esc(String(args.metric || 'balanced_accuracy')) + "', @_ml_score_val, " +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ")";

    case 'ml_model_export':
      return "CALL sys.ML_MODEL_EXPORT(" + at_h + ", '" +
             esc(String(args.output_table || 'auto')) + "')";

    case 'ml_model_import':
      return "CALL sys.ML_MODEL_IMPORT(" +
             (args.model_content ? "(SELECT MODEL_OBJECT FROM " + esc(String(args.model_content)) + " LIMIT 1)" : 'NULL') +
             ", CAST('" + esc(JSON.stringify({task: args.task || 'classification'})) + "' AS JSON), " + at_h + ")";

    case 'ml_embed_table':
    case 'ml_generate_table':
    case 'ml_rag_table':
      return 'CALL sys.' + tool_name.toUpperCase() + "('" +
             esc(String(args.input_column || '')) + "','" +
             esc(String(args.output_column || '')) + "'," +
             (opt_json ? "'" + esc(opt_json) + "'" : 'NULL') + ')';

    default:
      return 'CALL sys.' + tool_name.toUpperCase() + '(' +
             Object.keys(args || {}).map(function(k) {
               return k + '=' + JSON.stringify(args[k]);
             }).join(', ') + ')';
  }
}

function build_review_step(tool_obj, args, db, compute_estimate, execution_ctx) {
  var tool_name  = tool_obj && tool_obj.tool ? tool_obj.tool : 'unknown';
  var step_spec  = get_tool_spec(tool_name);
  var ml_meta    = tool_review_meta(tool_name);
  var sql = '', trusted_error = null, business_plan = null;
  if (step_spec && step_spec.trusted) {
    try { business_plan = trusted_plan(step_spec,args || {}); sql = business_plan.preview; }
    catch (e) { trusted_error = trusted_failure(e); }
  } else if (tool_obj && tool_obj.tool === 'plan_sql') {
    var steps = Array.isArray(args && args.steps) ? args.steps : [];
    sql = String((steps[0] && steps[0].sql) || '');
  } else if (tool_obj && tool_obj.tool === 'list_tables') {

    sql = "SHOW TABLES" + ((args && args.keyword) ? " LIKE '%" + args.keyword + "%'" : '');
  } else if (tool_obj && tool_obj.tool === 'describe_table') {
    var tn_disp = (args && args.table_name) ? args.table_name :
                  (Array.isArray(args && args.table_names) ? args.table_names.join(',') : '');
    sql = "DESC " + tn_disp;
  } else if (step_spec && typeof step_spec.displaySql === 'function') {

    sql = step_spec.displaySql(args || {});
  } else if (ml_meta) {
    /* ML procedures carry no user-authored SQL, so synthesize a CALL preview. */
    sql = ml_display_sql(tool_name, args || {});
  } else {
    sql = String((args && args.sql) || '');
  }
  if (!business_plan) sql = replace_ph(sql, db);
  var stmt4 = classify_statement(sql);

  var affected = ml_meta ? [] : infer_affected_tables(sql);
  if (ml_meta) {
    var tbl = (args && args[ml_meta.table_arg]) ||
              (ml_meta.fallback_arg && args && args[ml_meta.fallback_arg]);
    if (tbl) affected = [String(tbl).replace(/^([^.]+)\./, '')];
  }

  var step = {
    id: 1,
    execution_db: String(db || ''),
    tool: tool_name,
    sql: sql,
    args: args || {},
    affected_tables: affected,

    writes: ml_meta ? true : (stmt4.is_write || !!(step_spec && step_spec.write)),
    ddl: stmt4.is_ddl || !!(step_spec && step_spec.ddl),
    risk: ml_meta ? ml_meta.risk
                  : risk_max(stmt4.risk, (step_spec && step_spec.risk) || 'low'),

    transactional: ml_meta ? (ml_meta.transactional !== false)
                           : ((step_spec && step_spec.transactional === false)
                                ? false : !stmt4.is_ddl),
    estimated_rows: ml_meta ? t('不适用（ML 存储过程）', 'n/a (ML procedure)') : 'unknown',

    scope: step_spec ? step_spec.scope : 'db',
    requiresTx: !!(step_spec && step_spec.requiresTx),
    trusted_error: trusted_error,
    thought: tool_obj && tool_obj.thought ? tool_obj.thought : ''
  };

  if (business_plan) {
    step.affected_tables = [business_plan.table];
    if (execution_ctx) execution_ctx.trusted_plan = business_plan;
  }
  var has_display_sql = !!(step_spec && typeof step_spec.displaySql === 'function');
  if (compute_estimate && !ml_meta && !has_display_sql &&
      stmt4.is_write && !stmt4.is_ddl) {
    var est = estimate_affected_rows(sql);
    step.estimated_rows = (est === null)
      ? 'unknown'
      : t('≈' + est + ' 行（EXPLAIN 估算，非精确值）', '≈' + est + ' rows (EXPLAIN estimate, not exact)');
  }

  return step;
}

function evaluate_step_policy(step, policy) {
  var admission = step && trusted_tool_admission(step.tool,get_tool_spec(step.tool));
  if (admission || (step && step.trusted_error)) {
    var denied = admission || step.trusted_error;
    return {action:'reject',reason:denied.error,message:denied.response};
  }
  if (step && get_tool_spec(step.tool) && get_tool_spec(step.tool).trusted)
    return {action:'pause',reason:'trusted_approval_required',message:'Fixed business operations always require explicit approval.'};
  if (step && step.sql && ['query_db','update_data','run_ddl','plan_sql'].indexOf(step.tool) >= 0 &&
      classify_statement(step.sql).external_side_effects)
    return { action: 'reject', reason: 'sql_side_effect_not_allowed',
             message: 'File I/O, executable comments and raw procedural SQL require a dedicated reviewed tool.' };
  var is_ddl   = !!(step && step.ddl);
  var is_write = !!(step && step.writes);
  var is_risky = !!(step && step.risk === 'high');

  // Destructive DDL is refused regardless of review_mode unless an operator has opted in.
  if (is_ddl && step.sql) {
    var act_denied = check_ddl_action_policy(step.sql, policy);
    if (act_denied)
      return { action: 'reject', reason: act_denied.reason, message: act_denied.message };
  }

  if (is_ddl && step.sql && is_destructive_ddl(step.sql) &&
      !(policy && policy.allow_destructive_ddl))
    return { action: 'reject', reason: 'destructive_ddl_not_allowed',
             message: t('该语句会删除数据或对象，agent 默认不执行；如确需执行，' +
                        '请在 @chat_options 中设置 allow_destructive_ddl=true。',
                        'This statement drops data or objects. The agent will not run it ' +
                        'unless allow_destructive_ddl=true is set in @chat_options.') };

  if (!policy) return { action: 'execute', reason: 'review_disabled' };

  /* remember_fact writes only mysql.agent_*, so the write gate does not apply. */
  if (is_write && !is_ddl && !is_risky && step && tool_is_memory_scoped(step.tool))
    return { action: 'execute', reason: 'memory_scoped_write' };

  // These three are NOT gated on review_mode.
  if (is_ddl && policy.require_approval_for_ddl)
    return { action: 'pause', reason: 'ddl_requires_approval' };
  if (is_write && policy.require_approval_for_write)
    return { action: 'pause', reason: 'write_requires_approval' };
  if (is_risky && policy.require_approval_for_risky_sql)
    return { action: 'pause', reason: 'risky_sql_requires_approval' };

  if (policy.review_mode !== 'review')
    return { action: 'execute', reason: 'review_disabled' };

  // Read-only fast path — explicitly gated on !is_write.
  if (!is_write && !is_ddl && policy.auto_execute_read_only)
    return { action: 'execute', reason: 'safe_read_only' };

  if (is_write || is_ddl || is_risky)
    return { action: 'execute', reason: 'approval_explicitly_disabled_by_policy' };

  return { action: 'pause', reason: 'review_mode' };
}

var REVIEW_DIGEST_SQL = "SHA2(CAST(JSON_ARRAY(tool,COALESCE(sql_text,'')," +
  "CAST(COALESCE(args_json,'{}') AS JSON),writes,ddl,COALESCE(transactional,1),execution_db) AS CHAR),256)";

function review_step_digest(step) {
  var rows = query_checked("SELECT SHA2(CAST(JSON_ARRAY('" + esc(step.tool || '') + "','" +
    esc(step.sql || '') + "',CAST('" + esc(JSON.stringify(step.args || {})) + "' AS JSON)," +
    Number(!!step.writes) + "," + Number(!!step.ddl) + "," + Number(step.transactional !== false) +
    ",'" + esc(step.execution_db || '') + "') AS CHAR),256) AS digest");
  if (!rows || !rows[0] || !rows[0].digest) throw new Error('review_digest_unavailable');
  return String(rows[0].digest);
}

function render_review_prompt(plan, step, policy) {
  var trusted_guard = trusted_review_guard(step,step.execution_db);
  if (trusted_guard) return '['+trusted_guard.error+'] '+trusted_guard.response;
  var digest = review_step_digest(step);
  var saved = query_checked("UPDATE mysql.agent_review_plan_step SET approval_digest='" + digest +
    "' WHERE plan_id='" + esc(plan.plan_id) + "' AND step_no=" + Number(step.id) +
    " AND status IN ('pending','awaiting_approval','failed','error') AND " + REVIEW_DIGEST_SQL + "='" + digest + "'");
  if (!saved || (Number(saved.affected_rows) !== 1 && step.approval_digest !== digest))
    return 'Review content changed; refresh the plan before approving.';
  step.approval_digest = digest;

  var lines = [];
  lines.push(t('【审批中断】请确认下一步执行：', '[Approval pause] Please review the next step:'));
  lines.push(t('计划 ID：', 'Plan ID: ') + (plan && plan.plan_id ? plan.plan_id : ''));
  lines.push(t('步骤：', 'Step: ') + (step && step.id ? step.id : 1) + '/' + ((plan && plan.total_steps) ? plan.total_steps : 1));
  lines.push(t('工具：', 'Tool: ') + (step && step.tool ? step.tool : 'unknown'));
  lines.push('Database: ' + String(step.execution_db || '') + ' | SHA-256: ' + digest.substring(0, 12));
  lines.push(t('SQL：', 'SQL: ') + (step && step.sql ? step.sql : ''));
  lines.push(t('影响表：', 'Affected tables: ') + ((step && step.affected_tables && step.affected_tables.length) ? step.affected_tables.join(', ') : t('未知', 'unknown')));
  lines.push(t('预计影响行数：', 'Estimated rows: ') + (step && step.estimated_rows ? step.estimated_rows : 'unknown'));
  lines.push(t('会写入/修改结构：', 'Will write / modify schema: ') + ((step && (step.writes || step.ddl)) ? t('是', 'yes') : t('否', 'no')));
  lines.push(t('风险等级：', 'Risk: ') + (step && step.risk ? step.risk : 'low'));
  if (plan && plan.description) lines.push(t('计划描述：', 'Plan description: ') + plan.description);
  lines.push('');
  lines.push('Approve ' + plan.plan_id + ' ' + step.id + ' ' + digest);
  lines.push(t('或回复 Reject / Modify:SQL', 'Or reply Reject / Modify:SQL'));
  return lines.join('\n');
}

function build_review_steps(tool_obj, args, db) {
  if (!tool_obj || !tool_obj.tool) return [];
  if (tool_obj.tool === 'plan_sql') {
    var raw_steps = Array.isArray(args && args.steps) ? args.steps : [];
    var steps = [];
    for (var i = 0; i < raw_steps.length; i++) {
      var step_args = raw_steps[i] || {};

      var step_sql  = replace_ph(String(step_args.sql || ''), db);
      var step_stmt = classify_statement(step_sql);
      var step_tool = step_stmt.is_write ? 'update_data' : 'query_db';
      var step = build_review_step({ tool: step_tool, thought: step_args.desc || '' }, step_args, db, true);
      step.id = i + 1;
      step.description = step_args.desc || (t('步骤', 'Step') + ' ' + (i + 1));
      steps.push(step);
    }
    return steps;
  }
  var step = build_review_step(tool_obj, args, db, true);
  step.id = 1;
  step.description = tool_obj.thought || '';
  return [step];
}

function save_review_plan(plan) {
  // Creation/persistence is append-oriented.
  if (!plan || !plan.plan_id || !plan.conversation_id) return false;
  var plan_id = plan.plan_id;
  var conv_id = plan.conversation_id;
  var status = plan.status || 'awaiting_approval';
  var current_step_index = Number(plan.current_step_index || 0);
  var total_steps = Number(plan.steps && plan.steps.length ? plan.steps.length : 0);
  var description = String(plan.description || '');
  var plan_json = JSON.stringify({ plan_id: plan_id, current_step_index: current_step_index, total_steps: total_steps });

  try {
    // Steps first, plan row last.
    for (var i = 0; i < (plan.steps || []).length; i++) {
      var step = plan.steps[i];
      exec_sql(
        "INSERT IGNORE INTO mysql.agent_review_plan_step(" +
        "plan_id,step_no,tool,sql_text,args_json,affected_tables,writes,ddl,risk,estimated_rows,status,result_preview,error_text,transactional,execution_db) VALUES (" +
        "'" + esc(plan_id) + "'," + Number(step.id || (i + 1)) + "," +
        "'" + esc(step.tool || '') + "'," +
        "'" + esc(step.sql || '') + "'," +
        "'" + esc(JSON.stringify(step.args || {})) + "'," +
        "'" + esc((step.affected_tables || []).join(',')) + "'," +
        Number(step.writes ? 1 : 0) + "," +
        Number(step.ddl ? 1 : 0) + "," +
        "'" + esc(step.risk || '') + "'," +
        "'" + esc(step.estimated_rows || '') + "'," +
        "'" + esc(step.status || 'pending') + "'," +
        "'" + esc(step.result_preview || '') + "'," +
        "'" + esc(step.error_text || '') + "'," +
        Number(step.transactional !== false ? 1 : 0) + ",'" + esc(step.execution_db || A.current_db || '') + "')"
      );
    }

    // At most one plan per conversation may be awaiting approval.
    if (status === 'awaiting_approval') {
      exec_sql(
        "UPDATE mysql.agent_review_plan SET status='superseded', updated_at=CURRENT_TIMESTAMP" +
        " WHERE conversation_id='" + esc(conv_id) + "'" +
        "   AND status='awaiting_approval' AND plan_id<>'" + esc(plan_id) + "'");
    }

    exec_sql(
      "INSERT INTO mysql.agent_review_plan(plan_id,conversation_id,status,current_step_index,total_steps,description,plan_json) VALUES ('" +
        esc(plan_id) + "','" + esc(conv_id) + "','" + esc(status) + "'," +
        current_step_index + "," + total_steps + ", '" + esc(description) + "', '" + esc(plan_json) + "') " +
      "ON DUPLICATE KEY UPDATE status=VALUES(status), current_step_index=VALUES(current_step_index), " +
      "total_steps=VALUES(total_steps), description=VALUES(description), plan_json=VALUES(plan_json), " +
      "updated_at=CURRENT_TIMESTAMP"
    );
    return true;
  } catch (e) {
    return false;
  }
}

function update_review_plan_cursor(plan) {
  if (!plan || !plan.plan_id) return false;
  var idx = Number(plan.current_step_index || 0);
  var total = Number(plan.total_steps || (plan.steps ? plan.steps.length : 0));
  var plan_json = JSON.stringify({ plan_id: plan.plan_id, current_step_index: idx, total_steps: total });
  try {
    var raw = exec_sql(
      "UPDATE mysql.agent_review_plan SET current_step_index=" + idx +
      ", total_steps=" + total +
      ", plan_json='" + esc(plan_json) + "', updated_at=CURRENT_TIMESTAMP " +
      "WHERE plan_id='" + esc(plan.plan_id) + "' AND status='awaiting_approval'"
    );
    return !!(raw && Number(raw.affected_rows) === 1);
  } catch (e) { return false; }
}

function update_review_step_definition(plan_id, step) {
  if (!plan_id || !step || !step.id) return false;
  try {
    var raw = exec_sql(
      "UPDATE mysql.agent_review_plan_step SET " +
      "tool='" + esc(step.tool || '') + "', " +
      "sql_text='" + esc(step.sql || '') + "', " +
      "args_json='" + esc(JSON.stringify(step.args || {})) + "', " +
      "execution_db='" + esc(step.execution_db || '') + "', approval_digest=NULL, " +
      "affected_tables='" + esc((step.affected_tables || []).join(',')) + "', " +
      "writes=" + Number(step.writes ? 1 : 0) + ", " +
      "ddl=" + Number(step.ddl ? 1 : 0) + ", " +
      "risk='" + esc(step.risk || '') + "', " +
      "estimated_rows='" + esc(step.estimated_rows || '') + "', " +
      "transactional=" + Number(step.transactional !== false ? 1 : 0) + " " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step.id) +
      " AND status='pending'"
    );
    return !!(raw && Number(raw.affected_rows) === 1);
  } catch (e) { return false; }
}

function load_review_state(conv_id) {
  if (!conv_id) return null;
  try {
    var rows = query(
      "SELECT plan_id, conversation_id, status, current_step_index, total_steps, description, plan_json " +
      "FROM mysql.agent_review_plan " +
      "WHERE conversation_id='" + esc(conv_id) + "' AND status='awaiting_approval' " +

      "ORDER BY created_at DESC, plan_id DESC LIMIT 1"
    );
    if (!Array.isArray(rows) || !rows.length || !rows[0].plan_id) return null;
    var plan = rows[0];
    var step_rows = query(
      "SELECT step_no, tool, sql_text, args_json, affected_tables, writes, ddl, risk, estimated_rows, status, result_preview, error_text, transactional, execution_db, approval_digest " +
      "FROM mysql.agent_review_plan_step " +
      "WHERE plan_id='" + esc(plan.plan_id) + "' ORDER BY step_no"
    );
    var steps = [];
    if (Array.isArray(step_rows)) {
      for (var i = 0; i < step_rows.length; i++) {
        var sr = step_rows[i];
        var ml_meta = tool_review_meta(sr.tool);
        var sr_spec = get_tool_spec(sr.tool);
        steps.push({
          id: Number(sr.step_no),
          tool: sr.tool,
          execution_db: sr.execution_db,
          approval_digest: sr.approval_digest,
          sql: sr.sql_text,
          args: try_parse_json(sr.args_json) || {},
          affected_tables: String(sr.affected_tables || '').split(',').filter(function(x){ return x; }),
          writes: !!Number(sr.writes),
          ddl: !!Number(sr.ddl),
          risk: sr.risk,
          estimated_rows: sr.estimated_rows,
          status: sr.status,
          result_preview: sr.result_preview,
          error_text: sr.error_text,
          transactional: (typeof sr.transactional !== 'undefined' && sr.transactional !== null)
            ? !!Number(sr.transactional)
            : (ml_meta ? (ml_meta.transactional !== false) : true),

          scope: sr_spec ? sr_spec.scope : 'db',
          requiresTx: !!(sr_spec && sr_spec.requiresTx)
        });
      }
    }
    var loaded = {
      plan_id: plan.plan_id,
      conversation_id: plan.conversation_id,
      status: plan.status,
      current_step_index: Number(plan.current_step_index || 0),
      total_steps: Number(plan.total_steps || steps.length),
      description: plan.description,
      steps: steps
    };

    while (loaded.current_step_index < loaded.steps.length &&
           loaded.steps[loaded.current_step_index].status === 'completed') {
      loaded.current_step_index++;
    }
    if (loaded.current_step_index >= loaded.total_steps) {
      clear_review_state(conv_id, loaded.plan_id, 'completed');
      return null;
    }
    if (loaded.current_step_index !== Number(plan.current_step_index || 0)) {
      update_review_plan_cursor(loaded);
    }
    return loaded;
  } catch (e) {
    return null;
  }
}

function try_parse_json(text) {
  if (!text || typeof text !== 'string') return null;
  try { return JSON.parse(text); } catch (e) { return null; }
}

function get_pending_review_step(plan) {
  if (!plan || !Array.isArray(plan.steps) || plan.steps.length === 0) return null;
  var idx = Number(plan.current_step_index || 0);
  if (idx < 0 || idx >= plan.steps.length) return null;
  return plan.steps[idx];
}

function clear_review_state(conv_id, plan_id, final_status) {
  if (!conv_id || !plan_id) return;
  final_status = final_status || 'completed';
  try {
    exec_sql(
      "UPDATE mysql.agent_review_plan SET status='" + esc(final_status) + "', updated_at=CURRENT_TIMESTAMP " +
      "WHERE plan_id='" + esc(plan_id) + "'"
    );
  } catch (e) {}
}

// Auto-cancel awaiting_approval plans that have sat untouched past review_plan_ttl_minutes.
function cleanup_expired_review_plans(conv_id, ttl_minutes) {
  if (!conv_id) return;
  ttl_minutes = Math.max(1, Number(ttl_minutes) || 30);
  try {
    var expired = query(
      "SELECT plan_id FROM mysql.agent_review_plan " +
      "WHERE conversation_id='" + esc(conv_id) + "' AND status='awaiting_approval' " +
      "AND updated_at < DATE_SUB(NOW(), INTERVAL " + ttl_minutes + " MINUTE)"
    );
    if (!Array.isArray(expired) || !expired.length) return;
    for (var i = 0; i < expired.length; i++) {
      clear_review_state(conv_id, expired[i].plan_id, 'expired');
      log_rollback_event(conv_id, expired[i].plan_id, 0, '', '',
                         t('审批计划超时未确认，自动取消', 'Approval plan timed out with no response and was auto-cancelled'));
    }
  } catch (e) {}
}

// The other way a plan gets stuck, and the one the sweep above cannot see.
function recover_stuck_review_steps(conv_id, ttl_minutes) {
  if (!conv_id) return 0;
  ttl_minutes = Math.max(1, Number(ttl_minutes) || 120);
  var recovered = 0;
  try {
    var stuck = query(
      "SELECT s.plan_id AS plan_id, s.step_no AS step_no, s.sql_text AS sql_text " +
      "FROM mysql.agent_review_plan_step s " +
      "JOIN mysql.agent_review_plan p ON p.plan_id = s.plan_id " +
      "WHERE p.conversation_id='" + esc(conv_id) + "' AND s.status='executing' " +
      "AND p.updated_at < DATE_SUB(NOW(), INTERVAL " + ttl_minutes + " MINUTE) " +
      "LIMIT 100"
    );
    if (!Array.isArray(stuck) || !stuck.length) return 0;
    for (var i = 0; i < stuck.length; i++) {

      var moved = false;
      try {
        var raw = exec_sql(
          "UPDATE mysql.agent_review_plan_step SET status='indeterminate', " +
          "error_text='interrupted while executing; outcome unknown' " +
          "WHERE plan_id='" + esc(stuck[i].plan_id) + "' AND step_no=" + Number(stuck[i].step_no) +
          " AND status='executing'");
        moved = !!(raw && Number(raw.affected_rows) === 1);
      } catch (e) { moved = false; }
      if (!moved) continue;
      recovered++;
      log_rollback_event(conv_id, stuck[i].plan_id, stuck[i].step_no,
                         String(stuck[i].sql_text || ''), 'interrupted_while_executing',
                         t('步骤在执行中被中断，执行结果未知（可能已提交，也可能已回滚）。' +
                           '已标记为 indeterminate 并停止该计划；请人工确认数据状态后再决定是否重试。',
                           'Step was interrupted while executing and its outcome is unknown (it may have ' +
                           'committed or rolled back). Marked indeterminate and the plan was stopped; ' +
                           'verify the data by hand before deciding whether to retry.'));
      clear_review_state(conv_id, stuck[i].plan_id, 'error');
    }
  } catch (e) {}
  return recovered;
}

var REVIEW_STEP_TRANSITIONS = {
  'pending':            ['executing', 'rejected', 'error', 'pending'],
  'awaiting_approval':  ['executing', 'rejected', 'error', 'pending'],
  'executing':          ['completed', 'failed', 'indeterminate'],
  'completed':          [],
  'failed':             ['executing', 'pending', 'rejected'],
  'rejected':           [],
  'error':              ['pending'],
  // Terminal, and deliberately has no way back.
  'indeterminate':      []
};

function claim_review_step(plan_id, step_no, from_status, to_status, digest) {

  var allowed = REVIEW_STEP_TRANSITIONS[from_status];
  if (!allowed || allowed.indexOf(to_status) === -1) {
    try {
      exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'claim_review_step REFUSED: " + esc(from_status) + " -> " + esc(to_status) + " not in REVIEW_STEP_TRANSITIONS', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  try {
    var raw = exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status='" + esc(from_status) + "'" +
      (digest ? " AND approval_digest='" + esc(digest) + "' AND " + REVIEW_DIGEST_SQL + "='" + esc(digest) + "'" : '')
    );
    /* A rejected UPDATE returns {error}, i.e. no affected_rows: not a lost CAS. */
    if (raw && raw.error) {
      audit_tx_failure('claim_review_step_failed', plan_id, raw.error);
      return false;
    }
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    var won = (raw && Number(raw.affected_rows) === 1);
    if (won) {
      // Give recover_stuck_review_steps() a clock.
      try {
        exec_sql("UPDATE mysql.agent_review_plan SET updated_at=NOW() " +
                     "WHERE plan_id='" + esc(plan_id) + "'");
      } catch (e2) {}
    }
    return won;
  } catch (e) { return false; }
}

function transition_review_step(plan_id, step_no, to_status, result_preview, error_text) {
  /* Reverse-lookup: find all statuses from which to_status is reachable */
  var valid_from = [];
  for (var s in REVIEW_STEP_TRANSITIONS) {
    if (!Object.prototype.hasOwnProperty.call(REVIEW_STEP_TRANSITIONS, s)) continue;
    if (REVIEW_STEP_TRANSITIONS[s].indexOf(to_status) !== -1) valid_from.push(s);
  }

  if (!valid_from.length) {
    try {
      exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'No valid FROM status for transition to " + esc(to_status) + "', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  /* Single atomic UPDATE: check AND write in one statement */
  try {
    var raw = exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "', " +
      "result_preview='" + esc(result_preview || '') + "', " +
      "error_text='" + esc(error_text || '') + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status IN ('" + valid_from.map(esc).join("','") + "')"
    );
    /* A rejected UPDATE is not a lost CAS race; do not file it as one. */
    if (raw && raw.error) {
      audit_tx_failure('transition_review_step_failed', plan_id, raw.error);
      return false;
    }
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    if (raw && Number(raw.affected_rows) === 1) return true;
  } catch (e) {}

  try {
    exec_sql(
      "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
      "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
      "'Atomic transition REFUSED to " + esc(to_status) + " (valid from: " + esc(valid_from.join(',')) + ")', 'state_machine')"
    );
  } catch (e) {}
  return false;
}

function append_review_history(conv_id, plan_id, step_no, action, user_cmd, comment, result_preview, rollback_flag) {
  if (!conv_id || !plan_id || !action) return;
  try {
    exec_sql(
      "INSERT INTO mysql.agent_review_history(conversation_id, plan_id, step_no, action, user_cmd, comment, result_preview, rollback_flag) VALUES (" +
      "'" + esc(conv_id) + "', '" + esc(plan_id) + "', " + Number(step_no || 0) + ", '" + esc(action) + "', '" + esc(user_cmd || '') + "', '" + esc(comment || '') + "', '" + esc(result_preview || '') + "', " +
      Number(rollback_flag ? 1 : 0) + ")"
    );
  } catch (e) {}
}

function log_rollback_event(conv_id, plan_id, step_no, sql_text, error, rollback_reason) {
  if (!conv_id || !plan_id) return;
  try {
    exec_sql(
      "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
      "'" + esc(conv_id) + "', '" + esc(plan_id) + "', " + Number(step_no || 0) + ", '" + esc(sql_text || '') + "', '" + esc(error || '') + "', '" + esc(rollback_reason || '') + "')"
    );
  } catch (e) {}
}

function review_state_tables_transactional() {
  try {
    var rows = query_checked(
      "SELECT COUNT(*) AS cnt FROM information_schema.TABLES " +
      "WHERE TABLE_SCHEMA='mysql' " +
      "AND TABLE_NAME IN ('agent_review_plan','agent_review_plan_step') " +
      "AND UPPER(COALESCE(ENGINE,''))='INNODB'"
    );
    return Array.isArray(rows) && rows.length && Number(rows[0].cnt) === 2;
  } catch (e) { return false; }
}

function execute_review_step(step, db) {
  var saved_step = A.current_step_id, outcome;
  A.current_step_id = step && step.id || 0;
  try { outcome = execute_review_step_body(step,db); return outcome; }
  finally {
    var spec = step && get_tool_spec(step.tool);
    if (spec && spec.trusted) mem_log_audit('trusted','outcome',step.tool,JSON.stringify({
      trace_id:current_turn_id(),execution_id:A.execution ? A.execution.id : '',
      plan_id:A.current_plan_id || '',step_no:A.current_step_id,
      ok:!!(outcome && outcome.ok),error:outcome && outcome.error || '',
      contract_hash:outcome && outcome.contract_hash || ''
    }),1,0,'');
    A.current_step_id = saved_step;
  }
}

function execute_review_step_body(step, db) {
  // execute_tool validates the current trusted contract after all pre-hooks;
  // the approval entry point separately checks it before claiming the step.
  var current_spec = step && get_tool_spec(step.tool);
  var trusted_shape = current_spec && current_spec.trusted && trusted_review_error(step,db);
  if (trusted_shape) return trusted_shape;
  if (current_spec && current_spec.requiresTx && step.transactional === false)
    return {ok:false,error:'transaction_contract_invalid',response:'A required transaction cannot be disabled by a review step.'};
  if (step && step.approval_digest && (review_step_digest(step) !== step.approval_digest ||
      String(db || '') !== String(step.execution_db || '')))
    return { ok: false, error: 'approval_content_changed', response: 'Approved content or database changed; review again.' };
  if (!step || !step.tool) {
    return { ok: false, response: t('审批步骤无效。', 'Invalid approval step.'), error: 'invalid_step' };
  }

  var tx_started_here = false;
  var tx_ctx_before = get_tx_context();

  if ((step.writes || step.ddl || (current_spec && current_spec.requiresTx)) && step.transactional !== false) {
    if (!tx_ctx_before.active) {
      if (!tx_ctx_before.known && tx_ctx_before.owner === TX_OWNER_UNKNOWN) {
        return { ok: false,
                 response: t('无法可靠判断当前事务状态，拒绝执行写步骤。',
                             'Unable to reliably determine transaction state; refusing write step.'),
                 error: 'transaction_state_unknown' };
      }
      var begin_res = execute_tool('begin_tx', {}, db);
      if (!begin_res.ok) return begin_res;
      tx_started_here = (begin_res.tx_owner === TX_OWNER_AGENT);
      tx_ctx_before = get_tx_context();
    }
  }

  var res;
  var approved_args = A.approved_args, approved_tool = A.approved_tool, approved_sql = A.approved_sql;
  A.approved_args = step.approval_digest ? JSON.stringify(step.args || {}) : null;
  A.approved_tool = step.tool;
  A.approved_sql = step.sql;
  try {
    res = execute_tool(step.tool, step.args || {}, db);
  } catch (e) {
    res = { ok: false, response: t('步骤执行异常：', 'Step execution exception: ') + String(e), error: 'exception' };
  } finally { A.approved_args = approved_args; A.approved_tool = approved_tool; A.approved_sql = approved_sql; }

  if (!res || !res.ok) {
    if (tx_started_here) execute_tool('rollback_tx', {}, db);
    return res || { ok: false, response: t('步骤执行失败。', 'Step execution failed.'), error: 'step_failed' };
  }

  if (tx_started_here) {

    var staged_review_completion = false;
    if (A.current_plan_id && step.id && (step.writes || step.ddl)) {
      if (!review_state_tables_transactional()) {
        execute_tool('rollback_tx', {}, db);
        return { ok: false,
                 response: t('审批状态表不是 InnoDB，无法保证 DML 与审批状态原子提交，已拒绝执行。',
                             'Review state tables are not InnoDB; atomic DML/review-state commit cannot be guaranteed. Execution refused.'),
                 error: 'review_state_not_transactional' };
      }
      if (!transition_review_step(A.current_plan_id, step.id, 'completed',
                                  res.response || '', '')) {
        execute_tool('rollback_tx', {}, db);
        return { ok: false,
                 response: t('无法在同一事务内持久化审批完成状态，DML 已回滚。',
                             'Could not stage review completion in the same transaction; DML was rolled back.'),
                 error: 'review_state_update_failed' };
      }
      staged_review_completion = true;
    }

    var commit_res = execute_tool('commit_tx', {}, db);
    if (!commit_res.ok) {
      execute_tool('rollback_tx', {}, db);
      return { ok: false,
               response: commit_res.response || t('提交事务失败。', 'Failed to commit transaction.'),
               error: 'commit_failed' };
    }
    res.durable = true;
    res.review_state_committed = staged_review_completion;
  } else {
    var tx_ctx_after = get_tx_context();
    if (tx_ctx_after.active && tx_ctx_after.owner === TX_OWNER_CALLER) {
      res.durable = false;
      res.pending_caller_commit = true;
      res.response = (res.response || '') + '\n' +
        t('该操作已在调用者事务中执行，最终是否持久化由外层 COMMIT/ROLLBACK 决定。',
          'The operation executed inside the caller-owned transaction; durability is controlled by the outer COMMIT/ROLLBACK.');
    } else if (tx_ctx_after.active && tx_ctx_after.owner === TX_OWNER_AGENT) {
      res.durable = false;
      res.pending_agent_commit = true;
    } else {
      res.durable = true;
    }
  }

  if (typeof res.affected_rows !== 'undefined' && res.affected_rows >= 0) {
    res.diff_summary = t('影响行数：', 'Rows affected: ') + res.affected_rows;
  }
  return res;
}

function parse_tool_call(raw) {
  if (!raw) return null;
  var s = String(raw).trim();

  // Prefer exact JSON.
  s = s.replace(/^\s*```(?:json)?\s*/i, '').replace(/\s*```\s*$/i, '').trim();
  try {
    var direct = JSON.parse(s);
    if (direct && typeof direct.tool === 'string') return direct;
  } catch (e0) {}

  // Compatibility path for models that prepend prose.
  var depth = 0, start = -1, end = -1, in_string = false, escaped = false;
  for (var i = 0; i < s.length; i++) {
    var ch = s[i];
    if (in_string) {
      if (escaped) { escaped = false; continue; }
      if (ch === '\\') { escaped = true; continue; }
      if (ch === '"') in_string = false;
      continue;
    }
    if (ch === '"') { in_string = true; continue; }
    if (ch === '{') {
      if (depth++ === 0) start = i;
    } else if (ch === '}' && depth > 0) {
      if (--depth === 0) { end = i; break; }
    }
  }
  if (start === -1 || end === -1) return null;
  try {
    var obj = JSON.parse(s.substring(start, end + 1));
    return (obj && typeof obj.tool === 'string') ? obj : null;
  } catch(e) { return null; }
}
