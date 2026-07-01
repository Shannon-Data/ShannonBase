//@include lib_router.js

var TABLE_FALLBACK = {
  'INNODB_LOCKS':       'performance_schema.data_locks',
  'INNODB_LOCK_WAITS':  'performance_schema.data_lock_waits',
  'USER_STATISTICS':    'performance_schema.accounts',
  'INDEX_STATISTICS':   'information_schema.STATISTICS',
  'QUERY_CACHE_INFO':   'performance_schema.events_statements_summary_by_digest',
  'GLOBAL_STATUS':      'performance_schema.global_status',
  'GLOBAL_VARIABLES':   'performance_schema.global_variables'
};

function try_recover_unknown_table(result, original_sql) {
  if (!result || result.indexOf('Unknown table') === -1) return null;
  var m = result.match(/Unknown table\s+'?(?:[\w]+\.)?([\w]+)'?/i);
  if (!m) return null;
  var bad = m[1].toUpperCase(), replacement = TABLE_FALLBACK[bad];
  if (!replacement) return null;
  var new_sql = original_sql.replace(
    new RegExp('(?:information_schema\\.|performance_schema\\.)?' + bad, 'gi'), replacement
  );
  return { sql: new_sql,
           desc: t('⚡自动恢复：', '⚡Auto-recovered: ') + bad + ' → ' + replacement };
}

