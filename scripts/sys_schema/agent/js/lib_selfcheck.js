/* Pull in the whole agent environment: the tool impl_* bodies and the memory
 * facade call helpers spread across lib_tools / lib_schema / lib_ml /
 * lib_memory, and a JerryScript stored routine does not share a global scope
 * with any other routine -- a helper expanded into some other routine's body
 * is not visible here.
 *
 * Both self-checks live in one file, and behind one stored procedure, on
 * purpose: each routine body is a full ~350 KB copy of the agent in the
 * generated ml_agent_chat.sql, so a second diagnostic entry point would cost
 * another one for no benefit. */
//@include lib_tools.js
//@include lib_recall_eval.js

/* Self-check entry point behind sys.shannon_agent_selfcheck(kind, op, label).
 * Returns a (k, v) result set; see the two callers in
 * mysql-test/t/shannon_agent_tool_contract.test and
 * mysql-test/t/shannon_agent_memory.test. */
function shannon_agent_selfcheck(kind, op, label) {
  kind = String(kind || 'tools').toLowerCase();
  if (kind === 'memory') return shannon_memory_selfcheck(op, label);
  if (kind === 'recall') return shannon_recall_selfcheck(op, label);
  if (kind === 'sqlmode') return shannon_sql_mode_selfcheck();
  if (kind === 'tools') {
    var problems = shannon_tool_selfcheck();
    var rows = [];
    for (var i = 0; i < problems.length; i++) rows.push(['result', String(problems[i])]);
    return rows;
  }
  return [['error', 'unknown selfcheck kind: ' + kind]];
}


/* SQL-mode gate.  sys.shannon_agent_selfcheck('sqlmode', NULL, NULL).
 *
 * Reports what sql_mode_gate_check() makes of the *calling session's* current
 * sql_mode, so mysql-test can SET SESSION sql_mode and assert the verdict
 * without a live LLM.  shannon_agent_run() runs the same check before the
 * quota check and before any model call, and refuses the turn when it fails.
 *
 * Why the gate exists: the agent's write policy reads every statement through
 * sql_lex_info(), a hand-written lexer that assumes the default quoting
 * dialect.  Under NO_BACKSLASH_ESCAPES a backslash is an ordinary character
 * to the server but an escape to the lexer, so 'a\\' terminates for one and
 * not the other and the two are no longer reading the same statement; under
 * ANSI_QUOTES a double quote opens an identifier rather than a string.  The
 * gate refuses those rather than let the policy classify a statement the
 * server will not run.
 *
 * Reads nothing and writes nothing, so it is not behind the write opt-in. */
function shannon_sql_mode_selfcheck() {
  A.lang = 'en';
  var verdict = sql_mode_gate_check();
  var rows = [['ok', verdict.ok ? '1' : '0']];
  rows.push(['blocked_modes', (verdict.modes || []).join(',')]);
  /* The message is what the user sees, so assert that it is actionable rather
   * than just non-empty: it has to name the mode and say what to change. */
  var msg = String(verdict.message || '');
  rows.push(['message_names_mode',
             (!verdict.ok && verdict.modes.length && msg.indexOf(verdict.modes[0]) !== -1) ? '1' : '0']);
  rows.push(['message_is_actionable', (!verdict.ok && msg.indexOf('sql_mode') !== -1) ? '1' : '0']);
  return rows;
}


/* Recall quality.  sys.shannon_agent_selfcheck('recall', <op>, <label>).
 *
 * ops:
 *   seed     write the labelled set as this principal's long-term facts
 *   score    lexical-only scoring -- no embedding model required
 *   score:<modes>[:k]
 *            e.g. 'score:lexical,vector,hybrid:5'.  vector and hybrid need
 *            sys.ML_EMBED_ROW to work, so they are not what mysql-test runs.
 *   stats    how many documents are seeded, and how many carry a vector
 *   cleanup  remove this label's rows
 *
 * Gated on the same write opt-in as the memory self-check: it writes real
 * rows to mysql.agent_semantic_fact. */
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
    /* Zero vectors is the expected state on a machine with no embedding
     * model, not a failure -- it is precisely why the scorer takes a mode
     * list rather than always scoring all three. */
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


