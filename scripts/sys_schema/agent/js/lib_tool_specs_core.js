//@include lib_tool_registry.js
//@include lib_artifact.js

/* Core SQL / transaction / generation tools.
 *
 * Each register_tool() call below is the single source of truth for one tool:
 * its argument schema, the bilingual error text the old validate_tool_call()
 * switch produced, its approval metadata, its Prompt documentation, and its
 * execution body.
 *
 * Semantic checks stay where they were.  "Is this SQL read-only?" remains
 * inside impl_query_db, because the agent loop distinguishes a *validation*
 * failure (the model is told to fix its call, no error_count bump path) from
 * an *execution* failure ({ok:false, error:...}, which does feed error_count
 * and failed_tool_sigs).  Moving those checks into spec.validate would
 * silently change which of the two a rejected DELETE counts as.  run_ddl and
 * plan_sql keep their checks in spec.validate because that is exactly where
 * validate_tool_call() already had them. */

/* ---------------------------------------------------------------- query_db */
register_tool({
  name: 'query_db', order: 1, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: {
    zh: '执行单条只读 SQL（SELECT/SHOW/DESC/EXPLAIN/WITH）\n' +
        '若开启 review 模式，读操作可直接执行；写操作与 DDL 会先生成审批步骤并等待确认。' +
        'DDL 请使用 run_ddl 工具，不要放进 query_db / update_data',
    en: 'Execute a single read-only SQL (SELECT/SHOW/DESC/EXPLAIN/WITH)\n' +
        'When review mode is enabled, read-only steps may execute directly; writes pause for ' +
        'approval. Use run_ddl for DDL — never query_db / update_data'
  },
  example: { sql: 'SELECT ...' },
  args: { type: 'object', required: ['sql'],
          properties: { sql: { type: 'string', minLength: 3 } } },
  messages: {
    'sql#required':  { zh: 'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...',
                       en: 'query_db missing valid sql argument. Provide a complete SQL, e.g.: SHOW TABLES or SELECT ...' },
    'sql#type':      { zh: 'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...',
                       en: 'query_db missing valid sql argument. Provide a complete SQL, e.g.: SHOW TABLES or SELECT ...' },
    'sql#minLength': { zh: 'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...',
                       en: 'query_db missing valid sql argument. Provide a complete SQL, e.g.: SHOW TABLES or SELECT ...' }
  },
  handler: impl_query_db
});

function impl_query_db(args, ctx) {
  var sql   = replace_ph(String(args.sql || ''), ctx.db);
  var stmt2 = classify_statement(sql);
  if (!stmt2.is_read || stmt2.multiple_statements)
    return { ok: false,
             response: t('拒绝：query_db 只允许单条只读语句（SELECT/SHOW/DESC/EXPLAIN/WITH）。',
                         'Rejected: query_db only allows one read-only statement (SELECT/SHOW/DESC/EXPLAIN/WITH).'),
             error: 'invalid_read_only_sql' };
  /* The read ceiling. Returned as a normal tool failure rather than a
   * validation failure, deliberately: the model should see it in the tool
   * log, count it against error_count, and rewrite the query -- which is
   * exactly what the error text asks for. See guard_read_sql(). */
  sql = apply_read_ceiling(sql, stmt2);
  var read_guard = guard_read_sql(sql, stmt2);
  if (read_guard) { read_guard.sql = sql; return read_guard; }
  try {
    var qrows = query_checked(with_read_timeout(sql, stmt2, read_timeout_ms()));
    /* Not compress(..., 1200) any more: a result too big to say is stored and
     * handed back as a preview plus an artifact_id the model can page through
     * with read_artifact.  See lib_artifact.js -- the previous behaviour cut
     * the text off mid-row and discarded the rest, and rows_to_table had
     * already dropped everything past row 150 before that. */
    return { ok: true,
             response: (rows_truncated(qrows) ? truncation_note(qrows) : '') +
                       artifact_render_result(qrows, sql),
             sql: sql };
  } catch (e) {
    var qerr = String(e);
    var recovery2 = try_recover_unknown_table(qerr, sql);
    if (recovery2) {
      try {
        var rec_stmt = classify_statement(recovery2.sql);
        var rec_rows = query_checked(
            with_read_timeout(recovery2.sql, rec_stmt, read_timeout_ms()));
        return { ok: true,
                 response: recovery2.desc + '\n' +
                           (rows_truncated(rec_rows) ? truncation_note(rec_rows) : '') +
                           artifact_render_result(rec_rows, recovery2.sql),
                 sql: recovery2.sql };
      } catch (e2) {
        qerr = String(e2);
      }
    }
    return { ok: false,
             response: t('查询执行失败：', 'Query execution failed: ') + qerr,
             error: 'query_failed', sql: sql };
  }
}

