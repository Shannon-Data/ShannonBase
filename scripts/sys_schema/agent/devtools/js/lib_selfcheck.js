//@include lib_ragcheck.js

//@include lib_tools.js
//@include lib_recall_eval.js
//@include lib_trustedcheck.js
// Self-check entry point behind sys.shannon_agent_selfcheck(kind, op, label).
function shannon_agent_selfcheck(kind, op, label) {
  kind = String(kind || 'tools').toLowerCase();
  if (kind === 'rag') return shannon_rag_selfcheck(String(op || 'context'));
  if (kind === 'trusted') return shannon_trusted_selfcheck(String(op || ''), label);
  if (kind === 'harness') return shannon_harness_selfcheck(String(op || ''), label);
  if (kind === 'runtime') return shannon_runtime_selfcheck(String(op || ''), label);
  if (kind === 'memory') return shannon_memory_selfcheck(op, label);
  if (kind === 'recall') return shannon_recall_selfcheck(op, label);

  if (kind === 'sqlmode') return shannon_sql_mode_selfcheck();
  if (kind === 'review') return shannon_review_selfcheck(op, label);
  if (kind === 'tools') {
    var problems = shannon_tool_selfcheck();
    var rows = [];
    for (var i = 0; i < problems.length; i++) rows.push(['result', String(problems[i])]);
    return rows;
  }
  return [['error', 'unknown selfcheck kind: ' + kind]];
}

// Write-path review state machine.
function shannon_review_selfcheck(op, label) {
  op = String(op || '').toLowerCase();
  var plan_id = 'selfchk_' + String(label || 'default');
  var conv_id = A.conversation_id || plan_id;
  var out = [];
  function row(k, v) { out.push([String(k), String(v)]); }

  if (op === 'cleanup') {
    row('removed', _review_seed_cleanup(plan_id));
    return out;
  }

  if (op === 'transitions') {

    _review_seed_plan(plan_id, conv_id, 'awaiting_approval');
    row('approve_once', claim_review_step(plan_id, 1, 'awaiting_approval', 'executing') ? 'won' : 'lost');

    row('approve_twice', claim_review_step(plan_id, 1, 'awaiting_approval', 'executing') ? 'won' : 'lost');
    row('status_after', _review_step_status(plan_id, 1));

    /* completed is terminal. */
    claim_review_step(plan_id, 1, 'executing', 'completed');
    row('completed_status', _review_step_status(plan_id, 1));
    row('resurrect_completed', claim_review_step(plan_id, 1, 'completed', 'executing') ? 'won' : 'lost');

    /* An edge absent from the table is refused before any SQL runs. */
    row('illegal_edge', claim_review_step(plan_id, 1, 'completed', 'pending') ? 'won' : 'lost');
    row('final_status', _review_step_status(plan_id, 1));
    return out;
  }

  if (op === 'rejected') {
    // A rejected step must stay rejected: nothing may promote it to executing behind the reviewer's back.
    _review_seed_plan(plan_id, conv_id, 'awaiting_approval');
    row('reject', claim_review_step(plan_id, 1, 'awaiting_approval', 'rejected') ? 'won' : 'lost');
    row('status', _review_step_status(plan_id, 1));
    row('execute_rejected', claim_review_step(plan_id, 1, 'rejected', 'executing') ? 'won' : 'lost');
    row('final_status', _review_step_status(plan_id, 1));
    return out;
  }

  if (op === 'stuck') {

    _review_seed_plan(plan_id, conv_id, 'awaiting_approval');
    claim_review_step(plan_id, 1, 'awaiting_approval', 'executing');
    row('before', _review_step_status(plan_id, 1));

    /* Age the plan past the TTL; the plan row's updated_at is the clock. */
    try {
      sys.exec_sql("UPDATE mysql.agent_review_plan SET updated_at = NOW() - INTERVAL 240 MINUTE" +
                   " WHERE plan_id='" + esc(plan_id) + "'");
    } catch (e) {}
    row('recovered', recover_stuck_review_steps(conv_id, 120));
    row('after', _review_step_status(plan_id, 1));

    /* A fresh claim must not sweep a step that is still inside its TTL. */
    _review_seed_plan(plan_id + '_fresh', conv_id, 'awaiting_approval');
    claim_review_step(plan_id + '_fresh', 1, 'awaiting_approval', 'executing');
    row('fresh_recovered', recover_stuck_review_steps(conv_id, 120));
    row('fresh_status', _review_step_status(plan_id + '_fresh', 1));
    _review_seed_cleanup(plan_id + '_fresh');
    return out;
  }

  return [['error', 'unknown review selfcheck op: ' + op]];
}

// One plan, one step, at a known status.
function _review_seed_plan(plan_id, conv_id, step_status) {
  _review_seed_cleanup(plan_id);
  sys.exec_sql(
    "INSERT INTO mysql.agent_review_plan(plan_id, conversation_id, status, total_steps, created_at, updated_at)" +
    " VALUES ('" + esc(plan_id) + "', '" + esc(conv_id) + "', 'awaiting_approval', 1, NOW(), NOW())");
  sys.exec_sql(
    "INSERT INTO mysql.agent_review_plan_step(plan_id, step_no, sql_text, status)" +
    " VALUES ('" + esc(plan_id) + "', 1, 'SELECT 1', '" + esc(step_status) + "')");
}

function _review_seed_cleanup(plan_id) {
  var n = 0;
  var stmts = [
    "DELETE FROM mysql.agent_review_plan_step WHERE plan_id='" + esc(plan_id) + "'",
    "DELETE FROM mysql.agent_review_plan WHERE plan_id='" + esc(plan_id) + "'",
    "DELETE FROM mysql.agent_rollback_log WHERE plan_id='" + esc(plan_id) + "'"
  ];
  for (var i = 0; i < stmts.length; i++) {
    try {
      var r = sys.exec_sql(stmts[i]);
      if (r && Number(r.affected_rows) > 0) n += Number(r.affected_rows);
    } catch (e) {}
  }
  return n;
}

