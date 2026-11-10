//@include lib_tools.js
//@include lib_runtime.js
//@include lib_memory_registry.js
function shannon_agent_run_body(user_message, conversation_id) {

 // Initialize the module-level shared context.
  A.user_message    = user_message;
  conversation_id   = principal_scope_conversation_id(conversation_id);
  A.conversation_id = conversation_id;
  A.lang            = detect_lang(user_message);
  A.last_think      = '';
  A.write_tool_seen = false;
  // Why this turn ended.
  A.stop_reason     = '';
  A.current_plan_id  = '';
  var agent_response = '';

  function finish_review_reply(reason) {
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, reason);
    return agent_response;
  }

  try {

  var chat_opt = get_chat_options();

  // Loop limits.
  var MAX_TURNS         = max_turns();
  var MAX_ERRORS        = 3;
  // A turn budget is not a time budget.
  var TURN_DEADLINE_MS  = turn_deadline_ms();
  var turn_started_ms   = A.turn_started_ms;
  // Published so the model call can size its own wall clock from what is left of the turn.
  A.turn_deadline_at    = Math.min(A.turn_deadline_at, turn_started_ms + TURN_DEADLINE_MS);
  var PROMPT_TOK_LIMIT  = prompt_token_budget();
  var PROMPT_CHAR_LIMIT = prompt_char_budget();

  var mode_check = sql_mode_gate_check();
  if (!mode_check.ok) {
    agent_response = mode_check.message;
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    return agent_response;
  }

  var recovery = recover_agent_entry();
  if (!recovery.ok) return '[transaction_recovery_unverified] ' + recovery.error;

  var execution = execution_begin(user_message, chat_opt);
  if (!execution.ok) return '[' + execution.error + '] ' + (execution.execution_id || '');
  if (execution.replay) return execution.response;

  cleanup_expired_tx_leases();
  reap_dead_tx_leases();
  runtime_maintenance();

  if (conversation_id &&
      (!Array.isArray(chat_opt.chat_history) || chat_opt.chat_history.length === 0)) {
    var mem_opt_boot = get_memory_options(chat_opt);
    var recovered  = recover_chat_history_from_memory(
      conversation_id, Number(mem_opt_boot.short_term.recent_turns || 3));
    if (recovered.length > 0) {
      chat_opt.chat_history = recovered;
      save_chat_options(chat_opt);
    }
  }

  var db_rows    = query("SELECT CAST(DATABASE() AS CHAR) AS db");
  var current_db = (db_rows && Array.isArray(db_rows) && db_rows.length && db_rows[0].db)
                   ? db_rows[0].db : '';

  A.current_db = current_db;
  var request_intent = analyze_intent(user_message);
  A.request_intent = request_intent;

  var review_policy = get_review_policy(chat_opt);
  cleanup_expired_review_plans(conversation_id, review_policy.review_plan_ttl_minutes);

  recover_stuck_review_steps(conversation_id, review_policy.review_step_stuck_ttl_minutes);
  var pending_review = load_review_state(conversation_id);
  if (pending_review && pending_review.status === 'awaiting_approval') {
    var review_cmd = normalize_review_command(user_message);
    var current_step = get_pending_review_step(pending_review);
    if (!review_cmd) {
      agent_response = t('当前有待审批步骤，请先回复 Approve / Reject / Modify:SQL。',
                         'There is a pending approval step; please reply Approve / Reject / Modify:SQL.') +
                       '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
      return finish_review_reply('review:prompt');
    }

    if (!current_step) {
      clear_review_state(conversation_id, pending_review.plan_id, 'error');
      agent_response = t('审批计划状态异常，已取消。', 'Approval plan state is invalid and has been cancelled.');
      return finish_review_reply('review:error');
    }

    if (review_cmd === 'approve') {
      var approval = parse_review_approval(user_message);
      if (!approval || approval.plan_id !== pending_review.plan_id || approval.step_id !== current_step.id ||
          approval.digest !== current_step.approval_digest)
        return 'Copy the exact approval command from the review prompt.\n' +
          render_review_prompt(pending_review, current_step, review_policy);
      if (!current_step.approval_digest || review_step_digest(current_step) !== current_step.approval_digest) {
        return 'Approval content changed; review again.\n' +
          render_review_prompt(pending_review, current_step, review_policy);
      }

      A.current_plan_id = pending_review.plan_id;
      var trusted_approval_guard = trusted_review_guard(current_step,current_db);
      if (trusted_approval_guard) return '['+trusted_approval_guard.error+'] '+trusted_approval_guard.response;
      if (!claim_review_step(pending_review.plan_id, current_step.id,
                             current_step.status, 'executing', current_step.approval_digest)) {
        agent_response = t('该审批步骤已被处理或状态已变更，请刷新后重试。',
                           'This approval step has already been processed or its status changed. Please refresh and retry.');
        return finish_review_reply('review:already_claimed');
      }

      A.current_plan_id = pending_review.plan_id;

      var approved_result = execute_review_step(current_step, current_db);
      append_review_history(conversation_id, pending_review.plan_id, current_step.id, 'approve', user_message,
                            approved_result.error || approved_result.response || '', approved_result.response || '', !approved_result.ok);
      if (approved_result && approved_result.ok) {

        var state_completed = approved_result.review_state_committed ||
          transition_review_step(pending_review.plan_id, current_step.id,
                                 'completed', approved_result.response || '');
        if (!state_completed) {
          agent_response = t('步骤已执行，但审批状态更新失败；请检查 review history 后再继续。',
                             'The step executed, but review state update failed; inspect review history before continuing.');
        } else {
          pending_review.current_step_index += 1;
          if (pending_review.current_step_index < pending_review.total_steps) {
            update_review_plan_cursor(pending_review);
            current_step = get_pending_review_step(pending_review);
            agent_response = (approved_result.response || t('步骤已执行。', 'Step executed.')) +
                             '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
          } else {
            clear_review_state(conversation_id, pending_review.plan_id, 'completed');
            agent_response = approved_result.response || t('计划已全部执行完成。', 'Plan completed.');
          }
        }
      } else {
        transition_review_step(pending_review.plan_id, current_step.id, 'failed',
                               approved_result.response || '', approved_result.error || '');
        current_step.status = 'failed';
        current_step.error_text = approved_result.error || '';
        log_rollback_event(conversation_id, pending_review.plan_id, current_step.id,
                           current_step.sql, approved_result.error || '',
                           t('执行失败后回滚', 'Rolled back after failure'));
        agent_response = (approved_result && approved_result.response)
          ? approved_result.response
          : t('步骤执行失败。', 'Step execution failed.');
        agent_response += '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
      }
      return finish_review_reply('review:approve');
    }

    if (review_cmd === 'reject') {

      if (!claim_review_step(pending_review.plan_id, current_step.id,
                             current_step.status, 'rejected')) {
        agent_response = t('该审批步骤已被处理或状态已变更，请刷新后重试。',
                           'This approval step has already been processed or its status changed. Please refresh and retry.');
        return finish_review_reply('review:already_claimed');
      }

      var reject_tx_ctx = get_tx_context();
      if (reject_tx_ctx.active && reject_tx_ctx.owner === TX_OWNER_AGENT) {
        execute_tool('rollback_tx', {}, current_db);
      }

      append_review_history(conversation_id, pending_review.plan_id, current_step.id, 'reject', user_message,
                            t('用户拒绝执行当前步骤', 'User rejected current step'), '', false);
      clear_review_state(conversation_id, pending_review.plan_id, 'rejected');
      agent_response = t('已取消当前步骤。', 'Current step cancelled.');
      return finish_review_reply('review:reject');
    }

    if (review_cmd === 'modify') {
      var modifying_spec = get_tool_spec(current_step.tool);
      if (modifying_spec && modifying_spec.trusted) {
        agent_response = t('固定业务工具不能修改为自由 SQL；请 Reject 后重新提交业务参数。',
                           'Fixed business tools cannot be modified into SQL. Reject this plan and submit new business arguments.');
        return finish_review_reply('review:trusted_modify_refused');
      }
      var modified_sql = parse_review_modify(user_message);
      if (!modified_sql) {
        agent_response = t('Modify 指令格式错误，请使用 Modify:SQL。', 'Modify command format error. Use Modify:SQL.') +
                         '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
        return finish_review_reply('review:modify_error');
      }
      current_step.execution_db = current_db;
      current_step.sql = modified_sql;
      current_step.args = current_step.args || {};
      current_step.args.sql = modified_sql;
      current_step.affected_tables = infer_affected_tables(modified_sql);

      var modified_stmt = classify_statement(modified_sql);
      current_step.writes = modified_stmt.is_write;
      current_step.ddl = modified_stmt.is_ddl;
      current_step.tool = modified_stmt.is_write ? 'update_data' : 'query_db';
      current_step.risk = infer_step_risk(modified_sql, current_step.tool);

      if (!transition_review_step(pending_review.plan_id, current_step.id, 'pending', '', 'user_modified')) {
        agent_response = t('该审批步骤已被处理或状态已变更，修改无效，请刷新后重试。',
                           'This approval step has already been processed; modification is no longer valid. Please refresh and retry.');
        return finish_review_reply('review:modify_refused');
      }
      if (!update_review_step_definition(pending_review.plan_id, current_step)) {
        agent_response = t('审批步骤状态已变化，SQL 修改未能持久化，请刷新后重试。',
                           'Approval step state changed; modified SQL was not persisted. Please refresh and retry.');
      } else {
        current_step.status = 'pending';
        agent_response = render_review_prompt(pending_review, current_step, review_policy);
      }
      return finish_review_reply('review:modify');
    }
  }

  /* ROUTE A: CATALOG */
  var cat = catalog_match(user_message);
  if (cat && evaluate_step_policy(build_review_step(
      { tool: 'query_db' }, { sql: cat.sql }, current_db, false), review_policy).action === 'execute') {
    var cat_result = execute_tool('query_db', { sql: cat.sql }, current_db);
    agent_response = t('【', '[') + cat.desc + t('】\n', ']\n') + (cat_result.response || '');
    A.stop_reason = cat_result.ok ? 'finish' : 'tool_error';
    A.route = 'catalog';
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, 'catalog:' + cat.sql);
    log_sql_trace(conversation_id, 0, 1, 'catalog', 'query_db', cat.sql, cat.desc, agent_response, !!cat_result.ok && !cat_result.error);
    return agent_response;
  }

  // Rule planning and knowledge retrieval use the same tool loop.

  A.route = 'tool_loop';
  /* Shared tool loop. */
  // MEM.build_block is the loop's only memory entry point.
  var mem_opt = JSON.parse(JSON.stringify(get_memory_options(chat_opt)));
  var dialect = llm_tool_dialect();
  var native_tools = dialect ? llm_tools_extra_body(dialect) : null;
  var fixed_cost = est_tok(build_system_prompt(current_db, '', '', '', '')) +
    est_tok(user_message) + (native_tools ? est_tok(JSON.stringify(native_tools)) : 0) + 256;
  var remaining_tokens = Math.max(0, PROMPT_TOK_LIMIT - fixed_cost);
  if (!remaining_tokens) return '[context_budget_too_small] Increase the model context window or reduce tool metadata.';
  var context_cap=Math.floor(remaining_tokens*0.2);
  var rag_cap=Math.min(1000,context_cap);
  var rag_block=build_rag_context(user_message,chat_opt,current_db,rag_cap);
  var memory_cap = Math.min(Number(mem_opt.budget.max_block_tokens || 3000),
                            Math.max(0,context_cap-est_tok(rag_block)));
  mem_opt.budget.max_block_tokens = memory_cap;
  var mem_block = memory_cap > 0 ? MEM.build_block(user_message, conversation_id, mem_opt) : '';
  if(rag_block) mem_block+=(mem_block ? '\n\n' : '')+rag_block;
  var schema_embeddings_ready = check_schema_embeddings_ready(current_db);
  var logical_tasks = decompose_query(user_message);
  var available_tokens = Math.max(0, remaining_tokens - est_tok(mem_block));

  var schema_ctx = build_schema_context(current_db, available_tokens, chat_opt, request_intent, user_message,
    schema_embeddings_ready
  );

  var join_hint = '';
  if (!schema_embeddings_ready &&
      logical_tasks.some(function(task){ return task.op === 'join'; })) {
    var schema_graph = build_schema_graph(current_db);
    var tbl_m = schema_ctx.match(/^(\w+)\(/gm);
    if (tbl_m && tbl_m.length >= 2) {
      var candidates = tbl_m.slice(0, 4).map(function(s){ return s.replace('(', ''); });
      join_hint = suggest_joins(schema_graph, candidates);
    }
  }

  var plan_hint = logical_plan_to_hint(logical_tasks, schema_embeddings_ready);

  var system_prompt_base = build_system_prompt(
    current_db, schema_ctx, join_hint, plan_hint, mem_block
  );

  var prompt_prefix = build_task_header(request_intent) + '\n\n' + system_prompt_base;
  var full_prompt   = prompt_prefix;
  var prompt_tokens = est_tok(full_prompt);

  var tool_log = '', last_result = '', need_summary = false, error_count = 0;
  /* One correction per turn; see the handed-back-SQL branch in the loop. */
  var handed_back_sql_retried = false;
  var seen_tool_sigs = {};
  // Signatures whose execution failed.
  var failed_tool_sigs = {};
  var tx_turns = 0;

  var context_retry_used = false;

  for (var turn = 0; turn < MAX_TURNS; turn++) {
    A.turn_count = turn + 1;
    if (A.cancelled) {
      A.stop_reason = 'cancelled'; need_summary = false;
      agent_response = last_result || t('本轮已取消。', 'This turn was cancelled.');
      break;
    }

    if (Date.now() - turn_started_ms > TURN_DEADLINE_MS) {
      need_summary  = true;
      A.stop_reason = 'deadline';
      break;
    }

    var llm_out = ml_generate(full_prompt, { tools: true });

    // The model call itself failed, as opposed to the model having nothing to say.
    var llm_status = A.last_llm_status;
    if (llm_status && llm_status.ok === false) {
      if (llm_status.kind === 'context_length' && !context_retry_used) {

        context_retry_used = true;
        var shrunk = compact_transcript(prompt_prefix, tool_log, turn,
                                        Math.floor(full_prompt.length * 0.6));
        if (shrunk) {
          full_prompt   = shrunk;
          prompt_tokens = est_tok(full_prompt);
          tool_log += '\n' + t('[上下文压缩] 模型报告上下文过长，已压缩后重试。',
                               '[Compacted] The model reported the context was too long; retried smaller.') + '\n';
          A.compactions = (A.compactions || 0) + 1;
          turn--;   /* this turn did not happen */
          continue;
        }
      }
      agent_response = llm_failure_message();
      need_summary   = false;
      A.stop_reason  = llm_status.kind === 'cancelled' ? 'cancelled' : 'llm_error';
      break;
    }

    var tool_obj = (llm_status && llm_status.tool_call) || parse_tool_call(llm_out);

    if (!tool_obj) {
      var llm_text = llm_out ? llm_out.trim() : '';

      if (llm_text.length > 0 && !handed_back_sql_retried &&
          request_intent && request_intent.kind === 'write' && !A.write_tool_seen &&
          looks_like_handed_back_sql(llm_text)) {
        handed_back_sql_retried = true;
        var exec_hint = t(
          '\n[纠正] 用户要求的是执行这项变更，不是让他自己去跑 SQL。请改为调用 update_data（DML）'
          + '或 run_ddl（DDL）把上面的语句提交执行；需要确认时审批流程会暂停并展示给用户。'
          + '若你判断这项变更不应执行，请直接说明理由，不要给出待执行的 SQL。\n【助手】\n',
          '\n[Correction] The user asked for the change to be made, not for SQL to run themselves. '
          + 'Call update_data (DML) or run_ddl (DDL) to submit the statement above; where confirmation '
          + 'is needed the approval flow pauses and shows it to the user. If you believe the change '
          + 'should not be made, say so plainly instead of handing back SQL.\n[Assistant]\n');
        // Not gated on the prompt budget.
        var echoed = llm_text.length > 400 ? llm_text.substring(0, 400) : llm_text;
        full_prompt   += echoed + exec_hint;
        prompt_tokens += est_tok(echoed) + est_tok(exec_hint);
        continue;
      }

      if (llm_text.length > 0) {
        agent_response = llm_text;
        need_summary   = false;

        A.stop_reason = (llm_status && llm_status.finish_reason === 'length')
                        ? 'truncated' : 'finish';
      } else {

        need_summary  = true;
        A.stop_reason = 'empty_completion';
      }
      break;
    }

    var cur_sig = tool_obj.tool + '|' + JSON.stringify(tool_obj.args || {});
    if (seen_tool_sigs[cur_sig]) {
      tool_log += '\n' + t('[重复检测] 模型输出与之前某轮重复，终止循环: ',
                          '[Loop detected] Same tool call as a previous turn, breaking: ') +
                  cur_sig.substring(0, 80);
      A.stop_reason = 'loop_detected';

      need_summary = true;
      break;
    }
    seen_tool_sigs[cur_sig] = true;

    var validation_error = validate_tool_call(tool_obj, review_policy);
    if (validation_error) {
      var err_hint =
        '\n' + t('[校验错误] ', '[Validation error] ') + validation_error +
        t('\n请重新输出合法 JSON（args.sql 不能为空）：\n【助手】\n',
          '\nPlease re-output a valid JSON (args.sql cannot be empty):\n[Assistant]\n');
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 工具校验失败次数过多：', '[Aborted] Too many validation failures: ') +
          validation_error;
        need_summary = true;
        A.stop_reason = 'error_budget';
        break;
      }
      if (prompt_tokens + est_tok(llm_out) + est_tok(err_hint) <= PROMPT_TOK_LIMIT) {
        full_prompt   += llm_out.trim() + err_hint;
        prompt_tokens += est_tok(llm_out) + est_tok(err_hint);
      }
      continue;
    }

    var step_for_exec = build_review_step(tool_obj, tool_obj.args || {}, current_db);
    if (step_for_exec.writes || step_for_exec.ddl) A.write_tool_seen = true;
    var step_policy = evaluate_step_policy(step_for_exec, review_policy);
    if (step_policy.action === 'pause') {
      // Never keep an AGENT-owned transaction open while waiting for a human approval response.
      var pause_tx_ctx = get_tx_context();
      if (pause_tx_ctx.active && pause_tx_ctx.owner === TX_OWNER_AGENT) {
        execute_tool('rollback_tx', {}, current_db);
      }

      var review_steps = build_review_steps(tool_obj, tool_obj.args || {}, current_db);
      var review_state = {
        plan_id: gen_query_id(),
        conversation_id: conversation_id,
        status: 'awaiting_approval',
        description: tool_obj.thought || '',
        current_step_index: 0,
        total_steps: review_steps.length,
        steps: review_steps
      };

      if (!save_review_plan(review_state)) {
        agent_response = t('无法保存待审批计划，本次操作已取消（此前的待审批计划也已失效）。请重新发起。',
                           'The plan could not be saved, so nothing was executed. Any plan that ' +
                           'was previously awaiting approval has been superseded -- please ask again.');
        need_summary = false;
        A.stop_reason = 'review_save_failed';
        break;
      }
      var current_step = get_pending_review_step(review_state);
      agent_response = render_review_prompt(review_state, current_step, review_policy);
      need_summary = false;
      A.stop_reason = 'awaiting_approval';
      break;
    }

    var result_obj;
    if (step_policy.action === 'reject') {
      result_obj = { ok: false,
                     response: step_policy.message || t('该操作被策略拒绝。', 'Operation rejected by policy.'),
                     error: step_policy.reason || 'policy_reject' };
    } else {
    try {
      result_obj = execute_review_step(step_for_exec, current_db);
    } catch (e) {
      result_obj = { ok: false,
                     response: t('工具执行异常：', 'Tool execution exception: ') + String(e),
                     error: 'exception' };
    }
    }

    var result_text = result_obj && result_obj.response ? String(result_obj.response) : '';
    log_sql_trace(conversation_id, turn, turn + 1, 'agent_loop', tool_obj.tool,
                  (tool_obj.args && tool_obj.args.sql) ? tool_obj.args.sql : '',
                  tool_obj.thought, result_text, !!result_obj.ok && !result_obj.error);

    // Single consecutive-error counter for the whole turn.
    // Failure is read from the tool's own channel, never from the text it rendered for the model.
    var step_failed = !result_obj.ok || !!result_obj.error;

    if (step_failed) {
      failed_tool_sigs[cur_sig] = true;
      if (!result_obj.ok)
        tool_log += '\n[执行失败] ' + (result_obj.error || 'unknown_error') + '\n';
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 连续错误，强制摘要。', '[Aborted] Consecutive errors, forcing summary.');
        last_result   = result_text;
        need_summary  = true;
        A.stop_reason = 'error_budget';
        break;
      }
    } else {
      error_count = 0;

      for (var fsig in failed_tool_sigs) {
        if (Object.prototype.hasOwnProperty.call(failed_tool_sigs, fsig))
          delete seen_tool_sigs[fsig];
      }
      failed_tool_sigs = {};
    }

    var loop_tx_ctx = get_tx_context();
    if (loop_tx_ctx.active && loop_tx_ctx.owner === TX_OWNER_AGENT &&
        tool_obj.tool !== 'begin_tx') {
      tx_turns += 1;
      if (tx_turns >= 3) {
        tool_log += '\n[安全网] Agent 自有事务保持超过安全轮次，建议尽快提交或回滚。';
        need_summary = true;
        A.stop_reason = 'tx_safety';
        break;
      }
    }

    last_result = result_text;
    need_summary = true;

    if (tool_obj.tool === 'plan_sql') {
      tool_log += '\n[Step ' + (turn+1) + '] tool=plan_sql' +
                  ' thought=' + (tool_obj.thought || '') +
                  '\nresult=' + compress(result_text, cfg('plan_log_max_tokens', 4000)) + '\n';
      need_summary = true;
      // 'finish' only when the steps actually produced something.
      A.stop_reason = step_failed ? 'tool_failed' : 'finish';
      break;
    }

    tool_log += '\n[Step ' + (turn+1) + '] tool=' + tool_obj.tool +
                ' thought=' + (tool_obj.thought || '') +
                '\nresult=' + compress(result_text, 1200) + '\n';

    if (tool_obj.tool === 'commit_tx' || tool_obj.tool === 'rollback_tx') {
      tx_turns = 0;
      need_summary = true;
      A.stop_reason = step_failed ? 'tool_failed' : 'finish';
      break;
    }
    if (tool_obj.tool === 'generate_text') {
      if (result_text && result_text.length > 0) {
        agent_response = result_text;
        need_summary = false;
      } else {
        /* generate_text returned empty — force final_summary */
        need_summary = true;
      }
      A.stop_reason = step_failed ? 'tool_failed' : 'finish';
      break;
    }

    // Terminal ML tools: the tool result IS the answer, so stop the loop.
    if (['ml_train','ml_predict_row','ml_predict_table',
         'ml_explain','ml_explain_row','ml_explain_table',
         'ml_score','ml_model_export','ml_model_import',
         'ml_list_models','ml_embed_table','ml_generate_table',
         'ml_rag_table'].indexOf(tool_obj.tool) !== -1) {
      if (result_obj.ok && result_text && result_text.length > 0) {
        agent_response = result_text;
        need_summary = false;
      } else {
        need_summary = true;
      }
      A.stop_reason = step_failed ? 'tool_failed' : 'finish';
      break;
    }

    // Hand the result back with a reminder that the turn is not over yet.
    var append =
      '\n' + t('工具结果：', 'Tool result: ') +
      compress(result_text, 1200) + '\n' +
      'Evidence: ' + JSON.stringify({trace_id:result_obj.trace_id,span_id:result_obj.span_id,
        citations:(result_obj.citations || []).slice(0,8).map(function(ref) {
          return {artifact_id:ref.artifact_id,content_hash:ref.content_hash,source_sql:ref.source_sql,
            row_count:ref.row_count,truncated:ref.truncated,limit_reached:ref.limit_reached,preview_only:ref.preview_only};
        }),consistency:result_obj.result && result_obj.result.consistency}) + '\n' +
      t('若上述结果尚不足以回答用户问题（例如只确认了表结构、还没有取到数据），' +
        '请继续调用工具把数据取出来；只有在已经获得回答所需的全部数据后，才输出最终答案。\n',
        'If that is not yet enough to answer the user -- for example you have only confirmed ' +
        'the schema and have not retrieved any data -- call another tool and get it. Produce the ' +
        'final answer only once you actually hold the data the answer needs.\n') +
      t('【助手】\n', '[Assistant]\n');

    // What the assistant said on this turn, as the transcript records it.
    var turn_text = llm_out.trim();
    if (tool_obj && tool_obj.native) {
      turn_text = (tool_obj.thought ? tool_obj.thought.trim() + '\n' : '') +
                  JSON.stringify({ tool: tool_obj.tool, args: tool_obj.args || {} });
    }

    full_prompt   += turn_text + append;
    prompt_tokens += est_tok(turn_text) + est_tok(append);

    // Over budget: compact and keep going, rather than flagging and carrying on regardless.
    var over_tokens = prompt_tokens > PROMPT_TOK_LIMIT;
    var over_chars  = full_prompt.length > PROMPT_CHAR_LIMIT;
    if (over_tokens || over_chars) {
      if (request_intent && request_intent.kind === 'write' && !A.write_tool_seen) {
        agent_response = t(
          '由于本次涉及的表结构较多，上下文预算已用尽，写操作尚未提交确认，暂无法继续执行。请缩小范围后重试（例如指定更具体的部门或日期区间）。',
          'The schema context for this request was too large to fit the remaining budget, so the write operation was never proposed for approval. Please retry with a narrower scope.'
        );
        need_summary  = false;
        A.stop_reason = 'context_exhausted';
        break;
      }

      var compacted = compact_transcript(prompt_prefix, tool_log, turn,
                                         over_chars ? PROMPT_CHAR_LIMIT : 0);
      if (compacted) {
        full_prompt   = compacted;
        prompt_tokens = est_tok(full_prompt);
        tool_log += '\n' + t('[上下文压缩] 已将较早的步骤压缩为摘要以继续执行。',
                             '[Compacted] Older steps were summarised so the run could continue.') + '\n';
        A.compactions = (A.compactions || 0) + 1;
      } else {

        need_summary  = true;
        A.stop_reason = 'context_exhausted';
        break;
      }
    }
  }

  // The loop ran to its turn ceiling without any branch deciding to stop.
  if (!A.stop_reason) A.stop_reason = 'max_turns';

  /* Safety net: force-rollback any uncommitted transaction */
  A.runtime_cleanup = true;
  var safety_net_msg = finalize_tx_safety_net();
  A.runtime_cleanup = false;
  if (safety_net_msg) {
    tool_log += safety_net_msg;
    if (need_summary)
      last_result += safety_net_msg;
  }

  if (need_summary && tool_log.length > 0) {
    var summary = final_summary(system_prompt_base, tool_log);
    agent_response = (summary && summary.trim().length > 0) ? summary.trim() : last_result;
  }

  if (!agent_response || agent_response.length === 0)
    agent_response = last_result.length > 0
      ? last_result
      : t('抱歉，未能生成有效回答，请重试。',
          'Sorry, unable to generate a valid response. Please try again.');

  if (safety_net_msg) agent_response += safety_net_msg;

  // Say so when the answer is not a finished one.
  var looks_like_raw_sql = !!agent_response &&
    /^(?:共\s*\d+\s*条：|Total\s+\d+\s+rows:)/.test(agent_response);

  var leaked_tool_call = !!parse_tool_call(agent_response);
  if (!agent_response || agent_response.length === 0 ||
      leaked_tool_call || looks_like_raw_sql) {
    // Retry final_summary up to 2 times, re-checking for leaked tool calls on each attempt.
    var safety_retries = 0;
    while (safety_retries < 2 && tool_log.length > 0) {
      agent_response = final_summary(system_prompt_base, tool_log);
      if (!agent_response || agent_response.length === 0) break;
      leaked_tool_call = !!parse_tool_call(agent_response);
      if (!leaked_tool_call) break;
      safety_retries++;
    }
    if (!agent_response || agent_response.length === 0 || leaked_tool_call) {
      agent_response = (tool_log.length > 0 && last_result.length > 0)
        ? last_result
        : t('抱歉，未能生成有效回答，请重试。',
            'Sorry, unable to generate a valid response. Please try again.');
    }
  }

  if (A.truncated_evidence)
    agent_response += t('\n（结果仅展示部分数据，不能据此推导全量总计；请读取存档或使用聚合查询。）',
      '\n(Only part of the evidence was shown. Read the artifact or run an aggregate before claiming full-data totals.)');
  chat_opt.result_citations = A.result_citations || [];
  chat_opt.trace_id = current_turn_id();
  chat_opt.rag_context = A.rag_context;

  // Say so when the answer is not a finished one -- and say it last.
  var incomplete_note = stop_reason_note(A.stop_reason);
  if (incomplete_note) agent_response = agent_response + '\n\n' + incomplete_note;

  chat_opt = update_chat_history(chat_opt, user_message, agent_response);
  chat_opt.response = agent_response; chat_opt.request_completed = true;

  persist_turn(conversation_id, user_message, agent_response,
               tool_log + think_suffix(), 'agent_loop');


  MEM.short.compact(conversation_id, mem_opt);

  if (A.memory_degraded) {
    chat_opt.memory_degraded = true;
    chat_opt.memory_degraded_reason = A.memory_degraded_reason || '';
  }
  save_chat_options(chat_opt);
  return agent_response;

  } finally {

    A.runtime_cleanup = true;
    finalize_tx_safety_net();
  }
}

