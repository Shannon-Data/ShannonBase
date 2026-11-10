//@include lib_tool_registry.js
// Schema-introspection and retrieval tools.
/* list_tables */
register_tool({
  name: 'list_tables', order: 10, category: 'schema',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '【表目录检索】不带 keyword 时列出全部表（大库会截断并提示补充 keyword）；' +
             '带 keyword 时按 IDF 相关性排序返回候选表\n' +
             '当上方 schema 信息标注"未展开完整 DDL"或提到某表只在名称列表里时，优先用这个工具定位表',
         en: '[Table directory lookup] Without keyword, lists all tables (large schemas are capped ' +
             'with a hint to add a keyword); with keyword, returns candidates ranked by IDF relevance\n' +
             'Prefer this tool whenever the schema section above says DDL was not inlined, or a table ' +
             'only appears in a name-only list' },
  example: { zh: { keyword: '可选，按关键词检索表名' },
             en: { keyword: 'optional, search table names by keyword' } },

  args: { type: 'object',
          properties: { keyword: { type: 'string' },
                        top_k:   { type: 'integer', minimum: 1, maximum: 50 } } },
  handler: impl_list_tables
});

function impl_list_tables(args, ctx) {
  return tool_list_tables(ctx.db, args || {});
}

/*  describe_table */
register_tool({
  name: 'describe_table', order: 11, category: 'schema',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '获取单张表（或 table_names 数组，最多5张）完整列定义 + 关联的 FOREIGN KEY\n' +
             '在为陌生表写 SQL 之前，必须先用这个工具确认列名，禁止凭猜测',
         en: 'Get full column definitions (+ related FOREIGN KEYs) for one table, or up to 5 via a ' +
             'table_names array\n' +
             'Always call this before writing SQL against an unfamiliar table — never guess column names' },
  example: { table_name: '...' },

  args: { type: 'object',
          properties: { table_name:  { type: 'string' },
                        table_names: { type: 'array', items: { type: 'string' } } },
          anyOf: [ { required: ['table_name'] },
                   { required: ['table_names'], properties: { table_names: { minItems: 1 } } } ] },
  messages: {
    '*#anyOf': { zh: 'describe_table 缺少 table_name（或 table_names 数组）参数',
                 en: 'describe_table missing table_name (or table_names array) argument' }
  },
  handler: impl_describe_table
});

function impl_describe_table(args, ctx) {
  return tool_describe_table(ctx.db, args || {});
}

/*  check_secondary_load */
register_tool({
  name: 'check_secondary_load', order: 12, category: 'schema',
  readOnly: true, write: false, ddl: false, transactional: false,
  capabilities: ['rapid'],
  doc: { zh: '检查表是否已 SECONDARY_LOAD 到 RAPID 引擎。未加载则告知用户需先执行 ' +
             'ALTER TABLE db.table SECONDARY_LOAD',
         en: 'Check whether a table is SECONDARY_LOADed into the RAPID engine. If not, run ' +
             'ALTER TABLE db.table SECONDARY_LOAD first' },
  example: { table_name: 'db.table' },
  args: { type: 'object', required: ['table_name'],
          properties: { table_name: { type: 'string', minLength: 3 } } },
  messages: {
    '*#required': { zh: 'check_secondary_load 缺少 table_name 参数（schema.table 格式）',
                    en: 'check_secondary_load missing table_name argument (schema.table format)' },
    '*#type':     { zh: 'check_secondary_load 缺少 table_name 参数（schema.table 格式）',
                    en: 'check_secondary_load missing table_name argument (schema.table format)' },
    '*#minLength':{ zh: 'check_secondary_load 缺少 table_name 参数（schema.table 格式）',
                    en: 'check_secondary_load missing table_name argument (schema.table format)' }
  },
  handler: impl_check_secondary_load
});

function impl_check_secondary_load(args, ctx) {
  return tool_check_secondary_load(args || {});
}

/* ml_rag */
register_tool({
  name: 'ml_rag', order: 13, category: 'schema',
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '在已配置的向量知识库中检索并回答（vector_store 由 @chat_options.rag_options 决定）',
         en: 'Retrieve from the configured vector knowledge base (vector_store comes from ' +
             '@chat_options.rag_options)' },
  example: { question: '...', top_k: 6 },
  args: { type: 'object', required: ['question'],
          properties: { question: { type: 'string' },
                        top_k:    { type: 'integer', minimum: 1, maximum: 50 },
                        retrieve_only: { type: 'boolean' } } },
  messages: {
    'question#required': { zh: 'ml_rag 缺少 question 参数', en: 'ml_rag missing question argument' },
    'question#type':     { zh: 'ml_rag 缺少 question 参数', en: 'ml_rag missing question argument' }
  },
  handler: impl_ml_rag
});

function impl_ml_rag(args, ctx) {
  var rag_res=rag_retrieve(String(args.question || A.user_message),args.top_k,ctx.chat_opt,args.retrieve_only);
  if(!rag_res.ok)
    return {ok:false,error:rag_res.error || 'ml_rag_failed',response:rag_res.text || t('ml_rag 执行失败','ml_rag execution failed'),rag_meta:rag_res};
  return {ok:true,response:rag_res.text || '',rag_meta:rag_res};
}