/* ------------------------------------------------------------- explain_sql */
register_tool({
  name: 'explain_sql', order: 2, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '分析执行计划，⚠ 结果中出现全表扫描时必须先改写 SQL',
         en: 'Analyze execution plan; ⚠ rewrite SQL if full table scan is detected' },
  example: { sql: 'SELECT ...' },
  args: { type: 'object', required: ['sql'],
          properties: { sql: { type: 'string', minLength: 5 } } },
  messages: {
    '*#required':  { zh: 'explain_sql 缺少有效 sql 参数', en: 'explain_sql missing valid sql argument' },
    '*#type':      { zh: 'explain_sql 缺少有效 sql 参数', en: 'explain_sql missing valid sql argument' },
    '*#minLength': { zh: 'explain_sql 缺少有效 sql 参数', en: 'explain_sql missing valid sql argument' }
  },
  handler: impl_explain_sql
});

function impl_explain_sql(args, ctx) {
  var sql = replace_ph(String(args.sql || ''), ctx.db);
  var stmt_ex = classify_statement(sql);
  if (stmt_ex.multiple_statements)
    return { ok: false,
             response: t('EXPLAIN 仅允许单条 SQL。', 'EXPLAIN accepts a single SQL statement only.'),
             error: 'multi_statement_rejected' };
  try {
    var ex = query_checked("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !ex.length)
      return { ok: false, response: t('EXPLAIN 执行失败', 'EXPLAIN execution failed'),
               error: 'explain_failed' };
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'] || JSON.stringify(ex[0]);
    return { ok: true,
             response: t('执行计划：', 'Execution plan: ') + parse_explain(String(raw)),
             sql: sql };
  } catch (e) {
    return { ok: false,
             response: t('EXPLAIN 执行失败：', 'EXPLAIN execution failed: ') + String(e),
             error: 'explain_failed', sql: sql };
  }
}

/* ---------------------------------------------------------------- plan_sql */
register_tool({
  name: 'plan_sql', order: 3, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '【多步执行】一次提交有序 SQL 列表，引擎顺序执行并返回汇总结果\n' +
             '适用：需要先 SHOW TABLES 再 SHOW CREATE、先探查结构再聚合等场景',
         en: '[Multi-step] Submit an ordered SQL list executed sequentially\n' +
             'Use for: SHOW TABLES then inspect columns, check schema then aggregate, etc.' },
  example: { steps: [{ sql: '...', desc: '...' }] },
  args: { type: 'object', required: ['steps'],
          properties: {
            steps: { type: 'array', minItems: 1,
                     items: { type: 'object', required: ['sql'],
                              properties: { sql:  { type: 'string' },
                                            desc: { type: 'string' } } } }
          } },
  messages: {
    'steps#required': { zh: 'plan_sql 缺少 steps 数组，或 steps 为空',
                        en: 'plan_sql missing steps array or steps is empty' },
    'steps#type':     { zh: 'plan_sql 缺少 steps 数组，或 steps 为空',
                        en: 'plan_sql missing steps array or steps is empty' },
    'steps#minItems': { zh: 'plan_sql 缺少 steps 数组，或 steps 为空',
                        en: 'plan_sql missing steps array or steps is empty' }
  },
  validate: validate_plan_sql,
  handler:  impl_plan_sql
});