function estimate_complexity(text) {
  var signals = [
    /\bGROUP\s+BY\b|按.{1,8}分组/i,
    /\bJOIN\b|多表.*关联|关联.*多表/i,
    /子查询|\bIN\s*\(\s*SELECT/i,
    /同比|环比|趋势|占比|排名.*前\s*\d|TOP\s*\d/i,
    /先.*再.*(?:然后|最后)|第一步.*第二步/,
    /\bUNION\b|\bWITH\b.*\bAS\b/i
  ];
  var score = 0;
  for (var i = 0; i < signals.length; i++) if (signals[i].test(text)) score++;
  return score;
}

function decompose_query(text) {
  if (estimate_complexity(text) < 2) return [{ op: 'scan', note: text }];
  var tasks = [{ op: 'scan', note: t('主表扫描', 'main table scan') }];
  if (/最近|过去\s*\d+|大于|小于|等于|筛选|过滤|WHERE|filter|recent|last\s+\d/i.test(text))
    tasks.push({ op:'filter', note: t('条件过滤', 'filter condition') });
  if (/关联|联合|join/i.test(text))
    tasks.push({ op:'join', note: t('多表关联', 'multi-table join') });
  if (/聚合|汇总|统计|SUM|AVG|COUNT|aggregate|average/i.test(text))
    tasks.push({ op:'agg', note: t('聚合计算', 'aggregation') });
  if (/排名|排序|最高|最低|前\s*\d+|rank|sort|top\s*\d/i.test(text))
    tasks.push({ op:'sort', note: t('排序限行', 'sort + limit') });
  tasks.push({ op: 'explain', note: t('执行计划验证', 'explain plan check') });
  return tasks;
}

function logical_plan_to_hint(tasks, schema_from_embeddings) {
  if (!tasks || tasks.length <= 1) return '';
  var op_hints = {
    scan:    t('确定主表，必要时先用 query_db 检查表结构',
               'Identify main table; use query_db to inspect schema if needed'),
    filter:  t('构造 WHERE 条件，确认谓词列有索引',
               'Build WHERE clause; confirm predicate columns are indexed'),
    join:    schema_from_embeddings
             ? t('按上方 DDL 中的 FOREIGN KEY 子句拼写 JOIN 条件',
                 'Use FOREIGN KEY clauses from the DDL above to write JOIN conditions')
             : t('按【推荐JOIN路径】拼写 JOIN 子句',
                 'Use [Recommended JOIN paths] to write JOIN clauses'),
    agg:     t('构造 GROUP BY + 聚合函数，注意 HAVING 与 WHERE 顺序',
               'Build GROUP BY + aggregate functions; note HAVING vs WHERE ordering'),
    sort:    t('添加 ORDER BY；LIMIT 避免大结果集',
               'Add ORDER BY; use LIMIT to avoid large result sets'),
    explain: t('调用 explain_sql 验证；⚠全表扫描时先改写 SQL',
               'Call explain_sql to verify; ⚠ rewrite SQL if full table scan detected')
  };
  var lines = [t('【逻辑计划（按序执行，不得跳步）】',
                 '[Logical Plan (execute in order, no skipping)]')];
  for (var i = 0; i < tasks.length; i++)
    lines.push('Step' + (i+1) + ' [' + tasks[i].op.toUpperCase() + ']: ' +
               (op_hints[tasks[i].op] || tasks[i].op) + '  ← ' + tasks[i].note);
  return lines.join('\n');
}

function rule_planner(msg, db) {

  if (/有哪些(数据库|schema)|所有数据库|列出.*数据库|show.?databases|show.?schemas|list.*databases/i.test(msg)) {
    return [
      { sql: "SELECT SCHEMA_NAME AS database_name" +
            " FROM information_schema.SCHEMATA" +
            " ORDER BY SCHEMA_NAME",
        desc: t('当前实例所有数据库', 'All databases in current instance') }
    ];
  }

  if (/有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables/i.test(msg) &&
      !/结构|schema|column|字段|列|create|structure|definition/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS,TABLE_COMMENT" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY TABLE_NAME",
        desc: t('获取所有表概览', 'Get all tables overview') }
    ];
  }

  if (/(表.*结构|结构.*表|字段|列信息|column.*info|table.*structure|describe.*table)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
             "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
             "  TABLE_COMMENT" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY TABLE_NAME",
        desc: t('所有表概览（行数 / 大小 / 注释）',
                'All tables overview (rows / size / comment)') },
        { sql: "SELECT TABLE_NAME," +
              "  GROUP_CONCAT(" +
              "    CONCAT(COLUMN_NAME,' ',COLUMN_TYPE," +
              "      IF(COLUMN_KEY<>'',CONCAT(' ',COLUMN_KEY),'')," +
              "      IF(IS_NULLABLE='NO',' NOT NULL','')" +
              "    ) ORDER BY ORDINAL_POSITION SEPARATOR ', '" +
              "  ) AS columns" +
              " FROM information_schema.COLUMNS" +
              " WHERE TABLE_SCHEMA=DATABASE()" +
              " GROUP BY TABLE_NAME" +
              " ORDER BY TABLE_NAME",
          desc: t('所有表的列定义（每表一行紧凑格式）',
                  'Column definitions for all tables (one compact row per table)') },
       { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
              " FROM information_schema.KEY_COLUMN_USAGE" +
              " WHERE CONSTRAINT_SCHEMA=DATABASE()" +
              "   AND REFERENCED_TABLE_NAME IS NOT NULL",
         desc: t('外键关系（JOIN 路径）', 'Foreign key relationships (JOIN paths)') }
    ];
  }

  if (/(外键|foreign.?key|join.*关系|关联关系|table.*relation|related.*table)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME" +
             " FROM information_schema.KEY_COLUMN_USAGE" +
             " WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL",
        desc: t('所有外键关系（JOIN 路径）', 'All foreign key relationships (JOIN paths)') }
    ];
  }

  if (/(索引|index.*分析|缺少.*索引|覆盖.*索引|index.*analysis|missing.*index|covering.*index)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX," +
             "COLUMN_NAME,CARDINALITY,INDEX_TYPE" +
             " FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()" +
             " ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX",
        desc: t('当前库所有索引详情', 'All index details for current database') },
      { sql: "SELECT TABLE_NAME,COLUMN_NAME,DATA_TYPE" +
             " FROM information_schema.COLUMNS" +
             " WHERE TABLE_SCHEMA=DATABASE()" +
             "   AND COLUMN_NAME NOT IN (" +
             "     SELECT DISTINCT COLUMN_NAME FROM information_schema.STATISTICS" +
             "     WHERE TABLE_SCHEMA=DATABASE())" +
             " ORDER BY TABLE_NAME,ORDINAL_POSITION",
        desc: t('未被索引覆盖的列（候选索引）', 'Unindexed columns (index candidates)') }
    ];
  }

  if (/(数据量|行数|record.?count|row.?count|表.*大小|数据大小|table.*size)/i.test(msg)) {
    return [
      { sql: "SELECT TABLE_NAME,TABLE_ROWS," +
             "  ROUND((DATA_LENGTH+INDEX_LENGTH)/1024/1024,2) AS size_mb," +
             "  DATA_FREE" +
             " FROM information_schema.TABLES" +
             " WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'" +
             " ORDER BY DATA_LENGTH+INDEX_LENGTH DESC",
        desc: t('各表数据量与磁盘占用', 'Table row counts and disk usage') }
    ];
  }

  if (/(性能.*top|top.*性能|最慢.*查询|query.*perf|slow.*queries|performance.*top)/i.test(msg)) {
    return [
      { sql: "SELECT DIGEST_TEXT,COUNT_STAR," +
             "  ROUND(AVG_TIMER_WAIT/1e12,3) AS avg_sec," +
             "  ROUND(MAX_TIMER_WAIT/1e12,3) AS max_sec," +
             "  SUM_ROWS_EXAMINED,SUM_ROWS_SENT" +
             " FROM performance_schema.events_statements_summary_by_digest" +
             " ORDER BY SUM_TIMER_WAIT DESC LIMIT 20",
        desc: t('SQL 性能 TOP20', 'SQL performance TOP20') }
    ];
  }

  return null;
}

