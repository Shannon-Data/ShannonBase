//@include lib_schema.js
//@include lib_memory.js

function discover_vector_tables(chat_opt) {
  var rag_opt = get_rag_options(chat_opt);

  if (Array.isArray(rag_opt.vector_store) && rag_opt.vector_store.length > 0) {
    return rag_opt.vector_store.map(function(s) {
      var parts = String(s).split('.');
      return { schema_name: parts[0], table_name: parts.slice(1).join('.') };
    });
  }
  if (Array.isArray(chat_opt.tables) && chat_opt.tables.length > 0) return chat_opt.tables;

  var seg_col = 'segment', emb_col = 'segment_embedding';
  var schema_filter = '';
  if (Array.isArray(rag_opt.schema) && rag_opt.schema.length > 0) {
    schema_filter = " AND c.TABLE_SCHEMA IN ('" +
      rag_opt.schema.map(esc).join("','") + "')";
  } else if (chat_opt.schema_name) {
    schema_filter = " AND c.TABLE_SCHEMA='" + esc(chat_opt.schema_name) + "'";
  }

  var rows = query(
    "SELECT c.TABLE_SCHEMA, c.TABLE_NAME FROM information_schema.COLUMNS c" +
    " WHERE c.COLUMN_NAME='" + esc(emb_col) + "' AND c.DATA_TYPE='vector'" +
    schema_filter +
    "   AND EXISTS (SELECT 1 FROM information_schema.COLUMNS c2" +
    "     WHERE c2.TABLE_SCHEMA=c.TABLE_SCHEMA AND c2.TABLE_NAME=c.TABLE_NAME" +
    "       AND c2.COLUMN_NAME='" + esc(seg_col) + "')" +
    " ORDER BY c.TABLE_SCHEMA, c.TABLE_NAME"
  );
  if (!Array.isArray(rows) || !rows.length) return [];
  return rows.map(function(r) {
    return { schema_name: r.TABLE_SCHEMA, table_name: r.TABLE_NAME };
  });
}

function is_knowledge_query(text) {
  var schema_pat = new RegExp(
    '有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables|' +
    '有哪些数据库|所有数据库|列出.*数据库|show.?databases|list.*databases|what.*databases|' +
    '表.*结构|结构.*表|字段|列信息|column.*info|table.*structure|describe.*table|' +
    '关联关系|外键|foreign.?key|join.*关系|table.*relation|related.*tables|' +
    '索引|index.*info|index.*analysis|missing.*index|covering.*index|' +
    '数据量|行数|row.?count|record.?count|table.*size|数据大小', 'i'
  );
  if (schema_pat.test(text)) return false;

  var knowledge_pat = new RegExp(
    '(什么是|what is|how does|解释|explain|介绍|describe|为什么|why|' +
    'what are|有哪些|如何理解|原理|architecture|概念|区别|对比|compare|' +
    'shannonbase|heatwave|innodb|mysql.*特性|raft|mvcc|lsm)', 'i'
  );
  var sql_pat = /SELECT|INSERT|UPDATE|DELETE|查询|查找|统计|修改|删除|插入|导出/i;
  return knowledge_pat.test(text.trim()) && !sql_pat.test(text);
}

function heatwave_dispatch(user_msg, chat_opt, vector_tables) {
  var u_pre = t('用户：', 'User: ');
  var a_suf = t('\n助手：', '\nAssistant: ');
  var rag_opt = get_rag_options(chat_opt);

  if (!vector_tables || vector_tables.length === 0) {
    if (rag_opt.skip_generate)
      return { mode:'EMPTY',
              response: t('未找到向量知识库。', 'No vector knowledge base found.'),
              tables: [] };
    var hist_ctx0 = chat_history_to_text(chat_opt);
    return {
      mode: 'GENERATE',
      response: ml_generate(
        (hist_ctx0 ? hist_ctx0 + '\n\n' : '') + u_pre + user_msg + a_suf,
        chat_opt.model_options || {}),
      tables: []
    };
  }

  var rag_pass = Object.assign({}, rag_opt);
  var topK = rag_pass.n_citations || 6;
  delete rag_pass.n_citations;

  var hist_ctx     = chat_history_to_text(chat_opt);
  var rag_question = hist_ctx ? hist_ctx + '\n' + u_pre + user_msg : user_msg;
  var rag_res      = ml_rag(rag_question, topK, rag_pass);

  if (rag_res && rag_res.length >= 30)
    return { mode: 'RAG', response: rag_res, tables: vector_tables };

  if (rag_pass.skip_generate) {
    var fb_prompt = (hist_ctx ? hist_ctx + '\n\n' : '') + u_pre + user_msg + a_suf;
    return { mode:'GENERATE',
            response: ml_generate(fb_prompt, chat_opt.model_options || {}),
            tables: vector_tables };
  }
  return { mode:'EMPTY',
          response: t('知识库中未找到相关内容。',
                      'No relevant content found in knowledge base.'),
          tables: vector_tables };
}

