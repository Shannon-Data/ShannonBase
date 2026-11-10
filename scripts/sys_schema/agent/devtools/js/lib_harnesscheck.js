//@include lib_runtimecheck.js
function shannon_harness_selfcheck(op, label) {
  if (!selfcheck_writes_allowed()) return [['error', 'harness checks require write opt-in']];
  var out = [];
  function row(k, v) { out.push([String(k), String(v)]); }
  var db = String(query_checked('SELECT DATABASE() AS db')[0].db || 'test');
  if (op === 'approval') {
    A.current_db = db;
    A.conversation_id = principal_scope_conversation_id('harness_' + label);
    var id = 'harness_' + String(label).replace(/[^A-Za-z0-9_]/g, '_');
    var step = build_review_step({ tool: 'update_data' },
      { sql: 'UPDATE ' + db + '.harness_probe SET v=2 WHERE id=1' }, db, false);
    var plan = { plan_id: id, conversation_id: A.conversation_id, steps: [step], status: 'awaiting_approval' };
    try {
      row('saved', save_review_plan(plan));
      var prompt = render_review_prompt(plan, step, get_review_policy({}));
      var digest = step.approval_digest;
      row('digest_shown', !!digest && prompt.indexOf(digest) >= 0);
      var token = parse_review_approval('Approve ' + id + ' 1 ' + digest);
      row('token_bound', token && token.plan_id === id && token.step_id === 1 && token.digest === digest);
      query_checked("UPDATE mysql.agent_review_plan_step SET sql_text=CONCAT(sql_text,' ') WHERE plan_id='" + esc(id) + "'");
      row('tamper_rejected', !claim_review_step(id, 1, 'pending', 'executing', digest));
      row('bare_approval_rejected', parse_review_approval('Approve') === null);
      row('wrong_database_rejected', execute_review_step(step, db + '_changed').error === 'approval_content_changed');
    } finally {
      query_checked("DELETE FROM mysql.agent_review_plan_step WHERE plan_id='" + esc(id) + "'");
      query_checked("DELETE FROM mysql.agent_review_plan WHERE plan_id='" + esc(id) + "'");
    }
  } else if (op === 'shared_execution') {
    var saved_query=query_checked, calls=[], saved_lang=A.lang;
    var score_args={table_name:'test.harness_orders',target_column:'amount',model_handle:'fixture'};
    try {
      A.lang='en';
      query_checked=function(sql){calls.push(sql);return [{score:0.75}];};
      var score=impl_ml_score(score_args,{});
      row('ml_sequence_preserved',score.ok && calls.length===3 && calls[0]==='SET @_ml_score_val = 0' &&
        calls[1]==="CALL sys.ML_SCORE('test.harness_orders', 'amount', 'fixture', 'balanced_accuracy', @_ml_score_val, NULL)" &&
        calls[2]==='SELECT @_ml_score_val AS score' && score.sql===calls.join(';\n'));
      calls=[];
      query_checked=function(sql){calls.push(sql);if(calls.length===2)throw new Error('fixture_failure');return [];};
      score=impl_ml_score(score_args,{});
      row('ml_failure_stops_sequence',!score.ok && score.error==='ml_score_failed' && calls.length===2 && score.response.indexOf('ML_SCORE failed:')===0);
    } finally {query_checked=saved_query;A.lang=saved_lang;}
    var saved_options=A._mem_opt;
    A.conversation_id=principal_scope_conversation_id('harness_shared_'+label);
    var prefix=mem_principal_prefix(), scope='shared_filter_'+label;
    try {
      A._mem_opt=mem_merge_defaults(MEM_DEFAULTS,{long_term:{semantic_enabled:false,default_ttl_days:0},graph:{enabled:false}});
      var suffixes=['%','_','ordinary'];
      for(var i=0;i<suffixes.length;i++)
        mem_long_write_fact('literal wildcard '+suffixes[i]+' '+label,{scope:scope},A._mem_opt);
      var removed=impl_forget_memory({scope:scope,contains:'%'},{});
      row('forget_literal_percent',removed.ok && Number(saved_query("SELECT COUNT(*) AS n FROM mysql.agent_semantic_fact WHERE principal_prefix='"+esc(prefix)+"' AND scope='"+esc(scope)+"'")[0].n)===2);
      removed=impl_forget_memory({scope:scope,contains:'_'},{});
      row('forget_literal_underscore',removed.ok && Number(saved_query("SELECT COUNT(*) AS n FROM mysql.agent_semantic_fact WHERE principal_prefix='"+esc(prefix)+"' AND scope='"+esc(scope)+"'")[0].n)===1);
      row('forget_preview_escaped',display_forget_memory({scope:"owner's",contains:'%'}).indexOf("scope='owner''s'")>=0);
    } finally {
      saved_query("DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='"+esc(prefix)+"' AND scope='"+esc(scope)+"'");
      A._mem_opt=saved_options;
    }
  } else if (op === 'trace_enqueue') {
    var saved_mo = A._mem_opt;
    A.conversation_id = principal_scope_conversation_id('harness_trace_' + String(label));
    var prefix = mem_principal_prefix();
    function tasks() {
      return Number(query_checked("SELECT COUNT(*) AS n FROM mysql.agent_derive_queue q" +
        " JOIN mysql.agent_sql_trace t ON q.kind='embed_trace' AND q.target_id=t.id" +
        " WHERE t.conversation_id='" + esc(A.conversation_id) + "'")[0].n);
    }
    try {
      A._mem_opt = mem_merge_defaults(MEM_DEFAULTS, { derive: { enabled: false },
        long_term: { episodic_enabled: false, semantic_enabled: false } });
      log_sql_trace(A.conversation_id,1,1,'agent_loop','query_db','SELECT 1','eligible','1',true);
      row('derive_disabled_no_enqueue', tasks() === 0);
      log_sql_trace(A.conversation_id,1,2,'agent_loop','query_db','SELECT missing','failed',
        selfcheck_pad('x',1300) + 'Error: missing column',false);
      A._mem_opt.derive.enabled = true;
      mem_derive_enqueue_missing(A._mem_opt);
      row('sweep_only_successful', tasks() === 1);
      log_sql_trace(A.conversation_id,1,3,'agent_loop','query_db','SELECT 2','direct','2',true);
      row('writer_enqueues_exact_row', tasks() === 2 && Number(query_checked(
        "SELECT COUNT(*) AS n FROM mysql.agent_derive_queue q JOIN mysql.agent_sql_trace t" +
        " ON q.kind='embed_trace' AND q.target_id=t.id WHERE t.conversation_id='" +
        esc(A.conversation_id) + "' AND t.step_no=3")[0].n) === 1);
      // Leave a real, unqueued trace as LAST_INSERT_ID(). If the failed-INSERT
      // guard regresses, the next call would incorrectly enqueue this row.
      A._mem_opt.derive.enabled = false;
      log_sql_trace(A.conversation_id,1,4,'agent_loop','query_db','SELECT 4','unqueued victim','4',true);
      A._mem_opt.derive.enabled = true;
      log_sql_trace(A.conversation_id,'NaN',5,'agent_loop','query_db','SELECT 3','rejected insert','3',true);
      row('failed_insert_no_enqueue', tasks() === 2);
      var orphan = query_checked("SELECT id FROM mysql.agent_sql_trace WHERE conversation_id='" +
        esc(A.conversation_id) + "' AND step_no=3")[0].id;
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE id=" + Number(orphan));
      // The other task is deliberately ineligible: drain must finish both
      // without requiring the embedding model or using up retries.
      query_checked("UPDATE mysql.agent_sql_trace SET embed_eligible=0 WHERE conversation_id='" + esc(A.conversation_id) + "'");
      mem_derive_drain(A._mem_opt);
      row('orphan_and_ineligible_done', Number(query_checked("SELECT COUNT(*) AS n FROM mysql.agent_derive_queue" +
        " WHERE kind='embed_trace' AND target_id=" + Number(orphan) + " AND state='done'")[0].n) === 1);
    } finally {
      query_checked("DELETE q FROM mysql.agent_derive_queue q LEFT JOIN mysql.agent_sql_trace t" +
        " ON q.target_id=t.id WHERE q.kind='embed_trace' AND (t.conversation_id='" +
        esc(A.conversation_id) + "' OR q.target_id=" + Number(orphan || 0) + ")");
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" + esc(A.conversation_id) + "'");
      A._mem_opt = saved_mo;
    }
  } else if (op === 'trace_vector') {
    var saved_mo = A._mem_opt;
    A.conversation_id = principal_scope_conversation_id('harness_vector_' + String(label));
    try {
      A._mem_opt = mem_merge_defaults(MEM_DEFAULTS, {derive:{drain_batch:200},
        long_term:{episodic_enabled:false,semantic_enabled:false}});
      log_sql_trace(A.conversation_id,1,1,'agent_loop','query_db','SELECT 1','trace vector fixture','1',true);
      mem_derive_drain(A._mem_opt);
      var derived = query_checked("SELECT t.embedding IS NOT NULL AS embedded,q.state FROM mysql.agent_sql_trace t" +
        " JOIN mysql.agent_derive_queue q ON q.kind='embed_trace' AND q.target_id=t.id" +
        " WHERE t.conversation_id='" + esc(A.conversation_id) + "'");
      row('trace_vector_derived', derived.length===1 && Number(derived[0].embedded)===1 && derived[0].state==='done');
    } finally {
      query_checked("DELETE q FROM mysql.agent_derive_queue q JOIN mysql.agent_sql_trace t ON q.target_id=t.id" +
        " WHERE q.kind='embed_trace' AND t.conversation_id='" + esc(A.conversation_id) + "'");
      query_checked("DELETE FROM mysql.agent_sql_trace WHERE conversation_id='" + esc(A.conversation_id) + "'");
      A._mem_opt = saved_mo;
    }
  } else if (op === 'result_contract') {
    var sql = 'SELECT COUNT(*) AS n,COUNT(amount) AS c,SUM(amount) AS s,AVG(amount) AS a FROM test.harness_orders';
    var good = execute_tool('query_db', { sql: sql }, db);
    row('structured_content', good.ok && good.content[0].type === 'text' && good.content[1].type === 'json');
    row('inline_citation', good.citations.length === 1 && !!good.citations[0].source_sql);
    row('aggregate_identities_checked', good.result.consistency.ok && good.result.consistency.checked >= 3);
    row('bad_aggregate_refused', !aggregate_result_check(sql,[{n:3,c:4,s:10,a:20}]).ok);
    row('empty_aggregate_checked', aggregate_result_check(sql,[{n:0,c:0,s:null,a:null}]).ok);
    var page = execute_tool('query_db', { sql: 'SELECT id FROM test.harness_orders ORDER BY id LIMIT 1' }, db);
    row('limit_page_marked', page.ok && page.result.limit_reached && A.truncated_evidence);
    var saved_hooks = HOOKS.post_tool_use.slice();
    try {
      register_hook('post_tool_use', function(tool,args,ctx,result) {
        return { ok: true, response: 'replacement', result: { preview_only: true, artifact_id: 'art_fixture' } };
      });
      var replaced = execute_tool('query_db', {sql:'SELECT 1'}, db);
      row('hook_result_normalized', replaced.artifact_id === 'art_fixture' && replaced.content[1].data.preview_only);
    } finally { HOOKS.post_tool_use = saved_hooks; }
    var schema = get_tool_spec('ml_train').args;
    row('ensemble_score_string', !ts_validate(schema,{table_name:'test.t',target_column:'label',ensemble_score:'f1'}));
    row('ensemble_score_number_rejected', !!ts_validate(schema,{table_name:'test.t',target_column:'label',ensemble_score:1}));
    row('cancel_is_not_retried', classify_llm_error('Query execution was interrupted').kind === 'cancelled' &&
        !classify_llm_error('cancelled by KILL QUERY').retryable);
    var saved_query = query, saved_script = A.eval_script;
    var calls = 0;
    try {
      A.eval_script = null;
      query = function() { calls++; return [{result:JSON.stringify({text:'partial tool JSON',finish_reason:'cancelled'})}]; };
      row('cancelled_envelope_discarded', ml_generate_call('fixture','cancel fixture',null) === '' &&
        A.last_llm_status.kind === 'cancelled' && calls === 1);
    } finally { query = saved_query; A.eval_script = saved_script; }

  } else if (op === 'guards_and_budgets') {
    var saved_policy = A.operator_policy, saved_runtime = A.runtime_active;
    var saved_deadline = A.turn_deadline_at, saved_tool = A.tool_deadline_at;
    var saved_handler = get_tool_spec('generate_text').handler;
    var saved_cost = A.cost, saved_ratio = A.token_ratio, saved_opt = A.cached_chat_opt;
    try {
      A.operator_policy = Object.assign({}, get_operator_policy(), {read_plan_rows_max:'1',read_plan_cost_max:'1',
        ddl_table_bytes_max:'1',ddl_table_rows_max:'1'});
      var read = read_plan_guard('SELECT id FROM test.harness_orders',classify_statement('SELECT id FROM test.harness_orders'));
      row('expensive_read_blocked', read && read.error === 'read_plan_requires_rewrite' && !!read.plan);
      var ddl = ddl_plan_guard('ALTER TABLE test.harness_orders ADD COLUMN probe INT', db);
      row('large_blocking_ddl_blocked', ddl && ddl.error === 'ddl_online_clauses_required');
      row('online_ddl_allowed', ddl_plan_guard('ALTER TABLE test.harness_orders ADD COLUMN probe INT, ALGORITHM=INPLACE, LOCK=NONE',db) === null);
      A.cached_chat_opt = {tool_timeout_ms:500,ml_tool_timeout_ms:100,max_turns:40};
      A.operator_policy.max_turns='20';
      row('step_budget_operator_ceiling', max_turns()===20);
      row('ml_session_budget', tool_time_budget(get_tool_spec('ml_train'))===100);
      A.operator_policy.ml_tool_timeout_ms='50';
      row('ml_operator_ceiling', tool_time_budget(get_tool_spec('ml_train'))===50);
      A.cached_chat_opt = saved_opt;
      A.operator_policy.tool_timeout_ms = '20';
      A.runtime_active = true; A.turn_deadline_at = Date.now()+30000;
      get_tool_spec('generate_text').handler = function() {
        query_checked('SELECT SLEEP(2)'); return {ok:true,response:'done'};
      };
      var start = Date.now();
      var timed = execute_tool('generate_text',{prompt:'timeout fixture'},db);
      row('per_tool_timeout_bounded', Date.now()-start < 1500);
      row('tool_deadline_restored', A.tool_deadline_at === saved_tool);
      row('timeout_result_structured', !!timed.content && !!timed.span_id);
      A.runtime_active = false;
      A.token_ratio = 1; A.cost = null;
      var raw = est_tok_raw('token calibration fixture');
      llm_note_call('token calibration fixture','ok',10,{prompt_tokens:raw*2,completion_tokens:1});
      row('provider_tokens_calibrate', est_tok('token calibration fixture') >= raw*2 && A.cost.prompt_tokens === raw*2);
    } finally {
      A.operator_policy = saved_policy; A.runtime_active = saved_runtime;
      A.turn_deadline_at = saved_deadline; A.tool_deadline_at = saved_tool;
      get_tool_spec('generate_text').handler = saved_handler;
      A.cost = saved_cost; A.token_ratio = saved_ratio; A.cached_chat_opt = saved_opt; A.cancelled = false;
    }
  } else if (op === 'metrics') {
    A.conversation_id = principal_scope_conversation_id('harness_metrics_' + String(label));
    var prefix = mem_principal_prefix();
    try {
      var samples = [10,20,30,40,1000];
      for (var i=0;i<samples.length;i++) query_checked(
        "INSERT INTO mysql.agent_memory_audit(principal_prefix,conversation_id,tier,op,store,detail,elapsed_ms,created_at)" +
        " VALUES ('" + esc(prefix) + "','" + esc(A.conversation_id) + "','runtime','turn','agent'," +
        "'{\"prompt_tokens\":10,\"completion_tokens\":2}'," + samples[i] + ",CURRENT_DATE - INTERVAL 60 DAY)");
      var metrics = query_checked(runtime_metrics_sql(90));
      var found = false;
      for (var mi=0;mi<metrics.length;mi++) if (Number(metrics[mi].turns)===5 && Number(metrics[mi].p95_ms)===1000) {
        found = Number(metrics[mi].p50_ms)===30 && Number(metrics[mi].prompt_tokens)===50 && Number(metrics[mi].completion_tokens)===10;
      }
      row('percentiles_and_tokens', found);
    } finally {
      query_checked("DELETE FROM mysql.agent_memory_audit WHERE conversation_id='" + esc(A.conversation_id) + "'");
    }
  } else if (op === 'execution_match' || op === 'text2sql') {
    var cases = [
      { sql: 'SELECT c.id,COALESCE(SUM(o.amount),0) AS total FROM test.harness_customers c LEFT JOIN test.harness_orders o ON c.id=o.customer_id GROUP BY c.id ORDER BY c.id',
        oracle: 'SELECT 1 AS id,CAST(15.50 AS DECIMAL(32,2)) AS total UNION ALL SELECT 2,CAST(0 AS DECIMAL(32,2)) UNION ALL SELECT 3,CAST(0 AS DECIMAL(32,2))' },
      { sql: "SELECT id FROM test.harness_orders WHERE note NOT LIKE '%special%requests%' ORDER BY id",
        oracle: 'SELECT 1 AS id' },
      { sql: 'SELECT c.id FROM test.harness_customers c WHERE NOT EXISTS (SELECT 1 FROM test.harness_orders o WHERE o.customer_id=c.id) ORDER BY c.id',
        oracle: 'SELECT 3 AS id' },
      { sql: 'SELECT COUNT(*) AS n,COUNT(amount) AS c,SUM(amount) AS s,AVG(amount) AS a FROM test.harness_orders',
        oracle: 'SELECT 3 AS n,2 AS c,CAST(15.50 AS DECIMAL(32,2)) AS s,CAST(7.750000 AS DECIMAL(16,6)) AS a' },
      { sql: 'SELECT COUNT(*) AS n,SUM(amount) AS s FROM test.harness_orders WHERE id<0',
        oracle: 'SELECT 0 AS n,NULL AS s' },
      { sql: 'SELECT customer_id,COUNT(*) AS n FROM test.harness_orders GROUP BY customer_id HAVING COUNT(*)>1 ORDER BY customer_id',
        oracle: 'SELECT 1 AS customer_id,2 AS n' }
    ];
    var questions = [
      'List every customer id and total order amount, including customers without orders; use zero for missing totals.',
      'List order ids whose note does not match special%requests; exclude NULL notes.',
      'List customer ids without any orders.',
      'Return order row count, non-NULL amount count, sum and average of amount.',
      'For orders with id below zero, return row count and sum of amount.',
      'List customer ids with more than one order and their order counts.'
    ];
    function values(rows) {
      return rows.map(function(r) { return JSON.stringify(Object.keys(r).map(function(k) { return r[k]; })); }).sort();
    }
    for (var i = 0; i < cases.length; i++) {
      var candidate = cases[i].sql;
      if (op === 'text2sql') {
        var generated = ml_generate('Generate exactly one query_db tool JSON for this question. Use only SELECT. ' +
          'Schema: test.harness_customers(id INT PRIMARY KEY); test.harness_orders(id INT PRIMARY KEY,customer_id INT,amount DECIMAL(12,2),note VARCHAR(128)). ' +
          'The result must have columns in the requested order. Question: ' + questions[i], {tools:true});
        var call = (A.last_llm_status && A.last_llm_status.tool_call) || parse_tool_call(generated);
        if (!call || call.tool !== 'query_db' || !call.args || typeof call.args.sql !== 'string') {
          row('execution_match_' + (i+1),false); continue;
        }
        candidate = call.args.sql;
      }
      var result = execute_tool('query_db', { sql: candidate }, db);
      var actual = result.result && result.result.data;
      var expected = query_checked(cases[i].oracle);
      row('execution_match_' + (i + 1), result.ok && Array.isArray(actual) &&
          JSON.stringify(values(actual)) === JSON.stringify(values(expected)));
    }
  }
  return out;
}
