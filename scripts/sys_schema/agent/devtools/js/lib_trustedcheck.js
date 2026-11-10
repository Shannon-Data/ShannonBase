//@include lib_harnesscheck.js
//@include lib_agent_eval.js
// Fixed fixtures only. The production business tool still reads its target and
// allow-list from operator policy; no business tool definitions come from SQL args.
function shannon_trusted_selfcheck(op, label) {
  if (!selfcheck_writes_allowed()) return [['error','trusted checks require write opt-in']];
  var out = [];
  function row(k,v) { out.push([String(k),String(v)]); }
  A.conversation_id = principal_scope_conversation_id('trusted_'+String(label || op));
  A.current_db = 'test';
  var saved_policy = A.operator_policy, saved_chat = A.cached_chat_opt;
  A.operator_policy = Object.assign({},get_operator_policy(),{
    trusted_tool_allowlist:'approve_order',trusted_order_table:'trusted_business.trusted_orders'});
  A.cached_chat_opt = {memory_options:{enabled:false}};
  var spec = get_tool_spec('approve_order'), saved_version = spec.trusted.version;
  var saved_build = spec.trusted.build, saved_pre = HOOKS.pre_tool_use.slice();
  function prepare(id,version,suffix) {
    var step = build_review_step({tool:'approve_order'},{order_id:id,expected_version:version},'test',false);
    var plan = {plan_id:'trusted_'+String(label || op)+'_'+suffix,conversation_id:A.conversation_id,
      status:'awaiting_approval',steps:[step]};
    if (!save_review_plan(plan)) throw new Error('could not save trusted test plan');
    var prompt = render_review_prompt(plan,step,get_review_policy({}));
    if (!step.approval_digest) throw new Error(prompt);
    A.current_plan_id = plan.plan_id;
    return {plan:plan,step:step,prompt:prompt};
  }
  function approve(item) {
    if (!claim_review_step(item.plan.plan_id,1,'pending','executing',item.step.approval_digest))
      return {ok:false,error:'claim_lost'};
    var result = execute_review_step(item.step,'test');
    if (!result.ok) transition_review_step(item.plan.plan_id,1,'failed',result.response,result.error);
    return result;
  }
  function state(id) { return query_checked('SELECT status,version FROM trusted_business.trusted_orders WHERE id='+id)[0]; }
  try {
    if (op==='contracts') {
      var calls=0;
      register_tool({name:'trusted_tx_probe',category:'core',requiresTx:true,transactional:true,
        readOnly:true,args:{type:'object',properties:{}},handler:function(){calls++;return {ok:true,response:'probe'};}});
      row('requires_tx_without_transaction',execute_tool('trusted_tx_probe',{},'test').error==='transaction_required' && calls===0);
      var saved_context=get_tx_context;
      try {
        get_tx_context=function(){return {known:false,active:true,owner:TX_OWNER_UNKNOWN};};
        row('unknown_transaction_denied',execute_tool('trusted_tx_probe',{},'test').error==='transaction_required' && calls===0);
      } finally {get_tx_context=saved_context;}
      var invalid=false;
      try {register_tool({name:'bad_tx_contract',requiresTx:true,transactional:false});} catch(e){invalid=true;}
      row('contradictory_contract_rejected',invalid);
      query_checked('START TRANSACTION');
      var probe=execute_tool('trusted_tx_probe',{},'test');
      row('caller_transaction_accepted',probe.ok && get_tx_context().owner===TX_OWNER_CALLER);
      query_checked('ROLLBACK');
      delete TOOL_REGISTRY.trusted_tx_probe;
      TOOL_ORDER=TOOL_ORDER.filter(function(n){return n!=='trusted_tx_probe';});
      row('extra_sql_argument_rejected',validate_tool_call({tool:'approve_order',args:{order_id:1,expected_version:0,sql:'DELETE FROM orders'}},get_review_policy({}))!==null);
      row('unsafe_version_rejected',validate_tool_call({tool:'approve_order',args:{order_id:1,expected_version:9007199254740992}},get_review_policy({}))!==null);
      var item=prepare(1,0,'version');
      var permissive=get_review_policy({}); permissive.enabled=false;
      row('trusted_approval_mandatory',evaluate_step_policy(item.step,permissive).action==='pause');
      spec.trusted.build=function(args){var changed=saved_build(args);changed.expected.status='other';return changed;};
      row('verification_contract_bound',execute_review_step(item.step,'test').error==='trusted_template_changed');
      spec.trusted.build=saved_build;
      spec.trusted.version='2';
      row('template_version_bound',execute_review_step(item.step,'test').error==='trusted_template_changed');
      row('stale_plan_not_reapproved',render_review_prompt(item.plan,item.step,get_review_policy({})).indexOf('trusted_template_changed')>=0);
      spec.trusted.version=saved_version;
      A.operator_policy.trusted_order_table='trusted_business.trusted_orders_other';
      row('operator_target_bound',execute_review_step(item.step,'test').error==='trusted_template_changed');
      A.operator_policy.trusted_order_table='trusted_business.trusted_orders';
      register_hook('pre_tool_use',function(tool){if(tool==='approve_order')spec.trusted.version='hook_changed';});
      row('hook_contract_change_refused',execute_review_step(item.step,'test').error==='trusted_template_changed' && state(1).status==='pending');
      HOOKS.pre_tool_use=saved_pre.slice();spec.trusted.version=saved_version;
      register_hook('pre_tool_use',function(tool,args){if(tool==='approve_order')return {args:{order_id:2,expected_version:args.expected_version}};});
      row('hook_argument_change_refused',execute_review_step(item.step,'test').error==='approval_arguments_changed' && state(2).status==='pending');
      HOOKS.pre_tool_use=saved_pre.slice();
      A.operator_policy.trusted_tool_allowlist='';
      row('operator_authorization_required',execute_review_step(item.step,'test').error==='trusted_tool_not_allowed');
      A.operator_policy.trusted_tool_allowlist='approve_order';
      A.cached_chat_opt.trusted_tool_allowlist=[];
      row('session_only_tightens',execute_review_step(item.step,'test').error==='trusted_tool_not_allowed');
      delete A.cached_chat_opt.trusted_tool_allowlist;
      A.operator_policy.trusted_writes_only='true';
      row('raw_writes_denied',evaluate_step_policy(build_review_step({tool:'update_data'},
        {sql:"UPDATE trusted_business.trusted_orders SET status='approved' WHERE id=1"},'test',false),get_review_policy({})).reason==='trusted_write_required');
    } else if (op==='loop') {
      A._mem_opt=mem_merge_defaults(MEM_DEFAULTS,{enabled:false,retention:{enabled:false},
        procedural:{few_shot_top_k:0},long_term:{episodic_enabled:false,semantic_enabled:false},
        derive:{enabled:false,drain_batch:0}});
      A.eval_script={turns:[{tool:'approve_order',args:{order_id:8,expected_version:0}}],pos:0,calls:0};
      A.cached_chat_opt.idempotency_key='trusted_nl_prepare_'+String(label);
      var question='Approve order 8 with expected version 0.';
      var response=shannon_agent_run(question,A.conversation_id);
      var pending=load_review_state(A.conversation_id), nl_plan=pending && pending.plan_id;
      row('natural_language_pauses_for_approval',!!pending && response.indexOf('Approve '+nl_plan)>=0 && state(8).status==='pending');
      if (!pending) throw new Error(response);
      A.cached_chat_opt.idempotency_key='trusted_nl_modify_'+String(label);
      shannon_agent_run('Modify:UPDATE trusted_business.trusted_orders SET status=\'approved\' WHERE id=8',A.conversation_id);
      var unchanged=load_review_state(A.conversation_id);
      row('fixed_tool_cannot_become_free_sql',unchanged.steps[0].tool==='approve_order' && unchanged.steps[0].approval_digest===pending.steps[0].approval_digest && state(8).status==='pending');
      var step=pending.steps[0], token='Approve '+nl_plan+' 1 '+step.approval_digest;
      A.cached_chat_opt.idempotency_key='trusted_nl_approve_'+String(label);
      var builds=0, hashes=0, saved_query=query_checked;
      spec.trusted.build=function(args){builds++;return saved_build(args);};
      query_checked=function(sql,options){if(sql.indexOf("SELECT SHA2('")===0)hashes++;return saved_query(sql,options);};
      try {response=shannon_agent_run(token,A.conversation_id);}
      finally {spec.trusted.build=saved_build;query_checked=saved_query;}
      row('approval_contract_built_twice',builds===2 && hashes===2);
      row('approved_short_transaction',state(8).status==='approved' && Number(state(8).version)===1 && !get_tx_context().active);
      var calls=A.eval_script.calls;
      var replay=shannon_agent_run(token,A.conversation_id);
      row('approval_request_replayed',replay===response && A.eval_script.calls===calls && Number(state(8).version)===1);
      row('end_to_end_audit',Number(query_checked("SELECT COUNT(*) AS n FROM mysql.agent_memory_audit WHERE tier='trusted'"+
        " AND conversation_id='"+esc(A.conversation_id)+"' AND JSON_UNQUOTE(JSON_EXTRACT(detail,'$.plan_id'))='"+esc(nl_plan)+"'")[0].n)>0);
      query_checked("DELETE FROM mysql.agent_review_plan_step WHERE plan_id='"+esc(nl_plan)+"'");
      query_checked("DELETE FROM mysql.agent_review_plan WHERE plan_id='"+esc(nl_plan)+"'");
      query_checked("DELETE FROM mysql.agent_execution WHERE conversation_id='"+esc(A.conversation_id)+"'");
      A.eval_script=null;
    } else if (op==='flow') {
      var item=prepare(1,0,'success');
      row('approval_shows_fixed_template',item.prompt.indexOf('trusted:approve_order:')>=0 && item.prompt.indexOf('version=0')>=0);
      var builds=0, hashes=0, saved_query=query_checked;
      spec.trusted.build=function(args){builds++;return saved_build(args);};
      query_checked=function(sql,options){if(sql.indexOf("SELECT SHA2('")===0)hashes++;return saved_query(sql,options);};
      var result;
      try {result=approve(item);} finally {spec.trusted.build=saved_build;query_checked=saved_query;}
      var stored=state(1);
      row('one_contract_per_dispatch',builds===1 && hashes===1);
      row('approved_once',result.ok && result.durable && result.review_state_committed && stored.status==='approved' && Number(stored.version)===1);
      row('postcondition_verified',result.result && result.result.verified && result.result.consistency_source==='InnoDB');
      row('repeated_approval_loses',!claim_review_step(item.plan.plan_id,1,'pending','executing',item.step.approval_digest));
      row('review_completed',query_checked("SELECT status FROM mysql.agent_review_plan_step WHERE plan_id='"+esc(item.plan.plan_id)+"'")[0].status==='completed');
      row('audit_correlated',Number(query_checked("SELECT COUNT(*) AS n FROM mysql.agent_memory_audit WHERE tier='trusted' AND op='outcome'"+
        " AND conversation_id='"+esc(A.conversation_id)+"' AND JSON_EXTRACT(detail,'$.plan_id')='"+esc(item.plan.plan_id)+"'")[0].n)>0);
      item=prepare(2,0,'stale_version');
      query_checked('UPDATE trusted_business.trusted_orders SET version=1 WHERE id=2');
      result=approve(item);stored=state(2);
      row('changed_version_refused',result.error==='business_precondition_failed' && stored.status==='pending' && Number(stored.version)===1);
      item=prepare(3,0,'stale_status');
      query_checked("UPDATE trusted_business.trusted_orders SET status='cancelled' WHERE id=3");
      result=approve(item);
      row('changed_status_refused',result.error==='business_precondition_failed' && state(3).status==='cancelled');
      item=prepare(4,0,'verification');
      // A real fixture trigger changes NEW.status: the verification must
      // catch it after the DML and undo the version increment as well.
      result=approve(item);stored=state(4);
      row('failed_verification_rolls_back',result.error==='business_verification_failed' && stored.status==='pending' && Number(stored.version)===0);
      item=prepare(5,0,'hook');
      register_hook('pre_tool_use',function(tool){if(tool==='approve_order')throw new Error('mandatory fixture denial');},{enforce:true});
      result=approve(item);
      row('mandatory_hook_no_write',result.error==='mandatory_hook_failed' && state(5).status==='pending');
      HOOKS.pre_tool_use=saved_pre.slice();
      var options={idempotency_key:'trusted_request_'+String(label)};
      A.execution=null;
      row('request_admitted',execution_begin('approve order request',options).ok);
      execution_finish('approved request result',false);A.execution=null;
      var replay=execution_begin('approve order request',options);
      row('request_replay',replay.replay && replay.response==='approved request result');
      query_checked("DELETE FROM mysql.agent_execution WHERE execution_id='"+esc(options.execution_id)+"'");
    } else if (op==='caller') {
      var item=prepare(4,0,'caller_failure');
      query_checked('START TRANSACTION');
      query_checked('UPDATE trusted_business.trusted_orders SET version=99 WHERE id=6');
      var result=approve(item);
      row('caller_failure_rolled_back_only_tool',result.error==='business_verification_failed' && Number(state(4).version)===0 && Number(state(6).version)===99);
      row('caller_transaction_preserved',get_tx_context().owner===TX_OWNER_CALLER && get_tx_context().active);
      query_checked('ROLLBACK');
      row('caller_prior_write_rollback',Number(state(6).version)===0);
      item=prepare(5,0,'caller_success');
      query_checked('START TRANSACTION');
      result=approve(item);
      row('caller_success_pending_commit',result.ok && result.pending_caller_commit && !result.durable && state(5).status==='approved' && get_tx_context().owner===TX_OWNER_CALLER);
      query_checked('ROLLBACK');
      row('caller_success_can_rollback',state(5).status==='pending' && Number(state(5).version)===0);
    } else if (op==='permission') {
      var item=prepare(6,0,'permission'), result=approve(item);
      row('sql_privileges_preserved',!result.ok && result.error==='business_execution_failed' && state(6).status==='pending' && Number(state(6).version)===0);
    } else if (op==='race') {
      // Each connection persists/claims its own approval for the same version.
      var item=prepare(7,0,'race_'+String(label));
      var result=approve(item);
      row('race_result',result.ok?'approved':result.error);
    }
  } finally {
    HOOKS.pre_tool_use=saved_pre; spec.trusted.version=saved_version;spec.trusted.build=saved_build;
    A.operator_policy=saved_policy;A.cached_chat_opt=saved_chat;
    // Clean only labelled plans after the caller has explicitly closed its
    // transaction; race plans are cleaned by the MTR connection after join.
    if (op!=='race') {
      query_checked("DELETE FROM mysql.agent_review_plan_step WHERE plan_id LIKE 'trusted_"+esc(label || op)+"_%'");
      query_checked("DELETE FROM mysql.agent_review_plan WHERE plan_id LIKE 'trusted_"+esc(label || op)+"_%'");
    }
  }
  return out;
}
