//@include lib_agent_eval.js
function shannon_rag_selfcheck(op) {
  if(!selfcheck_writes_allowed()) return [['error','RAG checks require write opt-in']];
  var out=[], saved_chat=A.cached_chat_opt, saved_memory=A._mem_opt, saved_hooks=HOOKS.pre_tool_use.slice();
  var saved_generate=eval_script_respond, saved_rag=ml_rag;
  function row(k,v){out.push([k,String(!!v)]);}
  A.conversation_id=principal_scope_conversation_id('rag_context_'+op);
  A.cached_chat_opt={memory_options:{enabled:false},rag_options:{retrieval_options:{max_distance:2,percentage_distance:100}}};
  A._mem_opt=mem_merge_defaults(MEM_DEFAULTS,{enabled:false});
  A.rag_discovery=null;
  try {
    if(op==='permission') {
      var stores=discover_vector_tables(A.cached_chat_opt);
      row('discovery_respects_sql_grants',stores.length===1 && stores[0].schema_name==='test');
    } else {
      var stores=discover_vector_tables(A.cached_chat_opt);
      row('current_and_external_vectors_discovered',stores.length===2 && stores.some(function(s){return s.schema_name==='test';}) && stores.some(function(s){return s.schema_name==='rag_local';}));
      row('custom_vector_column_paired',stores.some(function(s){return s.segment==='内容' && s.segment_embedding==='向量';}));
      row('system_and_other_model_excluded',stores.every(function(s){return s.schema_name!=='mysql' && s.table_name!=='other_model';}));
      var scoped=discover_vector_tables({schema_name:'rag_local'});
      row('schema_scope_preserved',scoped.length===1 && scoped[0].schema_name==='rag_local');
      scoped=discover_vector_tables({tables:[{schema_name:'test',table_name:'rag_current'}],vector_store_columns:{segment:'segment',segment_embedding:'segment_embedding'}});
      row('table_and_column_options_preserved',scoped.length===1 && scoped[0].table_name==='rag_current');
      var question='How does rag_current store order amounts?';
      var block=build_rag_context(question,A.cached_chat_opt,'test',600);
      row('real_rag_returns_both_sources',!!block && A.rag_context.hits===2 && block.indexOf('[rag_local.docs]')>=0 && block.indexOf('[test.rag_current]')>=0);
      row('context_is_fenced_and_bounded',est_tok(block)<=600 && block.indexOf('untrusted="true"')>=0 && (block.match(/<\/retrieved_context>/g)||[]).length===1);
      row('rag_independent_of_agent_memory',A._mem_opt.enabled===false && !!block);
      var captured='';
      eval_script_respond=function(prompt,dialect){captured=prompt;return saved_generate(prompt,dialect);};
      A.eval_script={turns:[{text:'Amounts are stored in cents.'}],pos:0,calls:0};
      var answer=shannon_agent_run(question,A.conversation_id);
      row('loop_receives_knowledge_and_schema',answer.indexOf('Amounts are stored')>=0 && captured.indexOf('Local Knowledge References')>=0 && /rag_current[\s\S]{0,1000}segment_embedding/i.test(captured) && A.rag_context.hits===2);
      var native_calls=0;
      ml_rag=function(){native_calls++;return saved_rag.apply(null,arguments);};
      register_hook('pre_tool_use',function(tool){if(tool==='ml_rag')return {deny:true,error:'rag_fixture_denied'};},{enforce:true});
      block=build_rag_context(question,A.cached_chat_opt,'test',600);
      row('automatic_rag_obeys_hooks',block==='' && A.rag_context.error==='rag_fixture_denied' && native_calls===0);
      query_checked("DELETE FROM mysql.agent_execution WHERE conversation_id='"+esc(A.conversation_id)+"'");
    }
  } finally {
    A.cached_chat_opt=saved_chat;A._mem_opt=saved_memory;HOOKS.pre_tool_use=saved_hooks;
    eval_script_respond=saved_generate;ml_rag=saved_rag;A.eval_script=null;A.rag_discovery=null;
  }
  return out;
}
