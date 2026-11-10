//@include lib_tool_registry.js
// Compiled, declarative DML tools. No SQL, identifiers or implementations are
// loaded from a model response or a session option.
function trusted_fail(code, message) {
  var err = new Error(message); err.trusted_code = code; throw err;
}
function trusted_failure(err, fallback) {
  return {ok:false,error:err.trusted_code || fallback || 'trusted_template_invalid',response:String(err.message || err)};
}
function trusted_names(value) {
  if (Array.isArray(value)) return value;
  var text = String(value || '').trim();
  if (text.charAt(0) === '[') {
    try { var list = JSON.parse(text); return Array.isArray(list) ? list : []; }
    catch (e) { return []; }
  }
  return text ? text.split(',').map(function(s) { return s.trim(); }) : [];
}
function trusted_tool_admission(name, spec) {
  var base = get_operator_policy(), co = get_chat_options();
  if (spec && spec.trusted) {
    if (trusted_names(base.trusted_tool_allowlist).indexOf(name) < 0 ||
        (co.trusted_tool_allowlist !== undefined && trusted_names(co.trusted_tool_allowlist).indexOf(name) < 0))
      return {ok:false,error:'trusted_tool_not_allowed',response:'The operator has not authorized this business tool.'};
  } else if (spec && (spec.write || spec.ddl) && spec.category !== 'tx' && !tool_is_memory_scoped(name) &&
      (_pol_bool(base.trusted_writes_only) || co.trusted_writes_only === true)) {
    return {ok:false,error:'trusted_write_required',response:'This instance requires an authorized fixed business tool for writes.'};
  }
  return null;
}
function trusted_table(name) {
  var match = String(name || '').match(/^([A-Za-z0-9_$]{1,64})\.([A-Za-z0-9_$]{1,64})$/);
  if (!match || /^(mysql|sys|information_schema|performance_schema)$/i.test(match[1]))
    trusted_fail('trusted_table_not_configured','Configure an operator-owned schema.table in a user database.');
  return {schema:match[1],name:match[2],sql:'`'+esc_ident(match[1])+'`.`'+esc_ident(match[2])+'`'};
}
function trusted_integer(value) {
  if (!ts_is_integer(value) || value < 0 || value > 9007199254740990)
    trusted_fail('trusted_argument_invalid','Trusted integer argument is outside the supported exact range.');
  return String(value);
}
function trusted_plan(spec, args) {
  var denied = trusted_tool_admission(spec.name,spec);
  if (denied) trusted_fail(denied.error,denied.response);
  var invalid = ts_validate(spec.args,args);
  if (invalid) trusted_fail('trusted_argument_invalid',ts_error_message(spec,invalid));
  var mode = sql_mode_gate_check();
  if (!mode.ok) trusted_fail('sql_mode_unsupported',mode.message);
  var plan = spec.trusted.build(args), table = trusted_table(plan.table);
  var write = classify_statement(plan.sql), lock = classify_statement(plan.lock_sql), read = classify_statement(plan.verify_sql);
  if (!write.dml_keyword || write.is_ddl || write.multiple_statements || write.external_side_effects ||
      !lock.is_read || lock.multiple_statements || !read.is_read || read.multiple_statements ||
      !plan.expected || typeof plan.expected !== 'object')
    trusted_fail('trusted_template_invalid','A trusted template must be one DML with fixed lock and verification reads.');
  var contract = JSON.stringify({protocol:'trusted-dml-v1',version:spec.trusted.version,
    table:plan.table,sql:plan.sql,lock_sql:plan.lock_sql,verify_sql:plan.verify_sql,
    expected:plan.expected,args_schema:spec.args,affected_rows:1,requiresTx:true,transactional:true});
  var digest = query_checked("SELECT SHA2('"+esc(contract)+"',256) AS digest")[0].digest;
  plan.preview = '/* trusted:'+spec.name+':'+String(digest)+' */ '+plan.sql;
  plan.table_info = table;
  plan.contract_hash = String(digest);
  return plan;
}
function trusted_review_error(step, db, plan) {
  if ((plan && step.sql !== plan.preview) || String(step.execution_db || '') !== String(db || '') ||
      !step.writes || step.ddl || step.transactional === false)
    return {ok:false,error:'trusted_template_changed',response:'Trusted template, target or version changed. Reject this plan and request a fresh approval.'};
  return null;
}
function trusted_review_guard(step, db) {
  var spec = step && get_tool_spec(step.tool);
  if (!spec || !spec.trusted) return null;
  try {
    return trusted_review_error(step,db,trusted_plan(spec,step.args || {}));
  } catch (e) { return trusted_failure(e); }
}
function trusted_execute(spec, args, ctx) {
  var saved_primary = A.force_primary_sql;
  A.force_primary_sql = true;
  var savepoint = 'shannon_trusted_'+gen_query_id(), opened = false;
  try {
    // The dispatcher has checked transaction ownership, final hook arguments,
    // authorization and approval against this freshly built contract.
    var plan = ctx && ctx.trusted_plan;
    if (!plan)
      trusted_fail('trusted_execution_context_required','Use the tool dispatcher to execute a fixed business operation.');
    query_checked('SAVEPOINT '+savepoint); opened = true;
    // Acquire the target row/metadata lock before checking engine metadata;
    // concurrent DDL cannot replace the checked table while this tool runs.
    query_checked(plan.lock_sql,{max_rows:2});
    var metadata = query_checked("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA='"+
      esc(plan.table_info.schema)+"' AND TABLE_NAME='"+esc(plan.table_info.name)+"'");
    if (!metadata.length || String(metadata[0].ENGINE).toUpperCase() !== 'INNODB')
      trusted_fail('trusted_engine_required','Trusted writes require an InnoDB base table.');
    var changed = query_checked(plan.sql);
    if (!changed || Number(changed.affected_rows) !== 1)
      trusted_fail('business_precondition_failed','Order version/status changed or the target is missing. No business change was committed.');
    var rows = query_checked(plan.verify_sql,{max_rows:2});
    if (!Array.isArray(rows) || rows.length !== 1)
      trusted_fail('business_verification_failed','The business postcondition did not return exactly one row.');
    for (var key in plan.expected) {
      if (!Object.prototype.hasOwnProperty.call(plan.expected,key)) continue;
      if (String(rows[0][key]) !== String(plan.expected[key]))
        trusted_fail('business_verification_failed','The business postcondition failed for '+key+'.');
    }
    query_checked('RELEASE SAVEPOINT '+savepoint); opened = false;
    return {ok:true,response:'Order approved; version/status and affected row count verified.',
      affected_rows:1,sql:plan.sql,contract_hash:plan.contract_hash,
      result:{data:rows,row_count:1,source_sql:plan.verify_sql,verified:true,consistency_source:'InnoDB'}};
  } catch (e) {
    var result = trusted_failure(e,'business_execution_failed');
    result.contract_hash = plan && plan.contract_hash || '';
    if (opened) {
      var saved_cleanup = A.runtime_cleanup;
      A.runtime_cleanup = true;
      try { query_checked('ROLLBACK TO SAVEPOINT '+savepoint); query_checked('RELEASE SAVEPOINT '+savepoint); }
      catch (rollback_error) { result = {ok:false,error:'trusted_rollback_unverified',response:String(rollback_error)}; }
      finally { A.runtime_cleanup = saved_cleanup; }
    }
    return result;
  } finally { A.force_primary_sql = saved_primary; }
}
function register_trusted_tool(definition) {
  if (!definition || !/^[A-Za-z][A-Za-z0-9_]{0,63}$/.test(definition.name || '') ||
      !definition.version || typeof definition.build !== 'function' || get_tool_spec(definition.name))
    throw new Error('A trusted tool requires a fixed name, version and declarative builder.');
  var spec = {name:definition.name,order:definition.order || 180,category:'business',scope:'db',
    write:true,ddl:false,risk:'high',requiresTx:true,transactional:true,
    doc:definition.doc,example:definition.example,args:definition.args,
    trusted:{version:String(definition.version),build:definition.build}};
  spec.handler = function(args,ctx) { return trusted_execute(spec,args,ctx); };
  register_tool(spec);
}