/* Tool-contract self-check — the minimum viable evaluation harness.
 *
 * mysql-test had zero coverage of agent behaviour (sp-external.test only
 * proves LANGUAGE JAVASCRIPT works at all), so nothing caught the drift that
 * accumulated between the tool implementations, their validation, and the two
 * hand-written prompt catalogues.  This function is what
 * mysql-test/t/shannon_agent_tool_contract.test asserts against; it is pure
 * in-process reasoning over the registry and touches no user data.
 *
 * Reached as sys.shannon_agent_selfcheck('tools', NULL, NULL).  It returns one
 * row per problem, or a single 'OK'. */
function shannon_tool_selfcheck() {
  var out = [];

  /* --- 1. validation contract ----------------------------------------- */
  var cases = [
    { tool: 'query_db',       args: {},                        expect: 'query_db' },
    { tool: 'explain_sql',    args: { sql: 'SEL' },            expect: 'explain_sql' },
    { tool: 'run_ddl',        args: { sql: 'DROP TABLE t' },   expect: 'allow_destructive_ddl' },
    { tool: 'run_ddl',        args: { sql: 'SELECT 1 FROM t' },expect: 'run_ddl' },
    /* Each DDL class is refused by its own flag, not by the destructive one:
     * CREATE USER destroys nothing, and used to pass every gate there was. */
    { tool: 'run_ddl',        args: { sql: "CREATE USER a@localhost IDENTIFIED BY 'p'" },
                                                               expect: 'allow_account_ddl' },
    { tool: 'run_ddl',        args: { sql: 'ALTER USER a@localhost IDENTIFIED BY \'p\'' },
                                                               expect: 'allow_account_ddl' },
    /* The routine body is written with a quoted literal rather than the usual
     * AS $-$ ... $-$ form (spelled apart here for the same reason): these
     * files are inlined into CREATE FUNCTION ... AS $-$ <body> $-$, so a real
     * pair of dollar signs anywhere in this source closes the body early and
     * the server fails to bootstrap.  Only the tokens before the body decide
     * the classification, so the shape of the body is irrelevant here. */
    { tool: 'run_ddl',        args: { sql: 'CREATE FUNCTION f() RETURNS INT LANGUAGE JAVASCRIPT AS "return 1;"' },
                                                               expect: 'allow_code_ddl' },
    { tool: 'run_ddl',        args: { sql: 'CREATE EVENT e ON SCHEDULE EVERY 1 DAY DO SELECT 1' },
                                                               expect: 'allow_code_ddl' },
    /* Instance-level DDL destroys no object, so the destructive gate never
     * saw it and an unlisted object keyword put it in the same tier as
     * CREATE INDEX.  Both spellings below were allowed by default until
     * INSTANCE and RESOURCE were added to _DDL_OBJECT_CLASS. */
    { tool: 'run_ddl',        args: { sql: 'ALTER INSTANCE ROTATE INNODB MASTER KEY' },
                                                               expect: 'allow_instance_ddl' },
    { tool: 'run_ddl',        args: { sql: 'ALTER INSTANCE RELOAD TLS' },
                                                               expect: 'allow_instance_ddl' },
    /* Both TYPE spellings have to land in the same class.  Before RESOURCE
     * was listed, TYPE = USER reached the USER entry and was refused as
     * account DDL while TYPE = SYSTEM was allowed outright -- the same
     * statement family answered two different wrong ways. */
    { tool: 'run_ddl',        args: { sql: 'CREATE RESOURCE GROUP rg TYPE = SYSTEM' },
                                                               expect: 'allow_instance_ddl' },
    { tool: 'run_ddl',        args: { sql: 'CREATE RESOURCE GROUP rg TYPE = USER' },
                                                               expect: 'allow_instance_ddl' },
    /* And ordinary schema DDL still is not: SECONDARY_LOAD is a prerequisite
     * of ml_train, so a gate that caught it would break the ML path. */
    { tool: 'run_ddl',        args: { sql: 'ALTER TABLE db.t SECONDARY_LOAD' }, expect: null },
    /* CREATE TABLE user is schema DDL, not account DDL: the object keyword
     * decides, and TABLE comes first. */
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
    /* System-schema writes. run_ddl checks it at validation time; update_data
     * checks it in the handler, because the approval path can rewrite the SQL
     * after validation -- so update_data's case lives in the policy block
     * below rather than here. */
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

  /* --- 3. prompt <-> registry consistency, in both languages -----------
   * Plus a size ceiling.  The tool catalogue is part of every prompt and
   * est_tok(full_prompt) is what PROMPT_TOK_LIMIT budgets against, so a spec
   * whose doc grows without bound would quietly squeeze out the schema
   * context instead of failing anywhere visible.  The cap is deliberately
   * loose -- roughly double the current size -- because it is a runaway
   * detector, not a style rule. */
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

    /* The budgeted catalogue -- what a turn actually sends.  A collapsed tool
     * is not documented in full, but it must still be named with its
     * arguments, or the saving has been taken by making a tool unreachable.
     * This is the check that would catch a category being collapsed without
     * a way back. */
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

    /* A category that collapses must come back when the conversation is
     * about it, on the same turn -- otherwise the model has to guess that it
     * should ask, which it has no reason to do. */
    var ml_turn = render_tool_docs(langs[l], { message: '训练一个回归模型' });
    if (ml_turn.indexOf('"tool":"ml_train"') === -1)
      out.push('CATEGORY_NOT_REEXPANDED ' + langs[l] + ' ml');
  }
  A.lang = saved_lang;
  A.user_message = saved_msg;

  /* describe_tool is the escape hatch that makes collapsing safe; if it ever
   * stops returning a full entry, the budget becomes a capability loss. */
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

  /* --- 4b. system-schema write gate ------------------------------------
   * Asserted on check_system_schema_policy() directly rather than only
   * through a tool, because what it gets wrong is the *extraction*: which
   * table a statement writes to, as opposed to which tables it mentions.
   * Reading information_schema inside an INSERT ... SELECT is ordinary and
   * has to stay allowed, and that is the case a "does the SQL contain
   * 'mysql.'" check would break. */
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
    /* The qualifier is still there; only the spelling changed.  MySQL allows
     * whitespace around the dot, and the INSERT/REPLACE/DDL patterns used to
     * stop at the space and keep "mysql" alone -- which then read as an
     * unqualified write to db, i.e. allowed. */
    { sql: "INSERT INTO mysql . user (user) VALUES ('bob')",                   db: 'shop', denied: true },
    { sql: "DROP TABLE mysql . agent_memory",                                  db: 'shop', denied: true },
    /* A rename's destination is a write target, and it does not have to be in
     * the source's schema: this moves a table INTO mysql while the only name
     * the object clause sees is shop.t. */
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
    /* RENAME's own negative cases: a rename inside one ordinary schema, and
     * the RENAME COLUMN/INDEX spellings, whose destination is a part of a
     * table rather than a table. */
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

  /* --- 4b. the read ceiling -------------------------------------------
   *
   * guard_read_sql() is what stands between a model-written SELECT and the
   * 512KB engine heap, so its two failure modes both matter: letting an
   * unbounded read through, and refusing a read that was always safe. The
   * false-refusal cases are the larger half of this table on purpose --
   * an over-eager guard teaches the model to route around it. */
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
    { sql: "SELECT note FROM t WHERE note = 'COUNT(*)'",            deny: true  }
  ];
  for (var rc = 0; rc < read_cases.length; rc++) {
    var rstmt = classify_statement(read_cases[rc].sql);
    var rgot  = !!guard_read_sql(read_cases[rc].sql, rstmt);
    if (rgot !== read_cases[rc].deny)
      out.push('READ_GUARD #' + rc + ' expect=' + (read_cases[rc].deny ? 'deny' : 'allow') +
               ' got=' + (rgot ? 'deny' : 'allow') + ' sql=' + read_cases[rc].sql);
  }

  /* --- 4c. session options may tighten policy, never relax it ----------
   *
   * The whole point of mysql.agent_policy: a request that asks for more
   * than the instance allows gets the instance's answer. Exercised against
   * the combinators directly so the check does not depend on what is
   * actually in the table on this server. */
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

  /* --- 4d. the stop-reason taxonomy -----------------------------------
   *
   * Only 'finish' may be silent. Any other ending that reaches the user
   * without a note is an incomplete answer presented as a complete one,
   * which is the failure this taxonomy exists to prevent. */
  var stop_reasons = ['max_turns', 'truncated', 'error_budget', 'loop_detected',
                      'context_exhausted', 'empty_completion'];
  for (var sr = 0; sr < stop_reasons.length; sr++) {
    if (!stop_reason_note(stop_reasons[sr]))
      out.push('STOP_REASON_SILENT ' + stop_reasons[sr]);
  }
  if (stop_reason_note('finish'))
    out.push('STOP_REASON_NOISY finish should say nothing');

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