function validate_plan_sql(args, policy) {
  var steps = args.steps || [];
  /* Element-level "missing sql" keeps its indexed message: the schema's
   * generic items error would lose the step number the model needs. */
  for (var vi = 0; vi < steps.length; vi++) {
    if (!steps[vi] || !steps[vi].sql || typeof steps[vi].sql !== 'string')
      return t('plan_sql steps[', 'plan_sql steps[') + vi +
             t('] 缺少 sql 字段', '] missing sql field');
  }
  for (var wi = 0; wi < steps.length; wi++) {
    var step_stmt = classify_statement(String(steps[wi].sql || ''));
    if (!step_stmt.is_read) {
      return t(
        'plan_sql steps[' + wi + '] 是写操作/DDL（plan_sql 只能包含只读 SQL：SELECT/SHOW/DESC/EXPLAIN/WITH）。' +
        '请把只读探查步骤留在 plan_sql 里，写操作单独用 update_data 提交，以便进入审批流程。',
        'plan_sql steps[' + wi + '] is a write/DDL statement (plan_sql may only contain read-only SQL). ' +
        'Keep read-only steps in plan_sql and submit the write separately via update_data so it goes through the approval workflow.'
      );
    }
  }
  /* Cap how many steps a single plan_sql call may queue for individual
   * step-by-step approval under review_mode.  Each step becomes its own
   * awaiting_approval round trip (see build_review_steps); without a cap the
   * model could submit dozens of steps in one shot with no natural
   * checkpoint for the user to reconsider mid-stream.  Only relevant under
   * review_mode -- the non-review execute_plan() path is independently
   * capped by MAX_PLAN_STEPS and never executes writes. */
  if (policy && policy.review_mode === 'review') {
    var max_steps = policy.max_pending_steps || 3;
    if (steps.length > max_steps)
      return t('plan_sql 提交的步骤数（', 'plan_sql submitted ') + steps.length +
             t('）超过审批模式下的单批上限（', ' steps, exceeding the review-mode per-batch limit (') +
             max_steps + t('）。请拆分为多次更小的 plan_sql 调用。', '). Split into smaller plan_sql calls.');
  }
  return null;
}

function impl_plan_sql(args, ctx) {
  var steps2    = args.steps || [];
  var plan_res  = execute_plan(steps2, ctx.db);
  var out_lines = [t('【plan_sql 多步执行结果】', '[plan_sql multi-step results]')];
  var plan_ok   = true;
  for (var pi = 0; pi < plan_res.length; pi++) {
    var pr = plan_res[pi];
    if (pr.ok === false) plan_ok = false;
    out_lines.push('Step ' + pr.step + ': ' + pr.desc + ' ');
    out_lines.push('SQL: ' + pr.sql);
    out_lines.push(pr.result);
  }
  return { ok: plan_ok,
           response: compress(out_lines.join('\n'), cfg('plan_log_max_tokens', 4000)),
           error: plan_ok ? '' : 'plan_step_failed',
           sql: JSON.stringify(steps2) };
}

/* ---------------------------------------------------------------- begin_tx */
register_tool({
  name: 'begin_tx', order: 4, category: 'tx',
  readOnly: false, write: false, ddl: false, transactional: false,
  doc: { zh: '若 CALL shannon_chat() 的调用者已 START TRANSACTION，则复用 caller transaction，' +
             '不再 START；Agent 无权提交/回滚 caller transaction\n' +
             '⚠ 若 begin_tx 报错"不允许在存储函数内开启事务"，说明本次是经由存储函数入口调用的，' +
             '该入口无法自持事务。此时不要重试 begin_tx，直接告知用户：请先在会话中执行 ' +
             'START TRANSACTION 再重新发起写请求，并由其自行 COMMIT / ROLLBACK。',
         en: 'If the caller of CALL shannon_chat() already ran START TRANSACTION, the agent joins ' +
             'that caller-owned transaction and will not COMMIT/ROLLBACK it\n' +
             '⚠ If begin_tx reports that transactions are not allowed inside a stored function, this ' +
             'invocation came through the stored-function entry point, which cannot own one. Do not ' +
             'retry begin_tx — tell the user to run START TRANSACTION in their session, reissue the ' +
             'write, and COMMIT / ROLLBACK it themselves.' },
  example: {},
  args: { type: 'object', properties: {} },
  handler: impl_begin_tx
});

