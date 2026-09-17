//@include lib_router.js
/* Tool specs own their own registration.  Each specs file pulls in
 * lib_tool_registry.js itself, so the registry text lands in whichever
 * execution root expands first and the include dedup leaves the rest empty --
 * see run_genagentjsfile.cmake. */
//@include lib_tool_specs_core.js
//@include lib_tool_specs_schema.js
//@include lib_tool_specs_ml.js
//@include lib_tool_specs_memory.js
//@include lib_tool_specs_artifact.js

var TABLE_FALLBACK = {
  'INNODB_LOCKS':       'performance_schema.data_locks',
  'INNODB_LOCK_WAITS':  'performance_schema.data_lock_waits',
  'USER_STATISTICS':    'performance_schema.accounts',
  'INDEX_STATISTICS':   'information_schema.STATISTICS',
  'QUERY_CACHE_INFO':   'performance_schema.events_statements_summary_by_digest',
  'GLOBAL_STATUS':      'performance_schema.global_status',
  'GLOBAL_VARIABLES':   'performance_schema.global_variables'
};

/* Transaction ownership helpers.
 *
 * There are two independent facts:
 *   1) Is the current MySQL connection inside a physical transaction?
 *   2) If yes, who owns its COMMIT/ROLLBACK boundary?
 *
 * mysql.agent_tx_lease answers only (2) for AGENT-owned transactions.
 * It MUST NOT be used as a proxy for (1), because callers may invoke
 * CALL sys.shannon_chat() after START TRANSACTION without an agent lease. */
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
  } catch (e) { return false; }
}

/* Detect the physical transaction on THIS connection.  Performance Schema
 * exposes an ACTIVE transaction immediately after START TRANSACTION/BEGIN,
 * including the important case where no InnoDB row has been touched yet.
 *
 * If the transaction instrument is disabled/unavailable we return
 * known=false.  begin_tx then fails closed rather than risking an implicit
 * commit of a caller-owned transaction. */
function get_session_tx_state() {
  try {
    var inst = query_checked(
      "SELECT ENABLED FROM performance_schema.setup_instruments " +
      "WHERE NAME='transaction' LIMIT 1"
    );
    if (!Array.isArray(inst) || !inst.length ||
        String(inst[0].ENABLED || '').toUpperCase() !== 'YES') {
      return { known: false, active: false, access_mode: '', source: 'pfs_disabled' };
    }

    var rows = query_checked(
      "SELECT et.STATE, et.ACCESS_MODE, et.EVENT_ID, et.AUTOCOMMIT " +
      "FROM performance_schema.events_transactions_current et " +
      "JOIN performance_schema.threads th ON th.THREAD_ID=et.THREAD_ID " +
      "WHERE th.PROCESSLIST_ID=CONNECTION_ID() " +
      "AND et.STATE='ACTIVE' AND et.AUTOCOMMIT='NO' LIMIT 1"
    );
    return {
      known: true,
      active: Array.isArray(rows) && rows.length > 0,
      access_mode: (Array.isArray(rows) && rows.length) ? String(rows[0].ACCESS_MODE || '') : '',
      event_id: (Array.isArray(rows) && rows.length) ? Number(rows[0].EVENT_ID || 0) : 0,
      source: 'pfs'
    };
  } catch (e) {
    return { known: false, active: false, access_mode: '', source: 'unavailable', error: String(e) };
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
    if (event_id) sys.exec_sql("SET @_shannon_agent_tx_event_id=" + Number(event_id));
    else sys.exec_sql("SET @_shannon_agent_tx_event_id=NULL");
  } catch (e) {}
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

    /* Lease + exact Performance Schema EVENT_ID correlation prevents a
     * stale lease from misclassifying a later caller-owned transaction. */
    if (lease_owned && agent_event_marker &&
        Number(physical.event_id) === Number(agent_event_marker)) {
      set_tx_active_for(A.conversation_id, true);
      return { active: true, owner: TX_OWNER_AGENT, known: true,
               access_mode: physical.access_mode, event_id: physical.event_id, source: physical.source };
    }

    if (lease_owned) clear_tx_lease(A.conversation_id);
    set_tx_active_for(A.conversation_id, false);
    return { active: true, owner: TX_OWNER_CALLER, known: true,
             access_mode: physical.access_mode, event_id: physical.event_id, source: physical.source };
  }

  /* With P_S unavailable, only the same-invocation JS mirror can prove
   * ownership. A persisted lease alone is not enough because it may be stale. */
  if (tx_active_for(A.conversation_id) && lease_owned) {
    return { active: true, owner: TX_OWNER_AGENT, known: false,
             access_mode: '', event_id: agent_event_marker, source: physical.source };
  }
  return { active: false, owner: TX_OWNER_UNKNOWN, known: false,
           access_mode: '', event_id: 0, source: physical.source };
}

/* Backward-compatible name.  From this patch onward "real" means physical
 * session transaction, not merely "there is an agent lease". */
function is_real_tx_active() {
  return get_tx_context().active;
}

function is_agent_tx_active() {
  var ctx = get_tx_context();
  return ctx.active && ctx.owner === TX_OWNER_AGENT;
}

function begin_tx_lease(conv_id, plan_id, timeout_minutes) {
  timeout_minutes = timeout_minutes || 30;

  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_tx_lease(conversation_id, session_conn_id, plan_id, expires_at) " +
      "VALUES ('" + esc(conv_id) + "', CONNECTION_ID(), '" + esc(plan_id || '') + "', " +
      "DATE_ADD(NOW(), INTERVAL " + timeout_minutes + " MINUTE)) " +
      "ON DUPLICATE KEY UPDATE " +
      "session_conn_id = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(session_conn_id), session_conn_id), " +
      "plan_id         = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(plan_id), plan_id), " +
      "created_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), NOW(), created_at), " +
      "expires_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(expires_at), expires_at)"
    );
  } catch (e) {}

  return agent_tx_lease_owned(conv_id);
}

/** Remove the lease row — only for THIS session (CONNECTION_ID). */
function clear_tx_lease(conv_id) {
  try {
    sys.exec_sql(
      "DELETE FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(conv_id) + "' " +
      "AND session_conn_id = CONNECTION_ID()"
    );
  } catch (e) {}
}

/** Clean expired AGENT leases owned by this connection.  Never rollback a
 * caller-owned transaction merely because a lease row from some older
 * agent invocation expired. */
function cleanup_expired_tx_leases() {
  try {
    var expired = query(
      "SELECT conversation_id, plan_id, TIMESTAMPDIFF(SECOND, created_at, NOW()) AS age_sec " +
      "FROM mysql.agent_tx_lease WHERE expires_at < NOW() AND session_conn_id = CONNECTION_ID()"
    );
    if (!Array.isArray(expired) || !expired.length) return [];

    /* The lease is already expired, so get_tx_context() deliberately will
     * not trust it.  For timeout cleanup, correlate the session marker with
     * the physical P_S EVENT_ID directly: that proves the active transaction
     * is the same one the Agent started, without risking a caller rollback. */
    var physical = get_session_tx_state();
    var marker = get_agent_tx_event_marker();
    if (physical.known && physical.active && marker &&
        Number(physical.event_id) === Number(marker)) {
      try { sys.exec_sql('ROLLBACK'); } catch (e) {}
      set_tx_active_for(A.conversation_id, false);
      set_agent_tx_event_marker(0);
    }
    for (var i = 0; i < expired.length; i++) {
      clear_tx_lease(expired[i].conversation_id);
      log_rollback_event(expired[i].conversation_id, expired[i].plan_id, 0, '',
                         'expired', t('超时自动回滚：', 'Timeout auto-rollback: ') + expired[i].age_sec + 's');
    }
    return expired;
  } catch (e) { return []; }
}

/* Final safety net is allowed to rollback only AGENT-owned transactions.
 * CALLER-owned transactions deliberately survive CALL shannon_chat() and
 * remain under the caller's COMMIT/ROLLBACK control. */
function finalize_tx_safety_net() {
  var ctx = get_tx_context();
  if (!ctx.active || ctx.owner !== TX_OWNER_AGENT) return '';

  try { sys.exec_sql('ROLLBACK'); } catch (e) {}
  set_tx_active_for(A.conversation_id, false);
  clear_tx_lease(A.conversation_id);
  set_agent_tx_event_marker(0);
  return '\n' + t('[安全网] Agent 未提交事务已强制回滚。',
                  '[Safety net] Uncommitted agent-owned transaction force-rolled back.');
}