var MAX_PLAN_STEPS = 15;

function execute_plan(steps, db) {
  var results = [];
  var last_result_text = '';
  var per_step_limit = cfg('plan_step_max_tokens', 4000);

  for (var i = 0; i < Math.min(steps.length, MAX_PLAN_STEPS); i++) {
    var step = steps[i];
    var sql  = replace_ph(String(step.sql || ''), db);
    if (sql.indexOf('__LAST_RESULT__') !== -1)
      sql = sql.replace(/__LAST_RESULT__/g, esc(last_result_text.substring(0, 200)));

    var upper = sql.trim().toUpperCase().split(/\s+/)[0];
    var RO = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
    var result_text;
    if (RO[upper]) {
      var raw = query(sql);
      result_text = compress(rows_to_text(raw), per_step_limit);
      if (result_text.indexOf('Unknown table') !== -1) {
        var recovery = try_recover_unknown_table(result_text, sql);
        if (recovery)
          result_text = recovery.desc + '\n' + compress(rows_to_text(query(recovery.sql)), per_step_limit);
      }
    } else {
      result_text = t('跳过非只读 SQL（plan_sql 中仅执行 SELECT/SHOW/DESC）：',
                      'Skipped non-read-only SQL: ') + sql.substring(0, 80);
    }
    last_result_text = result_text;
    results.push({ step: i + 1, desc: step.desc || ('Step ' + (i+1)),
                  sql: sql, result: result_text });
  }

  for (var ri = 0; ri < results.length; ri++) {
    log_sql_trace(A.conversation_id, 0, results[ri].step, 'plan_sql',
                  'plan_sql', results[ri].sql, results[ri].desc, results[ri].result);
  }

  return results;
}