function impl_begin_tx(args, ctx) {
  var begin_ctx = get_tx_context();

  if (begin_ctx.active) {
    if (begin_ctx.owner === TX_OWNER_CALLER) {
      /* Joining is logical only: do NOT issue START TRANSACTION.  The caller
       * keeps the COMMIT/ROLLBACK boundary. */
      return { ok: true,
               response: t('检测到调用者已有事务；Agent 将复用该事务，但不会 COMMIT/ROLLBACK。',
                           'Caller-owned transaction detected; the agent will join it but will not COMMIT/ROLLBACK it.'),
               tx_owner: TX_OWNER_CALLER };
    }
    return { ok: false,
             response: t('警告：Agent 事务已活跃，禁止重复 begin_tx。',
                         'Warning: agent transaction already active; duplicate begin_tx forbidden.'),
             error: 'transaction_already_active' };
  }

  if (!begin_ctx.known && begin_ctx.owner === TX_OWNER_UNKNOWN) {
    return { ok: false,
             response: t('无法可靠检测当前连接是否已有事务，拒绝 START TRANSACTION 以避免隐式提交调用者事务。'
                         + '请启用 Performance Schema transaction instrument。',
                         'Unable to reliably detect whether this connection already has a transaction. '
                         + 'START TRANSACTION is refused to avoid implicitly committing a caller transaction. '
                         + 'Enable the Performance Schema transaction instrument.'),
             error: 'transaction_state_unknown' };
  }

  if (!begin_tx_lease(A.conversation_id, '', 30)) {
    return { ok: false,
             response: t('拒绝：该会话已有另一个连接持有活跃事务租约，无法开启新事务。',
                         'Rejected: another connection already holds an active transaction lease for this conversation.'),
             error: 'lease_owned_by_other_session' };
  }

  try {
    /* query_checked, not sys.exec_sql: the latter reports a rejected
     * statement by *returning* { error: ... } rather than throwing, so the
     * catch below never fired and a transaction that was never started was
     * reported as started. */
    query_checked('START TRANSACTION');
    set_tx_active_for(A.conversation_id, true);
    var started_state = get_session_tx_state();
    if (started_state.known && started_state.active && started_state.event_id)
      set_agent_tx_event_marker(started_state.event_id);
    return { ok: true, response: t('Agent 事务已开启', 'Agent transaction started'),
             tx_owner: TX_OWNER_AGENT };
  } catch (e) {
    clear_tx_lease(A.conversation_id);
    set_agent_tx_event_marker(0);
    set_tx_active_for(A.conversation_id, false);
    return { ok: false, response: t('开启事务失败：', 'Failed to start transaction: ') + String(e),
             error: 'begin_tx_failed' };
  }
}

/* ------------------------------------------------------------- update_data */
register_tool({
  name: 'update_data', order: 5, category: 'core',
  readOnly: false, write: true, ddl: false, risk: 'medium', transactional: true,
  doc: { zh: '执行单条 DML（INSERT/UPDATE/DELETE/REPLACE）。UPDATE/DELETE 必须带顶层 WHERE；' +
             '必须在明确的事务内执行（先 begin_tx，或由调用者 START TRANSACTION）',
         en: 'Execute a single DML statement (INSERT/UPDATE/DELETE/REPLACE). UPDATE/DELETE must ' +
             'carry a top-level WHERE, and a known transaction must be open (begin_tx first)' },
  example: { sql: 'INSERT/UPDATE/DELETE ...' },
  args: { type: 'object', required: ['sql'],
          properties: { sql: { type: 'string', minLength: 5 } } },
  messages: {
    '*#required':  { zh: 'update_data 缺少有效 sql 参数', en: 'update_data missing valid sql argument' },
    '*#type':      { zh: 'update_data 缺少有效 sql 参数', en: 'update_data missing valid sql argument' },
    '*#minLength': { zh: 'update_data 缺少有效 sql 参数', en: 'update_data missing valid sql argument' }
  },
  handler: impl_update_data
});