function catalog_match(text) {
  var catalog = [
    { pattern: /锁[\s\S]{0,15}情况|当前.*持有.*锁|data.?locks|current.*locks?|show.*locks?/i,
      sql: 'SELECT ENGINE_TRANSACTION_ID,OBJECT_SCHEMA,OBJECT_NAME,LOCK_TYPE,LOCK_MODE,' +
           'LOCK_STATUS,LOCK_DATA FROM performance_schema.data_locks LIMIT 50',
      desc: t('当前持有的行锁', 'Current row locks held') },

    { pattern: /锁等待|lock.?wait|谁在等|被谁锁|who.*block|blocking.*tr(x|ansaction)/i,
      sql: 'SELECT r.trx_id AS waiting_trx,r.trx_mysql_thread_id AS waiting_thread,' +
           'r.trx_query AS waiting_query,b.trx_id AS blocking_trx,' +
           'b.trx_mysql_thread_id AS blocking_thread,b.trx_query AS blocking_query,' +
           'w.BLOCKING_ENGINE_LOCK_ID FROM information_schema.INNODB_TRX r ' +
           'JOIN performance_schema.data_lock_waits w ' +
           '  ON r.trx_id=w.REQUESTING_ENGINE_TRANSACTION_ID ' +
           'JOIN information_schema.INNODB_TRX b ' +
           '  ON b.trx_id=w.BLOCKING_ENGINE_TRANSACTION_ID',
      desc: t('锁等待关系', 'Lock wait relationships') },

    { pattern: /死锁|deadlock/i,
      sql: 'SHOW ENGINE INNODB STATUS',
      desc: t('InnoDB 死锁详情', 'InnoDB deadlock details') },

    { pattern: /活跃事务|active.?tr(x|ansaction)|innodb.*trx|长事务|long.?tr(x|ansaction)/i,
      sql: 'SELECT trx_id,trx_state,trx_started,trx_mysql_thread_id,' +
           'LEFT(trx_query,100) AS trx_query,trx_rows_modified,trx_rows_locked ' +
           'FROM information_schema.INNODB_TRX ORDER BY trx_started',
      desc: t('当前活跃 InnoDB 事务', 'Active InnoDB transactions') },

    { pattern: /processlist|进程列表|正在执行|running.?quer|active.*queries/i,
      sql: "SELECT ID,USER,HOST,DB,COMMAND,TIME,STATE,LEFT(INFO,120) AS QUERY " +
           "FROM information_schema.PROCESSLIST WHERE COMMAND!='Sleep' ORDER BY TIME DESC",
      desc: t('当前活跃进程列表', 'Active process list') },

    { pattern: /连接数|thread.?connect|Threads_connected|connection.?count/i,
      sql: 'SHOW STATUS LIKE "Threads%"',
      desc: t('线程连接数统计', 'Thread / connection statistics') },

    { pattern: /慢查询|slow.?quer|top.*慢|耗时最长|slowest.?quer/i,
      sql: 'SELECT DIGEST_TEXT,COUNT_STAR,' +
           'ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec,' +
           'ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec,' +
           'SUM_ROWS_EXAMINED,SUM_ROWS_SENT ' +
           'FROM performance_schema.events_statements_summary_by_digest ' +
           'ORDER BY AVG_TIMER_WAIT DESC LIMIT 20',
      desc: t('慢查询 TOP20', 'Slow query TOP20') },

    { pattern: /buffer.?pool|缓冲池/i,
      sql: 'SELECT POOL_ID,POOL_SIZE,FREE_BUFFERS,DATABASE_PAGES,HIT_RATE,' +
           'PAGES_MADE_YOUNG FROM information_schema.INNODB_BUFFER_POOL_STATS',
      desc: t('InnoDB Buffer Pool 状态', 'InnoDB Buffer Pool status') },

    { pattern: /redo.?log|redo日志|log.?capacity/i,
      sql: 'SHOW STATUS LIKE "Innodb_redo%"',
      desc: t('InnoDB Redo Log 状态', 'InnoDB Redo Log status') },

    { pattern: /表空间|tablespace/i,
      sql: 'SELECT SPACE_ID,NAME,ROW_FORMAT,PAGE_SIZE,ZIP_PAGE_SIZE ' +
           'FROM information_schema.INNODB_TABLESPACES ORDER BY NAME LIMIT 50',
      desc: t('InnoDB 表空间信息', 'InnoDB tablespace info') },

    { pattern: /系统变量|global.?var|show.?variable/i,
      sql: 'SHOW VARIABLES LIKE "innodb%"',
      desc: t('InnoDB 相关系统变量', 'InnoDB system variables') }
  ];

  for (var i = 0; i < catalog.length; i++)
    if (catalog[i].pattern.test(text)) return catalog[i];
  return null;
}

