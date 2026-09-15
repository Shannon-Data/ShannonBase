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

/* Self-check entry point behind sys.shannon_agent_selfcheck(kind, op, label).
 * Returns a (k, v) result set; see the two callers in
 * mysql-test/t/shannon_agent_tool_contract.test and
 * mysql-test/t/shannon_agent_memory.test. */
function shannon_agent_selfcheck(kind, op, label) {
  kind = String(kind || 'tools').toLowerCase();
  if (kind === 'memory') return shannon_memory_selfcheck(op, label);
  if (kind === 'tools') {
    var problems = shannon_tool_selfcheck();
    var rows = [];
    for (var i = 0; i < problems.length; i++) rows.push(['result', String(problems[i])]);
    return rows;
  }
  return [['error', 'unknown selfcheck kind: ' + kind]];
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
  var langs = ['zh', 'en'];
  for (var l = 0; l < langs.length; l++) {
    A.lang = langs[l];
    var docs = render_tool_docs(langs[l]);
    if (docs.length > TOOL_DOCS_MAX_CHARS)
      out.push('DOC_TOO_LARGE ' + langs[l] + ' ' + docs.length +
               ' > ' + TOOL_DOCS_MAX_CHARS);
    for (var d = 0; d < names.length; d++) {
      if (TOOL_REGISTRY[names[d]].hidden) continue;
      if (docs.indexOf('"tool":"' + names[d] + '"') === -1)
        out.push('DOC_MISSING ' + langs[l] + ' ' + names[d]);
    }
  }
  A.lang = saved_lang;

  /* --- 4. approval metadata is derivable for every ML write ----------- */
  for (var m = 0; m < names.length; m++) {
    var ms = TOOL_REGISTRY[names[m]];
    if (ms.category !== 'ml' || !ms.write) continue;
    var meta = tool_review_meta(ms.name);
    if (!meta)              out.push('NO_REVIEW_META ' + ms.name);
    else if (!meta.table_arg) out.push('NO_TABLE_ARG ' + ms.name);
  }

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