function impl_update_data(args, ctx) {
  var sql   = replace_ph(String(args.sql || ''), ctx.db);
  var stmt3 = classify_statement(sql);
  var first = stmt3.dml_keyword || stmt3.first_keyword;

  /* Statement-shape checks first: they depend only on the SQL, so the model
   * gets the actionable message ("use run_ddl") regardless of whether a
   * transaction happens to be open. */
  if (stmt3.multiple_statements)
    return { ok: false,
             response: t('拒绝：update_data 仅允许单条 DML 语句。',
                         'Rejected: update_data accepts exactly one DML statement.'),
             error: 'multi_statement_rejected' };

  if (!stmt3.is_write)
    return { ok: false,
             response: t('拒绝：update_data 仅允许 INSERT/UPDATE/DELETE/REPLACE；DDL 请改用 run_ddl。',
                         'Rejected: update_data only allows INSERT/UPDATE/DELETE/REPLACE; use run_ddl for DDL.'),
             error: 'invalid_write_sql' };

  if ((first === 'UPDATE' || first === 'DELETE') && !stmt3.has_top_level_where)
    return { ok: false,
             response: t('拒绝：', 'Rejected: ') + first +
                       t(' 必须含顶层 WHERE 条件。', ' must contain a top-level WHERE clause.'),
             error: 'missing_where' };

  /* Checked here, in the handler, rather than only in a spec.validate hook:
   * the approval path reaches execute_tool() directly, so a step whose SQL
   * was rewritten by "Modify: ..." after validation would otherwise arrive
   * ungated.  For a gate over the grant tables that is the wrong place to
   * economise. */
  var sys_denied = check_system_schema_policy(sql, ctx.policy, ctx.db);
  if (sys_denied)
    return { ok: false, response: sys_denied.message, error: sys_denied.reason };

  var write_ctx = get_tx_context();
  if (!write_ctx.active || write_ctx.owner === TX_OWNER_UNKNOWN)
    return { ok: false,
             response: t('拒绝：写操作必须在明确的事务内，请先 begin_tx，或由调用者先 START TRANSACTION。',
                         'Rejected: writes require a known active transaction; call begin_tx or have the caller START TRANSACTION first.'),
             error: 'transaction_required' };

  try {
    /* query_checked, not sys.exec_sql: a denied or rejected DML comes back as
     * a { error: ... } return value, not an exception, so this used to fall
     * through and report 'Success' with affected_rows = -1 for a write that
     * never happened. */
    var raw_result = query_checked(sql);
    var affected = -1;
    if (raw_result && typeof raw_result.affected_rows !== 'undefined')
      affected = Number(raw_result.affected_rows);
    return { ok: true, response: t('执行成功', 'Success'), sql: sql,
             affected_rows: affected, tx_owner: write_ctx.owner };
  } catch (e) {
    return { ok: false, response: t('写操作执行失败：', 'Write execution failed: ') + String(e),
             error: 'write_failed', sql: sql };
  }
}

/* --------------------------------------------------------------- commit_tx */
register_tool({
  name: 'commit_tx', order: 6, category: 'tx',
  readOnly: false, write: false, ddl: false, transactional: false,
  doc: { zh: '提交 Agent 自己开启的事务（调用者事务无权提交）',
         en: 'Commit the agent-owned transaction (caller-owned transactions are refused)' },
  example: {},
  args: { type: 'object', properties: {} },
  handler: impl_commit_tx
});