function _review_step_status(plan_id, step_no) {
  try {
    var rows = query("SELECT status FROM mysql.agent_review_plan_step" +
                     " WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no));
    if (Array.isArray(rows) && rows.length) return String(rows[0].status);
  } catch (e) {}
  return 'missing';
}

// SQL-mode gate.
function shannon_sql_mode_selfcheck() {
  A.lang = 'en';
  var verdict = sql_mode_gate_check();
  var rows = [['ok', verdict.ok ? '1' : '0']];
  rows.push(['blocked_modes', (verdict.modes || []).join(',')]);

  var msg = String(verdict.message || '');
  rows.push(['message_names_mode',
             (!verdict.ok && verdict.modes.length && msg.indexOf(verdict.modes[0]) !== -1) ? '1' : '0']);
  rows.push(['message_is_actionable', (!verdict.ok && msg.indexOf('sql_mode') !== -1) ? '1' : '0']);
  return rows;
}

// Recall quality.
function shannon_recall_selfcheck(op, label) {
  op    = String(op || '').toLowerCase();
  label = String(label || 'default');

  if (!selfcheck_writes_allowed())
    return [['error', 'recall self-check writes to mysql.agent_semantic_fact; ' +
                      'SET @shannon_agent_selfcheck_allow_writes = 1 to allow it']];

  A.lang            = 'en';
  A.user_message    = '';
  A.memory_degraded = false;
  A.conversation_id = principal_scope_conversation_id('mtr-recall-' + label);
  A._mem_opt        = null;

  var mo  = get_memory_options(get_chat_options());
  var out = [];
  function row(k, v) { out.push([String(k), String(v)]); }

  if (op === 'seed') {
    var seeded = recall_eval_seed(label, mo);
    row('seeded', seeded.seeded);
    row('failed', seeded.failed);
    row('total',  seeded.total);
    return out;
  }

  if (op === 'stats') {
    var prefix = mem_principal_prefix();
    var rows = query("SELECT COUNT(*) AS docs, SUM(embedding IS NOT NULL) AS vectors" +
                     " FROM mysql.agent_semantic_fact" +
                     " WHERE principal_prefix='" + esc(prefix) + "'" +
                     "   AND scope='" + esc(recall_eval_scope(label)) + "'");
    var d = (Array.isArray(rows) && rows.length) ? rows[0] : { docs: 0, vectors: 0 };
    row('documents', Number(d.docs || 0));

    row('with_vector', Number(d.vectors || 0));
    row('queries', recall_eval_queries().length);
    return out;
  }

  if (op === 'cleanup') {
    row('removed', recall_eval_cleanup(label));
    return out;
  }

  if (op.indexOf('score') === 0) {
    /* 'score', 'score:lexical', 'score:lexical,hybrid:5' */
    var parts = op.split(':');
    var modes = (parts.length > 1 && parts[1]) ? parts[1] : 'lexical';
    var topK  = (parts.length > 2 && parts[2]) ? Number(parts[2]) : 3;
    return recall_eval_score(label, modes, topK, mo);
  }

  return [['error', 'unknown recall op: ' + op +
                    ' (seed|score[:modes[:k]]|stats|cleanup)']];
}

// Tool-contract self-check — the minimum viable evaluation harness.
function shannon_tool_selfcheck() {
  var out = [];

  /* --- 1. validation contract ----------------------------------------- */
  var cases = [
    { tool: 'query_db',       args: {},                        expect: 'query_db' },
    { tool: 'explain_sql',    args: { sql: 'SEL' },            expect: 'explain_sql' },
    { tool: 'run_ddl',        args: { sql: 'DROP TABLE t' },   expect: 'allow_destructive_ddl' },
    { tool: 'run_ddl',        args: { sql: 'SELECT 1 FROM t' },expect: 'run_ddl' },

    { tool: 'run_ddl',        args: { sql: "CREATE USER a@localhost IDENTIFIED BY 'p'" },
                                                               expect: 'allow_account_ddl' },
    { tool: 'run_ddl',        args: { sql: 'ALTER USER a@localhost IDENTIFIED BY \'p\'' },
                                                               expect: 'allow_account_ddl' },
    // The routine body is written with a quoted literal rather than the usual AS $-$ ...
    { tool: 'run_ddl',        args: { sql: 'CREATE FUNCTION f() RETURNS INT LANGUAGE JAVASCRIPT AS "return 1;"' },
                                                               expect: 'allow_code_ddl' },
    { tool: 'run_ddl',        args: { sql: 'CREATE EVENT e ON SCHEDULE EVERY 1 DAY DO SELECT 1' },
                                                               expect: 'allow_code_ddl' },

    { tool: 'run_ddl',        args: { sql: 'ALTER INSTANCE ROTATE INNODB MASTER KEY' },
                                                               expect: 'allow_instance_ddl' },
    { tool: 'run_ddl',        args: { sql: 'ALTER INSTANCE RELOAD TLS' },
                                                               expect: 'allow_instance_ddl' },
    // Both TYPE spellings have to land in the same class.
    { tool: 'run_ddl',        args: { sql: 'CREATE RESOURCE GROUP rg TYPE = SYSTEM' },
                                                               expect: 'allow_instance_ddl' },
    { tool: 'run_ddl',        args: { sql: 'CREATE RESOURCE GROUP rg TYPE = USER' },
                                                               expect: 'allow_instance_ddl' },

    { tool: 'run_ddl',        args: { sql: 'ALTER TABLE db.t SECONDARY_LOAD' }, expect: null },
    // CREATE TABLE user is schema DDL, not account DDL: the object keyword decides, and TABLE comes first.
    { tool: 'run_ddl',        args: { sql: 'CREATE TABLE user (id INT)' },      expect: null },
    { tool: 'plan_sql',       args: { steps: [] },             expect: 'plan_sql' },
    { tool: 'plan_sql',       args: { steps: [{ sql: 'DELETE FROM t WHERE id=1' }] }, expect: 'plan_sql steps[0]' },
    { tool: 'update_data',    args: {},                        expect: 'update_data' },
    { tool: 'ml_train',       args: { table_name: 'db.t' },    expect: 'target_column' },
    { tool: 'ml_train',       args: { table_name: 'db.t', target_column: 'y', task: 'nope' }, expect: 'task' },
    { tool: 'ml_predict_row', args: { model_handle: 'm' },     expect: 'data' },
    { tool: 'ml_model_active',args: { user: 'nobody' },        expect: 'ml_model_active' },
    { tool: 'ml_embed_table', args: { input_column: 'db.t.c', output_column: 'bad' }, expect: 'output_column' },
    { tool: 'remember_fact',  args: {},                        expect: 'remember_fact' },
    { tool: 'remember_fact',  args: { statement: 'x' },        expect: 'remember_fact' },
    { tool: 'describe_table', args: {},                        expect: 'describe_table' },
    /* Calls that must validate cleanly. */
    { tool: 'query_db',       args: { sql: 'SHOW TABLES' },    expect: null },
    { tool: 'describe_table', args: { table_names: ['a','b'] },expect: null },
    { tool: 'describe_table', args: { table_name: 'a' },       expect: null },
    { tool: 'list_tables',    args: {},                        expect: null },
    { tool: 'begin_tx',       args: {},                        expect: null },
    { tool: 'ml_model_load',  args: {},                        expect: null },
    { tool: 'ml_model_active',args: { user: 'CURRENT' },       expect: null },
    { tool: 'remember_fact',  args: { statement: '用户偏好中文回答' }, expect: null },
    { tool: 'read_artifact',  args: {},                        expect: 'read_artifact' },
    { tool: 'read_artifact',  args: { artifact_id: 'art_abc123' }, expect: null },
    { tool: 'describe_tool',  args: {},                        expect: 'describe_tool' },
    { tool: 'describe_tool',  args: { tool: 'ml_train' },      expect: null },
    // System-schema writes.
    { tool: 'run_ddl',        args: { sql: 'ALTER TABLE mysql.user ADD COLUMN x INT' },
                                                               expect: 'allow_system_schema_writes' },
    { tool: 'run_ddl',        args: { sql: 'CREATE TABLE sys.foo (id INT)' },
                                                               expect: 'allow_system_schema_writes' },
    /* Reading a system schema is not writing to one, and never was. */
    { tool: 'query_db',       args: { sql: 'SELECT * FROM mysql.user' },   expect: null },
    { tool: 'run_ddl',        args: { sql: 'CREATE TABLE shop.t2 (id INT)' }, expect: null },
    /* Unknown tool. */
    { tool: '__nope__',       args: {},                        expect: '__nope__' }
  ];

  var policy = get_review_policy(get_chat_options());
  for (var i = 0; i < cases.length; i++) {
    var c = cases[i];
    var verr = validate_tool_call({ tool: c.tool, args: c.args }, policy);
    var ok;
    if (c.expect === null) ok = (verr === null);
    else                   ok = !!verr && String(verr).indexOf(c.expect) !== -1;
    if (!ok)
      out.push('MISMATCH #' + i + ' ' + c.tool +
               ' expect=' + (c.expect === null ? '<valid>' : c.expect) +
               ' got=' + (verr === null ? '<valid>' : verr));
  }

  /* --- 2. every registered tool is executable and self-consistent ------ */
  var names = tool_names();
  for (var n = 0; n < names.length; n++) {
    var s = TOOL_REGISTRY[names[n]];
    if (typeof s.handler !== 'function') out.push('NO_HANDLER ' + s.name);
    if (!s.doc || !s.doc.zh || !s.doc.en) out.push('NO_DOC ' + s.name);
    if (s.write && !s.risk)               out.push('NO_RISK ' + s.name);
    if (s.readOnly && (s.write || s.ddl)) out.push('READONLY_CONFLICT ' + s.name);
  }

  // --- 3. prompt <-> registry consistency, in both languages ----------- Plus a size ceiling.
  var TOOL_DOCS_MAX_CHARS = 16000;
  var saved_lang = A.lang;
  var saved_msg  = A.user_message;
  var langs = ['zh', 'en'];
  for (var l = 0; l < langs.length; l++) {
    A.lang = langs[l];
    A.user_message = '';
    var docs = render_tool_docs(langs[l], { expand_all: true });
    if (docs.length > TOOL_DOCS_MAX_CHARS)
      out.push('DOC_TOO_LARGE ' + langs[l] + ' ' + docs.length +
               ' > ' + TOOL_DOCS_MAX_CHARS);
    for (var d = 0; d < names.length; d++) {
      if (TOOL_REGISTRY[names[d]].hidden) continue;
      if (docs.indexOf('"tool":"' + names[d] + '"') === -1)
        out.push('DOC_MISSING ' + langs[l] + ' ' + names[d]);
    }

    // The budgeted catalogue -- what a turn actually sends.
    var budgeted = render_tool_docs(langs[l], { message: 'show me the tables' });
    if (budgeted.length > docs.length)
      out.push('BUDGET_NOT_SMALLER ' + langs[l] + ' ' + budgeted.length + ' >= ' + docs.length);
    for (var b = 0; b < names.length; b++) {
      var bs = TOOL_REGISTRY[names[b]];
      if (bs.hidden) continue;
      if (budgeted.indexOf('"tool":"' + names[b] + '"') === -1 &&
          budgeted.indexOf(names[b] + '(') === -1)
        out.push('BUDGET_UNREACHABLE ' + langs[l] + ' ' + names[b]);
    }

    var ml_turn = render_tool_docs(langs[l], { message: '训练一个回归模型' });
    if (ml_turn.indexOf('"tool":"ml_train"') === -1)
      out.push('CATEGORY_NOT_REEXPANDED ' + langs[l] + ' ml');
  }
  A.lang = saved_lang;
  A.user_message = saved_msg;

  var dt = impl_describe_tool({ tool: 'ml_train' }, { chat_opt: {}, db: '' });
  if (!dt.ok || String(dt.response).indexOf('"tool":"ml_train"') === -1)
    out.push('DESCRIBE_TOOL_BROKEN');
  var dt_hidden = impl_describe_tool({ tool: 'forget_memory' }, { chat_opt: {}, db: '' });
  if (dt_hidden.ok) out.push('DESCRIBE_TOOL_EXPOSES_HIDDEN');

  /* --- 4. approval metadata is derivable for every ML write ----------- */
  for (var m = 0; m < names.length; m++) {
    var ms = TOOL_REGISTRY[names[m]];
    if (ms.category !== 'ml' || !ms.write) continue;
    var meta = tool_review_meta(ms.name);
    if (!meta)              out.push('NO_REVIEW_META ' + ms.name);
    else if (!meta.table_arg) out.push('NO_TABLE_ARG ' + ms.name);
  }

  var open_policy   = { allow_system_schema_writes: true };
  var closed_policy = { allow_system_schema_writes: false };
  var sys_cases = [
    { sql: "UPDATE mysql.user SET authentication_string='x' WHERE user='root'", db: 'shop', denied: true },
    { sql: "DELETE FROM mysql.agent_review_history WHERE id > 0",              db: 'shop', denied: true },
    { sql: "INSERT INTO sys.sys_config VALUES ('a','b',NOW(),'u')",            db: 'shop', denied: true },
    { sql: "TRUNCATE TABLE mysql.agent_memory",                                db: 'shop', denied: true },
    { sql: "DROP DATABASE mysql",                                              db: 'shop', denied: true },
    /* Unqualified, but the session is sitting in the mysql schema. */
    { sql: "DELETE FROM user WHERE user='bob'",                                db: 'mysql', denied: true },
    // The qualifier is still there; only the spelling changed.
    { sql: "INSERT INTO mysql . user (user) VALUES ('bob')",                   db: 'shop', denied: true },
    { sql: "DROP TABLE mysql . agent_memory",                                  db: 'shop', denied: true },

    { sql: "RENAME TABLE shop.t TO mysql.evil",                                db: 'shop', denied: true },
    { sql: "ALTER TABLE shop.t RENAME TO mysql.evil",                          db: 'shop', denied: true },
    /* CREATE INDEX names the index first and the table it alters after ON. */
    { sql: "CREATE INDEX idx ON mysql.user (user)",                            db: 'shop', denied: true },
    /* Reads of system schemas, and writes that only read from them. */
    { sql: "SELECT * FROM mysql.user",                                         db: 'shop', denied: false },
    { sql: "INSERT INTO shop.audit SELECT * FROM information_schema.TABLES",    db: 'shop', denied: false },
    { sql: "UPDATE shop.orders o JOIN shop.customers c ON o.cid=c.id SET o.n=1 WHERE o.id=1",
                                                                               db: 'shop', denied: false },
    { sql: "DELETE FROM shop.orders WHERE id=1",                               db: 'shop', denied: false },
    { sql: "CREATE INDEX idx ON shop.orders (status)",                         db: 'shop', denied: false },

    { sql: "RENAME TABLE shop.a TO shop.b",                                    db: 'shop', denied: false },
    { sql: "ALTER TABLE shop.t RENAME COLUMN a TO b",                          db: 'shop', denied: false },
    { sql: "ALTER TABLE shop.t RENAME INDEX i TO j",                           db: 'shop', denied: false },
    /* A system schema named only in a string literal is not a target. */
    { sql: "INSERT INTO shop.t (note) VALUES ('rename table x to mysql.y')",   db: 'shop', denied: false }
  ];
  for (var sc = 0; sc < sys_cases.length; sc++) {
    var got = check_system_schema_policy(sys_cases[sc].sql, closed_policy, sys_cases[sc].db);
    if (!!got !== sys_cases[sc].denied)
      out.push('SYS_SCHEMA_GATE #' + sc + ' expect=' + (sys_cases[sc].denied ? 'denied' : 'allowed') +
               ' got=' + (got ? ('denied:' + got.target) : 'allowed') + ' sql=' + sys_cases[sc].sql);
    /* The opt-in has to actually open it, or the flag is decoration. */
    if (check_system_schema_policy(sys_cases[sc].sql, open_policy, sys_cases[sc].db))
      out.push('SYS_SCHEMA_OPTIN_IGNORED #' + sc);
  }

  var read_cases = [
    /* Unbounded: must be refused. */
    { sql: 'SELECT * FROM fact_sales',                              deny: true  },
    { sql: 'SELECT a, b FROM t WHERE x = 1',                        deny: true  },
    { sql: 'WITH c AS (SELECT * FROM t) SELECT * FROM c',           deny: true  },
    { sql: 'SELECT * FROM a JOIN b ON a.id = b.id ORDER BY a.id',   deny: true  },
    /* Bounded: must pass. */
    { sql: 'SELECT * FROM fact_sales LIMIT 10',                     deny: false },
    { sql: 'SELECT * FROM t LIMIT 10, 20',                          deny: false },
    { sql: 'SELECT * FROM t LIMIT 20 OFFSET 10',                    deny: false },
    /* Implicit aggregate: exactly one row, no LIMIT needed. */
    { sql: 'SELECT COUNT(*) FROM fact_sales',                       deny: false },
    { sql: 'SELECT SUM(amount), AVG(amount) FROM orders',           deny: false },
    { sql: 'SELECT MAX(created_at) FROM events WHERE kind = 1',     deny: false },
    /* GROUP BY re-opens the row count, so the LIMIT is required again. */
    { sql: 'SELECT cat, COUNT(*) FROM t GROUP BY cat',              deny: true  },
    { sql: 'SELECT cat, COUNT(*) FROM t GROUP BY cat LIMIT 50',     deny: false },
    /* A set operation of aggregates is not one row. */
    { sql: 'SELECT COUNT(*) FROM a UNION SELECT COUNT(*) FROM b',   deny: true  },
    /* Over the ceiling. */
    { sql: 'SELECT * FROM t LIMIT 100000',                          deny: true  },
    /* Not a SELECT: bounded by the catalogue, not by user data. */
    { sql: 'SHOW TABLES',                                           deny: false },
    { sql: 'DESCRIBE orders',                                       deny: false },
    { sql: 'EXPLAIN SELECT * FROM t',                               deny: false },
    /* An aggregate name inside a string literal is not an aggregate. */
    { sql: "SELECT note FROM t WHERE note = 'COUNT(*)'",            deny: true  },

    { sql: 'SELECT x AS count FROM big',                            deny: true  },
    { sql: 'SELECT count FROM metrics WHERE id = 1',                deny: true  },
    { sql: 'SELECT a FROM sum WHERE b = 2',                         deny: true  },
    /* ...and the aggregate itself must still be recognised when it is one. */
    { sql: 'SELECT COUNT(*) AS count FROM big',                     deny: false }
  ];
  for (var rc = 0; rc < read_cases.length; rc++) {
    var rstmt = classify_statement(read_cases[rc].sql);
    var rgot  = !!guard_read_sql(read_cases[rc].sql, rstmt);
    if (rgot !== read_cases[rc].deny)
      out.push('READ_GUARD #' + rc + ' expect=' + (read_cases[rc].deny ? 'deny' : 'allow') +
               ' got=' + (rgot ? 'deny' : 'allow') + ' sql=' + read_cases[rc].sql);
  }

  var relax_cases = [
    /* key,                        operator baseline, session asks, expected */
    ['allow_destructive_ddl',      'false', true,  false],
    ['allow_destructive_ddl',      'true',  true,  true ],
    ['allow_destructive_ddl',      'true',  false, false],  /* session tightens */
    ['allow_system_schema_writes', 'false', true,  false],
    ['allow_account_ddl',          'false', true,  false],
    ['allow_instance_ddl',         'false', true,  false]
  ];
  for (var pc = 0; pc < relax_cases.length; pc++) {
    var rk = relax_cases[pc][0], base_v = relax_cases[pc][1];
    var want = relax_cases[pc][3];
    var base_obj = {};
    base_obj[rk] = base_v;
    var got_v = _combine_bool(base_obj, rk, relax_cases[pc][2], false);
    if (got_v !== want)
      out.push('POLICY_RELAX #' + pc + ' key=' + rk + ' base=' + base_v +
               ' session=' + relax_cases[pc][2] + ' expect=' + want + ' got=' + got_v);
  }
  /* require_* switches run the other way round: true is the safe value. */
  if (_combine_bool({ require_approval_for_write: 'true' }, 'require_approval_for_write', false, true) !== true)
    out.push('POLICY_RELAX require_approval_for_write could be switched off by the session');
  /* A baseline that says nothing must leave the session's choice alone. */
  if (_combine_bool({}, 'allow_destructive_ddl', true, false) !== true)
    out.push('POLICY_NO_OPINION baseline with no row changed the session value');
  /* Numeric ceilings take the smaller of the two. */
  if (_combine_min({ read_row_limit_max: '100' }, 'read_row_limit_max', 1000, 1) !== 100)
    out.push('POLICY_MIN operator ceiling did not lower the session value');
  if (_combine_min({ read_row_limit_max: '5000' }, 'read_row_limit_max', 1000, 1) !== 1000)
    out.push('POLICY_MIN operator ceiling raised the session value');

  if (_combine_min({ max_turns: '3' }, 'max_turns', MAX_TURNS_DEFAULT, 1) !== 3)
    out.push('POLICY_MIN operator max_turns did not lower the loop budget');
  if (_combine_min({ turn_deadline_ms: '999' }, 'turn_deadline_ms',
                   TURN_DEADLINE_MS_DEFAULT, 10000) !== 10000)
    out.push('POLICY_MIN turn_deadline_ms floor was not applied');

  var rag_cases = [
    ['mysql.agent_memory',          true ],
    ['mysql.agent_semantic_fact',   true ],
    ['mysql.agent_sql_trace',       true ],
    ['MySQL.Agent_Memory',          true ],   /* schema names are not case sensitive here */
    ['`mysql`.`agent_memory`',      true ],   /* nor is it fooled by quoting */
    ['sys.ml_model_catalog',        true ],
    ['information_schema.COLUMNS',  true ],
    ['performance_schema.threads',  true ],
    ['agent_memory',                true ],   /* unqualified: resolves in the session's own db */
    ['kb.documents',                false],
    ['myapp.agent_notes',           false]    /* a user table is not an agent table */
  ];
  for (var gc = 0; gc < rag_cases.length; gc++) {
    var got_blocked = rag_store_blocked(rag_cases[gc][0]);
    if (got_blocked !== rag_cases[gc][1])
      out.push('RAG_TARGET #' + gc + ' store=' + rag_cases[gc][0] +
               ' expect=' + (rag_cases[gc][1] ? 'blocked' : 'allowed') +
               ' got=' + (got_blocked ? 'blocked' : 'allowed'));
  }
  var rag_split = rag_filter_vector_store(['kb.docs', 'mysql.agent_memory', 'kb.more']);
  if (rag_split.allowed.join(',') !== 'kb.docs,kb.more' ||
      rag_split.blocked.join(',') !== 'mysql.agent_memory')
    out.push('RAG_TARGET filter kept the wrong set: allowed=[' + rag_split.allowed.join(',') +
             '] blocked=[' + rag_split.blocked.join(',') + ']');

  if (rag_filter_vector_store(undefined).blocked.length)
    out.push('RAG_TARGET filter invented a blocked store from no input');

  var saved_deadline = A.turn_deadline_at;
  A.turn_deadline_at = 0;
  if (llm_attempt_timeout_ms('openai') !== 0)
    out.push('LLM_TIMEOUT a call with no deadline in force must keep the backend default');
  A.turn_deadline_at = Date.now() + 30000;
  var tmo_cloud = llm_attempt_timeout_ms('openai');
  if (tmo_cloud * LLM_MAX_ATTEMPTS > 30000)
    out.push('LLM_TIMEOUT retries do not fit in the remaining deadline: ' + tmo_cloud);
  A.turn_deadline_at = Date.now() - 1000;
  if (llm_attempt_timeout_ms('openai') !== 1)
    out.push('LLM_TIMEOUT an expired deadline received more than the minimum timer tick');
  A.turn_deadline_at = Date.now() + 3600000;
  if (llm_attempt_timeout_ms('ollama') !== 30000)
    out.push('LLM_TIMEOUT a long deadline raised the backend default instead of leaving it');
  A.turn_deadline_at = saved_deadline;

  var saved_pol = A.operator_policy;

  A.operator_policy = { allow_endpoint_override: 'false' };
  var eg1 = apply_egress_policy({ provider: 'openai', endpoint: 'http://elsewhere/v1' });
  if (eg1.endpoint !== undefined)
    out.push('EGRESS a locked endpoint survived the session override');
  if (eg1.provider !== 'openai')
    out.push('EGRESS locking the endpoint also dropped the provider');

  A.operator_policy = { allow_remote_provider: 'false' };
  var eg2 = apply_egress_policy({ provider: 'openai', endpoint: 'http://elsewhere/v1',
                                  api_key: 'k', workspace_id: 'w', region: 'r',
                                  api_config: 'c', model_id: 'local' });
  for (var egk = 0; egk < EGRESS_KEYS.length; egk++)
    if (eg2[EGRESS_KEYS[egk]] !== undefined)
      out.push('EGRESS local-only left ' + EGRESS_KEYS[egk] + ' in the options');
  if (eg2.model_id !== 'local')
    out.push('EGRESS local-only dropped the local model id as well');

  A.operator_policy = {};
  if (apply_egress_policy({ endpoint: 'http://elsewhere/v1' }).endpoint === undefined)
    out.push('EGRESS an empty baseline dropped the endpoint anyway');

  // The nesting sys.ML_RAG reads.
  A.operator_policy = { allow_endpoint_override: 'false' };
  var eg3 = apply_egress_policy_nested({
    endpoint: 'http://elsewhere/v1',
    model_options: { provider: 'openai', endpoint: 'http://elsewhere/v1' }
  });
  if (eg3.endpoint !== undefined)
    out.push('EGRESS_NESTED top-level endpoint survived');
  if (eg3.model_options.endpoint !== undefined)
    out.push('EGRESS_NESTED model_options endpoint survived');
  A.operator_policy = {};
  if (apply_egress_policy_nested({ endpoint: 'http://elsewhere/v1' }).endpoint !== undefined)
    out.push('EGRESS_NESTED a batch blob kept a destination no batch routine reads');

  A.operator_policy = _restrictive_policy();
  if (apply_egress_policy({ provider: 'openai', endpoint: 'http://elsewhere/v1' }).endpoint !== undefined)
    out.push('EGRESS the restrictive fallback still let the session choose the host');

  A.operator_policy = { allowed_providers: 'ollama' };
  if (apply_egress_policy({ provider: 'ollama', endpoint: 'http://elsewhere/v1' }).endpoint !== undefined)
    out.push('EGRESS an allow-list left the endpoint open');
  var eg_threw = false;
  try { apply_egress_policy({ provider: 'openai' }); } catch (e) { eg_threw = true; }
  if (!eg_threw)
    out.push('EGRESS a provider outside the allow-list was accepted');
  A.operator_policy = saved_pol;

  var sel = { first_keyword: 'SELECT' };
  var tmo_cases = [
    /* A model opting out of the ceiling is clamped back to it. */
    { sql: 'SELECT /*+ MAX_EXECUTION_TIME(0) */ a FROM t LIMIT 1',
      want: 'SELECT /*+ MAX_EXECUTION_TIME(5000) */ a FROM t LIMIT 1' },
    /* A stricter value is the caller's own, and is left alone. */
    { sql: 'SELECT /*+ MAX_EXECUTION_TIME(100) */ a FROM t LIMIT 1',
      want: 'SELECT /*+ MAX_EXECUTION_TIME(100) */ a FROM t LIMIT 1' },
    /* A looser one is not. */
    { sql: 'SELECT /*+ MAX_EXECUTION_TIME(99999) */ a FROM t LIMIT 1',
      want: 'SELECT /*+ MAX_EXECUTION_TIME(5000) */ a FROM t LIMIT 1' },
    /* An existing hint block keeps its other hints. */
    { sql: 'SELECT /*+ SET_VAR(sort_buffer_size=16M) */ a FROM t LIMIT 1',
      want: 'SELECT /*+ MAX_EXECUTION_TIME(5000) SET_VAR(sort_buffer_size=16M) */ a FROM t LIMIT 1' },
    /* No hint block at all: one is added. */
    { sql: 'SELECT a FROM t LIMIT 1',
      want: 'SELECT /*+ MAX_EXECUTION_TIME(5000) */ a FROM t LIMIT 1' },
    // The literal cases.
    { sql: "SELECT note FROM t WHERE note LIKE '%MAX_EXECUTION_TIME(0)%' LIMIT 1",
      want: "SELECT /*+ MAX_EXECUTION_TIME(5000) */ note FROM t WHERE note LIKE '%MAX_EXECUTION_TIME(0)%' LIMIT 1" },
    { sql: "SELECT /*+ MAX_EXECUTION_TIME(0) */ note FROM t WHERE note = 'MAX_EXECUTION_TIME(0)' LIMIT 1",
      want: "SELECT /*+ MAX_EXECUTION_TIME(5000) */ note FROM t WHERE note = 'MAX_EXECUTION_TIME(0)' LIMIT 1" }
  ];
  for (var tc = 0; tc < tmo_cases.length; tc++) {
    var tgot = with_read_timeout(tmo_cases[tc].sql, sel, 5000);
    if (tgot !== tmo_cases[tc].want)
      out.push('READ_TIMEOUT #' + tc + ' got=' + tgot + ' want=' + tmo_cases[tc].want);
  }

  // --- 4d. the stop-reason taxonomy ----------------------------------- Only 'finish' may be silent.
  var stop_reasons = ['max_turns', 'truncated', 'error_budget', 'loop_detected',
                      'context_exhausted', 'empty_completion', 'deadline'];
  for (var sr = 0; sr < stop_reasons.length; sr++) {
    if (!stop_reason_note(stop_reasons[sr]))
      out.push('STOP_REASON_SILENT ' + stop_reasons[sr]);
  }
  if (stop_reason_note('finish'))
    out.push('STOP_REASON_NOISY finish should say nothing');

  /* --- 4e. tool hooks: observers fail open, enforcers fail closed ------ */
  var saved_pre_hooks = HOOKS.pre_tool_use;
  HOOKS.pre_tool_use = [];

  var hook_args = { tool: 'describe_tool' };
  var hook_base = execute_tool('describe_tool', hook_args, '');
  if (!hook_base || !hook_base.ok) out.push('HOOK_BASELINE describe_tool failed');

  /* Observer that throws: audited, skipped, call proceeds. */
  register_hook('pre_tool_use', function () { throw new Error('observer bug'); });
  var hook_obs = execute_tool('describe_tool', hook_args, '');
  if (!hook_obs || !hook_obs.ok) out.push('HOOK_OBSERVER a throwing observer blocked the call');
  HOOKS.pre_tool_use = [];

  /* Enforcer that throws: refused. */
  register_hook('pre_tool_use', function () { throw new Error('policy engine down'); },
                { enforce: true });
  var hook_enf = execute_tool('describe_tool', hook_args, '');
  if (!hook_enf || hook_enf.ok || hook_enf.error !== 'mandatory_hook_failed')
    out.push('HOOK_ENFORCER a failing mandatory policy did not fail closed: ' +
             (hook_enf && hook_enf.error));
  HOOKS.pre_tool_use = [];

  /* A denial is still a return value, not a throw. */
  register_hook('pre_tool_use', function () { return { deny: true, response: 'no' }; });
  var hook_deny = execute_tool('describe_tool', hook_args, '');
  if (!hook_deny || hook_deny.ok || hook_deny.error !== 'denied_by_hook')
    out.push('HOOK_DENY an explicit denial was not honoured');
  HOOKS.pre_tool_use = [];

  /* Substituted args go back through schema + spec.validate(). */
  register_hook('pre_tool_use', function () { return { args: {} }; });
  var hook_bad = execute_tool('describe_tool', hook_args, '');
  if (!hook_bad || hook_bad.ok || hook_bad.error !== 'hook_args_invalid')
    out.push('HOOK_REWRITE args substituted by a hook skipped validation');
  HOOKS.pre_tool_use = [];

  /* ...and a valid rewrite still reaches the handler. */
  register_hook('pre_tool_use', function () { return { args: { tool: 'query_db' } }; });
  var hook_ok = execute_tool('describe_tool', hook_args, '');
  if (!hook_ok || !hook_ok.ok || hook_ok.tool !== 'query_db')
    out.push('HOOK_REWRITE a valid argument rewrite did not reach the handler');

  HOOKS.pre_tool_use = saved_pre_hooks;

  selfcheck_transaction_cleanup(out);
  var unknown_arg = ts_validate({ type: 'object', properties: { sql: { type: 'string' } },
    additionalProperties: false }, { sql: 'SELECT 1', endpoint: 'http://blocked' });
  if (!unknown_arg || unknown_arg.keyword !== 'additionalProperties') out.push('UNKNOWN_TOOL_ARGUMENT');
  var unsafe_reads = ["SELECT 1 INTO OUTFILE '/tmp/blocked'", "SELECT 1 INTO DUMPFILE '/tmp/blocked'",
    "SELECT /*!50000 INTO OUTFILE '/tmp/blocked' */ 1", "LOAD DATA INFILE 'x' INTO TABLE t",
    'CALL unsafe_procedure()', "SELECT LOAD_FILE('/tmp/blocked')"];
  for (var ur = 0; ur < unsafe_reads.length; ur++) {
    var classified = classify_statement(unsafe_reads[ur]);
    if (classified.is_read || !classified.external_side_effects) out.push('SQL_SIDE_EFFECT ' + ur);
  }
  if (!classify_statement("SELECT 'INTO OUTFILE' AS literal").is_read) out.push('SQL_LITERAL_SIDE_EFFECT');

  /* --- 4f. lookups that must not depend on the session database ------ */
  var dt_q = tool_describe_table('', { table_name: 'sys.sys_config' });
  if (!dt_q.ok)
    out.push('DESCRIBE_QUALIFIED a schema-qualified name was refused without a default database: ' +
             (dt_q.error || dt_q.response));
  var dt_u = tool_describe_table('', { table_name: 'sys_config' });
  if (dt_u.ok || dt_u.error !== 'no_db')
    out.push('DESCRIBE_UNQUALIFIED an unqualified name was resolved without a default database');
  var ml_list = impl_ml_list_models({}, { db: 'not_the_user' });
  if (ml_list && ml_list.error === 'ml_list_models_invalid_db')
    out.push('ML_LIST_MODELS still derives the catalog schema from the current database');

  /* --- 5. the manifest round-trips ------------------------------------ */
  try {
    var manifest = tool_manifest();
    if (manifest.length !== names.length) out.push('MANIFEST_SIZE');
    JSON.stringify(manifest);
  } catch (e) {
    out.push('MANIFEST_ERROR ' + String(e));
  }

  return out.length ? out : ['OK'];
}

// Fault injection stays in devtools and never executes transaction SQL.
function selfcheck_transaction_cleanup(out) {
  var saved = { query: query, context: get_tx_context, state: get_session_tx_state,
    mirror: set_tx_active_for, marker: set_agent_tx_event_marker,
    clear: clear_tx_lease, begin: begin_tx_lease, audit: audit_tx_failure };
  var effects = [], context, after, reject, lease_ok;
  function reset(owner, state, denied, clear_ok) {
    effects = [];
    context = { active: owner !== TX_OWNER_NONE, owner: owner, known: true };
    after = state; reject = denied; lease_ok = clear_ok;
  }
  function cleared() {
    return effects.indexOf('mirror:false') >= 0 || effects.indexOf('marker:0') >= 0 ||
           effects.indexOf('lease:clear') >= 0;
  }
  try {
    query = function (sql) {
      effects.push(sql);
      return reject ? { error: 'injected transaction SQL failure' } : [];
    };
    get_tx_context = function () { return context; };
    get_session_tx_state = function () { return after; };
    set_tx_active_for = function (id, active) { effects.push('mirror:' + active); };
    set_agent_tx_event_marker = function (id) { effects.push('marker:' + id); };
    clear_tx_lease = function () { effects.push('lease:clear'); return lease_ok; };
    begin_tx_lease = function () { return true; };
    audit_tx_failure = function () {};

    var unverified = [{ known: false, active: false }, { known: true, active: true }];
    for (var i = 0; i < unverified.length; i++) {
      reset(TX_OWNER_AGENT, unverified[i], false, true);
      var uncertain = rollback_agent_transaction('selfcheck');
      if (uncertain.ok || uncertain.error !== 'rollback_not_verified' || cleared())
        out.push('TX_ROLLBACK_UNVERIFIED ownership evidence was discarded: ' + i);
    }
    reset(TX_OWNER_AGENT, { known: true, active: true }, true, true);
    var denied = rollback_agent_transaction('selfcheck');
    if (denied.ok || denied.error !== 'rollback_failed' || cleared())
      out.push('TX_ROLLBACK_REJECTED a returned SQL error was treated as success');

    reset(TX_OWNER_CALLER, { known: true, active: true }, false, true);
    var caller = rollback_agent_transaction('selfcheck');
    if (caller.ok || caller.error !== 'transaction_not_owned' || effects.length)
      out.push('TX_CALLER_OWNERSHIP the caller transaction was touched');

    reset(TX_OWNER_AGENT, { known: true, active: false }, false, true);
    var closed = rollback_agent_transaction('selfcheck');
    if (!closed.ok || effects.indexOf('lease:clear') < 0 || effects.indexOf('marker:0') < 0)
      out.push('TX_ROLLBACK_CONFIRMED verified closure did not clear ownership');

    reset(TX_OWNER_AGENT, { known: true, active: false }, false, false);
    var retained = rollback_agent_transaction('selfcheck');
    if (!retained.ok || !retained.lease_retained)
      out.push('TX_LEASE_CLEANUP a failed lease delete was hidden');

    reset(TX_OWNER_NONE, { known: false, active: false }, false, true);
    var started = impl_begin_tx({}, {});
    if (started.ok || started.error !== 'begin_tx_unverified' || cleared() ||
        effects.indexOf('ROLLBACK') < 0)
      out.push('TX_BEGIN_UNVERIFIED ownership was dropped without confirmed closure');
  } catch (e) {
    out.push('TX_CLEANUP_SELFCHECK ' + String(e));
  } finally {
    query = saved.query; get_tx_context = saved.context; get_session_tx_state = saved.state;
    set_tx_active_for = saved.mirror; set_agent_tx_event_marker = saved.marker;
    clear_tx_lease = saved.clear; begin_tx_lease = saved.begin; audit_tx_failure = saved.audit;
  }
}

// Memory-layer self-check driver.
function selfcheck_pad(tag, n) {
  var p = tag;
  while (p.length < n) p += ' ' + tag;
  return p.substring(0, n);
}

function selfcheck_rank_list(names) {
  var a = [];
  for (var i = 0; i < names.length; i++) a.push({ statement: names[i] });
  return a;
}
function selfcheck_rank_tags(items) {
  var o = [];
  for (var i = 0; i < items.length; i++) o.push(items[i].statement);
  return o.join(',');
}
function selfcheck_rank_key(x) { return x.statement; }

function selfcheck_writes_allowed() {
  var rows = query("SELECT COALESCE(@shannon_agent_selfcheck_allow_writes, 0) AS v");
  return !!(Array.isArray(rows) && rows.length && Number(rows[0].v) === 1);
}

function shannon_memory_selfcheck(op, label) {
  op    = String(op || '').toLowerCase();
  label = String(label || 'default');

  if (!selfcheck_writes_allowed())
    return [['error', 'memory self-check writes to mysql.agent_*; ' +
                      'SET @shannon_agent_selfcheck_allow_writes = 1 to allow it']];

  A.lang            = 'en';
  A.user_message    = '';
  A.memory_degraded = false;
  A.conversation_id = principal_scope_conversation_id('mtr-mem-' + label);
  A._mem_opt        = null;

  var conv   = A.conversation_id;
  var prefix = mem_principal_prefix();
  var mo     = get_memory_options(get_chat_options());
  var out    = [];

  function row(k, v) { out.push([String(k), String(v)]); }

  if (op === 'seed') {
    // Five turns written back to back.
    for (var i = 1; i <= 5; i++) {
      var q = 'question ' + i + ' for ' + label;
      if (i === 3) q = q + ' my key is sk-ABCDEF123456 please keep it';
      MEM.short.append_turn(conv, q, 'answer ' + i + ' for ' + label,
                            'thought ' + i, { route: 'agent_loop' });
    }

    MEM.long.write_fact('fact-' + label + ': the user prefers concise answers',
                        { scope: 'mtr', confidence: 90 }, mo);
    MEM.long.write_fact('fact-' + label + ': the user prefers concise answers',
                        { scope: 'mtr', confidence: 90 }, mo);

    row('conversation_scoped', /^[0-9a-f]{16}:[0-9a-f]{47}$/.test(conv) ? 'yes' : 'no');
    row('principal_prefix_len', prefix.length);
    row('degraded', A.memory_degraded ? 'yes' : 'no');
    row('degraded_reason', A.memory_degraded_reason || '');

  } else if (op === 'verify') {
    function one(sql, key) {
      var r = query(sql);
      row(key, (Array.isArray(r) && r.length) ? r[0].c : 'error');
    }
    one("SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE conversation_id='" +
        esc(conv) + "'", 'total_rows');

    one("SELECT COUNT(*) AS c FROM (SELECT turn_no FROM mysql.agent_memory" +
        " WHERE conversation_id='" + esc(conv) + "' GROUP BY turn_no" +
        " HAVING SUM(role='user') <> 1 OR SUM(role='assistant') <> 1) t",
        'incomplete_turns');
    var seqs = query(
      "SELECT MIN(seq) AS mn, MAX(seq) AS mx, COUNT(DISTINCT seq) AS d" +
      " FROM mysql.agent_memory WHERE conversation_id='" + esc(conv) + "'");
    if (Array.isArray(seqs) && seqs.length) {
      row('min_seq', seqs[0].mn);
      row('max_seq', seqs[0].mx);
      row('distinct_seq', seqs[0].d);
    }
    one("SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE conversation_id='" +
        esc(conv) + "' AND (document_name IS NULL OR document_name <> '" +
        esc(prefix) + "')", 'rows_without_isolation_key');
    one("SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE conversation_id='" +
        esc(conv) + "' AND content LIKE '%sk-ABCDEF%'", 'rows_leaking_secret');
    one("SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE conversation_id='" +
        esc(conv) + "' AND content LIKE '%[REDACTED]%'", 'rows_redacted');
    var au = query(
      "SELECT op, tier FROM mysql.agent_memory_audit WHERE conversation_id='" +
      esc(conv) + "' GROUP BY op, tier ORDER BY op, tier");
    if (Array.isArray(au))
      for (var a = 0; a < au.length; a++)
        row('audit' + (a + 1), au[a].op + '/' + au[a].tier);

  } else if (op === 'load') {

    var loaded = MEM.short.load(conv, mo);
    row('degraded', loaded.degraded ? 'yes' : 'no');
    row('turn_rows', loaded.turns.length);
    for (var n = 0; n < loaded.turns.length; n++)
      row('row' + (n + 1), loaded.turns[n].role + '|' + loaded.turns[n].turn_no +
                           '|' + loaded.turns[n].content);

  } else if (op === 'isolation') {

    var mine = query(
      "SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE document_name IN ('" + esc(prefix) + "')" +
      " AND content LIKE '%for " + esc_like(label) + "%'");
    var others = query(
      "SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE (document_name IS NULL" +
      " OR document_name NOT IN ('" + esc(prefix) + "')) AND content LIKE '%for " + esc_like(label) + "%'");
    var my_facts = query(
      "SELECT COUNT(*) AS c FROM mysql.agent_semantic_fact" +
      " WHERE principal_prefix='" + esc(prefix) + "' AND statement LIKE 'fact-" + esc_like(label) + "%'");
    var my_conv = query(
      "SELECT COUNT(*) AS c FROM mysql.agent_memory WHERE conversation_id='" + esc(conv) + "'");
    row('principal_prefix_len',  prefix.length);
    row('visible_to_recall',     Array.isArray(mine)     && mine.length     ? mine[0].c     : 'error');
    row('present_but_isolated',  Array.isArray(others)   && others.length   ? others[0].c   : 'error');
    row('own_conversation_rows', Array.isArray(my_conv)  && my_conv.length  ? my_conv[0].c  : 'error');
    row('own_facts',             Array.isArray(my_facts) && my_facts.length ? my_facts[0].c : 'error');

  } else if (op === 'recall_facts') {
    var facts = MEM.long.recall_facts('concise answers', mo);
    row('recalled', facts.length);
    for (var f = 0; f < facts.length; f++) row('fact' + (f + 1), facts[f].statement);

  } else if (op === 'redact') {
    row('redacted', MEM.redact('token sk-ABCDEF123456 and password = hunter2', mo));

  } else if (op === 'compact') {
    // Under the summarize_after_turns threshold this must be a no-op and must not call the model at all.
    row('compacted', MEM.short.compact(conv, mo) ? 'yes' : 'no');

  } else if (op === 'expire') {
    try {
      query_checked("UPDATE mysql.agent_memory SET expires_at=DATE_SUB(NOW(), INTERVAL 1 DAY)" +
                   " WHERE conversation_id='" + esc(conv) + "'");
      query_checked("UPDATE mysql.agent_semantic_fact SET expires_at=DATE_SUB(NOW(), INTERVAL 1 DAY)" +
                   " WHERE principal_prefix='" + esc(prefix) + "' AND statement LIKE 'fact-" +
                   esc_like(label) + "%'");
    } catch (e) { row('expire_error', String(e)); }
    row('purged', MEM.long.purge_expired(mo));
    var left = query("SELECT COUNT(*) AS c FROM mysql.agent_memory" +
                     " WHERE conversation_id='" + esc(conv) + "'");
    row('rows_left', (Array.isArray(left) && left.length) ? left[0].c : 'error');

  } else if (op === 'forget') {
    var res = MEM.long.forget({ contains: 'fact-' + label }, mo);
    row('ok', res.ok ? 'yes' : 'no');
    row('removed', res.removed);
    var bad = MEM.long.forget({}, mo);
    row('unfiltered_ok', bad.ok ? 'yes' : 'no');
    row('unfiltered_error', bad.error || '');

  } else if (op === 'budget') {

    var secs = [];
    mem_block_section(secs, selfcheck_pad('S-summary', 300), 2);
    mem_block_section(secs, selfcheck_pad('R-recent',  300), 1);
    mem_block_section(secs, selfcheck_pad('F-fewshot', 300), 5);

    var bo = get_memory_options(get_chat_options());
    bo.budget.max_block_tokens = 250;          /* fits two 100-token sections */

    var block = mem_block_assemble(secs, bo);
    row('kept_recent',     block.indexOf('R-recent')  !== -1 ? 'yes' : 'no');
    row('kept_summary',    block.indexOf('S-summary') !== -1 ? 'yes' : 'no');
    row('dropped_fewshot', block.indexOf('F-fewshot') === -1 ? 'yes' : 'no');
    row('reading_order',
        block.indexOf('S-summary') < block.indexOf('R-recent') ? 'yes' : 'no');
    row('tokens_within_cap', A.mem_block_tokens <= 250 ? 'yes' : 'no');

    A._mem_opt = null;
    var lo = get_memory_options(get_chat_options());
    MEM.build_block('question 1 for ' + label, conv, lo);
    row('real_block_counted',
        Number(A.mem_block_tokens || 0) > 0 ? 'yes' : 'no');
    row('real_block_within_default_cap',
        Number(A.mem_block_tokens || 0) <= Number(lo.budget.max_block_tokens)
          ? 'yes' : 'no');

  } else if (op === 'rank') {
    // Ranking is asserted on the pure functions, not on a live recall.
    var rk_def = MEM.rank.options(mo);

    row('default_active',  MEM.rank.active(rk_def) ? 'yes' : 'no');
    row('default_lambda',  rk_def.lambda);

    var sig = { columns: ['use_count', 'last_used_at', 'created_at'],
                recency:    'COALESCE(last_used_at, created_at)',
                importance: { expr: 'confidence', max: 100 },
                usage:      'use_count' };
    row('default_sql', MEM.rank.sql(rk_def, sig));

    var rk_on = MEM.rank.options(MEM.merge_defaults(mo, { long_term: { ranking: {
      weight_recency: 0.3, weight_importance: 0.2, weight_usage: 0.1 } } }));
    row('weighted_active', MEM.rank.active(rk_on) ? 'yes' : 'no');
    var expr = MEM.rank.sql(rk_on, sig);
    row('has_recency',    expr.indexOf('TIMESTAMPDIFF') !== -1 ? 'yes' : 'no');
    row('has_importance', expr.indexOf('confidence')    !== -1 ? 'yes' : 'no');
    row('has_usage',      expr.indexOf('use_count')     !== -1 ? 'yes' : 'no');

    row('absent_signal_absent',
        MEM.rank.sql(rk_on, { recency: null, importance: null, usage: null })
          .indexOf('TIMESTAMPDIFF') === -1 ? 'yes' : 'no');

    row('inner_list', MEM.rank.inner_list('fact_id, statement, confidence, scope', sig.columns));

    // MMR: three near-identical candidates and one different one.
    var cand = [
      { txt: 'total revenue by region for last quarter',  rel: 1.00 },
      { txt: 'total revenue by region for last quarter!', rel: 0.99 },
      { txt: 'total revenue by region last quarter',      rel: 0.98 },
      { txt: 'how many employees joined in March',        rel: 0.60 }
    ];
    function cand_txt(c) { return c.txt; }
    function cand_rel(c) { return c.rel; }
    var pure = mem_select_candidates(cand,2,1,cand_txt,cand_rel);
    var divd = mem_select_candidates(cand,2,0.5,cand_txt,cand_rel);
    row('candidate_pool_bounded',mem_candidate_limit(30,0.5)===50 && mem_candidate_limit(2,0.5)===6 && mem_candidate_limit(2,1)===2 ? 'yes' : 'no');
    row('pure_relevance_2nd', pure[1].txt);
    row('diversified_2nd',    divd[1].txt);

    row('top_hit_kept', (pure[0].txt === cand[0].txt && divd[0].txt === cand[0].txt)
          ? 'yes' : 'no');
    row('near_dupes_similar',
        MEM.rank.similarity(MEM.rank.tokens(cand[0].txt),
                            MEM.rank.tokens(cand[1].txt)) > 0.8 ? 'yes' : 'no');
    row('unrelated_dissimilar',
        MEM.rank.similarity(MEM.rank.tokens(cand[0].txt),
                            MEM.rank.tokens(cand[3].txt)) < 0.2 ? 'yes' : 'no');

  } else if (op === 'conflict') {

    row('dup_key_is_conflict',
        mem_is_write_conflict("Duplicate entry 'c-3' for key 'agent_memory.uk_conv_seq'")
          ? 'yes' : 'no');
    row('deadlock_is_conflict',
        mem_is_write_conflict('Deadlock found when trying to get lock; try restarting transaction')
          ? 'yes' : 'no');
    row('lock_wait_is_conflict',
        mem_is_write_conflict('Lock wait timeout exceeded; try restarting transaction')
          ? 'yes' : 'no');
    row('denied_is_not_conflict',
        mem_is_write_conflict("INSERT command denied to user 'x'@'localhost'")
          ? 'no' : 'yes');

    // A write that can never win: it claims a seq this conversation already uses, on every attempt.
    var taken = query("SELECT COALESCE(MAX(seq),0) AS s FROM mysql.agent_memory" +
                      " WHERE conversation_id='" + esc(conv) + "'");
    var seq_taken = (Array.isArray(taken) && taken.length) ? Number(taken[0].s) : 0;
    var dup_stmt =
      "INSERT INTO mysql.agent_memory (conversation_id, seq, turn_no, role," +
      " content, document_name) VALUES ('" + esc(conv) + "'," + seq_taken +
      ",1,'user','conflict probe for " + esc(label) + "','" + esc(prefix) + "')";
    var res = mem_insert_turn(dup_stmt);
    row('conflicting_write_ok',  res.ok ? 'yes' : 'no');
    row('retries_spent',         res.retries);
    row('classified_as_conflict', mem_is_write_conflict(res.err) ? 'yes' : 'no');

  } else if (op === 'recover') {

    var base = query("SELECT COALESCE(MAX(seq),0) AS s, COALESCE(MAX(turn_no),0) AS t" +
                     "  FROM mysql.agent_memory WHERE conversation_id='" + esc(conv) + "'");
    var bseq = (Array.isArray(base) && base.length) ? Number(base[0].s) : 0;
    var bturn = (Array.isArray(base) && base.length) ? Number(base[0].t) : 0;
    try {
      query_checked(
        "INSERT INTO mysql.agent_memory (conversation_id, seq, turn_no, role," +
        " content, document_name) VALUES ('" + esc(conv) + "'," + (bseq + 1) + "," +
        (bturn + 1) + ",'user','orphan question for " + esc(label) + "','" +
        esc(prefix) + "')");
    } catch (e) { row('orphan_insert_error', String(e).substring(0, 120)); }

    var hist = recover_chat_history_from_memory(conv, 3);
    var missing_user = 0;
    for (var h = 0; h < hist.length; h++)
      if (!hist[h].user_message) missing_user++;
    row('recovered_turns', hist.length);
    row('turns_missing_user', missing_user);

    try {
      query_checked("DELETE FROM mysql.agent_memory WHERE conversation_id='" + esc(conv) +
                    "' AND seq=" + (bseq + 1));
    } catch (e2) { row('orphan_cleanup_error', String(e2).substring(0, 120)); }

  } else if (op === 'hybrid') {
    // Fusion, asserted on the pure function.
    var L      = selfcheck_rank_list;
    var tags   = selfcheck_rank_tags;
    var key_of = selfcheck_rank_key;

    var fused = mem_fuse_rrf(
      [ { name: 'vector',  items: L(['a', 'b', 'c']), weight: 1 },
        { name: 'lexical', items: L(['d', 'b', 'e']), weight: 1 } ],
      key_of, 1, 3);
    row('rrf_order', tags(fused));
    row('rrf_top_sources', fused.length ? fused[0].fused_sources : '');

    var only_vec = mem_fuse_rrf(
      [ { name: 'vector',  items: L(['a', 'b']), weight: 1 },
        { name: 'lexical', items: L(['x', 'y']), weight: 0 } ],
      key_of, 60, 4);
    row('rrf_zero_weight', tags(only_vec));

    var deep_v = L(['a', 'x', 'y', 'z', 'b']);
    var deep_l = L(['d', 'e', 'f', 'g', 'b']);
    var damped = mem_fuse_rrf(
      [ { name: 'vector', items: deep_v, weight: 1 },
        { name: 'lexical', items: deep_l, weight: 1 } ], key_of, 60, 2);
    row('rrf_damped_order', tags(damped));
    var sharp = mem_fuse_rrf(
      [ { name: 'vector', items: deep_v, weight: 1 },
        { name: 'lexical', items: deep_l, weight: 1 } ], key_of, 0.0001, 2);
    row('rrf_sharp_order', tags(sharp));

    // The emitted lexical SQL, so the one place the full-text dialect is written down is pinned.
    var lex_sql = mem_lexical_sql('mysql.agent_semantic_fact', 'statement', 'fact_sales',
                                  'principal_prefix', ['deadbeef'], null, 'fact_id, statement', 12);
    row('lexical_sql', lex_sql);

    var vec_sql = mem_vector_scan_sql(
      'mysql.agent_memory',
      { segment: 'content', segment_embedding: 'embedding' },
      '@shannon_mem_qvec',
      { documents: ['deadbeef'], isolation_column: 'document_name',
        select_list: 'content',
        extra_where: '(expires_at IS NULL OR expires_at > NOW())',
        max_distance: 0.6 },
      null, 3);
    row('vector_sql', vec_sql);
    row('vector_sql_has_isolation',
        (vec_sql.indexOf("`document_name` IN ('deadbeef')") !== -1) ? 'yes' : 'no');

    var ro = mem_retrieval_options(mo);
    row('retrieval_mode', ro.mode);
    row('min_lex_ratio', ro.min_lex_ratio);
    // An unrecognised mode must fall back to the default, not to "no retriever ran".
    row('bad_mode_falls_back',
        mem_retrieval_options(mem_merge_defaults(mo, { retrieval: { mode: 'nonsense' } })).mode);

  } else if (op === 'graph') {
    // Edges and a multi-hop walk.
    try {
      query_checked("DELETE FROM mysql.agent_memory_edge WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}
    mem_graph_link('fact', 'g1', 'scoped_to', 'table', 'orders',   1, null, mo);
    mem_graph_link('table', 'orders',    'joins', 'table', 'customers', 1, null, mo);
    mem_graph_link('table', 'customers', 'joins', 'table', 'regions',   1, null, mo);
    /* Writing the same edge twice is one edge. */
    mem_graph_link('fact', 'g1', 'scoped_to', 'table', 'orders', 1, null, mo);

    var one = mem_graph_expand([{ kind: 'fact', id: 'g1' }], 1, 10, mo);
    row('depth1_nodes', one.length);
    row('depth1', mem_graph_to_text(one).replace(/\n/g, ' | '));

    // Three hops from the fact: the walk must cross fact -> orders -> customers -> regions.
    var three = mem_graph_expand([{ kind: 'fact', id: 'g1' }], 3, 20, mo);
    var reached_regions = 0;
    for (var gi = 0; gi < three.length; gi++)
      if (three[gi].id === 'regions') reached_regions = 1;
    row('depth3_reaches_regions', reached_regions);

    // Edges are undirected for the walk: asking from the far end finds the fact again.
    var back = mem_graph_expand([{ kind: 'table', id: 'regions' }], 3, 20, mo);
    var reached_fact = 0;
    for (var bi = 0; bi < back.length; bi++)
      if (back[bi].kind === 'fact' && back[bi].id === 'g1') reached_fact = 1;
    row('reverse_walk_reaches_fact', reached_fact);

    var cnt = query("SELECT COUNT(*) AS c FROM mysql.agent_memory_edge" +
                    " WHERE principal_prefix='" + esc(prefix) + "'");
    row('edges_stored', (Array.isArray(cnt) && cnt.length) ? cnt[0].c : 'read_failed');
    try {
      query_checked("DELETE FROM mysql.agent_memory_edge WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}

  } else if (op === 'artifact') {

    var big = selfcheck_pad('artifact-' + label, 5000);
    var put = artifact_put('text', 'text/plain', big, { row_count: 7 }, mo);
    row('stored', put.ok ? 'yes' : ('no: ' + put.error));
    row('size_bytes', put.size_bytes);

    /* Same content again is the same row, not a second copy. */
    var put2 = artifact_put('text', 'text/plain', big, { row_count: 7 }, mo);
    row('dedup_same_id', (put2.ok && put2.artifact_id === put.artifact_id) ? 'yes' : 'no');

    var p1 = artifact_read(put.artifact_id, 0, 100, mo);
    row('page1_len', p1.ok ? p1.length : ('err:' + p1.error));
    row('page1_next_offset', p1.ok ? p1.next_offset : '');
    row('page1_eof', p1.ok ? (p1.eof ? 'yes' : 'no') : '');
    var p2 = artifact_read(put.artifact_id, p1.next_offset, 100, mo);

    row('page2_contiguous',
        (p2.ok && big.substring(100, 200) === p2.text) ? 'yes' : 'no');

    var last = artifact_read(put.artifact_id, 4990, 100, mo);
    row('last_page_eof', (last.ok && last.eof) ? 'yes' : 'no');
    row('unknown_handle', artifact_read('art_nosuchartifact', 0, 10, mo).error);

    // Spilling: a small result is returned as itself, a large one comes back as a preview naming a handle.
    var small_out = artifact_spill('tiny result', 'result_set', {}, mo);
    row('small_not_spilled', (small_out === 'tiny result') ? 'yes' : 'no');
    var big_out = artifact_spill(big, 'result_set', { row_count: 7 }, mo);
    row('big_spilled', (big_out.indexOf('artifact_id=') !== -1) ? 'yes' : 'no');
    row('preview_shorter_than_source', (big_out.length < big.length) ? 'yes' : 'no');

    try {
      query_checked("DELETE FROM mysql.agent_artifact WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}

  } else if (op === 'derive') {

    try {
      query_checked("DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}
    var derive_mo = mem_merge_defaults(mo, { long_term: { semantic_enabled: true,
                                                          episodic_enabled: false } });
    query_checked("INSERT INTO mysql.agent_semantic_fact" +
                  " (principal_prefix, scope, statement, confidence)" +
                  " VALUES ('" + esc(prefix) + "','derive-" + esc(label) + "'," +
                  "'derive-" + esc(label) + " fact with no vector',80)" +
                  " ON DUPLICATE KEY UPDATE confidence=80");
    var n1 = mem_derive_enqueue_missing(derive_mo);
    row('enqueued_first_sweep', (n1 > 0) ? 'yes' : 'no');
    // uk_task: sweeping again adds nothing, so calling this every turn is free once the backlog is empty.
    var n2 = mem_derive_enqueue_missing(derive_mo);
    row('enqueued_second_sweep', n2);
    var backlog = mem_derive_backlog(derive_mo);
    row('backlog_pending_gt0', (backlog.pending > 0) ? 'yes' : 'no');

    // The drain.
    var drain_mo = mem_merge_defaults(derive_mo, { derive: { drain_batch: 10 } });
    mem_derive_drain(drain_mo);
    var after = query(
      "SELECT SUM(state='running') AS still_running, SUM(lease_owner<>'') AS still_leased," +
      "       MIN(attempts) AS min_attempts" +
      "  FROM mysql.agent_derive_queue WHERE principal_prefix='" + esc(prefix) + "'");
    if (Array.isArray(after) && after.length) {
      row('tasks_left_running', Number(after[0].still_running || 0));
      row('leases_left_held',   Number(after[0].still_leased || 0));
      row('attempt_counted',    (Number(after[0].min_attempts || 0) >= 1) ? 'yes' : 'no');
    } else {
      row('drain_readback', 'read_failed');
    }

    try {
      query_checked("DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='" +
                    esc(prefix) + "' AND scope='derive-" + esc(label) + "'");
      query_checked("UPDATE mysql.agent_derive_queue SET state='pending', lease_owner=''," +
                    " available_at=NOW() WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}
    mem_derive_drain(drain_mo);
    var gone = query("SELECT state FROM mysql.agent_derive_queue" +
                     " WHERE principal_prefix='" + esc(prefix) + "'");
    row('vanished_target_state',
        (Array.isArray(gone) && gone.length) ? String(gone[0].state) : 'no_rows');

    try {
      query_checked("DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" +
                    esc(prefix) + "'");
    } catch (e) {}

    /* Trace vectors: the writer only enqueues one, so the sweep is the safety
     * net for a row whose enqueue was lost -- and this is the third kind the
     * drain has to know about. */
    try {
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" +
                    esc(prefix) + ":derive' AND desc_text='trace-derive-" + esc(label) + "'");
      query_checked("INSERT INTO mysql.agent_sql_trace" +
                    " (conversation_id, turn_no, step_no, route, tool, sql_text," +
                    "  desc_text, result_preview, is_write, embed_eligible)" +
                    " VALUES ('" + esc(prefix) + ":derive',1,1,'agent_loop','query_db'," +
                    " 'SELECT 1','trace-derive-" + esc(label) + "','1',0,1)");
    } catch (e) {}
    mem_derive_enqueue_missing(derive_mo);
    var trace_q = query("SELECT COUNT(*) AS n FROM mysql.agent_derive_queue" +
                        " WHERE principal_prefix='" + esc(prefix) + "' AND kind='embed_trace'");
    row('trace_task_enqueued',
        (Array.isArray(trace_q) && trace_q.length && Number(trace_q[0].n) > 0) ? 'yes' : 'no');
    try {
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" +
                    esc(prefix) + ":derive' AND desc_text='trace-derive-" + esc(label) + "'");
      query_checked("DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" +
                    esc(prefix) + "' AND kind='embed_trace'");
    } catch (e) {}

    var ep_mo = mem_merge_defaults(mo, { long_term: { episodic_enabled: true,
                                                      semantic_enabled: false } });
    try {
      query_checked(
        "INSERT INTO mysql.agent_memory (conversation_id, seq, turn_no, role, content," +
        " content_hash, document_name, meta) VALUES" +
        " ('derive-skip-" + esc(label) + "',1,1,'user','skipped chatter'," +
        " SHA2('skipped chatter',256),'" + esc(prefix) + "'," +
        " JSON_OBJECT('embed_skipped', true))," +
        " ('derive-skip-" + esc(label) + "',2,1,'user','ordinary content'," +
        " SHA2('ordinary content',256),'" + esc(prefix) + "', JSON_OBJECT())");
    } catch (e) {}
    mem_derive_enqueue_missing(ep_mo);
    var sk = query(
      "SELECT SUM(m.meta->>'$.embed_skipped' = 'true') AS skipped_enqueued," +
      "       SUM(COALESCE(m.meta->>'$.embed_skipped','') <> 'true') AS wanted_enqueued" +
      "  FROM mysql.agent_derive_queue q JOIN mysql.agent_memory m ON m.id=q.target_id" +
      " WHERE q.principal_prefix='" + esc(prefix) + "' AND q.kind='embed_memory'");
    if (Array.isArray(sk) && sk.length) {
      row('skipped_rows_enqueued', Number(sk[0].skipped_enqueued || 0));
      row('wanted_rows_enqueued',  Number(sk[0].wanted_enqueued || 0));
    }
    try {
      query_checked("DELETE FROM mysql.agent_memory WHERE conversation_id='derive-skip-" +
                    esc(label) + "'");
    } catch (e) {}

    try {
      query_checked("DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" + esc(prefix) + "'");
      query_checked("DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='" + esc(prefix) +
                    "' AND scope='derive-" + esc(label) + "'");
    } catch (e) {}

  } else if (op === 'turn_join') {

    A.turn_id = '';
    var tid = current_turn_id();
    mem_short_append_turn(conv, 'join probe question ' + label,
                          'join probe answer', 'thought', { route: 'agent_loop' });
    log_sql_trace(conv, 1, 1, 'agent_loop', 'query_db',
                  'SELECT 1', 'join probe step', 'ok');

    var j = query(
      "SELECT COUNT(*) AS joined FROM mysql.agent_memory m" +
      " JOIN mysql.agent_sql_trace t ON t.turn_id = m.turn_id" +
      " WHERE m.conversation_id='" + esc(conv) + "' AND m.turn_id='" + esc(tid) + "'");
    row('memory_joined_to_trace', (Array.isArray(j) && j.length) ? j[0].joined : 'read_failed');

    var g = query("SELECT COUNT(*) AS c FROM mysql.agent_memory" +
                  " WHERE conversation_id='" + esc(conv) + "'" +
                  "   AND turn_id = meta->>'$.turn_id'");
    row('turn_id_matches_meta', (Array.isArray(g) && g.length) ? g[0].c : 'read_failed');
    try {
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" + esc(conv) + "'");
    } catch (e) {}

  } else if (op === 'cleanup') {
    try {
      query_checked("DELETE FROM mysql.agent_memory WHERE conversation_id='" + esc(conv) + "'");
      query_checked("DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='" + esc(prefix) +
                   "' AND statement LIKE 'fact-" + esc_like(label) + "%'");
      query_checked("DELETE FROM mysql.agent_memory_audit WHERE conversation_id='" + esc(conv) + "'");
      query_checked("DELETE FROM mysql.agent_conversation_summary WHERE conversation_id='" + esc(conv) + "'");
      row('cleaned', 'yes');
    } catch (e) { row('cleaned', 'no'); }

  } else {
    row('error', 'unknown op: ' + op);
  }

  return out;
}
