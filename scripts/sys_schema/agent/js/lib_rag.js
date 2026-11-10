//@include lib_memory_registry.js
// Local knowledge retrieval. Discovery is scoped by SQL visibility and cached
// only for the current invocation; RAG and schema context feed the same agent.
function rag_identifier_supported(name) {
  // ML_RAG consumes schema.table strings and backtick-delimited columns.
  return /^[^.`'\\\u0000]+$/.test(String(name || ''));
}
function discover_vector_tables(chat_opt) {
  var opt=get_rag_options(chat_opt), key=JSON.stringify(opt);
  if (A.rag_discovery && A.rag_discovery.key===key) return A.rag_discovery.stores;
  if (opt.blocked_stores && opt.blocked_stores.length) return [];
  var cols=opt.vector_store_columns || {}, where="c.DATA_TYPE='vector' AND c.TABLE_SCHEMA NOT IN ('"+RAG_BLOCKED_SCHEMAS.map(esc).join("','")+"')";
  if (Array.isArray(opt.vector_store) && opt.vector_store.length)
    where+=" AND CONCAT(c.TABLE_SCHEMA,'.',c.TABLE_NAME) IN ('"+opt.vector_store.map(esc).join("','")+"')";
  else if (Array.isArray(opt.schema) && opt.schema.length)
    where+=" AND c.TABLE_SCHEMA IN ('"+opt.schema.map(esc).join("','")+"')";
  if (cols.segment_embedding) where+=" AND c.COLUMN_NAME='"+esc(cols.segment_embedding)+"'";
  if (cols.segment) where+=" AND s.COLUMN_NAME='"+esc(cols.segment)+"'";
  var rows=query_checked("SELECT c.TABLE_SCHEMA,c.TABLE_NAME,c.COLUMN_NAME AS vector_column,c.COLUMN_COMMENT,s.COLUMN_NAME AS text_column"+
    " FROM information_schema.COLUMNS c JOIN information_schema.COLUMNS s ON s.TABLE_SCHEMA=c.TABLE_SCHEMA AND s.TABLE_NAME=c.TABLE_NAME"+
    " WHERE "+where+" AND s.DATA_TYPE IN ('char','varchar','tinytext','text','mediumtext','longtext')"+
    " ORDER BY (c.TABLE_SCHEMA=DATABASE()) DESC,c.TABLE_SCHEMA,c.TABLE_NAME,c.COLUMN_NAME,"+
    " CASE s.COLUMN_NAME WHEN 'segment' THEN 0 WHEN 'content' THEN 1 WHEN 'text' THEN 2 WHEN 'body' THEN 3 ELSE 4 END,s.ORDINAL_POSITION");
  var stores=[], seen={};
  for(var i=0;i<rows.length;i++) {
    var row=rows[i], name=row.TABLE_SCHEMA+'.'+row.TABLE_NAME, id=name+'.'+row.vector_column;
    if(seen[id]) continue;
    seen[id]=true;
    if(![row.TABLE_SCHEMA,row.TABLE_NAME,row.text_column,row.vector_column].every(rag_identifier_supported)) continue;
    if(Array.isArray(opt.exclude_vector_store) && opt.exclude_vector_store.indexOf(name)>=0) continue;
    // Comments are optional in ShannonBase's ML_EMBED_TABLE. An explicit
    // model declaration must match; unlabelled vectors use the configured model.
    var declared=String(row.COLUMN_COMMENT || '').trim();
    if(declared.charAt(0)==='{') {
      try {var meta=JSON.parse(declared);declared=String(meta.model_id || meta.embed_model_id || '');}
      catch(e){declared='';}
    } else if(!/^[A-Za-z0-9_.\/-]+$/.test(declared)) declared='';
    if(declared && declared!==String(opt.embed_model_id)) continue;
    stores.push({schema_name:row.TABLE_SCHEMA,table_name:row.TABLE_NAME,
      segment:row.text_column,segment_embedding:row.vector_column});
  }
  A.rag_discovery={key:key,stores:stores,truncated:rows_truncated(rows)};
  return stores;
}

function rag_retrieve(question, top_k, chat_opt, retrieve_only) {
  var opt=get_rag_options(chat_opt);
  if(opt.blocked_stores && opt.blocked_stores.length)
    return {ok:false,error:'rag_store_forbidden',text:rag_blocked_message(opt.blocked_stores)};
  var mapped=opt.vector_store_columns || {};
  if(!Object.keys(mapped).every(function(k){return rag_identifier_supported(mapped[k]);}))
    return {ok:false,error:'rag_identifier_unsupported',text:'The RAG column mapping contains an unsupported identifier.'};
  var stores=discover_vector_tables(chat_opt), groups={}, keys=[];
  for(var i=0;i<stores.length;i++) {
    var store=stores[i], key=store.segment+'\u0000'+store.segment_embedding;
    if(!groups[key]) {groups[key]={columns:store,tables:[]};keys.push(key);}
    groups[key].tables.push(store.schema_name+'.'+store.table_name);
  }
  var citations=[], errors=[], answers=[], any_ok=!keys.length;
  for(var g=0;g<keys.length;g++) {
    if(A.runtime_active && runtime_sql_timeout()<=0) {errors.push('agent_deadline_exceeded');break;}
    var group=groups[keys[g]], options=Object.assign({},opt,{vector_store:group.tables,
      vector_store_columns:Object.assign({},opt.vector_store_columns || {},{
        segment:group.columns.segment,segment_embedding:group.columns.segment_embedding})});
    if(retrieve_only) options.skip_generate=1;
    options.n_citations=Number(top_k || opt.n_citations || 6);
    var result=ml_rag(question,options.n_citations,options);
    if(!result.ok) {errors.push(result.error || 'rag_retrieval_failed');continue;}
    any_ok=true;
    if(!options.skip_generate && result.text) answers.push(result.text);
    citations=citations.concat(result.citations || []);
  }
  citations.sort(function(a,b){return Number(a.distance)-Number(b.distance);});
  citations=citations.slice(0,Number(top_k || opt.n_citations || 6));
  var text=[];
  for(var c=0;c<citations.length;c++)
    text.push('['+citations[c].vector_store+'] '+String(citations[c].segment || ''));
  return {ok:any_ok,text:answers.length ? answers.join('\n\n') : text.join('\n\n'),found:citations.length>0,hits:citations.length,
    citations:citations,stores:stores,degraded:errors.length>0,errors:errors,retrieved_only:!!retrieve_only || !!opt.skip_generate};
}

function build_rag_context(question, chat_opt, db, token_cap) {
  if(token_cap<=0) return '';
  try {
    var opt=get_rag_options(chat_opt);
    if(Number(opt.n_citations)===0) return '';
    if(opt.blocked_stores && opt.blocked_stores.length) {
      A.rag_context={ok:false,hits:0,sources:[],error:'rag_store_forbidden'};
      mem_log_audit('rag','denied','local_vectors','rag_store_forbidden',0,0,'');
      return '';
    }
    var stores=discover_vector_tables(chat_opt);
    if(!stores.length) return '';
    var top_k=Math.max(1,Math.min(50,Number(opt.n_citations)||6));
    var result=execute_tool('ml_rag',{question:question,top_k:top_k,retrieve_only:true},db);
    var rag=result.rag_meta;
    A.rag_context={ok:!!result.ok,hits:rag ? rag.hits : 0,sources:[],error:result.error || '',degraded:!!(rag && rag.degraded)};
    if(!result.ok || !rag || !rag.hits) return '';
    var body=String(result.response || ''), title=t('【本地知识库参考】','[Local Knowledge References]');
    function wrap(n){return mem_wrap_untrusted(title+'\n'+body.substring(0,n).replace(/<\/retrieved_context>/gi,'&lt;/retrieved_context&gt;'));}
    var low=0,high=body.length;
    if(est_tok(wrap(0))>token_cap) {A.rag_context.truncated=true;A.rag_context.error='rag_context_budget_too_small';return '';}
    while(low<high) {var mid=Math.ceil((low+high)/2);if(est_tok(wrap(mid))<=token_cap)low=mid;else high=mid-1;}
    if(!low) {A.rag_context.truncated=true;A.rag_context.error='rag_context_budget_too_small';return '';}
    A.rag_context.truncated=low<body.length || !!(A.rag_discovery && A.rag_discovery.truncated);
    for(var i=0;i<rag.citations.length && i<8;i++) {
      var ref=rag.citations[i];
      A.rag_context.sources.push({vector_store:ref.vector_store,document_name:ref.document_name,distance:ref.distance});
    }
    mem_log_audit('rag','context','local_vectors',JSON.stringify({hits:rag.hits,degraded:A.rag_context.degraded,truncated:A.rag_context.truncated,
      sources:A.rag_context.sources.slice(0,3).map(function(s){return String(s.vector_store).substring(0,80);})}),rag.hits,0,'');
    return wrap(low);
  } catch(e) {
    A.rag_context={ok:false,hits:0,error:String(e),sources:[]};
    mem_log_audit('rag','degraded','local_vectors',String(e),0,0,'');
    return '';
  }
}