function impl_commit_tx(args, ctx) {
  var commit_ctx = get_tx_context();
  if (!commit_ctx.active)
    return { ok: false,
             response: t('警告：当前无活跃事务。', 'Warning: no active transaction.'),
             error: 'no_active_transaction' };
  if (commit_ctx.owner !== TX_OWNER_AGENT)
    return { ok: false,
             response: t('拒绝：当前事务属于调用者，Agent 无权 COMMIT；请由外层调用者提交。',
                         'Rejected: the current transaction is caller-owned; the agent cannot COMMIT it.'),
             error: 'caller_owned_transaction' };

  try {
    query_checked('COMMIT');
    set_tx_active_for(A.conversation_id, false);
    clear_tx_lease(A.conversation_id);
    set_agent_tx_event_marker(0);
    return { ok: true, response: t('Agent 事务已提交', 'Agent transaction committed') };
  } catch (e) {
    /* Keep the lease/mirror until rollback is attempted by the caller. */
    return { ok: false, response: t('提交事务失败：', 'Transaction commit failed: ') + String(e),
             error: 'commit_failed' };
  }
}

/* ------------------------------------------------------------- rollback_tx */
register_tool({
  name: 'rollback_tx', order: 7, category: 'tx',
  readOnly: false, write: false, ddl: false, transactional: false,
  doc: { zh: '回滚 Agent 自己开启的事务（调用者事务无权回滚）',
         en: 'Roll back the agent-owned transaction (caller-owned transactions are refused)' },
  example: {},
  args: { type: 'object', properties: {} },
  handler: impl_rollback_tx
});

function impl_rollback_tx(args, ctx) {
  var rollback_ctx = get_tx_context();
  if (!rollback_ctx.active)
    return { ok: true, response: t('当前无活跃事务。', 'No active transaction.') };
  if (rollback_ctx.owner !== TX_OWNER_AGENT)
    return { ok: false,
             response: t('拒绝：当前事务属于调用者，Agent 无权 ROLLBACK；请由外层调用者回滚。',
                         'Rejected: the current transaction is caller-owned; the agent cannot ROLLBACK it.'),
             error: 'caller_owned_transaction' };
  try {
    query_checked('ROLLBACK');
    set_tx_active_for(A.conversation_id, false);
    clear_tx_lease(A.conversation_id);
    set_agent_tx_event_marker(0);
    return { ok: true, response: t('Agent 事务已回滚', 'Agent transaction rolled back') };
  } catch (e) {
    return { ok: false, response: t('回滚事务失败：', 'Transaction rollback failed: ') + String(e),
             error: 'rollback_failed' };
  }
}

/* ----------------------------------------------------------------- run_ddl */
register_tool({
  name: 'run_ddl', order: 8, category: 'ddl',
  readOnly: false, write: false, ddl: true, risk: 'high', transactional: false,
  doc: { zh: '【DDL】执行 CREATE / ALTER / RENAME 等结构变更，典型用途：\n' +
             '  ALTER TABLE db.t SECONDARY_LOAD（把表加载进 RAPID，ml_train 的前置条件）、\n' +
             '  ALTER TABLE db.t SECONDARY_UNLOAD、CREATE INDEX / ALTER TABLE ADD COLUMN\n' +
             'DDL 会隐式提交事务：若当前存在活跃事务会被拒绝，需先 COMMIT/ROLLBACK 再执行\n' +
             'DROP / TRUNCATE 等破坏性语句由系统策略控制，默认拒绝。\n' +
             '账户/角色（CREATE USER、ALTER USER、CREATE ROLE）、存储程序/触发器/事件' +
             '（CREATE FUNCTION / PROCEDURE / TRIGGER / EVENT）与实例级语句' +
             '（ALTER INSTANCE、CREATE RESOURCE GROUP）各有独立开关，同样默认拒绝\n' +
             '是否允许由系统判定，不要自行查询 @chat_options 来推断：直接发起 run_ddl 调用，' +
             '若被策略拒绝会返回明确错误，再把该错误原文转达用户即可',
         en: '[DDL] Run CREATE / ALTER / RENAME schema changes. Typical uses:\n' +
             '  ALTER TABLE db.t SECONDARY_LOAD (loads the table into RAPID — a prerequisite of ml_train),\n' +
             '  ALTER TABLE db.t SECONDARY_UNLOAD, CREATE INDEX, ALTER TABLE ADD COLUMN\n' +
             'DDL implicitly commits: it is refused while a transaction is open, so COMMIT/ROLLBACK first\n' +
             'DROP / TRUNCATE are governed by system policy and refused by default.\n' +
             'Accounts and roles (CREATE USER, ALTER USER, CREATE ROLE), stored code ' +
             '(CREATE FUNCTION / PROCEDURE / TRIGGER / EVENT) and instance-level statements ' +
             '(ALTER INSTANCE, CREATE RESOURCE GROUP) each have their own switch and are ' +
             'likewise refused by default\n' +
             'Do not try to read @chat_options to work out whether any of them are allowed — just ' +
             'issue the run_ddl call; if policy refuses it you get a clear error to relay to the user' },
  example: { sql: 'ALTER TABLE db.t SECONDARY_LOAD' },
  args: { type: 'object', required: ['sql'],
          properties: { sql: { type: 'string', minLength: 5 } } },
  messages: {
    '*#required':  { zh: 'run_ddl 缺少有效 sql 参数', en: 'run_ddl missing valid sql argument' },
    '*#type':      { zh: 'run_ddl 缺少有效 sql 参数', en: 'run_ddl missing valid sql argument' },
    '*#minLength': { zh: 'run_ddl 缺少有效 sql 参数', en: 'run_ddl missing valid sql argument' }
  },
  validate: validate_run_ddl,
  handler:  impl_run_ddl
});