function classify_query(text) {
  var sys_pat = new RegExp(
    '进程|processlist|线程|thread|连接数|connection|慢查询|slow.?quer|变量|variable|' +
    '系统状态|status|存储引擎|engine|权限|grant|表空间|tablespace|redo|buffer.?pool|' +
    '锁|lock|死锁|deadlock|活跃.*事务|active.*tr[xa]|innodb.*trx|' +
    'information.?schema|performance.?schema|当前.*用户|current.*user|版本|version', 'i'
  );
  var olap_pat = new RegExp(
    'GROUP\\s+BY|HAVING|SUM\\s*\\(|AVG\\s*\\(|COUNT\\s*\\(|OVER\\s*\\(|' +
    'PARTITION\\s+BY|ROLLUP|CUBE|聚合|汇总|统计|分析|趋势|占比|排名|同比|环比', 'i'
  );
  if (sys_pat.test(text)) return 'system';
  if (olap_pat.test(text)) return 'olap';
  return 'oltp';
}

function get_workload_hint(qtype) {
  if (qtype === 'system') return t(
    '⚠ 系统元数据查询 — 必须使用正确表：\n' +
    '  行锁 → performance_schema.data_locks\n' +
    '  锁等待 → performance_schema.data_lock_waits\n' +
    '  活跃事务 → information_schema.INNODB_TRX\n' +
    '  死锁 → SHOW ENGINE INNODB STATUS\n' +
    '  ⛔ 禁止：INNODB_LOCKS / INNODB_LOCK_WAITS（MySQL 8.0 已删除）\n' +
    '  慢查询 → performance_schema.events_statements_summary_by_digest\n' +
    '  ⛔ 报 Unknown table → 立即换正确表，禁止重试同一表名。',
    '⚠ System metadata query — use the correct tables:\n' +
    '  Row locks    → performance_schema.data_locks\n' +
    '  Lock waits   → performance_schema.data_lock_waits\n' +
    '  Active trx   → information_schema.INNODB_TRX\n' +
    '  Deadlocks    → SHOW ENGINE INNODB STATUS\n' +
    '  ⛔ Forbidden: INNODB_LOCKS / INNODB_LOCK_WAITS (removed in MySQL 8.0)\n' +
    '  Slow queries → performance_schema.events_statements_summary_by_digest\n' +
    '  ⛔ On Unknown table error → switch to correct table, do not retry.'
  );
  if (qtype === 'olap') return t(
    '⚠ OLAP：优先列存；GROUP BY 列需索引；大结果集主键偏移分页。',
    '⚠ OLAP: prefer columnar storage; GROUP BY columns need indexes; use keyset pagination for large result sets.'
  );
  return t(
    'OLTP：主键命中优先；UPDATE/DELETE 必须带 WHERE；写操作走 begin_tx 流程。',
    'OLTP: prefer primary key lookups; UPDATE/DELETE must include WHERE; wrap writes with begin_tx.'
  );
}