/* The sql_mode the policy gate assumes, checked once per turn.
 *
 * Everything the gate knows about a statement -- classify_statement(),
 * sql_write_targets(), check_system_schema_policy(), is_destructive_ddl(),
 * the multi-statement check -- comes from sql_lex_info(), a hand-written
 * lexer.  Inside a string literal it treats a backslash as an escape and a
 * double quote as a string delimiter.  Both are sql_mode-dependent:
 *
 *   NO_BACKSLASH_ESCAPES  the server reads a backslash as an ordinary
 *                         character, so 'a\' is a *complete* literal to the
 *                         server and an unterminated one to the lexer -- from
 *                         there the two disagree about which bytes are string
 *                         and which are SQL, and the gate is classifying a
 *                         statement the server will not run.
 *   ANSI_QUOTES           " opens an identifier, not a string.
 *
 * The same modes corrupt data on the way in: esc() escapes backslashes *and*
 * doubles quotes, so under NO_BACKSLASH_ESCAPES the backslash half is wrong
 * and a stored message comes back with its backslashes doubled, while
 * esc_like()'s \% stops escaping anything.
 *
 * So rather than teach the lexer every mode, refuse the turn when the session
 * is in one the lexer cannot read.  This is a check, not a SET: silently
 * changing the caller's sql_mode would change how their own statements
 * behave for the rest of the connection. */
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

/* ML write-tool metadata used to live here as a standalone ML_WRITE_TOOLS
 * table, maintained in parallel with execute_tool()'s if-chain and
 * validate_tool_call()'s switch, with nothing keeping the three in sync.
 * It is now derived from the tool spec itself -- see tool_review_meta() in
 * lib_tool_registry.js and the `write` / `risk` / `tableArg` /
 * `fallbackArg` / `transactional` fields on each ml_* spec.
 *
 * Why this metadata has to exist at all: classify_statement() keys off the
 * leading SQL keyword.  It does not recognise CALL, and these tools do not
 * carry a `sql` argument anyway (their arguments are table_name /
 * model_handle), so build_review_step() synthesizes sql='' for them and
 * classify_statement('') reports is_write=false.  Without explicit metadata
 * evaluate_step_policy() would wave every ML write through as
 * 'safe_read_only' even under review_mode with require_approval_for_write.
 *
 * transactional:false likewise still matters: sys.ML_* procedures own their
 * own transaction boundaries, so execute_review_step() must not wrap them in
 * an explicit BEGIN/COMMIT. */

function sql_lex_info(sql) {
  var s = String(sql || '');
  var tokens = [];
  var token = '';
  var depth = 0;
  var in_single = false, in_double = false, in_backtick = false;
  var in_line_comment = false, in_block_comment = false;
  var escaped = false;
  var top_level_semicolons = 0;

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
    if (ch === '/' && nx === '*') { flush(); in_block_comment = true; i++; continue; }
    if (ch === "'") { flush(); in_single = true; continue; }
    if (ch === '"') { flush(); in_double = true; continue; }
    if (ch === '`') { flush(); in_backtick = true; continue; }

    if (ch === '(') { flush(); depth++; continue; }
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

  var top = tokens.filter(function(t) { return t.depth === 0; }).map(function(t) { return t.text; });
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

  /* A single trailing semicolon is harmless; any top-level semicolon with
   * tokens following it is a multi-statement payload. */
  var trimmed = s.trim();
  var trailing_only = top_level_semicolons === 1 && /;\s*$/.test(trimmed);
  return {
    first_keyword: first,
    dml_keyword: dml,
    top_tokens: top,
    has_top_level_where: top.indexOf('WHERE') !== -1,
    multiple_statements: top_level_semicolons > (trailing_only ? 1 : 0)
  };
}

/* ------------------------------------------------------ read-path ceiling
 *
 * Why a read needs a policy at all.
 *
 * Every row a tool reads is materialised into the per-thread JerryScript
 * heap, which is 512KB and is a hard ceiling -- jerry-core addresses it with
 * 16-bit compressed pointers, so a bigger heap needs JERRY_CPOINTER_32_BIT
 * and misbehaves silently without it (see kJerryHeapBytes in sql/sp_head.cc).
 * `SELECT * FROM fact_sales` is therefore not a slow query, it is an
 * out-of-memory, and until the port's fatal handler was overridden it took
 * the whole server down with exit().
 *
 * The model was asked to always write a LIMIT, in the prompt, and that is
 * exactly the kind of instruction a model drops on turn six of a hard
 * question. So the ceiling is enforced here instead.
 *
 * Reject rather than rewrite. Injecting a LIMIT the model did not write
 * means the number it reasons about afterwards -- a total, a rank, a row
 * count -- is quietly not the number the database holds, and the model has
 * no way to tell. Refusing costs one turn and tells it exactly what to
 * change; a wrong answer delivered confidently costs the user's trust in
 * every other answer. The refusal text is written to be actionable, because
 * a refusal the model cannot act on is just a slower failure.
 *
 * Implicit-aggregate reads are exempt: a SELECT with an aggregate outside
 * any parenthesis and no top-level GROUP BY returns exactly one row by
 * definition, and making the model write `SELECT COUNT(*) ... LIMIT 1` would
 * be noise it learns to route around. */
var READ_ROW_LIMIT_MAX_DEFAULT = 1000;
var REVIEW_STEP_STUCK_TTL_DEFAULT = 120;

/* The ceiling an operator can lower but a session cannot raise, for the same
 * reason the approval switches work that way -- see get_review_policy(). */
function read_row_limit_max() {
  return _combine_min(get_operator_policy(), 'read_row_limit_max',
                      READ_ROW_LIMIT_MAX_DEFAULT, 1);
}

function read_sql_limit_info(lex) {
  var top = lex.top_tokens || [];
  var idx = top.indexOf('LIMIT');
  if (idx === -1) return { has_limit: false, value: null };
  /* `LIMIT n`, `LIMIT off, n` and `LIMIT n OFFSET off` all put the row count
   * in a numeric token after LIMIT; in the two-number comma form it is the
   * second one. A placeholder or an expression leaves value null, which the
   * caller treats as "present but unknown" rather than as unbounded. */
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
  for (var i = 0; i < top.length; i++)
    if (_AGG_FUNCS.indexOf(top[i]) !== -1) return true;
  return false;
}

/* Returns null when the statement may run as written, or a tool-shaped
 * rejection when it may not. `stmt` is the classify_statement() result the
 * caller already has. */
