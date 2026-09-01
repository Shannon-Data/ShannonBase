//@include lib_tools.js

function shannon_agent_run(user_message, conversation_id) {

 /* Initialize the module-level shared context. This step must be performed
  * before any t() invocation — it aligns exactly with the original code's
  * requirement that "var lang = detect_lang(user_message) must come first",
  * with the only difference being that the assignment target has shifted from
  * a local variable to A.* properties. */

  A.user_message    = user_message;
  conversation_id   = principal_scope_conversation_id(conversation_id);
  A.conversation_id = conversation_id;
  A.lang            = detect_lang(user_message);
  A.last_think      = '';
  A.write_tool_seen = false;
  A.current_plan_id  = '';
  /* Rollback any dangling transaction left behind from a previous invocation
   * (e.g. the agent loop threw an unhandled exception before reaching the
   * safety-net).  Also clean up any tx leases that have exceeded their
   * timeout (e.g. user opened begin_tx then went idle for 30+ minutes). */
  if (typeof A.conversation_id !== 'undefined') {
    cleanup_expired_tx_leases();
    finalize_tx_safety_net();
  }

  var agent_response = '';

  try {

  var MAX_TURNS        = 10;
  var PROMPT_TOK_LIMIT = 102800;
  var MAX_ERRORS       = 3;

  var chat_opt = get_chat_options();

  if (conversation_id &&
      (!Array.isArray(chat_opt.chat_history) || chat_opt.chat_history.length === 0)) {
    var max_turns  = (chat_opt.history_length >= 0) ? chat_opt.history_length : 3;
    var recovered  = recover_chat_history_from_memory(conversation_id, max_turns);
    if (recovered.length > 0) {
      chat_opt.chat_history = recovered;
      save_chat_options(chat_opt);
    }
  }

  var db_rows    = query("SELECT CAST(DATABASE() AS CHAR) AS db");
  var current_db = (db_rows && Array.isArray(db_rows) && db_rows.length && db_rows[0].db)
                   ? db_rows[0].db : '';
  var request_intent = analyze_intent(user_message);
  A.request_intent = request_intent;

  var review_policy = get_review_policy(chat_opt);
  cleanup_expired_review_plans(conversation_id, review_policy.review_plan_ttl_minutes);
  var pending_review = load_review_state(conversation_id);
  if (pending_review && pending_review.status === 'awaiting_approval') {
    var review_cmd = normalize_review_command(user_message);
    var current_step = get_pending_review_step(pending_review);
    if (!review_cmd) {
      agent_response = t('当前有待审批步骤，请先回复 Approve / Reject / Modify:SQL。',
                         'There is a pending approval step; please reply Approve / Reject / Modify:SQL.') +
                       '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'review:prompt');
      return agent_response;
    }

    if (!current_step) {
      clear_review_state(conversation_id, pending_review.plan_id, 'error');
      agent_response = t('审批计划状态异常，已取消。', 'Approval plan state is invalid and has been cancelled.');
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'review:error');
      return agent_response;
    }

    if (review_cmd === 'approve') {
      /* Atomically claim the step — prevents two concurrent approve requests
       * from both executing the same write SQL.  The UPDATE ... WHERE
       * status=? clause ensures only one caller wins. */
      if (!claim_review_step(pending_review.plan_id, current_step.id,
                             current_step.status, 'executing')) {
        agent_response = t('该审批步骤已被处理或状态已变更，请刷新后重试。',
                           'This approval step has already been processed or its status changed. Please refresh and retry.');
        chat_opt = update_chat_history(chat_opt, user_message, agent_response);
        chat_opt.response = agent_response; chat_opt.request_completed = true;
        save_chat_options(chat_opt);
        persist_turn(conversation_id, user_message, agent_response, 'review:already_claimed');
        return agent_response;
      }

      A.current_plan_id = pending_review.plan_id;

      var approved_result = execute_review_step(current_step, current_db);
      append_review_history(conversation_id, pending_review.plan_id, current_step.id, 'approve', user_message,
                            approved_result.error || approved_result.response || '', approved_result.response || '', !approved_result.ok);
      if (approved_result && approved_result.ok) {
        /* execute_review_step() only returns ok=true after an AGENT-owned
         * transaction it started has committed successfully.  For a
         * CALLER-owned transaction it returns pending_caller_commit=true and
         * never commits/rolls back the caller boundary. */
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
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'review:approve');
      return agent_response;
    }

    if (review_cmd === 'reject') {
      /* Atomically claim the step as rejected — same CAS pattern as
       * approve, preventing a concurrent approve from having already
       * executed the write while we blindly clear the plan. */
      if (!claim_review_step(pending_review.plan_id, current_step.id,
                             current_step.status, 'rejected')) {
        agent_response = t('该审批步骤已被处理或状态已变更，请刷新后重试。',
                           'This approval step has already been processed or its status changed. Please refresh and retry.');
        chat_opt = update_chat_history(chat_opt, user_message, agent_response);
        chat_opt.response = agent_response; chat_opt.request_completed = true;
        save_chat_options(chat_opt);
        persist_turn(conversation_id, user_message, agent_response, 'review:already_claimed');
        return agent_response;
      }

      var reject_tx_ctx = get_tx_context();
      if (reject_tx_ctx.active && reject_tx_ctx.owner === TX_OWNER_AGENT) {
        execute_tool('rollback_tx', {}, current_db);
      }

      append_review_history(conversation_id, pending_review.plan_id, current_step.id, 'reject', user_message,
                            t('用户拒绝执行当前步骤', 'User rejected current step'), '', false);
      clear_review_state(conversation_id, pending_review.plan_id, 'rejected');
      agent_response = t('已取消当前步骤。', 'Current step cancelled.');
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'review:reject');
      return agent_response;
    }

    if (review_cmd === 'modify') {
      var modified_sql = parse_review_modify(user_message);
      if (!modified_sql) {
        agent_response = t('Modify 指令格式错误，请使用 Modify:SQL。', 'Modify command format error. Use Modify:SQL.') +
                         '\n\n' + render_review_prompt(pending_review, current_step, review_policy);
        chat_opt = update_chat_history(chat_opt, user_message, agent_response);
        chat_opt.response = agent_response; chat_opt.request_completed = true;
        save_chat_options(chat_opt);
        persist_turn(conversation_id, user_message, agent_response, 'review:modify_error');
        return agent_response;
      }
      current_step.sql = modified_sql;
      current_step.args = current_step.args || {};
      current_step.args.sql = modified_sql;
      current_step.affected_tables = infer_affected_tables(modified_sql);

      var modified_stmt = classify_statement(modified_sql);
      current_step.writes = modified_stmt.is_write;
      current_step.ddl = modified_stmt.is_ddl;
      current_step.tool = modified_stmt.is_write ? 'update_data' : 'query_db';
      current_step.risk = infer_step_risk(modified_sql, current_step.tool);
      /* transition_review_step now returns false if the step is no longer
       * in a modifiable state (e.g. a concurrent approve already claimed
       * it).  We must honour that refusal instead of blindly saving. */
      if (!transition_review_step(pending_review.plan_id, current_step.id, 'pending', '', 'user_modified')) {
        agent_response = t('该审批步骤已被处理或状态已变更，修改无效，请刷新后重试。',
                           'This approval step has already been processed; modification is no longer valid. Please refresh and retry.');
        chat_opt = update_chat_history(chat_opt, user_message, agent_response);
        chat_opt.response = agent_response; chat_opt.request_completed = true;
        save_chat_options(chat_opt);
        persist_turn(conversation_id, user_message, agent_response, 'review:modify_refused');
        return agent_response;
      }
      if (!update_review_step_definition(pending_review.plan_id, current_step)) {
        agent_response = t('审批步骤状态已变化，SQL 修改未能持久化，请刷新后重试。',
                           'Approval step state changed; modified SQL was not persisted. Please refresh and retry.');
      } else {
        current_step.status = 'pending';
        agent_response = render_review_prompt(pending_review, current_step, review_policy);
      }
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'review:modify');

      return agent_response;
    }
  }

  /* ROUTE A: CATALOG */
  var cat = catalog_match(user_message);
  if (cat) {
    agent_response = t('【', '[') + cat.desc + t('】\n', ']\n') +
                     rows_to_text(query(cat.sql));
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, 'catalog:' + cat.sql);
    log_sql_trace(conversation_id, 0, 1, 'catalog', 'query_db', cat.sql, cat.desc, agent_response);
    return agent_response;
  }

  /* ROUTE B: RULE PLANNER (schema / DDL introspection) */
  var rule_steps = rule_planner(user_message, current_db);
  if (rule_steps !== null) {
    var plan_results = execute_plan(rule_steps, current_db);
    var plan_log = '';
    for (var pri = 0; pri < plan_results.length; pri++) {
      plan_log += '\n--- Step ' + plan_results[pri].step + ': ' +
                  plan_results[pri].desc + ' ---\n' +
                  plan_results[pri].result + '\n';
    }

    var rule_summary_prompt =
      t('你是 ShannonBase 数据库助手。\n', 'You are ShannonBase database assistant.\n') +
      t('当前库：', 'Current database: ') + current_db + '\n\n' +
      t('用户问题：', 'User question: ')  + user_message + '\n\n' +
      t('已执行查询结果：\n', 'Query results:\n') +
      compress(plan_log, cfg('plan_log_max_tokens', 4000)) + '\n\n' +
      t('请用清晰的中文直接回答用户问题。如果当前信息（仅有表名、行数、注释）不足以回答，' +
        '你需要先获取相关表的列结构（字段名、数据类型），请输出如下 JSON 工具调用：\n' +
        '{"tool":"describe_table","args":{"tables":["table1","table2"]},"thought":"需要查看这些表的列信息"}\n' +
        '系统会执行 DESCRIBE 并将列信息反馈给你。获取列信息后，如果需要查询实际数据，' +
        '请使用 query_db 工具：\n' +
        '{"tool":"query_db","args":{"sql":"你的SQL语句"},"thought":"查询原因"}\n' +
        '如果你已有足够信息直接回答，就不要输出 JSON，直接给出答案。\n' +
        '⛔ 严格要求：表名、列名必须与查询结果一致，禁止编造：\n',
        'Answer the user\'s question clearly and directly. If current info (only table names, row counts, comments) ' +
        'is insufficient, first get column structures by outputting this JSON tool call:\n' +
        '{"tool":"describe_table","args":{"tables":["table1","table2"]},"thought":"need column info for these tables"}\n' +
        'The system will DESCRIBE those tables and feed back column info. After getting column info, ' +
        'if you need to query actual data, use query_db tool:\n' +
        '{"tool":"query_db","args":{"sql":"your SQL"},"thought":"reason"}\n' +
        'If you already have enough info, answer directly without JSON.\n' +
        '⛔ Strict: table/column names must match query results, never fabricate:\n');

    // Mini-loop: allow up to 5 tool-call rounds (describe_table / query_db) to gather info
    var rule_tool_log = '';
    var MAX_RULE_TURNS = 5;
    for (var rule_turn = 0; rule_turn < MAX_RULE_TURNS; rule_turn++) {
      agent_response = ml_generate(rule_summary_prompt, { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) });

      var rule_tool = parse_tool_call(agent_response);
      if (!rule_tool) {
        // No tool call — final answer
        break;
      }

      // --- describe_table -------------------------------------------------
      if (rule_tool.tool === 'describe_table') {
        var tables = rule_tool.args && rule_tool.args.tables;
        if (!Array.isArray(tables) || tables.length === 0) break;

        // Execute DESCRIBE for each requested table — use raw output as-is
        var describe_results = '';
        for (var ti = 0; ti < tables.length && ti < 8; ti++) {
          var tname = String(tables[ti]);
          if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(tname)) continue;
          var desc_raw = query("DESCRIBE " + tname);
          describe_results += '\n--- ' + tname + ' ---\n' + rows_to_text(desc_raw) + '\n';
        }

        rule_tool_log += '\n[Schema Tool] describe_table: ' + JSON.stringify(tables) +
                         '\n' + compress(describe_results, 2000);

        var is_last = (rule_turn >= MAX_RULE_TURNS - 1);
        rule_summary_prompt += '\n' + agent_response.trim() + '\n' +
          t('工具执行结果（列信息）：\n', 'Tool result (column info):\n') +
          compress(describe_results, 3000) + '\n';

        if (is_last) {
          rule_summary_prompt +=
            t('现在请基于完整的列信息直接回答用户问题，不要再输出 JSON：\n',
              'Now please answer the user question directly based on the full column info, no more JSON:\n');
        } else {
          rule_summary_prompt +=
            t('现在你有了列信息。如果可以直接回答，请给出最终答案。\n' +
              '如果还需要执行 SQL 查询获取实际数据，请输出 JSON 工具调用：\n' +
              '{"tool":"query_db","args":{"sql":"你的SQL语句"},"thought":"查询原因"}\n' +
              '【助手】\n',
              'Now you have column info. If you can answer directly, give the final answer.\n' +
              'If you still need to execute SQL queries to get actual data, output a JSON tool call:\n' +
              '{"tool":"query_db","args":{"sql":"your SQL"},"thought":"reason"}\n' +
              '[Assistant]\n');
        }
        continue;
      }

      // --- query_db --------------------------------------------------------
      if (rule_tool.tool === 'query_db') {
        var sql = String((rule_tool.args && rule_tool.args.sql) || '');
        if (!sql || sql.trim().length < 3) {
          rule_summary_prompt += '\n' +
            t('[校验错误] query_db 的 sql 参数不能为空。\n【助手】\n',
              '[Validation error] query_db sql cannot be empty.\n[Assistant]\n');
          continue;
        }
        var stmt = classify_statement(sql);
        if (!stmt.is_read) {
          rule_summary_prompt += '\n' +
            t('[错误] 仅允许只读查询（SELECT/SHOW/DESC/EXPLAIN）。\n【助手】\n',
              '[Error] Only read-only queries (SELECT/SHOW/DESC/EXPLAIN) allowed.\n[Assistant]\n');
          continue;
        }
        var qresult;
        try {
          qresult = query(sql);
        } catch(e) {
          qresult = [{ error: String(e) }];
        }
        var result_text = compress(rows_to_text(qresult), 3000);

        log_sql_trace(conversation_id, rule_turn, rule_turn + 1, 'rule_planner', 'query_db',
                      sql, rule_tool.thought || '', result_text);

        var is_last = (rule_turn >= MAX_RULE_TURNS - 1);
        rule_summary_prompt += '\n' + agent_response.trim() + '\n' +
          t('工具执行结果（SQL查询）：\n', 'Tool result (SQL query):\n') +
          result_text + '\n';

        if (is_last) {
          rule_summary_prompt +=
            t('现在请基于以上所有信息直接回答用户问题，不要再输出 JSON：\n',
              'Now please answer the user question directly based on all the above info, no more JSON:\n');
        } else {
          rule_summary_prompt +=
            t('请基于以上信息继续。如果可以直接回答，请给出最终答案。\n' +
              '如果还需要更多查询，请继续输出 JSON 工具调用。\n【助手】\n',
              'Continue based on the above info. If you can answer directly, give the final answer.\n' +
              'If you need more queries, continue with JSON tool calls.\n[Assistant]\n');
        }
        continue;
      }

      // Unrecognized tool — treat as final answer
      break;
    }

    /* The loop can exit with agent_response still holding a tool-call JSON:
     * on the last allowed turn the branches above append the "now answer
     * directly, no more JSON" instruction and `continue`, but the loop
     * condition then ends the loop before that instruction is ever sent.
     * The describe_table branch can also break out on malformed args while
     * agent_response is the JSON.  Either way the raw JSON would be handed
     * straight to the user, so spend the already-prepared prompt on one
     * final generation instead. */
    if (parse_tool_call(agent_response)) {
      var rule_final = ml_generate(rule_summary_prompt,
                                   { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) });
      agent_response = (rule_final && rule_final.trim().length > 0 && !parse_tool_call(rule_final))
        ? rule_final
        : compress(plan_log, cfg('plan_log_max_tokens', 4000));
    }

    if (!agent_response || agent_response.trim().length < 5)
      agent_response = compress(plan_log, cfg('plan_log_max_tokens', 4000));

    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, plan_log + rule_tool_log + think_suffix());
    return agent_response;
  }

  /* ROUTE C: RAG / knowledge base.
   *
   * Only claim the turn when a vector store actually exists.  Previously any
   * message is_knowledge_query() liked was answered with heatwave_dispatch's
   * "未找到向量知识库。" dead end whenever no vector store was configured —
   * which is the default state of a fresh instance, so the whole route was a
   * dead end out of the box.  is_knowledge_query() is also lexical and
   * imprecise (its '有哪些' alternative matches "orders 表有哪些列？"), so a
   * plain schema question could be swallowed here and never reach the agent
   * loop that could actually answer it.  Falling through costs one
   * information_schema lookup and lets Route D handle it instead. */
  if (is_knowledge_query(user_message)) {
    var vector_tables = discover_vector_tables(chat_opt);
    if (vector_tables && vector_tables.length > 0) {
      var hw_result  = heatwave_dispatch(user_message, chat_opt, vector_tables);
      agent_response = hw_result.response || '';
      if (hw_result.tables && hw_result.tables.length > 0)
        chat_opt.tables = hw_result.tables;
      chat_opt = update_chat_history(chat_opt, user_message, agent_response);
      chat_opt.response = agent_response; chat_opt.request_completed = true;
      save_chat_options(chat_opt);
      persist_turn(conversation_id, user_message, agent_response, 'hw_mode:' + hw_result.mode + think_suffix());
      return agent_response;
    }
  }

  /* ROUTE D: LLM Agent Loop */
  var history      = get_history(conversation_id, 8);
  var few_shot     = retrieve_few_shot(user_message, 3);
  var hw_hist_text = chat_history_to_text(chat_opt);

  var schema_embeddings_ready = check_schema_embeddings_ready(current_db);
  var logical_tasks           = decompose_query(user_message);

  var fixed_cost       = est_tok(hw_hist_text || history) + est_tok(few_shot) + 1000;
  var available_tokens = Math.max(400, PROMPT_TOK_LIMIT - fixed_cost);

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
    current_db, schema_ctx, join_hint, plan_hint, few_shot, history, hw_hist_text
  );
  var full_prompt   = build_task_header(request_intent) + '\n\n' + system_prompt_base;
  var prompt_tokens = est_tok(full_prompt);

  var tool_log = '', last_result = '', need_summary = false, error_count = 0;
  var seen_tool_sigs = {};
  /* Signatures whose execution failed.  A later successful step may have
   * removed whatever made them fail, so they stop counting as repeats. */
  var failed_tool_sigs = {};
  var tx_turns = 0;

  for (var turn = 0; turn < MAX_TURNS; turn++) {
    var llm_out = ml_generate(full_prompt, {});
    var tool_obj = parse_tool_call(llm_out);

    if (!tool_obj) {
      var llm_text = llm_out ? llm_out.trim() : '';
      if (llm_text.length > 0) {
        agent_response = llm_text;
        need_summary   = false;
      } else {
        /* LLM returned empty/whitespace — force final_summary instead of
           leaking a raw SQL result as the user-facing response */
        need_summary = true;
      }
      break;
    }

    var cur_sig = tool_obj.tool + '|' + JSON.stringify(tool_obj.args || {});
    if (seen_tool_sigs[cur_sig]) {
      tool_log += '\n' + t('[重复检测] 模型输出与之前某轮重复，终止循环: ',
                          '[Loop detected] Same tool call as a previous turn, breaking: ') +
                  cur_sig.substring(0, 80);
      /* Always force final_summary — last_result may be raw SQL output
         that is not suitable as a user-facing response */
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
      /* Never keep an AGENT-owned transaction open while waiting for a human
       * approval response.  A CALLER-owned transaction is outside the
       * agent's ownership and must be left untouched. */
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
      save_review_plan(review_state);
      var current_step = get_pending_review_step(review_state);
      agent_response = render_review_prompt(review_state, current_step, review_policy);
      need_summary = false;
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
                  tool_obj.thought, result_text);

    /* Single consecutive-error counter for the whole turn.  A step counts as
     * failed either because execute_review_step reported ok=false, or because
     * the result text carries a SQL error that came back as an ordinary
     * string.  These used to be two separate checks further apart, and the
     * second one's `else { error_count = 0; }` reset the counter right after
     * the first had incremented it — so a tool that kept failing never
     * reached MAX_ERRORS and the loop ran all MAX_TURNS rounds. */
    var step_failed = !result_obj.ok ||
        result_text.indexOf('执行出错') !== -1 ||
        result_text.indexOf('Error: ') !== -1  ||
        result_text.indexOf('Unknown table') !== -1;

    if (step_failed) {
      failed_tool_sigs[cur_sig] = true;
      if (!result_obj.ok)
        tool_log += '\n[执行失败] ' + (result_obj.error || 'unknown_error') + '\n';
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 连续错误，强制摘要。', '[Aborted] Consecutive errors, forcing summary.');
        last_result  = result_text;
        need_summary = true;
        break;
      }
    } else {
      error_count = 0;
      /* This step changed the database, so a call that failed earlier may now
       * succeed -- e.g. ALTER TABLE ... SECONDARY_LOAD after the preceding
       * turn set SECONDARY_ENGINE = RAPID.  Retrying it is progress, not a
       * loop, so drop the failed signatures.  Signatures of steps that
       * SUCCEEDED stay recorded, so a model that keeps re-issuing the same
       * working call is still cut off. */
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
      break;
    }

    tool_log += '\n[Step ' + (turn+1) + '] tool=' + tool_obj.tool +
                ' thought=' + (tool_obj.thought || '') +
                '\nresult=' + compress(result_text, 1200) + '\n';

    if (tool_obj.tool === 'commit_tx' || tool_obj.tool === 'rollback_tx') {
      tx_turns = 0;
      need_summary = true; break;
    }
    if (tool_obj.tool === 'generate_text') {
      if (result_text && result_text.length > 0) {
        agent_response = result_text;
        need_summary = false;
      } else {
        /* generate_text returned empty — force final_summary */
        need_summary = true;
      }
      break;
    }

    /* Terminal ML tools: the tool result IS the answer, so stop the loop.
     * ml_model_load / ml_model_unload are deliberately NOT here — loading a
     * model is a prerequisite step the agent should follow with the
     * prediction the user actually asked for, and ml_model_active is a
     * lookup the model should summarize rather than dump verbatim. */
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
      break;
    }

    var append =
      '\n' + t('工具结果：', 'Tool result: ') +
      compress(result_text, 1200) + '\n' +
      t('【助手】\n', '[Assistant]\n');
    if (prompt_tokens + est_tok(llm_out) + est_tok(append) > PROMPT_TOK_LIMIT) {
      need_summary = true;
      if (request_intent && request_intent.kind === 'write' && !A.write_tool_seen) {
        agent_response = t(
          '由于本次涉及的表结构较多，上下文预算已用尽，写操作尚未提交确认，暂无法继续执行。请缩小范围后重试（例如指定更具体的部门或日期区间）。',
          'The schema context for this request was too large to fit the remaining budget, so the write operation was never proposed for approval. Please retry with a narrower scope.'
        );
        need_summary = false;
        break;
      }
    }
    full_prompt   += llm_out.trim() + append;
    prompt_tokens += est_tok(llm_out) + est_tok(append);
  }

  /* Safety net: force-rollback any uncommitted transaction */
  var safety_net_msg = finalize_tx_safety_net();
  if (safety_net_msg) {
    tool_log += safety_net_msg;
    if (need_summary)
      last_result += t('\n（事务已被强制回滚）', '\n(Transaction force-rolled back)');
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

  /* Safety net: catch raw SQL table-format output that leaked through,
     as well as stray JSON tool calls */
  /* Anchor on the exact header rows_to_text()/rows_to_table() emit ("共 N
   * 条：" / "Total N rows:") rather than on '---' or ' | ' anywhere in the
   * text.  final_summary() explicitly instructs the model to answer with a
   * Markdown table first, and every Markdown table contains both of those
   * substrings — so the loose test fired on essentially every well-formed
   * tool-using answer, throwing away a good response and paying for a
   * redundant final_summary() round trip. */
  var looks_like_raw_sql = !!agent_response &&
    /^(?:共\s*\d+\s*条：|Total\s+\d+\s+rows:)/.test(agent_response);
  /* Reuse parse_tool_call() instead of a weak charAt(0)==='{' check —
   * the latter misses JSON wrapped in ```json fences, which is the most
   * common leakage pattern from chat models that have been exposed to
   * many Few-Shot tool-call examples in the prompt. */
  var leaked_tool_call = !!parse_tool_call(agent_response);
  if (!agent_response || agent_response.length === 0 ||
      leaked_tool_call || looks_like_raw_sql) {
    /* Retry final_summary up to 2 times, re-checking for leaked
     * tool calls on each attempt.  The model may repeat the same
     * leakage pattern on retry. */
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

  chat_opt = update_chat_history(chat_opt, user_message, agent_response);
  chat_opt.response = agent_response; chat_opt.request_completed = true;
  save_chat_options(chat_opt);

  persist_turn(conversation_id, user_message, agent_response, tool_log + think_suffix());
  return agent_response;

  } finally {
    /* The finally block guarantees that no uncommitted transaction outlives
     * this invocation, regardless of which exit path was taken.  Route D
     * already calls finalize_tx_safety_net() above for message-appending
     * purposes, so this will be a no-op for that path — but it catches
     * every other return, including future routes and unhandled exceptions
     * thrown anywhere in the try block. */
    finalize_tx_safety_net();
  }
}