function build_system_prompt(db, schema_ctx, join_hint, plan_hint,
                             few_shot, history, hw_history) {
  var qtype        = classify_query(A.user_message);
  var hist_section = hw_history || history || t('（无历史）', '(No history)');

  var now_rows = query("SELECT DATE_FORMAT(NOW(),'%Y-%m-%d') AS today, CAST(YEAR(NOW()) AS CHAR) AS yr");
  var today    = (Array.isArray(now_rows) && now_rows.length) ? (now_rows[0].today || '') : '';
  var cur_year = (Array.isArray(now_rows) && now_rows.length) ? (now_rows[0].yr    || '') : '';
  var current_time_info = t(
      '【当前时间】\n今天日期：' + today + '，当前年份：' + cur_year + '\n\n',
      '[Current Time]\nToday: ' + today + ', Current Year: ' + cur_year + '\n\n'
  );

  var inline_few_shot = t(
    '【Few-Shot 强制示例（必须照此格式）】\n' +
    'Q: 有哪些表？\n' +
    'A: {"thought":"列出当前库所有表","tool":"query_db","args":{"sql":"SHOW TABLES"}}\n\n' +
    'Q: 每张表的结构是什么？\n' +
    'A: {"thought":"plan_sql 先列表再查列定义","tool":"plan_sql","args":{"steps":[\n' +
    '     {"sql":"SHOW TABLES","desc":"枚举所有表"},\n' +
    '     {"sql":"SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION","desc":"所有列定义"}\n' +
    '   ]}}\n\n' +
    'Q: 统计每张表的行数\n' +
    'A: {"thought":"查 information_schema.TABLES","tool":"query_db","args":{"sql":"SELECT TABLE_NAME,TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE=\'BASE TABLE\'"}}\n\n' +
    'Q: 查看 orders 表最近 7 天的数据\n' +
    'A: {"thought":"滑动窗口过滤，用 NOW()-INTERVAL，不指定固定年份","tool":"query_db","args":{"sql":"SELECT * FROM orders WHERE created_at >= NOW()-INTERVAL 7 DAY LIMIT 100"}}\n\n' +
    'Q: 分析订单金额按月汇总\n' +
    'A: {"thought":"多步：先看结构再聚合","tool":"plan_sql","args":{"steps":[\n' +
    '     {"sql":"DESC orders","desc":"确认列名"},\n' +
    '     {"sql":"SELECT DATE_FORMAT(created_at,\'%Y-%m\') AS month,SUM(amount) AS total FROM orders GROUP BY month ORDER BY month","desc":"按月汇总"}\n' +
    '   ]}}\n\n' +
    'Q: 分析2024年每个月的收入\n' +
    'A: {"thought":"用户指定固定年份2024，必须用 YEAR()=N，禁止用 NOW()-INTERVAL 滑动窗口","tool":"query_db","args":{"sql":"SELECT DATE_FORMAT(tx_date,\'%Y-%m\') AS month,SUM(amount) AS total FROM revenue_items WHERE YEAR(tx_date)=2024 GROUP BY month ORDER BY month"}}\n\n' +
    'Q: 统计2023年各部门费用合计\n' +
    'A: {"thought":"固定年份过滤 + GROUP BY dept_name，用 YEAR()=N","tool":"query_db","args":{"sql":"SELECT dept_name,SUM(amount) AS total FROM expense_items WHERE YEAR(tx_date)=2023 GROUP BY dept_name ORDER BY total DESC"}}\n',
    'Q: 生成2024年1-6月每月利润表（收入/成本/毛利润/期间费用/净利润）\n' +
    'A: {"thought":"需要一个完整的1-6月月份序列，用单个派生表生成月份号，再分别LEFT JOIN各表按月聚合的结果；⛔禁止用两个平级派生表互相引用对方别名（如 (SELECT...) months JOIN (SELECT ... months.m ...) m ON 1=1），MySQL不支持子查询引用同级FROM子句中其它表的列","tool":"query_db","args":{"sql":"SELECT CONCAT(\'2024-\',LPAD(m.mo,2,\'0\')) AS month_label, COALESCE(r.revenue,0) AS revenue, COALESCE(c.cost,0) AS cost FROM (SELECT 1 AS mo UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6) m LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(amount) AS revenue FROM revenue_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' GROUP BY MONTH(tx_date)) r ON m.mo=r.mo LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(cogs_amount) AS cost FROM cost_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' AND cost_type=\'cogs\' GROUP BY MONTH(tx_date)) c ON m.mo=c.mo ORDER BY m.mo"}}\n' +
    '[Few-Shot Examples (required format)]\n' +
    'Q: What tables are in this database?\n' +
    'A: {"thought":"List all tables in current database","tool":"query_db","args":{"sql":"SHOW TABLES"}}\n\n' +
    'Q: What is the structure of each table?\n' +
    'A: {"thought":"Use plan_sql to enumerate tables then get column definitions","tool":"plan_sql","args":{"steps":[\n' +
    '     {"sql":"SHOW TABLES","desc":"List all tables"},\n' +
    '     {"sql":"SELECT TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME,ORDINAL_POSITION","desc":"All column definitions"}\n' +
    '   ]}}\n\n' +
    'Q: Count rows in each table\n' +
    'A: {"thought":"Query information_schema.TABLES","tool":"query_db","args":{"sql":"SELECT TABLE_NAME,TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE=\'BASE TABLE\'"}}\n\n' +
    'Q: Show orders from the last 7 days\n' +
    'A: {"thought":"Sliding window filter using NOW()-INTERVAL, not a fixed year","tool":"query_db","args":{"sql":"SELECT * FROM orders WHERE created_at >= NOW()-INTERVAL 7 DAY LIMIT 100"}}\n\n' +
    'Q: Analyze monthly order amount summary\n' +
    'A: {"thought":"Multi-step: inspect schema then aggregate","tool":"plan_sql","args":{"steps":[\n' +
    '     {"sql":"DESC orders","desc":"Confirm column names"},\n' +
    '     {"sql":"SELECT DATE_FORMAT(created_at,\'%Y-%m\') AS month,SUM(amount) AS total FROM orders GROUP BY month ORDER BY month","desc":"Monthly aggregation"}\n' +
    '   ]}}\n\n' +
    'Q: Analyze monthly revenue for 2024\n' +
    'A: {"thought":"User specified fixed year 2024; must use YEAR()=N, never NOW()-INTERVAL sliding window","tool":"query_db","args":{"sql":"SELECT DATE_FORMAT(tx_date,\'%Y-%m\') AS month,SUM(amount) AS total FROM revenue_items WHERE YEAR(tx_date)=2024 GROUP BY month ORDER BY month"}}\n\n' +
    'Q: Summarize department expenses for 2023\n' +
    'A: {"thought":"Fixed year filter + GROUP BY dept_name using YEAR()=N","tool":"query_db","args":{"sql":"SELECT dept_name,SUM(amount) AS total FROM expense_items WHERE YEAR(tx_date)=2023 GROUP BY dept_name ORDER BY total DESC"}}\n\n' +
    'Q: Generate a monthly profit & loss statement for Jan-Jun 2024 (revenue/cost/gross profit/period expenses/net profit)\n' +
    'A: {"thought":"Need a complete month sequence (1-6) from a single derived table, then LEFT JOIN separately-aggregated per-table results onto it; ⛔ never let one derived table\'s subquery reference another sibling derived table\'s alias/column in the same FROM clause (e.g. (SELECT...) months JOIN (SELECT ... months.m ...) m ON 1=1) — MySQL does not support a subquery referencing a sibling table in the same FROM clause, this raises Unknown table","tool":"query_db","args":{"sql":"SELECT CONCAT(\'2024-\',LPAD(m.mo,2,\'0\')) AS month_label, COALESCE(r.revenue,0) AS revenue, COALESCE(c.cost,0) AS cost FROM (SELECT 1 AS mo UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6) m LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(amount) AS revenue FROM revenue_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' GROUP BY MONTH(tx_date)) r ON m.mo=r.mo LEFT JOIN (SELECT MONTH(tx_date) AS mo, SUM(cogs_amount) AS cost FROM cost_items WHERE tx_date>=\'2024-01-01\' AND tx_date<\'2024-07-01\' AND cost_type=\'cogs\' GROUP BY MONTH(tx_date)) c ON m.mo=c.mo ORDER BY m.mo"}}\n'
  );

  if (A.lang === 'zh') {
    return (
      '你是 ShannonBase Agent v4，内置于 ShannonBase 数据库的智能 SQL 助手。\n' +
      '当前数据库：' + db + '（SQL 中直接使用该库名，禁止任何占位符）\n\n' +
      current_time_info +
      get_workload_hint(qtype) + '\n\n' +
      (schema_ctx ? schema_ctx + '\n\n' : '') +
      (join_hint  ? join_hint  + '\n\n' : '') +
      (plan_hint  ? plan_hint  + '\n\n' : '') +
      '【可用工具】每次只输出一个合法 JSON，禁止在 JSON 前后添加任何文字：\n' +
      '1. {"thought":"...","tool":"query_db","args":{"sql":"SELECT ..."}}\n' +
      '   → 执行单条只读 SQL（SELECT/SHOW/DESC/EXPLAIN/WITH）\n' +
      '2. {"thought":"...","tool":"explain_sql","args":{"sql":"SELECT ..."}}\n' +
      '   → 分析执行计划，⚠ 结果中出现全表扫描时必须先改写 SQL\n' +
      '3. {"thought":"...","tool":"plan_sql","args":{"steps":[{"sql":"...","desc":"..."},...]}}\n' +
      '   → 【多步执行】一次提交有序 SQL 列表，引擎顺序执行并返回汇总结果\n' +
      '   → 适用：需要先 SHOW TABLES 再 SHOW CREATE、先探查结构再聚合等场景\n' +
      '4. {"thought":"...","tool":"begin_tx","args":{}}\n' +
      '5. {"thought":"...","tool":"update_data","args":{"sql":"INSERT/UPDATE/DELETE ..."}}\n' +
      '6. {"thought":"...","tool":"commit_tx","args":{}}\n' +
      '7. {"thought":"...","tool":"rollback_tx","args":{}}\n' +
      '8. {"thought":"...","tool":"ml_rag","args":{"question":"...","top_k":6}}\n' +
      '9. {"thought":"...","tool":"generate_text","args":{"prompt":"..."}}\n\n' +
      '【args 严格约束 - 违反视为错误】\n' +
      '  ① query_db / explain_sql / update_data：args.sql 必须是完整可执行 SQL，禁止为空或省略\n' +
      '  ② plan_sql：args.steps 必须是非空数组，每个元素含 sql 字段\n' +
      '  ③ 不确定 SQL 时：先用 query_db/plan_sql 探查 schema，再构造目标 SQL\n' +
      '  ④ 禁止输出 {"tool":"query_db","args":{}} 这类空 args\n\n' +
      '【关键约束】列名/表名必须来自上方 DDL；⛔ 禁用废弃系统表；' +
      'JOIN 优先使用 DDL 中的 FOREIGN KEY 子句；explain_sql 含⚠时先改写；' +
      '无需工具时直接输出自然语言；' +
      '⛔ 生成月份/日期序列时只能用单个派生表（如 SELECT 1 UNION ALL SELECT 2 ...），' +
      '禁止让一个派生表的子查询引用同级 FROM 子句中另一个派生表的别名/列' +
      '（MySQL 不支持，会报 Unknown table）；' +
      '⛔ 上方【相关表 DDL】中可能附带少量示例数据或检索片段，这些仅用于帮助理解表结构，' +
      '不代表该表实际的数据覆盖范围（时间区间、行数等）；遇到统计/汇总/聚合类问题时，' +
      '必须通过 query_db/plan_sql 执行真实的 SUM/COUNT/GROUP BY 等聚合 SQL 来得到结论，' +
      '禁止仅凭检索到的少量示例行就判断"数据不足""月份不全"而拒绝查询或直接给出免责声明。\n\n' +
      inline_few_shot + '\n\n' +
      (few_shot ? few_shot + '\n\n' : '') +
      '【历史对话】\n' + hist_section + '\n\n' +
      '【用户问题】\n' + A.user_message + '\n\n【助手】\n'
    );
  } else {
    return (
      'You are ShannonBase Agent v4, an intelligent SQL assistant embedded in ShannonBase.\n' +
      'Current database: ' + db + ' (use this name directly in SQL; no placeholders allowed)\n\n' +
      current_time_info +
      get_workload_hint(qtype) + '\n\n' +
      (schema_ctx ? schema_ctx + '\n\n' : '') +
      (join_hint  ? join_hint  + '\n\n' : '') +
      (plan_hint  ? plan_hint  + '\n\n' : '') +
      '[Available Tools] Output exactly one valid JSON per turn; no surrounding text:\n' +
      '1. {"thought":"...","tool":"query_db","args":{"sql":"SELECT ..."}}\n' +
      '   → Execute a single read-only SQL (SELECT/SHOW/DESC/EXPLAIN/WITH)\n' +
      '2. {"thought":"...","tool":"explain_sql","args":{"sql":"SELECT ..."}}\n' +
      '   → Analyze execution plan; ⚠ rewrite SQL if full table scan is detected\n' +
      '3. {"thought":"...","tool":"plan_sql","args":{"steps":[{"sql":"...","desc":"..."},...]}}\n' +
      '   → [Multi-step] Submit an ordered SQL list executed sequentially\n' +
      '   → Use for: SHOW TABLES then inspect columns, check schema then aggregate, etc.\n' +
      '4. {"thought":"...","tool":"begin_tx","args":{}}\n' +
      '5. {"thought":"...","tool":"update_data","args":{"sql":"INSERT/UPDATE/DELETE ..."}}\n' +
      '6. {"thought":"...","tool":"commit_tx","args":{}}\n' +
      '7. {"thought":"...","tool":"rollback_tx","args":{}}\n' +
      '8. {"thought":"...","tool":"ml_rag","args":{"question":"...","top_k":6}}\n' +
      '9. {"thought":"...","tool":"generate_text","args":{"prompt":"..."}}\n\n' +
      '[args Strict Constraints – violations are errors]\n' +
      '  ① query_db / explain_sql / update_data: args.sql must be a complete executable SQL; cannot be empty\n' +
      '  ② plan_sql: args.steps must be a non-empty array; each element must have a sql field\n' +
      '  ③ When SQL is uncertain: use query_db/plan_sql to explore schema first\n' +
      '  ④ Forbidden: {"tool":"query_db","args":{}} style empty args\n\n' +
      '[Key Constraints] Column/table names must come from the DDL above; ⛔ no deprecated system tables; ' +
      'prefer FOREIGN KEY clauses from DDL for JOINs; rewrite SQL when explain_sql shows ⚠; ' +
      'output natural language directly when no tool is needed; ' +
      '⛔ When generating a month/date sequence, use only a single derived table ' +
      '(e.g. SELECT 1 UNION ALL SELECT 2 ...) — never let one derived table\'s subquery ' +
      'reference another sibling derived table\'s alias/column in the same FROM clause ' +
      '(MySQL does not support this and will raise "Unknown table"); ' +
      '⛔ The [Relevant Table DDL] section above may include a few sample rows or retrieval ' +
      'snippets meant only to illustrate table structure — they do NOT represent the table\'s ' +
      'actual data coverage (date range, row count, etc.); for any statistical/aggregation ' +
      'question you MUST run a real SUM/COUNT/GROUP BY query via query_db/plan_sql to get the ' +
      'answer, never conclude "insufficient data" or "incomplete months" just from a handful of ' +
      'retrieved sample rows and refuse to query.\n\n' +
      inline_few_shot + '\n\n' +
      (few_shot ? few_shot + '\n\n' : '') +
      '[Conversation History]\n' + hist_section + '\n\n' +
      '[User Question]\n' + A.user_message + '\n\n[Assistant]\n'
    );
  }
}

function final_summary(system_prompt_base, tool_log) {
  return ml_generate(
    system_prompt_base +
    t('\n\n【已执行工具及结果】\n', '\n\n[Tool Execution Results]\n') +
    compress(tool_log, cfg('plan_log_max_tokens', 4000)) +
    t('\n\n请根据以上工具结果用清晰专业的中文直接回答用户问题。' +
      '禁止输出 JSON，禁止逐行复述原始数据，只输出结论和分析；' +
      '⛔ 表名/列名必须与工具结果中出现的原始名称完全一致，禁止编造未出现过的字段名：\n',
      '\n\nBased on the above results, answer the user\'s question clearly and professionally. ' +
      'Do not output JSON, do not repeat raw data row by row; output only conclusions and analysis. ' +
      '⛔ Table/column names must exactly match what appeared in the tool results — never fabricate ' +
      'a field name that did not actually appear:\n'),
    { temperature: 0.3, max_tokens: cfg('summary_max_tokens', 2000) }
  );
}