function validate_tool_call(tool_obj) {
  if (!tool_obj || typeof tool_obj.tool !== 'string')
    return t('工具调用格式错误：缺少 tool 字段',
             'Tool call format error: missing "tool" field');
  var args = tool_obj.args || {};
  switch (tool_obj.tool) {
    case 'query_db':
      if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 3)
        return t(
          'query_db 缺少有效 sql 参数。请提供完整的 SQL 语句，例如：SHOW TABLES 或 SELECT ...',
          'query_db missing valid sql argument. Provide a complete SQL, e.g.: SHOW TABLES or SELECT ...'
        );
      break;
    case 'explain_sql':
      if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
        return t('explain_sql 缺少有效 sql 参数', 'explain_sql missing valid sql argument');
      break;
    case 'update_data':
      if (!args.sql || typeof args.sql !== 'string' || args.sql.trim().length < 5)
        return t('update_data 缺少有效 sql 参数', 'update_data missing valid sql argument');
      break;
    case 'plan_sql':
      if (!Array.isArray(args.steps) || args.steps.length === 0)
        return t('plan_sql 缺少 steps 数组，或 steps 为空',
                 'plan_sql missing steps array or steps is empty');
      for (var vi = 0; vi < args.steps.length; vi++) {
        if (!args.steps[vi].sql || typeof args.steps[vi].sql !== 'string')
          return t('plan_sql steps[', 'plan_sql steps[') + vi +
                 t('] 缺少 sql 字段', '] missing sql field');
      }
      break;
    case 'ml_rag':
      if (!args.question || typeof args.question !== 'string')
        return t('ml_rag 缺少 question 参数', 'ml_rag missing question argument');
      break;
    case 'generate_text':
      if (!args.prompt || typeof args.prompt !== 'string')
        return t('generate_text 缺少 prompt 参数', 'generate_text missing prompt argument');
      break;
    case 'begin_tx': case 'commit_tx': case 'rollback_tx':
      break;
    default:
      return t('未知工具：', 'Unknown tool: ') + tool_obj.tool;
  }
  return null;
}

function replace_ph(sql, db) {
  if (!db) return sql;
  return sql.replace(
    /your_database_name|your_db_name|<database_name>|\[database_name\]|\{database_name\}|your_schema/gi,
    db
  );
}

function parse_explain(json_str) {
  try {
    var plan = JSON.parse(json_str), out = [];
    function walk(node) {
      if (!node || typeof node !== 'object') return;
      if (node.table_name)             out.push(t('表=', 'table=')   + node.table_name);
      if (node.access_type) {
        out.push(t('访问=', 'access=') + node.access_type);
        if (node.access_type === 'ALL') out.push(t('⚠全表扫描', '⚠full table scan'));
      }
      if (node.rows_examined_per_scan) out.push(t('扫描行≈', 'rows≈') + node.rows_examined_per_scan);
      if (node.using_filesort)  out.push(t('⚠需filesort', '⚠filesort required'));
      if (node.using_temporary) out.push(t('⚠临时表',     '⚠temp table'));
      if (node.key) out.push(t('索引=', 'key=') + node.key);
      ['nested_loop','attached_subqueries','query_block',
       'ordering_operation','grouping_operation'].forEach(function(k) {
        if (!node[k]) return;
        if (Array.isArray(node[k])) node[k].forEach(function(c){ walk(c.table || c); });
        else walk(node[k]);
      });
    }
    walk(plan.query_block || plan);
    return out.length ? out.join(' ') : json_str.substring(0, 150);
  } catch(e) { return json_str.substring(0, 150); }
}