/* Memory-layer self-check driver.
 *
 * The layered memory in lib_memory_registry.js is only reachable through
 * sys.shannon_chat(), which needs a live LLM -- so none of it could be tested
 * in mysql-test without one. This exposes the memory primitives directly, so
 * mysql-test/t/shannon_agent_memory.test can assert on ordering, atomicity,
 * deduplication, redaction, retention, isolation and degradation using plain
 * SQL, with no model involved.
 *
 * Every operation runs entirely inside the calling principal's own scope, the
 * same way the agent does. Notably 'isolation' cannot be made to read another
 * principal's rows through the recall predicate: the prefix it filters on is
 * the caller's own and there is no argument that can widen it.
 *
 * Output is a fixed (k, v) result set so the test can diff it directly. */
/* The memory self-check exercises the real primitives against the real
 * tables, so it writes rows to mysql.agent_memory and mysql.agent_semantic_fact
 * as the invoker.  The procedure ships in every install, not just debug
 * builds, so writing is opt-in rather than a bare CALL away: an operator who
 * runs it out of curiosity on a production instance gets told what it would
 * do instead of getting test conversations seeded into their agent memory.
 * The tools self-check reasons only over the registry and needs no gate. */
/* Top level, not inside the op branch that uses it: JerryScript does not
 * hoist a function declared inside a block, so a nested one is undefined by
 * the time the branch calls it. */