register_tool_category({name:'business',expand:'ondemand',match:/订单|审批|order|approv|business/i});
register_trusted_tool({
  name:'approve_order',version:'1',
  doc:{zh:'固定订单审批操作，必须显式审批，在短事务内执行；需要 operator 配置 trusted_order_table 和 trusted_tool_allowlist。仅审批 pending 且版本匹配的订单，审批后增加版本。',
       en:'Fixed order approval with mandatory approval and a short transaction. Requires operator trusted_order_table and trusted_tool_allowlist. Approves a pending order with the exact expected version, then increments its version.'},
  example:{order_id:1,expected_version:0},
  args:{type:'object',required:['order_id','expected_version'],properties:{
    order_id:{type:'integer',minimum:1,maximum:9007199254740990},
    expected_version:{type:'integer',minimum:0,maximum:9007199254740990}}},
  build:function(args) {
    var target = String(get_operator_policy().trusted_order_table || '');
    var table = trusted_table(target), id = trusted_integer(args.order_id), version = trusted_integer(args.expected_version);
    return {table:target,
      lock_sql:'SELECT id FROM '+table.sql+' WHERE id='+id+' FOR UPDATE',
      sql:'UPDATE '+table.sql+" SET status='approved',version=version+1 WHERE id="+id+" AND version="+version+" AND status='pending'",
      verify_sql:'SELECT id,status,version FROM '+table.sql+' WHERE id='+id,
      expected:{id:args.order_id,status:'approved',version:args.expected_version+1}};
  }
});
