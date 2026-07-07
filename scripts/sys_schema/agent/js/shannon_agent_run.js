//@include lib_tools.js

function shannon_agent_run(user_message, conversation_id) {

 /* Initialize the module-level shared context. This step must be performed
  * before any t() invocation — it aligns exactly with the original code's
  * requirement that "var lang = detect_lang(user_message) must come first",
  * with the only difference being that the assignment target has shifted from
  * a local variable to A.* properties. */

  A.user_message    = user_message;
  A.conversation_id = conversation_id;
  A.lang            = detect_lang(user_message);
  A.last_think      = '';
  if (typeof A.conversation_id !== 'undefined' && tx_active_for(A.conversation_id)) {
    try { sys.exec_sql('ROLLBACK'); } catch (e) {}
    set_tx_active_for(A.conversation_id, false);
  }

  var MAX_TURNS        = 10;
  var PROMPT_TOK_LIMIT = 2800;
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
  var agent_response = '';

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
      t('请用清晰的中文直接回答用户问题，不要输出 JSON，不要逐行复述原始数据。' +
        '⛔ 严格要求：表名、列名、数据类型必须与上方查询结果中出现的原始文字完全一致，' +
        '禁止凭经验编造或"补全"任何未出现在查询结果里的列名/字段；如果查询结果信息不足以' +
        '回答某个细节，直接说明信息不足，不要猜测填补：\n',
        'Answer the user\'s question clearly and directly. Do not output JSON; do not repeat raw data. ' +
        '⛔ Strict requirement: table/column names and data types must exactly match what appears in the ' +
        'query results above — never fabricate or "fill in" column names; if the results are insufficient ' +
        'to answer some detail, say so explicitly rather than guessing:\n');

    agent_response = ml_generate(rule_summary_prompt, { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) });
    if (!agent_response || agent_response.trim().length < 5)
      agent_response = compress(plan_log, cfg('plan_log_max_tokens', 4000));

    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, plan_log + think_suffix());
    return agent_response;
  }

  /* ROUTE C: RAG / knowledge base */
  if (is_knowledge_query(user_message)) {
    var vector_tables = discover_vector_tables(chat_opt);
    var hw_result     = heatwave_dispatch(user_message, chat_opt, vector_tables);
    agent_response    = hw_result.response || '';
    if (hw_result.tables && hw_result.tables.length > 0)
      chat_opt.tables = hw_result.tables;
    chat_opt = update_chat_history(chat_opt, user_message, agent_response);
    chat_opt.response = agent_response; chat_opt.request_completed = true;
    save_chat_options(chat_opt);
    persist_turn(conversation_id, user_message, agent_response, 'hw_mode:' + hw_result.mode + think_suffix());
    return agent_response;
  }

  /* ROUTE D: LLM Agent Loop */
  var history      = get_history(conversation_id, 8);
  var few_shot     = retrieve_few_shot(user_message, 3);
  var hw_hist_text = chat_history_to_text(chat_opt);

  var schema_embeddings_ready = check_schema_embeddings_ready(current_db);
  var logical_tasks           = decompose_query(user_message);

  var fixed_cost       = est_tok(hw_hist_text || history) + est_tok(few_shot) + 1000;
  var available_tokens = Math.max(400, PROMPT_TOK_LIMIT - fixed_cost);

  var schema_ctx = build_schema_context(current_db, available_tokens, chat_opt, request_intent, user_message);

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
  var tx_turns = 0;

  for (var turn = 0; turn < MAX_TURNS; turn++) {
    var llm_out = ml_generate(full_prompt, {});
    var tool_obj = parse_tool_call(llm_out);

    if (!tool_obj) {
      agent_response = llm_out.trim() || last_result;
      need_summary   = false;
      break;
    }

    var cur_sig = tool_obj.tool + '|' + JSON.stringify(tool_obj.args || {});
    if (seen_tool_sigs[cur_sig]) {
      if (last_result && last_result.length > 0) {
        agent_response = last_result;
        need_summary = false;
      } else {
        tool_log += '\n' + t('[重复检测] 模型输出与之前某轮重复，终止循环: ',
                            '[Loop detected] Same tool call as a previous turn, breaking: ') +
                    cur_sig.substring(0, 80);
        need_summary = true;
      }
      break;
    }
    seen_tool_sigs[cur_sig] = true;

    var validation_error = validate_tool_call(tool_obj);
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

    var result_obj;
    try {
      result_obj = execute_tool(tool_obj.tool, tool_obj.args || {}, current_db);
    } catch (e) {
      result_obj = { ok: false,
                     response: t('工具执行异常：', 'Tool execution exception: ') + String(e),
                     error: 'exception' };
    }

    var result_text = result_obj && result_obj.response ? String(result_obj.response) : '';
    log_sql_trace(conversation_id, turn, turn + 1, 'agent_loop', tool_obj.tool,
                  (tool_obj.args && tool_obj.args.sql) ? tool_obj.args.sql : '',
                  tool_obj.thought, result_text);

    if (!result_obj.ok) {
      tool_log += '\n[执行失败] ' + (result_obj.error || 'unknown_error') + '\n';
      if (++error_count >= MAX_ERRORS) {
        tool_log += '\n[终止] 结果校验失败次数过多。';
        need_summary = true;
        break;
      }
    } else {
      error_count = 0;
    }

    if (tx_active_for(A.conversation_id) && tool_obj.tool !== 'begin_tx') {
      tx_turns += 1;
      if (tx_turns >= 3) {
        tool_log += '\n[安全网] 事务保持超过安全轮次，建议尽快提交或回滚。';
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

    if (result_text.indexOf('执行出错') !== -1 ||
        result_text.indexOf('Error: ') !== -1  ||
        result_text.indexOf('Unknown table') !== -1) {
      if (++error_count >= MAX_ERRORS) {
        tool_log +=
          '\n' + t('[终止] 连续错误，强制摘要。', '[Aborted] Consecutive errors, forcing summary.');
        break;
      }
    } else { error_count = 0; }

    tool_log += '\n[Step ' + (turn+1) + '] tool=' + tool_obj.tool +
                ' thought=' + (tool_obj.thought || '') +
                '\nresult=' + compress(result_text, 600) + '\n';

    if (tool_obj.tool === 'commit_tx' || tool_obj.tool === 'rollback_tx') {
      tx_turns = 0;
      need_summary = true; break;
    }
    if (tool_obj.tool === 'generate_text') {
      agent_response = result_text; need_summary = false; break;
    }

    var append =
      '\n' + t('工具结果：', 'Tool result: ') +
      compress(result_text, 600) + '\n' +
      t('【助手】\n', '[Assistant]\n');
    if (prompt_tokens + est_tok(llm_out) + est_tok(append) > PROMPT_TOK_LIMIT) { need_summary = true; break; }
    full_prompt   += llm_out.trim() + append;
    prompt_tokens += est_tok(llm_out) + est_tok(append);
  }

  /* Safety net: force-rollback any uncommitted transaction */
  if (tx_active_for(A.conversation_id)) {
    try { sys.exec_sql('ROLLBACK'); } catch(e) {}
    set_tx_active_for(A.conversation_id, false);
    tool_log +=
      '\n' + t('[安全网] 未提交事务已强制回滚。',
               '[Safety net] Uncommitted transaction force-rolled back.');
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

  if (!agent_response || agent_response.length === 0 ||
      (agent_response.trim().charAt(0) === '{' && agent_response.indexOf('"tool"') !== -1)) {
    if (tool_log.length > 0) {
      agent_response = final_summary(system_prompt_base, tool_log);
    } else {
      agent_response = t('抱歉，未能生成有效回答，请重试。',
                        'Sorry, unable to generate a valid response. Please try again.');
    }
  }

  chat_opt = update_chat_history(chat_opt, user_message, agent_response);
  chat_opt.response = agent_response; chat_opt.request_completed = true;
  save_chat_options(chat_opt);

  persist_turn(conversation_id, user_message, agent_response, tool_log + think_suffix());
  return agent_response;
}

return shannon_agent_run(user_message, conversation_id);