function execute_tool(tool, args, db) {
  var sql, upper, first;

  if (tool === 'query_db') {
    sql = replace_ph(String(args.sql || ''), db);
    first = sql.trim().toUpperCase().split(/\s+/)[0];
    var RO2 = {SELECT:1,SHOW:1,DESCRIBE:1,DESC:1,EXPLAIN:1,WITH:1};
    if (!RO2[first])
      return t('拒绝：query_db 只允许只读语句（SELECT/SHOW/DESC/EXPLAIN/WITH）。',
               'Rejected: query_db only allows read-only statements (SELECT/SHOW/DESC/EXPLAIN/WITH).');
    var result = compress(rows_to_text(query(sql)), 900);
    if (result.indexOf('Unknown table') !== -1) {
      var recovery2 = try_recover_unknown_table(result, sql);
      if (recovery2)
        return recovery2.desc + '\n' + compress(rows_to_text(query(recovery2.sql)), 900);
    }
    return result;
  }

  if (tool === 'explain_sql') {
    sql = replace_ph(String(args.sql || ''), db);
    var ex = query("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !ex.length) return t('EXPLAIN 执行失败', 'EXPLAIN execution failed');
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'] || JSON.stringify(ex[0]);
    return t('执行计划：', 'Execution plan: ') + parse_explain(String(raw));
  }

  if (tool === 'plan_sql') {
    var steps2      = args.steps || [];
    var plan_res    = execute_plan(steps2, db);
    var out_lines   = [t('【plan_sql 多步执行结果】', '[plan_sql multi-step results]')];
    for (var pi = 0; pi < plan_res.length; pi++) {
      var pr = plan_res[pi];
      out_lines.push('Step ' + pr.step + ': ' + pr.desc + ' ');
      out_lines.push('SQL: ' + pr.sql);
      out_lines.push(pr.result);
    }
    return compress(out_lines.join('\n'), cfg('plan_log_max_tokens', 4000));
  }

  if (tool === 'begin_tx') {
    if (A.tx_active)
      return t('警告：事务已活跃，禁止重复 begin_tx。',
               'Warning: transaction already active; duplicate begin_tx forbidden.');
    sys.exec_sql('START TRANSACTION');
    A.tx_active = true;
    return t('事务已开启', 'Transaction started');
  }

  if (tool === 'update_data') {
    if (!A.tx_active)
      return t('拒绝：写操作必须在事务内，请先 begin_tx。',
               'Rejected: write operations must be inside a transaction; call begin_tx first.');
    sql   = replace_ph(String(args.sql || ''), db);
    upper = sql.trim().toUpperCase();
    first = upper.split(/\s+/)[0];
    if ((first==='UPDATE'||first==='DELETE') && !/\bWHERE\b/.test(upper))
      return t('拒绝：', 'Rejected: ') + first +
             t(' 必须含 WHERE 条件。', ' must include a WHERE clause.');
    sys.exec_sql(sql);
    return t('执行成功', 'Success');
  }

  if (tool === 'commit_tx') {
    if (!A.tx_active)
      return t('警告：当前无活跃事务。', 'Warning: no active transaction.');
    sys.exec_sql('COMMIT');
    A.tx_active = false;
    return t('事务已提交', 'Transaction committed');
  }

  if (tool === 'rollback_tx') {
    if (A.tx_active) { sys.exec_sql('ROLLBACK'); A.tx_active = false; }
    return t('事务已回滚', 'Transaction rolled back');
  }

  if (tool === 'ml_rag') {
    var rag_opt_for_tool = get_rag_options(get_chat_options());
    return compress(
      ml_rag(String(args.question || A.user_message), args.top_k || rag_opt_for_tool.n_citations || 6, rag_opt_for_tool),
      800
    );
  }
  if (tool === 'generate_text')
    return ml_generate(String(args.prompt || ''), args.options || {});

  return t('错误：未知工具 "', 'Error: unknown tool "') + tool + '"';
}

function parse_tool_call(raw) {
  if (!raw) return null;
  var s = raw.replace(/```json|```/g, '').trim();
  var depth = 0, start = -1, end = -1;
  for (var i = 0; i < s.length; i++) {
    if      (s[i]==='{') { if (depth++ === 0) start = i; }
    else if (s[i]==='}') { if (--depth === 0) { end = i; break; } }
  }
  if (start === -1 || end === -1) return null;
  try {
    var obj = JSON.parse(s.substring(start, end + 1));
    return (obj && typeof obj.tool === 'string') ? obj : null;
  } catch(e) { return null; }
}