function validate_run_ddl(args, policy) {
  var ddl_stmt = classify_statement(String(args.sql));
  if (ddl_stmt.multiple_statements)
    return t('run_ddl 仅允许单条 DDL 语句', 'run_ddl accepts a single DDL statement only');
  if (!ddl_stmt.is_ddl)
    return t('run_ddl 仅允许 DDL（CREATE/ALTER/DROP/TRUNCATE/RENAME）；' +
             '读查询用 query_db，写数据用 update_data。',
             'run_ddl only accepts DDL (CREATE/ALTER/DROP/TRUNCATE/RENAME); ' +
             'use query_db to read and update_data to write rows.');
  var act_denied = check_ddl_action_policy(String(args.sql), policy);
  if (act_denied) return act_denied.message;
  var sys_denied = check_system_schema_policy(String(args.sql), policy, A.current_db);
  if (sys_denied) return sys_denied.message;
  if (is_destructive_ddl(String(args.sql)) && !(policy && policy.allow_destructive_ddl))
    return t('该 DDL 会删除数据或对象，默认禁止；如确需执行请设置 ' +
             '@chat_options.allow_destructive_ddl=true。',
             'This DDL drops data or objects and is refused by default; set ' +
             '@chat_options.allow_destructive_ddl=true if it is really intended.');
  return null;
}

function impl_run_ddl(args, ctx) {
  var sql = replace_ph(String(args.sql || ''), ctx.db);
  var stmt_ddl = classify_statement(sql);

  if (stmt_ddl.multiple_statements)
    return { ok: false,
             response: t('拒绝：run_ddl 仅允许单条 DDL 语句。',
                         'Rejected: run_ddl accepts exactly one DDL statement.'),
             error: 'multi_statement_rejected' };
  if (!stmt_ddl.is_ddl)
    return { ok: false,
             response: t('拒绝：run_ddl 仅允许 DDL 语句。',
                         'Rejected: run_ddl only allows DDL statements.'),
             error: 'invalid_ddl_sql' };

  /* DDL implicitly commits whatever transaction is open.  Refuse rather than
   * silently committing the caller's -- or the agent's own -- work as a side
   * effect of an ALTER. */
  var ddl_ctx = get_tx_context();
  if (ddl_ctx.active)
    return { ok: false,
             response: t('拒绝：当前有活跃事务，执行 DDL 会隐式提交它。请先 COMMIT 或 ROLLBACK，再执行 DDL。',
                         'Rejected: a transaction is open and DDL would implicitly commit it. ' +
                         'COMMIT or ROLLBACK first, then run the DDL.'),
             error: 'ddl_would_commit_transaction' };

  /* See the same call in impl_update_data: the approval path does not
   * re-validate, so the gate has to stand here too. */
  var ddl_sys_denied = check_system_schema_policy(sql, ctx.policy, ctx.db);
  if (ddl_sys_denied)
    return { ok: false, response: ddl_sys_denied.message, error: ddl_sys_denied.reason };

  try {
    query_checked(sql);
    return { ok: true,
             response: t('DDL 执行成功：', 'DDL executed successfully: ') + compress(sql, 300),
             sql: sql };
  } catch (e) {
    return { ok: false,
             response: t('DDL 执行失败：', 'DDL execution failed: ') + String(e),
             error: 'ddl_failed', sql: sql };
  }
}