function guard_read_sql(sql, stmt) {
  if (!stmt || !stmt.is_read) return null;
  /* SHOW / DESCRIBE / EXPLAIN take no LIMIT worth speaking of and are
   * bounded by the catalogue rather than by user data. */
  if (stmt.first_keyword !== 'SELECT' && stmt.first_keyword !== 'WITH')
    return null;

  var lex = sql_lex_info(sql);
  if (read_sql_is_single_row(lex)) return null;

  var lim = read_sql_limit_info(lex);
  var row_max = read_row_limit_max();
  if (!lim.has_limit) {
    return { ok: false, error: 'read_limit_required',
             response: t(
      '拒绝执行：该 SELECT 没有 LIMIT。结果集会被读进 JavaScript 引擎堆（每线程 512KB 且无法调大），' +
      '无 LIMIT 的查询可能耗尽它。请加上 LIMIT（最大 ' + row_max + '），' +
      '或改写为聚合查询（例如 SELECT COUNT(*) / SUM() / GROUP BY ... LIMIT n）后重试。',
      'Rejected: this SELECT has no LIMIT. Results are read into the JavaScript engine heap ' +
      '(512KB per thread, not enlargeable), which an unbounded query can exhaust. Add a LIMIT ' +
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

/* Default statement wall clock for agent reads, in milliseconds. Long
 * enough that an ordinary analytical query over a warm Rapid table is not
 * cut off, short enough that a mistake does not hold the turn open for
 * minutes. Not a per-request option: an operator can lower it through
 * mysql.agent_policy, and nothing else needs to reach it. */
var READ_TIMEOUT_MS_DEFAULT = 30000;

function read_timeout_ms() {
  return _combine_min(get_operator_policy(), 'read_timeout_ms',
                      READ_TIMEOUT_MS_DEFAULT, 1000);
}

/* Prefix telling the model it is looking at a prefix, not the answer.
 * Without it a truncated read is indistinguishable from a complete one, and
 * the model will happily total up a fraction of the rows and report it as
 * the total. */
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

/* The statement-level wall clock. Without it a read that passes the row
 * ceiling can still hold the turn open indefinitely on an unindexed join,
 * and the agent has no way to interrupt it. Expressed as an optimizer hint
 * so it travels with the statement and needs no session state -- which
 * matters because the agent shares its session with the caller.
 *
 * SELECT only: MAX_EXECUTION_TIME is ignored with a warning on anything
 * else, and a warning per statement is noise in the trace. */
function with_read_timeout(sql, stmt, ms) {
  ms = Number(ms) || 0;
  if (ms <= 0 || !stmt || stmt.first_keyword !== 'SELECT') return sql;
  var hint = 'MAX_EXECUTION_TIME(' + Math.round(ms) + ')';
  /* A statement that already carries a hint block gets the timeout merged
   * into that block. A second hint comment would simply be ignored with a
   * warning, and skipping the statement instead would let a model-written
   * hint -- even an innocent one -- opt the query out of the wall clock. */
  if (/^\s*SELECT\s+\/\*\+/i.test(sql)) {
    if (/MAX_EXECUTION_TIME/i.test(sql)) return sql;   /* caller set its own */
    return sql.replace(/(\/\*\+)/, '$1 ' + hint);
  }
  return sql.replace(/^(\s*SELECT)(\s)/i, '$1 /*+ ' + hint + ' */$2');
}

function classify_statement(sql) {
  var lex = sql_lex_info(sql);
  var first = lex.first_keyword;

  var _READ  = ['SELECT','SHOW','DESCRIBE','DESC','EXPLAIN','WITH'];
  var _DDL   = ['CREATE','ALTER','DROP','TRUNCATE','RENAME'];
  var _TCL   = ['BEGIN','START','COMMIT','ROLLBACK','SAVEPOINT'];

  var is_write = !!lex.dml_keyword;
  var is_read  = _READ.indexOf(first) !== -1 && !is_write;
  var is_ddl   = _DDL.indexOf(first) !== -1;
  var is_tcl   = _TCL.indexOf(first) !== -1;

  var risk = 'low';
  if (is_ddl) {
    risk = 'high';
  } else if (is_write) {
    risk = ((lex.dml_keyword === 'DELETE' || lex.dml_keyword === 'UPDATE') &&
            !lex.has_top_level_where) ? 'high' : 'medium';
  }

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

/* The refusal shared by validate_run_ddl() and evaluate_step_policy(), so the
 * two gates cannot drift apart -- which is how require_approval_for_ddl
 * became dead code once before.  Returns null when the statement is allowed.
 *
 * Like allow_destructive_ddl and unlike the require_approval_* flags, these
 * are enforced whatever review_mode says: an operator who has not turned
 * review on has not thereby asked for the agent to be able to mint logins. */
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

/* Schemas the agent must not write to without an explicit opt-in.
 *
 * `mysql` is the one that matters: it holds the grant tables, and it also
 * holds the agent's own memory, approval plans and audit trail.  A model that
 * can UPDATE mysql.user can grant itself anything; a model that can DELETE
 * FROM mysql.agent_review_history can erase the record of what it did.  Until
 * this gate existed, neither was refused -- review_mode defaults to 'off',
 * where require_approval_for_write does not apply, so the only thing standing
 * between the model and the grant tables was whether the *caller* happened to
 * hold UPDATE on mysql.*.  A DBA using the agent generally does.
 *
 * sys is included because it holds this agent's own stored program: rewriting
 * sys.shannon_chat is the agent editing itself.  The two read-only schemas
 * are included for completeness; the server refuses writes to them anyway,
 * and a clear refusal beats an engine error the model will try to work
 * around. */
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

/* One table reference as SQL spells it: `db` . `tbl`, backquotes optional and
 * whitespace allowed around the dot.  Every pattern below is built from this
 * one string so they cannot disagree about what a name looks like: while only
 * add_refs() knew that "mysql . user" is a single qualified reference,
 * INSERT INTO mysql . user captured "mysql" alone and the policy check read
 * it as an unqualified write to the session's own (non-system) database. */
var _SQL_REF = '`?[A-Za-z0-9_$]+`?(?:\\s*\\.\\s*`?[A-Za-z0-9_$]+`?)?';
var _RE_INSERT  = new RegExp('\\binsert\\s+(?:low_priority\\s+|delayed\\s+|high_priority\\s+|' +
                             'ignore\\s+)*(?:into\\s+)?(' + _SQL_REF + ')', 'i');
var _RE_REPLACE = new RegExp('\\breplace\\s+(?:low_priority\\s+|delayed\\s+)*(?:into\\s+)?(' +
                             _SQL_REF + ')', 'i');
var _RE_DDL_OBJ = new RegExp('\\b(?:table|view|index|trigger|event|database|schema|tablespace)\\s+' +
                             '(?:if\\s+(?:not\\s+)?exists\\s+)?(' + _SQL_REF + ')', 'i');
var _RE_DDL_ON  = new RegExp('\\bon\\s+(' + _SQL_REF + ')', 'i');
/* RENAME TABLE takes a list of pairs, so every destination in the statement
 * is a target; global, and lastIndex is reset at each use. */
var _RE_RENAME_TO = new RegExp('\\bto\\s+(' + _SQL_REF + ')', 'gi');
/* ALTER TABLE t RENAME [TO|AS] new.  The object keywords are excluded so this
 * fires neither on RENAME TABLE, which the pair scan above already covers,
 * nor on RENAME COLUMN/INDEX/KEY, whose destination is part of a table the
 * clauses above have already collected. */
var _RE_ALTER_RENAME = new RegExp('\\brename\\s+(?:to\\s+|as\\s+)?' +
                                  '(?!table\\b|column\\b|index\\b|key\\b)(' + _SQL_REF + ')', 'i');

/* Which tables does this statement WRITE to?
 *
 * Deliberately not infer_affected_tables(): that one collects every table
 * mentioned anywhere and then throws the schema qualifier away, which is
 * exactly backwards for this question.  Reading information_schema inside an
 * INSERT ... SELECT is ordinary and must stay allowed; writing to it is not.
 * So only the target position of each write verb is examined.
 *
 * Conservative where it is unsure: an UPDATE's target list is everything
 * between UPDATE and SET, because a multi-table UPDATE can write to any of
 * them, and a DELETE's is both the explicit target list and the FROM list for
 * the same reason. */
function sql_write_targets(sql) {
  var s = String(sql || '');
  var out = [], seen = {};

  function add(ref) {
    /* "mysql . user" and "mysql.user" are the same reference: MySQL allows
     * whitespace around the qualifier dot, and a name that loses its
     * qualifier here reads downstream as an unqualified write. */
    var name = String(ref || '').trim().replace(/[`;,()]/g, '').replace(/\s*\.\s*/g, '.');
    if (!name || seen[name]) return;
    /* Aliases, keywords and derived tables are not write targets. */
    if (!/^[A-Za-z0-9_$]+(\.[A-Za-z0-9_$]+)?$/.test(name)) return;
    seen[name] = true;
    out.push(name);
  }

  function add_refs(segment) {
    /* A table reference is the first identifier of each comma- or
     * JOIN-separated item; anything after it is an alias or a join
     * condition. */
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

  /* DDL names its object after the object keyword.  DATABASE/SCHEMA is the
   * case with no dot to find: DROP DATABASE mysql names a schema directly. */
  var first = String((s.match(/^\s*(\w+)/) || ['', ''])[1]).toUpperCase();
  if (['CREATE','ALTER','DROP','TRUNCATE','RENAME'].indexOf(first) !== -1) {
    if ((m = s.match(_RE_DDL_OBJ))) add(m[1]);
    /* An index or a trigger is named first and the table it writes to comes
     * after ON, so the clause above collects the wrong one of the two:
     * CREATE INDEX i ON mysql.user (x) named only `i`. */
    if (/\b(?:index|trigger)\b/i.test(s) && (m = s.match(_RE_DDL_ON))) add(m[1]);
    /* A rename's destination is a write target of its own, and it does not
     * have to be in the source's schema: the clauses above see shop.t in
     * RENAME TABLE shop.t TO mysql.evil and nothing sees where it lands. */
    if (/^\s*rename\s+table\b/i.test(s)) {
      _RE_RENAME_TO.lastIndex = 0;
      while ((m = _RE_RENAME_TO.exec(s))) add(m[1]);
    } else if ((m = s.match(_RE_ALTER_RENAME))) {
      add(m[1]);
    }
  }
  return out;
}

/* Refuse a write whose target is in a system schema.
 *
 * Enforced regardless of review_mode, like allow_destructive_ddl: an approval
 * prompt is the wrong control here, because the operator approving it is
 * being shown a statement whose danger is not in its shape.
 *
 * `db` is the session's current database, because an unqualified target in a
 * system schema is the same write with the qualifier left implicit --
 * USE mysql; DELETE FROM user WHERE ... reads as ordinary DML otherwise. */
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

/**
 * Does this DDL statement destroy data or schema objects?
 *
 * DROP anywhere at the top level covers DROP TABLE / DROP DATABASE and also
 * ALTER TABLE ... DROP COLUMN / DROP INDEX, which are equally irreversible
 * from the agent's point of view.  TRUNCATE is included for the obvious
 * reason.  Deliberately conservative: this only gates a statement behind an
 * explicit operator opt-in (review_policy.allow_destructive_ddl), so erring
 * toward "destructive" costs a configuration flag, never data.
 */
function is_destructive_ddl(sql) {
  var lex = sql_lex_info(sql);
  if (lex.first_keyword === 'TRUNCATE') return true;
  return lex.top_tokens.indexOf('DROP') !== -1;
}

/* What KIND of thing does this DDL act on?
 *
 * run_ddl used to ask one question -- "is the first keyword CREATE / ALTER /
 * DROP / TRUNCATE / RENAME?" -- and then one more, "does it contain DROP?".
 * Everything that passed both was equivalent.  That put these in the same
 * tier as ALTER TABLE ... SECONDARY_LOAD:
 *
 *   CREATE USER a IDENTIFIED BY 'p'        a new login
 *   ALTER USER root IDENTIFIED BY 'p'      a changed password
 *   CREATE FUNCTION f ... LANGUAGE JAVASCRIPT
 *                                          new server-side code, which for
 *                                          this project means new agent code
 *   CREATE EVENT e ON SCHEDULE ... DO ...  a scheduler entry that outlives
 *                                          the conversation that made it
 *
 * and review_mode defaults to 'off', where allow_destructive_ddl is the only
 * gate that still applies -- so by default none of the four was reviewed or
 * refused.  They are separated here so each can carry its own opt-in.
 *
 * The rule is "first recognised object keyword at the top level wins", not
 * "first token after CREATE", because the modifiers between them are
 * open-ended (DEFINER=u@h, ALGORITHM=UNDEFINED, SQL SECURITY INVOKER, OR
 * REPLACE, IF NOT EXISTS) while the object keywords are a closed set.  It
 * also keeps CREATE TABLE user from reading as account DDL: TABLE is seen
 * first.  sql_lex_info() has already dropped comments, string literals and
 * backquoted identifiers, so a column named `user` or a DEFAULT 'USER'
 * cannot reach this. */
var _DDL_OBJECT_CLASS = {
  USER: 'account', ROLE: 'account',
  FUNCTION: 'code', PROCEDURE: 'code', TRIGGER: 'code', EVENT: 'code',
  /* Server-wide rather than schema-scoped, and reachable through run_ddl
   * because both spellings start with a keyword it accepts:
   *
   *   ALTER INSTANCE ROTATE INNODB MASTER KEY    re-keys tablespace encryption
   *   ALTER INSTANCE RELOAD TLS                  swaps the server's certificate
   *   ALTER INSTANCE DISABLE INNODB REDO_LOG     leaves the server uncrashsafe
   *   CREATE RESOURCE GROUP rg TYPE = SYSTEM     CPU affinity / thread priority
   *
   * None destroys an object, so is_destructive_ddl() does not see them, and
   * before they were listed here the fallback below filed them under
   * 'schema' -- i.e. the same tier as adding an index.  RESOURCE also has to
   * be named for a second reason: the scan stops at the first recognised
   * keyword, so without it CREATE RESOURCE GROUP rg TYPE = USER reached the
   * USER entry above and was refused as account DDL, while the otherwise
   * identical TYPE = SYSTEM fell through and was allowed. */
  INSTANCE: 'instance', RESOURCE: 'instance',
  TABLE: 'schema', INDEX: 'schema', VIEW: 'schema', DATABASE: 'schema',
  SCHEMA: 'schema', TABLESPACE: 'schema', SERVER: 'schema', SEQUENCE: 'schema',
  /* Named so the "closed set" claim below is true rather than nearly true;
   * both are ordinary storage/metadata DDL and stay in the schema tier. */
  LOGFILE: 'schema', SPATIAL: 'schema'
};

/* `lex` is an optional already-computed sql_lex_info(sql) for the same
 * statement -- classify_statement() has one in hand, and lexing is the whole
 * cost of this function. */
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
    /* An unrecognised shape stays 'schema', i.e. exactly how it was treated
     * before this function existed, because failing closed on anything
     * unparsed would refuse ordinary DDL.
     *
     * The cost of that choice is that this fallback is fail-OPEN, so every
     * gated class has to be listed above exhaustively or it silently lands in
     * the ungated tier -- which is precisely what happened to ALTER INSTANCE
     * and CREATE RESOURCE GROUP while this comment claimed the set was
     * "closed and fully enumerated".  A new gated class means auditing the
     * map, not just adding a flag; the selfcheck below pins the cases that
     * have bitten so far. */
    action_class: klass || 'schema',
    ddl_object:   object,
    /* Called out separately because it is the self-modifying case: the agent
     * is a LANGUAGE JAVASCRIPT routine, so this is the agent writing agent
     * code.  A capability for the platform, a hole for the governance -- and
     * either way not something to grant by the same flag as a new index. */
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

function rule_planner(msg, db) {

  /* Every route below is read-only introspection.  A request to CREATE an
   * index, ALTER a table or SECONDARY_LOAD one contains the very same nouns
   * these patterns key on ("索引", "表结构"), so without this guard an action
   * request was answered with a catalog dump and the DDL never happened.
   * Hand those to the agent loop, which has run_ddl. */
  if (/建立?索引|创建索引|新建索引|加索引|添加索引|删除索引|建表|创建表|新建表|删表|删除表|改表|修改表结构|加载到|卸载|secondary_?load|secondary_?unload|\bcreate\s+(table|index|database|schema)\b|\balter\s+table\b|\bdrop\s+(table|index|database|schema)\b|\btruncate\b|\brename\s+table\b|\badd\s+index\b/i.test(String(msg || '')))
    return null;

  if (/有哪些(数据库|schema)|所有数据库|列出.*数据库|show.?databases|show.?schemas|list.*databases/i.test(msg)) {
    return [
      { sql: "SELECT SCHEMA_NAME AS database_name" +
            " FROM information_schema.SCHEMATA" +
            " ORDER BY SCHEMA_NAME",
        desc: t('当前实例所有数据库', 'All databases in current instance') }
    ];
  }

  if (/有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables/i.test(msg) &&
      !/结构|schema|column|字段信息|列信息|列名|create|structure|definition|利润表|现金流量表|资产负债表|报表/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS,TABLE_COMMENT" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY TABLE_NAME",
        desc: t('获取所有表概览', 'Get all tables overview') }
    ];
  }

  if (/(表.*结构|结构.*表|字段|列信息|column.*info|table.*structure|describe.*table)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
             "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
             "  TABLE_COMMENT" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY TABLE_NAME",
        desc: t('所有表概览（行数 / 大小 / 注释）',
                'All tables overview (rows / size / comment)') },
        { sql: "SELECT TABLE_NAME," +
              "  GROUP_CONCAT(" +
              "    CONCAT(COLUMN_NAME,' ',COLUMN_TYPE," +
              "      IF(COLUMN_KEY<>'',CONCAT(' ',COLUMN_KEY),'')," +
              "      IF(IS_NULLABLE='NO',' NOT NULL','')" +
              "    ) ORDER BY ORDINAL_POSITION SEPARATOR ', '" +
              "  ) AS columns" +
              " FROM information_schema.COLUMNS" +
              " WHERE TABLE_SCHEMA=DATABASE()" +
              " GROUP BY TABLE_NAME" +
              " ORDER BY TABLE_NAME",
          desc: t('所有表的列定义（每表一行紧凑格式）',
                  'Column definitions for all tables (one compact row per table)') },
       { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
              " FROM information_schema.KEY_COLUMN_USAGE" +
              " WHERE CONSTRAINT_SCHEMA=DATABASE()" +
              "   AND REFERENCED_TABLE_NAME IS NOT NULL",
         desc: t('外键关系（JOIN 路径）', 'Foreign key relationships (JOIN paths)') }
    ];
  }

  if (/(外键|foreign.?key|join.*关系|关联关系|table.*relation|related.*table)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
             " FROM information_schema.KEY_COLUMN_USAGE" +
             " WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL",
        desc: t('所有外键关系（JOIN 路径）', 'All foreign key relationships (JOIN paths)') }
    ];
  }

  if (/(索引|index.*分析|缺少.*索引|覆盖.*索引|index.*analysis|missing.*index|covering.*index)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX," +
             "COLUMN_NAME,CARDINALITY,INDEX_TYPE" +
             " FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()" +
             " ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX",
        desc: t('当前库所有索引详情', 'All index details for current database') },
      { sql: "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE" +
             " FROM information_schema.COLUMNS" +
             " WHERE TABLE_SCHEMA=DATABASE()" +
             "   AND COLUMN_NAME NOT IN (" +
             "     SELECT DISTINCT COLUMN_NAME FROM information_schema.STATISTICS" +
             "     WHERE TABLE_SCHEMA=DATABASE())" +
             " ORDER BY TABLE_NAME,ORDINAL_POSITION",
        desc: t('未被索引覆盖的列（候选索引）', 'Unindexed columns (index candidates)') }
    ];
  }

  if (/(数据量|行数|record.?count|row.?count|表.*大小|数据大小|table.*size)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
             "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
             "  DATA_FREE" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY DATA_LENGTH+INDEX_LENGTH DESC",
        desc: t('各表数据量与磁盘占用', 'Table row counts and disk usage') }
    ];
  }

  if (/(性能.*top|top.*性能|最慢.*查询|query.*perf|slow.*queries|performance.*top)/i.test(msg)) {
    return [
      { sql: "SELECT DIGEST_TEXT,COUNT_STAR," +
             "  ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec," +
             "  ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec," +
             "  SUM_ROWS_EXAMINED,SUM_ROWS_SENT" +
             " FROM performance_schema.events_statements_summary_by_digest" +
             " ORDER BY SUM_TIMER_WAIT DESC LIMIT 20",
        desc: t('SQL 性能 TOP20', 'SQL performance TOP20') }
    ];
  }

  return null;
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
      try {
        var raw = query_checked(with_read_timeout(sql, stmt, read_timeout_ms()));
        result_text = compress(rows_to_text(raw), per_step_limit);
        if (rows_truncated(raw)) result_text = truncation_note(raw) + result_text;
      } catch (e) {
        step_ok = false;
        var err_text = String(e);
        var recovery = try_recover_unknown_table(err_text, sql);
        if (recovery) {
          try {
            var rec_rows = query_checked(
                with_read_timeout(recovery.sql, classify_statement(recovery.sql),
                                  read_timeout_ms()));
            result_text = recovery.desc + '\n' +
                          (rows_truncated(rec_rows) ? truncation_note(rec_rows) : '') +
                          compress(rows_to_text(rec_rows), per_step_limit);
            sql = recovery.sql;
            step_ok = true;
          } catch (e2) {
            result_text = t('执行出错：', 'Error: ') + String(e2);
          }
        } else {
          result_text = t('执行出错：', 'Error: ') + err_text;
        }
      }
    } else {
      step_ok = false;
      result_text = t('跳过非只读或多语句 SQL（plan_sql 仅执行单条 SELECT/SHOW/DESC/EXPLAIN/WITH）：',
                      'Skipped non-read-only or multi-statement SQL: ') + sql.substring(0, 80);
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

function validate_result(result, sql) {
  if (!result || String(result).trim().length === 0) return 'empty_result';
  var s = String(result);
  if (/unknown table/i.test(s)) return 'unknown_table';
  if (/unknown column/i.test(s)) return 'unknown_column';
  if (/syntax error/i.test(s)) return 'syntax_error';
  if (/denied/i.test(s) || /access denied/i.test(s)) return 'access_denied';
  return null;
}

/* Registry-driven validation.
 *
 * This replaces a ~200-line switch that hand-wrote argument checks and
 * bilingual error text for every tool.  The checks now come from the tool's
 * JSON Schema (lib_tool_schema.js) and its optional spec.validate hook; the
 * error text comes from the spec's `messages` map, into which every one of
 * the old switch's strings was carried verbatim so user-visible output does
 * not regress.
 *
 * Return contract is unchanged: null means valid, a string is the message
 * shown to the model. */
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

/**
 * Shared EXPLAIN FORMAT=JSON tree walker. MySQL's JSON explain plan nests
 * table/access-method nodes under a handful of well-known keys depending on
 * whether the plan involves joins, subqueries, or grouping/ordering. Both
 * parse_explain (human-readable text) and sum_explain_rows (numeric
 * estimate) need to visit the same nodes, so the traversal lives here once.
 */
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

/**
 * Numeric row estimate extracted from the same EXPLAIN JSON tree.
 *
 * For a single-table UPDATE/DELETE (the common review-gated case) there is
 * exactly one table node, so this degenerates to that table's
 * rows_examined_per_scan — exactly "how many rows will this WHERE clause
 * touch" that the review preview was missing. For multi-table plans (JOINs)
 * we report the largest single-table scan rather than summing across
 * tables, since summing overstates the actual result cardinality and would
 * mislead more than 'unknown' did.
 */
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

/**
 * EXPLAIN-based best-effort row estimate for a write statement. This is an
 * optimizer estimate (same statistics EXPLAIN always uses), not an exact
 * count — labelled as such wherever it's displayed. Returns null if EXPLAIN
 * fails or the statement shape doesn't produce a usable estimate (e.g. a
 * plain INSERT...VALUES with no scan).
 */
function estimate_affected_rows(sql) {
  try {
    var ex = query("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !Array.isArray(ex) || !ex.length) return null;
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'];
    if (!raw) return null;
    return sum_explain_rows(String(raw));
  } catch (e) { return null; }
}

/* Registry-driven dispatch.
 *
 * The ~900-line if-chain this replaces now lives as impl_* functions next to
 * the spec that declares them (lib_tool_specs_*.js).  Handler contract:
 *   handler(args, ctx) -> { ok, response, ... }
 *   ctx = { tool, db, chat_opt, policy, conversation_id }
 * ctx.tool is what lets one handler back several spellings of the same
 * routine (ml_model_load / ml_model_unload, the three batch column tools),
 * exactly as the old chain did by testing `tool` inside a shared branch. */
function execute_tool(tool, args, db) {
  var spec = get_tool_spec(tool);
  if (!spec || !spec.handler) {
    return { ok: false,
             response: t('错误：未知工具 "', 'Error: unknown tool "') + tool + '"',
             error: 'unknown_tool' };
  }
  /* Counted here because this is the one place every tool call passes
   * through, including the ones the approval path executes. */
  A.tool_calls = Number(A.tool_calls || 0) + 1;
  var chat_opt = get_chat_options();
  var ctx = {
    tool:            String(tool),
    db:              db,
    chat_opt:        chat_opt,
    policy:          get_review_policy(chat_opt),
    conversation_id: A.conversation_id
  };

  /* Hooks bracket the handler here, rather than at each call site, because
   * this is the only place every tool call passes through -- including the
   * ones the approval path and the rule planner make. A rule that can be
   * bypassed by calling a tool a different way is not a rule. */
  var pre = run_pre_tool_hooks(ctx.tool, args || {}, ctx);
  if (pre.deny) return run_post_tool_hooks(ctx.tool, args || {}, ctx, pre.result);

  var result = spec.handler(pre.args || {}, ctx);
  return run_post_tool_hooks(ctx.tool, pre.args || {}, ctx, result);
}

/* ------------------------------------------------- operator policy baseline
 *
 * mysql.agent_policy holds what the instance allows; @chat_options holds
 * what this request wants. The request may tighten the baseline and may
 * never relax it.
 *
 * Read once per turn and cached on A: the table is tiny, but the read
 * happens on a path that runs for every tool call, and a policy that
 * changed halfway through a turn would be worse than one read slightly
 * stale.
 *
 * Failing open is deliberate. The routines are SQL SECURITY INVOKER, so a
 * caller who cannot read mysql.agent_policy also cannot be restrained by it
 * in any meaningful sense -- whatever the agent would refuse, that caller
 * can still type by hand. Failing closed would therefore buy no safety and
 * would break every deployment that has not granted the new table, which is
 * all of them at the moment of upgrade. */
function get_operator_policy() {
  if (A.operator_policy) return A.operator_policy;
  var base = {};
  try {
    var rows = query("SELECT policy_key, policy_value FROM mysql.agent_policy");
    if (Array.isArray(rows)) {
      for (var i = 0; i < rows.length; i++) {
        var k = String(rows[i].policy_key || '').trim();
        if (k) base[k] = String(rows[i].policy_value === null ? '' : rows[i].policy_value).trim();
      }
    }
  } catch (e) { /* see the note above on failing open */ }
  A.operator_policy = base;
  return base;
}

function _pol_bool(v) {
  var x = String(v === undefined || v === null ? '' : v).toLowerCase();
  return (x === '1' || x === 'true' || x === 'on' || x === 'yes');
}
function _pol_has(base, key) {
  return Object.prototype.hasOwnProperty.call(base, key);
}

/* Combine one boolean option.
 *
 * `safe_value` is the value that restricts the agent more. Whichever side
 * asks for it wins, so a session can turn a permission off but not on, and
 * an operator who has expressed no opinion leaves the session's choice
 * alone. */
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
  /* review_mode is the one option whose tighter value is "on" rather than
   * "off": an operator who has switched review on is saying that nothing
   * runs unreviewed, and a session must not be able to answer that with
   * review_mode='off'. */
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
    /* Opt-in, and unlike the require_approval_* flags this one is enforced
     * even when review_mode is off: DROP / TRUNCATE are irreversible, so the
     * agent never issues them unless an operator has explicitly said it may. */
    allow_destructive_ddl:
      _combine_bool(base, 'allow_destructive_ddl', cfg.allow_destructive_ddl === true, false),
    /* Same shape and the same reasoning as allow_destructive_ddl, for the two
     * DDL classes that were previously indistinguishable from adding an
     * index.  See check_ddl_action_policy(). */
    allow_account_ddl:
      _combine_bool(base, 'allow_account_ddl', cfg.allow_account_ddl === true, false),
    allow_code_ddl:
      _combine_bool(base, 'allow_code_ddl', cfg.allow_code_ddl === true, false),
    allow_instance_ddl:
      _combine_bool(base, 'allow_instance_ddl', cfg.allow_instance_ddl === true, false),
    /* Same opt-in shape, and enforced with review_mode off for the same
     * reason: see check_system_schema_policy(). */
    allow_system_schema_writes:
      _combine_bool(base, 'allow_system_schema_writes',
                    cfg.allow_system_schema_writes === true, false),
    require_approval_for_risky_sql:
      _combine_bool(base, 'require_approval_for_risky_sql',
                    cfg.require_approval_for_risky_sql !== false, true),
    max_pending_steps: _combine_min(base, 'max_pending_steps',
                                    Number(cfg.max_pending_steps || 3), 1),
    /* How long an awaiting_approval plan may sit untouched before it's
     * auto-cancelled (see cleanup_expired_review_plans). Mirrors the tx
     * lease's 30-minute default so a forgotten approval prompt doesn't
     * permanently hijack every subsequent message in the conversation. */
    review_plan_ttl_minutes: _combine_min(base, 'review_plan_ttl_minutes',
                                          Number(cfg.review_plan_ttl_minutes || 30), 1),
    /* How long a step may sit in `executing` before it is treated as
     * interrupted. Much longer than the approval TTL on purpose: this one
     * races a statement that may still be running. Operator-tunable only,
     * like the read ceilings. See recover_stuck_review_steps(). */
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

function parse_review_modify(text) {
  var s = String(text || '').trim();
  /* [\s\S] rather than . — '.' does not match a newline, so a pasted
   * multi-line "Modify: UPDATE ...\n  WHERE ..." failed the anchored match
   * entirely and was reported as a format error. */
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

/* Risk is a ladder, and a spec's declared risk is a floor rather than an
 * alternative to what the SQL says.  forget_memory carries no SQL of its own,
 * so classify_statement() would rate it 'low' and it would sail past the
 * approval gate its spec explicitly asks for; conversely an UPDATE with no
 * WHERE must stay 'high' even if its spec only claims 'medium'. */
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

function build_review_step(tool_obj, args, db, compute_estimate) {
  var tool_name  = tool_obj && tool_obj.tool ? tool_obj.tool : 'unknown';
  var step_spec  = get_tool_spec(tool_name);
  var ml_meta    = tool_review_meta(tool_name);
  var sql = '';
  if (tool_obj && tool_obj.tool === 'plan_sql') {
    var steps = Array.isArray(args && args.steps) ? args.steps : [];
    sql = String((steps[0] && steps[0].sql) || '');
  } else if (tool_obj && tool_obj.tool === 'list_tables') {
    /* Pure metadata lookup; synthesize an equivalent SHOW TABLES for display
     * purposes only (classify_statement/infer_affected_tables need *some*
     * SQL-shaped string to reason about — this tool has no user-authored SQL). */
    sql = "SHOW TABLES" + ((args && args.keyword) ? " LIKE '%" + args.keyword + "%'" : '');
  } else if (tool_obj && tool_obj.tool === 'describe_table') {
    var tn_disp = (args && args.table_name) ? args.table_name :
                  (Array.isArray(args && args.table_names) ? args.table_names.join(',') : '');
    sql = "DESC " + tn_disp;
  } else if (step_spec && typeof step_spec.displaySql === 'function') {
    /* Tools whose arguments are not SQL render their own approval preview --
     * otherwise the review prompt shows an empty "SQL:" line. */
    sql = step_spec.displaySql(args || {});
  } else if (ml_meta) {
    /* ML procedures carry no user-authored SQL, so synthesize a CALL preview. */
    sql = ml_display_sql(tool_name, args || {});
  } else {
    sql = String((args && args.sql) || '');
  }
  sql = replace_ph(sql, db);
  var stmt4 = classify_statement(sql);

  var affected = ml_meta ? [] : infer_affected_tables(sql);
  if (ml_meta) {
    var tbl = (args && args[ml_meta.table_arg]) ||
              (ml_meta.fallback_arg && args && args[ml_meta.fallback_arg]);
    if (tbl) affected = [String(tbl).replace(/^([^.]+)\./, '')];
  }

  var step = {
    id: 1,
    tool: tool_name,
    sql: sql,
    args: args || {},
    affected_tables: affected,
    /* The spec's declaration is a floor, not an alternative: a tool whose
     * arguments are not SQL (the memory tools) would otherwise be rated
     * is_write=false / risk='low' by classify_statement and skip the gate its
     * spec asks for, while an UPDATE with no WHERE must stay 'high' whatever
     * its spec claims. */
    writes: ml_meta ? true : (stmt4.is_write || !!(step_spec && step_spec.write)),
    ddl: stmt4.is_ddl || !!(step_spec && step_spec.ddl),
    risk: ml_meta ? ml_meta.risk
                  : risk_max(stmt4.risk, (step_spec && step_spec.risk) || 'low'),
    /* DDL implicitly commits, so it must never be wrapped in an explicit
     * BEGIN/COMMIT by execute_review_step.  Neither may a tool that declares
     * transactional:false (sys.ML_* procedures, memory writes). */
    transactional: ml_meta ? (ml_meta.transactional !== false)
                           : ((step_spec && step_spec.transactional === false)
                                ? false : !stmt4.is_ddl),
    estimated_rows: ml_meta ? t('不适用（ML 存储过程）', 'n/a (ML procedure)') : 'unknown',
    /* scope/requiresTx come from the spec, not from the SQL text: a
     * memory-scoped tool writes only to mysql.agent_*, so the approval gate
     * that exists to protect user tables should not fire for it. */
    scope: step_spec ? step_spec.scope : 'db',
    requiresTx: !!(step_spec && step_spec.requiresTx),
    thought: tool_obj && tool_obj.thought ? tool_obj.thought : ''
  };

  /* EXPLAIN-based row estimate — only for write statements (the case the
   * approval preview actually needs to protect against: a WHERE clause
   * that touches far more rows than intended), and only when the caller
   * has confirmed we're actually about to render a review prompt.
   * build_review_step is also called on every single turn purely to
   * evaluate policy (see evaluate_step_policy call site) — running an
   * extra EXPLAIN round-trip there would double the SQL calls per turn
   * for the common auto-execute path, so compute_estimate defaults falsy
   * and only build_review_steps (called exclusively from the pause
   * branch) opts in. */
  /* Not for a spec-rendered preview: that SQL is illustrative, not
   * executable, so EXPLAIN would only buy a wasted round trip and an error. */
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
  var is_ddl   = !!(step && step.ddl);
  var is_write = !!(step && step.writes);
  var is_risky = !!(step && step.risk === 'high');

  /* Destructive DDL is refused regardless of review_mode unless an operator
   * has opted in.  Everything else about DDL is a policy decision below --
   * it used to be rejected outright here, which made require_approval_for_ddl
   * dead code and left ALTER TABLE ... SECONDARY_LOAD (a prerequisite of
   * ml_train) impossible to perform through the agent at all. */
  /* Checked before the destructive gate only so that DROP USER reports the
   * more specific of the two refusals.  Both still apply: opting in to
   * account DDL does not also opt in to dropping things. */
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

  if (!policy || policy.review_mode !== 'review')
    return { action: 'execute', reason: 'review_disabled' };

  /* Memory-scoped writes (remember_fact) touch only mysql.agent_* and join no
   * transaction, so require_approval_for_write -- which exists to guard user
   * tables -- does not apply to them.  forget_memory is deliberately not
   * exempt: it carries risk:'high' and still lands in the gate below. */
  if (is_write && !is_ddl && !is_risky &&
      step && step.scope === 'memory' && !step.requiresTx)
    return { action: 'execute', reason: 'memory_scoped_write' };

  if (is_ddl && policy.require_approval_for_ddl)
    return { action: 'pause', reason: 'ddl_requires_approval' };
  if (is_write && policy.require_approval_for_write)
    return { action: 'pause', reason: 'write_requires_approval' };
  if (is_risky && policy.require_approval_for_risky_sql)
    return { action: 'pause', reason: 'risky_sql_requires_approval' };

  /* Read-only fast path — explicitly gated on !is_write. Previously this
   * branch fired whenever auto_execute_read_only was true regardless of
   * whether the step actually wrote anything, so a write step could slip
   * through mislabeled as 'safe_read_only' whenever an operator had
   * disabled require_approval_for_write for an unrelated reason. */
  if (!is_write && !is_ddl && policy.auto_execute_read_only)
    return { action: 'execute', reason: 'safe_read_only' };

  /* Reaching here means the step IS a write or high-risk statement, but
   * policy explicitly exempted it from approval (require_approval_for_write
   * / require_approval_for_risky_sql set to false). Execute it — that's the
   * operator's explicit intent — but under its own accurate reason instead
   * of borrowing 'safe_read_only', so audit logs reflect what actually
   * happened. */
  if (is_write || is_ddl || is_risky)
    return { action: 'execute', reason: 'approval_explicitly_disabled_by_policy' };

  return { action: 'pause', reason: 'review_mode' };
}

function render_review_prompt(plan, step, policy) {
  var lines = [];
  lines.push(t('【审批中断】请确认下一步执行：', '[Approval pause] Please review the next step:'));
  lines.push(t('计划 ID：', 'Plan ID: ') + (plan && plan.plan_id ? plan.plan_id : '')); 
  lines.push(t('步骤：', 'Step: ') + (step && step.id ? step.id : 1) + '/' + ((plan && plan.total_steps) ? plan.total_steps : 1));
  lines.push(t('工具：', 'Tool: ') + (step && step.tool ? step.tool : 'unknown'));
  lines.push(t('SQL：', 'SQL: ') + (step && step.sql ? step.sql : ''));
  lines.push(t('影响表：', 'Affected tables: ') + ((step && step.affected_tables && step.affected_tables.length) ? step.affected_tables.join(', ') : t('未知', 'unknown')));
  lines.push(t('预计影响行数：', 'Estimated rows: ') + (step && step.estimated_rows ? step.estimated_rows : 'unknown'));
  lines.push(t('会写入/修改结构：', 'Will write / modify schema: ') + ((step && (step.writes || step.ddl)) ? t('是', 'yes') : t('否', 'no')));
  lines.push(t('风险等级：', 'Risk: ') + (step && step.risk ? step.risk : 'low'));
  if (plan && plan.description) lines.push(t('计划描述：', 'Plan description: ') + plan.description);
  lines.push('');
  lines.push(t('请回复：Approve / Reject / Modify:SQL', 'Please reply: Approve / Reject / Modify:SQL'));
  return lines.join('\n');
}

function build_review_steps(tool_obj, args, db) {
  if (!tool_obj || !tool_obj.tool) return [];
  if (tool_obj.tool === 'plan_sql') {
    var raw_steps = Array.isArray(args && args.steps) ? args.steps : [];
    var steps = [];
    for (var i = 0; i < raw_steps.length; i++) {
      var step_args = raw_steps[i] || {};
      /* Pick the tool that actually matches the statement kind: a write
       * (INSERT/UPDATE/DELETE/REPLACE) sub-step must route to 'update_data',
       * or execute_tool('query_db', ...) will reject it as non-read-only at
       * execution time — even after the step sailed through review and was
       * Approved. Read-only sub-steps keep 'query_db'. */
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
  /* Creation/persistence is append-oriented.  Step rows are never DELETE +
   * rebuilt because their status/result/error fields are the CAS-protected
   * source of truth for approval execution. */
  if (!plan || !plan.plan_id || !plan.conversation_id) return false;
  var plan_id = plan.plan_id;
  var conv_id = plan.conversation_id;
  var status = plan.status || 'awaiting_approval';
  var current_step_index = Number(plan.current_step_index || 0);
  var total_steps = Number(plan.steps && plan.steps.length ? plan.steps.length : 0);
  var description = String(plan.description || '');
  var plan_json = JSON.stringify({ plan_id: plan_id, current_step_index: current_step_index, total_steps: total_steps });

  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_review_plan(plan_id,conversation_id,status,current_step_index,total_steps,description,plan_json) VALUES ('" +
        esc(plan_id) + "','" + esc(conv_id) + "','" + esc(status) + "'," +
        current_step_index + "," + total_steps + ", '" + esc(description) + "', '" + esc(plan_json) + "') " +
      "ON DUPLICATE KEY UPDATE status=VALUES(status), current_step_index=VALUES(current_step_index), " +
      "total_steps=VALUES(total_steps), description=VALUES(description), plan_json=VALUES(plan_json), " +
      "updated_at=CURRENT_TIMESTAMP"
    );

    for (var i = 0; i < (plan.steps || []).length; i++) {
      var step = plan.steps[i];
      sys.exec_sql(
        "INSERT IGNORE INTO mysql.agent_review_plan_step(" +
        "plan_id,step_no,tool,sql_text,args_json,affected_tables,writes,ddl,risk,estimated_rows,status,result_preview,error_text,transactional) VALUES (" +
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
        Number(step.transactional !== false ? 1 : 0) + ")"
      );
    }
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
    var raw = sys.exec_sql(
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
    var raw = sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET " +
      "tool='" + esc(step.tool || '') + "', " +
      "sql_text='" + esc(step.sql || '') + "', " +
      "args_json='" + esc(JSON.stringify(step.args || {})) + "', " +
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
      "ORDER BY created_at DESC LIMIT 1"
    );
    if (!Array.isArray(rows) || !rows.length || !rows[0].plan_id) return null;
    var plan = rows[0];
    var step_rows = query(
      "SELECT step_no, tool, sql_text, args_json, affected_tables, writes, ddl, risk, estimated_rows, status, result_preview, error_text, transactional " +
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
          /* Derived from the registry rather than persisted: the spec is the
           * authority and needs no extra columns on the plan-step table. */
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

    /* Crash recovery: a reviewed DML and its step.status='completed' may
     * commit atomically before the separate plan cursor update runs. Skip
     * already-completed steps so a retry can never execute them twice. */
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

/**
 * DEPRECATED — kept for backward compatibility with any external callers.
 * New code should use the affected_rows field returned by execute_tool()
 * (especially execute_tool('update_data', ...).affected_rows) instead of
 * the approximate information_schema.TABLE_ROWS statistic.
 */
function capture_table_counts(step, db) {
  if (!step || !Array.isArray(step.affected_tables) || !step.affected_tables.length || !db) return null;
  try {
    var names = step.affected_tables.map(function(t){ return "'" + esc(t) + "'"; }).join(',');
    var rows = query(
      "SELECT TABLE_NAME, TABLE_ROWS FROM information_schema.TABLES " +
      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN (" + names + ")"
    );
    if (!Array.isArray(rows)) return null;
    var counts = {};
    for (var i = 0; i < rows.length; i++) {
      counts[String(rows[i].TABLE_NAME || '')] = Number(rows[i].TABLE_ROWS || 0);
    }
    return counts;
  } catch (e) { return null; }
}

/**
 * DEPRECATED — see capture_table_counts above.
 */
function compare_table_counts(before_counts, after_counts) {
  if (!before_counts || !after_counts) return null;
  var lines = [];
  for (var table in before_counts) {
    if (!Object.prototype.hasOwnProperty.call(before_counts, table)) continue;
    var before = Number(before_counts[table] || 0);
    var after = Number(after_counts[table] || 0);
    if (before !== after) {
      lines.push(table + ': ' + before + ' -> ' + after);
    }
  }
  return lines.length ? lines.join('; ') : null;
}

function get_pending_review_step(plan) {
  if (!plan || !Array.isArray(plan.steps) || plan.steps.length === 0) return null;
  var idx = Number(plan.current_step_index || 0);
  if (idx < 0 || idx >= plan.steps.length) return null;
  return plan.steps[idx];
}

function update_review_step_status(plan_id, step_no, status, result_preview, error_text) {
  if (!plan_id || !step_no || !status) return;
  try {
    sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(status) + "', " +
      "result_preview='" + esc(result_preview || '') + "', " +
      "error_text='" + esc(error_text || '') + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no)
    );
  } catch (e) {}
}

function clear_review_state(conv_id, plan_id, final_status) {
  if (!conv_id || !plan_id) return;
  final_status = final_status || 'completed';
  try {
    sys.exec_sql(
      "UPDATE mysql.agent_review_plan SET status='" + esc(final_status) + "', updated_at=CURRENT_TIMESTAMP " +
      "WHERE plan_id='" + esc(plan_id) + "'"
    );
  } catch (e) {}
}

/**
 * Auto-cancel awaiting_approval plans that have sat untouched past
 * review_plan_ttl_minutes. Without this, a user who gets a pause prompt and
 * then wanders off to a different question stays permanently stuck: every
 * subsequent message hits the load_review_state() check at the top of
 * shannon_agent_run and gets reinterpreted as an Approve/Reject/Modify
 * command with no escape hatch. Mirrors cleanup_expired_tx_leases's
 * pattern/TTL semantics. Runs with no LIMIT, so it also sweeps up any
 * orphaned older plans for the same conversation (e.g. left behind by a
 * crash between saving a new plan and clearing the previous one) — not
 * just the single latest row that load_review_state would have looked at.
 */
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

/* The other way a plan gets stuck, and the one the sweep above cannot see.
 *
 * claim_review_step() sets a step to `executing` in its own autocommitted
 * UPDATE, before the statement it guards runs -- that ordering is what makes
 * the claim exclusive. If the server dies in the window between the claim
 * and the transition to completed/failed, the step stays `executing`
 * forever: the plan is no longer `awaiting_approval`, so the TTL sweep skips
 * it, and `executing` has no transition back to `pending`, so nothing else
 * moves it either. Every later message in that conversation is then read as
 * an approval command for a step that will never finish.
 *
 * Such a step is resolved to `indeterminate` rather than retried or failed,
 * because which of those is true is exactly what was lost. See the note on
 * the status in REVIEW_STEP_TRANSITIONS.
 *
 * The clock is the plan row's updated_at, which claim_review_step() touches
 * on every successful claim; the step table has no updated_at of its own.
 * The TTL is deliberately separate from and longer than the approval TTL: a
 * legitimately slow ALTER TABLE is still `executing` an hour in, and calling
 * that one in doubt would be a worse bug than the one being fixed. */
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
      /* CAS on status='executing' so a step that just finished on another
       * connection is not clobbered by this sweep. */
      var moved = false;
      try {
        var raw = sys.exec_sql(
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
  /* Terminal, and deliberately has no way back.
   *
   * A step reaches it when it was claimed as `executing` and the process
   * that claimed it never said what happened -- a crash, a kill, a lost
   * connection. The write it was running may have committed, may have
   * rolled back, and nothing on this side can tell which: the claim is
   * autocommitted before the statement runs, so its presence proves only
   * that execution started.
   *
   * Retrying it automatically is the one thing that must not happen. Half
   * of the time the statement did commit, and `UPDATE ... SET balance =
   * balance - 100` applied twice is worse than not applied at all. So the
   * plan stops here and a human decides, with the rollback log saying
   * exactly which statement is in doubt. */
  'indeterminate':      []
};

function get_step_status(plan_id, step_no) {
  try {
    var rows = query(
      "SELECT status FROM mysql.agent_review_plan_step " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no)
    );
    if (Array.isArray(rows) && rows.length > 0) return String(rows[0].status || 'pending');
  } catch (e) {}
  return 'pending';
}

function claim_review_step(plan_id, step_no, from_status, to_status) {
  /* Validate against the shared transition table — same source of truth
   * that transition_review_step uses via its reverse-lookup. */
  var allowed = REVIEW_STEP_TRANSITIONS[from_status];
  if (!allowed || allowed.indexOf(to_status) === -1) {
    try {
      sys.exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'claim_review_step REFUSED: " + esc(from_status) + " -> " + esc(to_status) + " not in REVIEW_STEP_TRANSITIONS', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  try {
    var raw = sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status='" + esc(from_status) + "'"
    );
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    var won = (raw && Number(raw.affected_rows) === 1);
    if (won) {
      /* Give recover_stuck_review_steps() a clock. The step table has no
       * updated_at, so the plan row's is what dates the claim; without this
       * touch a step claimed long after the plan was created would look
       * stale immediately. Best-effort: losing the touch costs a premature
       * indeterminate at worst, never a wrong execution. */
      try {
        sys.exec_sql("UPDATE mysql.agent_review_plan SET updated_at=NOW() " +
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
      sys.exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'No valid FROM status for transition to " + esc(to_status) + "', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  /* Single atomic UPDATE: check AND write in one statement */
  try {
    var raw = sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "', " +
      "result_preview='" + esc(result_preview || '') + "', " +
      "error_text='" + esc(error_text || '') + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status IN ('" + valid_from.map(esc).join("','") + "')"
    );
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    if (raw && Number(raw.affected_rows) === 1) return true;
  } catch (e) {}

  /* Transition refused — either the current status is not in valid_from
   * (concurrent claim_review_step changed it) or the row doesn't exist.
   * Log for audit and return false. */
  try {
    sys.exec_sql(
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
    sys.exec_sql(
      "INSERT INTO mysql.agent_review_history(conversation_id, plan_id, step_no, action, user_cmd, comment, result_preview, rollback_flag) VALUES (" +
      "'" + esc(conv_id) + "', '" + esc(plan_id) + "', " + Number(step_no || 0) + ", '" + esc(action) + "', '" + esc(user_cmd || '') + "', '" + esc(comment || '') + "', '" + esc(result_preview || '') + "', " +
      Number(rollback_flag ? 1 : 0) + ")"
    );
  } catch (e) {}
}

function log_rollback_event(conv_id, plan_id, step_no, sql_text, error, rollback_reason) {
  if (!conv_id || !plan_id) return;
  try {
    sys.exec_sql(
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
  if (!step || !step.tool) {
    return { ok: false, response: t('审批步骤无效。', 'Invalid approval step.'), error: 'invalid_step' };
  }

  var tx_started_here = false;
  var tx_ctx_before = get_tx_context();

  if ((step.writes || step.ddl) && step.transactional !== false) {
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
  try {
    res = execute_tool(step.tool, step.args || {}, db);
  } catch (e) {
    res = { ok: false, response: t('步骤执行异常：', 'Step execution exception: ') + String(e), error: 'exception' };
  }

  if (!res || !res.ok) {
    if (tx_started_here) execute_tool('rollback_tx', {}, db);
    return res || { ok: false, response: t('步骤执行失败。', 'Step execution failed.'), error: 'step_failed' };
  }

  /* If this function created the AGENT transaction, durable success is not
   * reported until COMMIT itself succeeds.  For reviewed transactional DML,
   * the completed transition is staged inside that same transaction so the
   * data change and review state become durable atomically. */
  if (tx_started_here) {
    /* For reviewed transactional writes, stage executing->completed in the
     * SAME InnoDB transaction as the user DML.  Although the UPDATE is issued
     * before COMMIT, "completed" does not become durable until that COMMIT,
     * eliminating the COMMIT-success/state-update crash window. */
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

  /* Prefer exact JSON.  The model is instructed to output one object, so
   * this path is both simpler and immune to braces inside JSON strings. */
  s = s.replace(/^\s*```(?:json)?\s*/i, '').replace(/\s*```\s*$/i, '').trim();
  try {
    var direct = JSON.parse(s);
    if (direct && typeof direct.tool === 'string') return direct;
  } catch (e0) {}

  /* Compatibility path for models that prepend prose. Track string/escape
   * state so SQL such as JSON_OBJECT('{...}') does not break brace depth. */
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