function selfcheck_pad(tag, n) {
  var p = tag;
  while (p.length < n) p += ' ' + tag;
  return p.substring(0, n);
}

/* Top level for the same reason selfcheck_pad() is: JerryScript does not
 * hoist a function declared inside a block, so one declared inside the op
 * branch that uses it is undefined by the time the branch runs. */
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
    /* Five turns written back to back. They land in the same wall-clock
     * second, which is exactly the case ORDER BY created_at could not order:
     * TIMESTAMP has one-second resolution. */
    for (var i = 1; i <= 5; i++) {
      var q = 'question ' + i + ' for ' + label;
      if (i === 3) q = q + ' my key is sk-ABCDEF123456 please keep it';
      MEM.short.append_turn(conv, q, 'answer ' + i + ' for ' + label,
                            'thought ' + i, { route: 'agent_loop' });
    }
    /* Same fact twice: the unique key on (principal_prefix, statement) must
     * turn the second write into a use_count bump, not a second row. */
    MEM.long.write_fact('fact-' + label + ': the user prefers concise answers',
                        { scope: 'mtr', confidence: 90 }, mo);
    MEM.long.write_fact('fact-' + label + ': the user prefers concise answers',
                        { scope: 'mtr', confidence: 90 }, mo);

    /* Deliberately not the raw values: conv and prefix are derived from
     * SHA2(CURRENT_USER()), so printing them would pin the recorded test
     * result to one account name. Shape is what matters here. */
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
    /* One INSERT writes both rows of a turn, so no turn_no may be half
     * written -- the orphan the old two-INSERT persist_turn could leave. */
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
    /* Reads back through MEM.short.load(), i.e. through the seq ordering the
     * agent itself relies on -- not through a hand-written ORDER BY here. */
    var loaded = MEM.short.load(conv, mo);
    row('degraded', loaded.degraded ? 'yes' : 'no');
    row('turn_rows', loaded.turns.length);
    for (var n = 0; n < loaded.turns.length; n++)
      row('row' + (n + 1), loaded.turns[n].role + '|' + loaded.turns[n].turn_no +
                           '|' + loaded.turns[n].content);

  } else if (op === 'isolation') {
    /* visible_to_recall applies exactly the predicate the recall path builds:
     * document_name IN (<this principal's prefix>). The prefix is injected by
     * mem_vector_search() and there is deliberately no argument -- here or in
     * @chat_options -- that can widen it.
     *
     * present_but_isolated counts rows under the same conversation label that
     * belong to some other principal. It is deliberately non-zero in the
     * test: the point is not that the other principal's rows are absent from
     * the table, it is that the isolation key keeps them out of this
     * principal's reach. */
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
    /* Under the summarize_after_turns threshold this must be a no-op and must
     * not call the model at all. */
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
    /* The memory block shares the prompt with the schema context, and only
     * the schema context is indispensable, so the block needs a ceiling of
     * its own: the per-section caps do not add up to one (few-shot examples
     * are bounded by top_k, not by size).
     *
     * The admission rule is tested on synthetic sections rather than on
     * whatever this conversation happens to contain, so the assertions say
     * something fixed: sections are admitted by priority, dropped from the
     * cheapest to lose, and emitted in reading order regardless. */
    var secs = [];
    mem_block_section(secs, selfcheck_pad('S-summary', 300), 2);
    mem_block_section(secs, selfcheck_pad('R-recent',  300), 1);
    mem_block_section(secs, selfcheck_pad('F-fewshot', 300), 5);

    var bo = get_memory_options(get_chat_options());
    bo.budget.max_block_tokens = 250;          /* fits two 100-token sections */
    /* Not `out`: that name is the rows accumulator this function's row()
     * closes over, and `var` is function-scoped here. */
    var block = mem_block_assemble(secs, bo);
    row('kept_recent',     block.indexOf('R-recent')  !== -1 ? 'yes' : 'no');
    row('kept_summary',    block.indexOf('S-summary') !== -1 ? 'yes' : 'no');
    row('dropped_fewshot', block.indexOf('F-fewshot') === -1 ? 'yes' : 'no');
    row('reading_order',
        block.indexOf('S-summary') < block.indexOf('R-recent') ? 'yes' : 'no');
    row('tokens_within_cap', A.mem_block_tokens <= 250 ? 'yes' : 'no');

    /* And end to end: a real block is counted, and the count is what the
     * agent loop charges against its prompt budget. */
    A._mem_opt = null;
    var lo = get_memory_options(get_chat_options());
    MEM.build_block('question 1 for ' + label, conv, lo);
    row('real_block_counted',
        Number(A.mem_block_tokens || 0) > 0 ? 'yes' : 'no');
    row('real_block_within_default_cap',
        Number(A.mem_block_tokens || 0) <= Number(lo.budget.max_block_tokens)
          ? 'yes' : 'no');

  } else if (op === 'rank') {
    /* Ranking is asserted on the pure functions, not on a live recall.
     * Not a question of what can be run: the documented build fetches
     * multilingual-e5-small into extra/llm-models/, and shannon_embedding
     * uses it unguarded.
     *
     * A live recall would assert the cosine distances a particular model
     * build produces -- a retrieval-quality question, wanting its own test
     * and its own tolerances.  What changed here is the ranking arithmetic
     * on top of those distances, and mem_rank_sql() and mem_diversify() are
     * functions of their arguments alone, so it can be checked exactly
     * rather than approximately. */
    var rk_def = MEM.rank.options(mo);
    /* The default must be the old behaviour, or every recorded result in
     * this suite changes meaning: relevance only, no diversity. */
    row('default_active',  MEM.rank.active(rk_def) ? 'yes' : 'no');
    row('default_lambda',  rk_def.lambda);

    var sig = { columns: ['use_count', 'last_used_at', 'created_at'],
                recency:    'COALESCE(last_used_at, created_at)',
                importance: { expr: 'confidence', max: 100 },
                usage:      'use_count' };
    row('default_sql', MEM.rank.sql(rk_def, sig));

    /* With weights on, every signal the caller supplied has to appear, and
     * the whole expression stays in 0..1 so the weights mean what they say. */
    var rk_on = MEM.rank.options(MEM.merge_defaults(mo, { long_term: { ranking: {
      weight_recency: 0.3, weight_importance: 0.2, weight_usage: 0.1 } } }));
    row('weighted_active', MEM.rank.active(rk_on) ? 'yes' : 'no');
    var expr = MEM.rank.sql(rk_on, sig);
    row('has_recency',    expr.indexOf('TIMESTAMPDIFF') !== -1 ? 'yes' : 'no');
    row('has_importance', expr.indexOf('confidence')    !== -1 ? 'yes' : 'no');
    row('has_usage',      expr.indexOf('use_count')     !== -1 ? 'yes' : 'no');

    /* A signal the caller did not supply contributes nothing rather than
     * some invented default -- the reason L2a can take diversity but not
     * recency, and the reason that asymmetry is safe. */
    row('absent_signal_absent',
        MEM.rank.sql(rk_on, { recency: null, importance: null, usage: null })
          .indexOf('TIMESTAMPDIFF') === -1 ? 'yes' : 'no');

    /* The derived table may not name a column twice: confidence is already
     * in the select list, the other three are not. */
    row('inner_list', MEM.rank.inner_list('fact_id, statement, confidence, scope', sig.columns));

    /* MMR: three near-identical candidates and one different one.  Pure
     * relevance order returns the three duplicates; lambda 0.5 has to reach
     * past them for the one that says something else. */
    var cand = [
      { txt: 'total revenue by region for last quarter',  rel: 1.00 },
      { txt: 'total revenue by region for last quarter!', rel: 0.99 },
      { txt: 'total revenue by region last quarter',      rel: 0.98 },
      { txt: 'how many employees joined in March',        rel: 0.60 }
    ];
    function cand_txt(c) { return c.txt; }
    function cand_rel(c) { return c.rel; }
    var pure = MEM.rank.diversify(cand, cand_txt, cand_rel, 1,   2);
    var divd = MEM.rank.diversify(cand, cand_txt, cand_rel, 0.5, 2);
    row('pure_relevance_2nd', pure[1].txt);
    row('diversified_2nd',    divd[1].txt);
    /* Both keep the best candidate first: diversity reorders what follows
     * the top hit, it does not demote the top hit. */
    row('top_hit_kept', (pure[0].txt === cand[0].txt && divd[0].txt === cand[0].txt)
          ? 'yes' : 'no');
    row('near_dupes_similar',
        MEM.rank.similarity(MEM.rank.tokens(cand[0].txt),
                            MEM.rank.tokens(cand[1].txt)) > 0.8 ? 'yes' : 'no');
    row('unrelated_dissimilar',
        MEM.rank.similarity(MEM.rank.tokens(cand[0].txt),
                            MEM.rank.tokens(cand[3].txt)) < 0.2 ? 'yes' : 'no');

  } else if (op === 'conflict') {
    /* A turn that loses the race to another writer must be retried, not
     * dropped: before the retry, append_turn audited the conflict and
     * returned false, which made the loss visible but still lost it.
     *
     * Tested without real concurrency on purpose -- two simultaneous
     * LANGUAGE JAVASCRIPT routine calls abort the server inside
     * jerry_init() (jmem_heap_alloc assertion, one global JerryScript heap
     * shared by every thread), so a test that raced two agent sessions would
     * crash rather than assert.  What can be checked here is the whole of
     * what this layer decides: which errors mean "someone went first", and
     * that the loop spends its retries on them instead of giving up. */
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

    /* A write that can never win: it claims a seq this conversation already
     * uses, on every attempt.  The loop has to spend its retries and then
     * report, rather than stop at the first failure. */
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
    /* Recovering @chat_options.chat_history from the server side selects the
     * last N *turns*, not the last N*2 rows.  A turn is two rows only when
     * both legs were written, so one orphan row -- what the pre-atomic
     * two-INSERT persist_turn could leave behind, and what an older server
     * still leaves -- used to consume a whole turn's budget and hand back a
     * group with no user message in it. */
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
    /* Fusion, asserted on the pure function.
     *
     * Not on a live recall, for the same reason the ranking check is not: a
     * live hybrid recall asserts the cosine distances one embedding model
     * build happens to produce, on a machine that may have no model at all.
     * What this change introduced is the *fusion arithmetic* on top of two
     * ranked lists, and that is a function of its arguments alone. */
    var L      = selfcheck_rank_list;
    var tags   = selfcheck_rank_tags;
    var key_of = selfcheck_rank_key;

    /* Agreement beats depth: 'b' is rank 2 in both lists and wins over 'a',
     * which one list ranks first and the other does not return.  That is the
     * property RRF is chosen for, and it is what a score-averaging fusion
     * would get wrong. */
    var fused = mem_fuse_rrf(
      [ { name: 'vector',  items: L(['a', 'b', 'c']), weight: 1 },
        { name: 'lexical', items: L(['d', 'b', 'e']), weight: 1 } ],
      key_of, 1, 3);
    row('rrf_order', tags(fused));
    row('rrf_top_sources', fused.length ? fused[0].fused_sources : '');

    /* A zero weight removes a retriever entirely -- this is how
     * retrieval.mode='vector' and ='lexical' stay exactly the old behaviour
     * rather than approximately it. */
    var only_vec = mem_fuse_rrf(
      [ { name: 'vector',  items: L(['a', 'b']), weight: 1 },
        { name: 'lexical', items: L(['x', 'y']), weight: 0 } ],
      key_of, 60, 4);
    row('rrf_zero_weight', tags(only_vec));

    /* k is the knob that trades "both retrievers found it" against "one
     * retriever ranked it first", and these two rows are the same inputs
     * either side of the trade.  'b' is fifth in both lists, 'a' is first in
     * one and absent from the other:
     *   k=60    a=1/61=0.016,  b=2/65=0.031  -> agreement wins
     *   k~0     a=1/1 =1.0,    b=2/5 =0.400  -> top rank wins
     * The ranks matter: with 'b' second in both lists the crossover sits
     * exactly at k=0, so no positive k inverts it and the knob looks inert.
     * That is why this case uses a deeper 'b' rather than reusing the lists
     * above. */
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

    /* The emitted lexical SQL, so the one place the full-text dialect is
     * written down is pinned. Replacing the ngram parser means this string
     * changes and this line is the diff. */
    var lex_sql = mem_lexical_sql('mysql.agent_semantic_fact', 'statement', 'fact_sales',
                                  'principal_prefix', ['deadbeef'], null, 'fact_id, statement', 12);
    row('lexical_sql', lex_sql);

    var ro = mem_retrieval_options(mo);
    row('retrieval_mode', ro.mode);
    row('min_lex_ratio', ro.min_lex_ratio);
    /* An unrecognised mode must fall back to the default, not to "no
     * retriever ran". */
    row('bad_mode_falls_back',
        mem_retrieval_options(mem_merge_defaults(mo, { retrieval: { mode: 'nonsense' } })).mode);

  } else if (op === 'graph') {
    /* Edges and a multi-hop walk. No model involved: this is a join.
     *
     * Cleared first, not only afterwards: remember_fact writes a scoped_to
     * edge for every fact carrying a scope, so any earlier test in the same
     * run leaves edges behind and edges_stored counts them. */
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

    /* Three hops from the fact: the walk must cross fact -> orders ->
     * customers -> regions.  An uncast recursive CTE seed returns zero rows
     * here and reports no error, which is the failure this pins. */
    var three = mem_graph_expand([{ kind: 'fact', id: 'g1' }], 3, 20, mo);
    var reached_regions = 0;
    for (var gi = 0; gi < three.length; gi++)
      if (three[gi].id === 'regions') reached_regions = 1;
    row('depth3_reaches_regions', reached_regions);

    /* Edges are undirected for the walk: asking from the far end finds the
     * fact again. */
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
    /* Spill, page, and the two failure modes that matter: a handle from
     * another principal, and a handle that does not exist. */
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
    /* Pages must not overlap or skip: page 2 starts exactly where page 1
     * ended, which is the contract next_offset states. */
    row('page2_contiguous',
        (p2.ok && big.substring(100, 200) === p2.text) ? 'yes' : 'no');

    var last = artifact_read(put.artifact_id, 4990, 100, mo);
    row('last_page_eof', (last.ok && last.eof) ? 'yes' : 'no');
    row('unknown_handle', artifact_read('art_nosuchartifact', 0, 10, mo).error);

    /* Spilling: a small result is returned as itself, a large one comes back
     * as a preview naming a handle. */
    var small_out = artifact_spill('tiny result', 'result_set', {}, mo);
    row('small_not_spilled', (small_out === 'tiny result') ? 'yes' : 'no');
    var big_out = artifact_spill(big, 'result_set', { row_count: 7 }, mo);
    row('big_spilled', (big_out.indexOf('artifact_id=') !== -1) ? 'yes' : 'no');
    row('preview_shorter_than_source', (big_out.length < big.length) ? 'yes' : 'no');

    try {
      query_checked("DELETE FROM mysql.agent_artifact WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}

  } else if (op === 'derive') {
    /* The backlog is derived from the tables, not tracked by the writer, so
     * a row written without a vector is enqueued by the next sweep whether
     * or not anything remembered to enqueue it. */
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
    /* uk_task: sweeping again adds nothing, so calling this every turn is
     * free once the backlog is empty. */
    var n2 = mem_derive_enqueue_missing(derive_mo);
    row('enqueued_second_sweep', n2);
    var backlog = mem_derive_backlog(derive_mo);
    row('backlog_pending_gt0', (backlog.pending > 0) ? 'yes' : 'no');

    /* The drain.  Whether the embedding itself succeeds depends on an ONNX
     * model being present, so that is deliberately not what is asserted.
     * What has to hold either way is that a claim is always resolved: no
     * task is left in 'running' holding a lease nobody will come back for,
     * and the attempt is counted so max_attempts can eventually retire a
     * task that can never succeed. */
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

    /* A task whose target row retention has already deleted is complete, not
     * stuck: without that it would retry a vanished row to max_attempts. */
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

    /* The skip policy and the backlog sweep must not contradict each other:
     * a row the writer deliberately left unembedded is not a gap to fill. */
    try {
      query_checked("DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" +
                    esc(prefix) + "'");
    } catch (e) {}
    /* Episodic only, and no extra fact row: this half of the contract is
     * about agent_memory, and an additional agent_semantic_fact document --
     * even one deleted immediately afterwards -- moves the ngram relevance
     * the recall evaluation below measures.  InnoDB keeps deleted documents
     * in FTS_DELETED, where they still count toward document frequency until
     * an OPTIMIZE, so a probe row that cleans up after itself is still not
     * invisible to a later MATCH. */
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
    /* One conversation turn writes rows to two tables, and until now they had
     * no key in common: agent_memory counts conversation turns, while
     * agent_sql_trace.turn_no counts agent-loop iterations, so "which
     * statements did this turn run" was unanswerable.  Both now carry the
     * turn_id minted by current_turn_id() -- generated from meta on one side,
     * a plain column on the other. */
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

    /* The generated column really is derived from meta, not written
     * separately -- so a row whose meta carries no turn_id has none here,
     * rather than a stale or invented one. */
    var g = query("SELECT COUNT(*) AS c FROM mysql.agent_memory" +
                  " WHERE conversation_id='" + esc(conv) + "'" +
                  "   AND turn_id = meta->>'$.turn_id'");
    row('turn_id_matches_meta', (Array.isArray(g) && g.length) ? g[0].c : 'read_failed');
    try {
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" + esc(conv) + "'");
    } catch (e) {}

  } else if (op === 'usage') {
    try {
      query_checked("DELETE FROM mysql.agent_usage WHERE principal_prefix='" + esc(prefix) + "'");
    } catch (e) {}
    usage_add({ turns: 1, llm_calls: 2, prompt_tokens: 100 });
    usage_add({ turns: 1, llm_calls: 3, prompt_tokens: 50 });
    var today = usage_today();
    row('turns', today.turns);
    row('llm_calls', today.llm_calls);
    row('prompt_tokens', today.prompt_tokens);
    /* Unconfigured means unlimited: an instance nobody metered must behave
     * exactly as it did before this existed. */
    row('no_quota_configured_ok', usage_check_quota({}).ok ? 'yes' : 'no');
    var tight = { memory_options: { quota: { max_llm_calls_per_day: 1 } } };
    var res_q = usage_check_quota(tight);
    row('over_quota_blocked', res_q.ok ? 'no' : 'yes');
    row('over_quota_reason', res_q.ok ? '' : res_q.reason);
    try {
      query_checked("DELETE FROM mysql.agent_usage WHERE principal_prefix='" + esc(prefix) + "'");
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