/* ----------------------------------------------------------- generate_text */
register_tool({
  name: 'generate_text', order: 9, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '直接调用 LLM 生成文本（无需访问数据库时使用）',
         en: 'Call the LLM directly to generate text (no database access needed)' },
  example: { prompt: '...' },
  args: { type: 'object', required: ['prompt'],
          properties: { prompt:  { type: 'string' },
                        options: { type: 'object' } } },
  messages: {
    'prompt#required': { zh: 'generate_text 缺少 prompt 参数', en: 'generate_text missing prompt argument' },
    'prompt#type':     { zh: 'generate_text 缺少 prompt 参数', en: 'generate_text missing prompt argument' }
  },
  handler: impl_generate_text
});

function impl_generate_text(args, ctx) {
  return { ok: true, response: ml_generate(String(args.prompt || ''), args.options || {}) };
}

/* ----------------------------------------------------------- describe_tool */
/* The other half of the collapsed catalogue.
 *
 * Collapsing a category to signatures is only safe if the full entry is still
 * reachable, otherwise the saving is paid for in tools the model calls wrong
 * or does not call at all.  This returns exactly what render_tool_docs()
 * would have emitted for one tool -- same text, same example -- so there is
 * one description of a tool, not a full one and a summarised one that drift. */
register_tool({
  name: 'describe_tool', order: 16, category: 'core',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: {
    zh: '查看某个工具的完整参数说明与调用示例\n' +
        '当工具在【其余工具】里只给了签名、而你不确定参数怎么填时使用',
    en: 'Show one tool\'s full argument documentation and a call example\n' +
        'Use it when a tool appeared only as a signature under [Other tools] and you are not ' +
        'sure how to fill its arguments'
  },
  example: { tool: 'ml_train' },
  args: { type: 'object', required: ['tool'],
          properties: { tool: { type: 'string', minLength: 2, maxLength: 64 } } },
  messages: {
    '*#required': { zh: 'describe_tool 缺少 tool 参数（要查看的工具名）',
                    en: 'describe_tool missing tool argument (the tool name to look up)' },
    '*#type':     { zh: 'describe_tool 缺少 tool 参数（要查看的工具名）',
                    en: 'describe_tool missing tool argument (the tool name to look up)' }
  },
  handler: impl_describe_tool
});

function impl_describe_tool(args, ctx) {
  var name = String(args.tool || '').trim();
  var spec = get_tool_spec(name);
  /* A hidden tool is hidden from the catalogue because the model should not
   * be choosing it, so it stays hidden from this too -- otherwise the lookup
   * becomes a way around the catalogue. */
  if (!spec || spec.hidden) {
    var names = tool_names(), visible = [];
    for (var i = 0; i < names.length; i++)
      if (!TOOL_REGISTRY[names[i]].hidden) visible.push(names[i]);
    return { ok: false, error: 'unknown_tool',
             response: t('没有名为 ' + name + ' 的工具。可用工具：',
                         'There is no tool named ' + name + '. Available tools: ') +
                       visible.join(', ') };
  }
  return { ok: true,
           response: render_tool_docs(A.lang, { only: name }),
           tool: name };
}
