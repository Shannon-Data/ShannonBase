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

/* Transaction session helpers: track active transactions per conversation/session
 * Use A._tx_sessions keyed by A.conversation_id to avoid global cross-session flags. */
function tx_active_for(convo) {
  A._tx_sessions = A._tx_sessions || {};
  var k = String(convo || A.conversation_id || 'global');
  return !!A._tx_sessions[k];
}
function set_tx_active_for(convo, val) {
  A._tx_sessions = A._tx_sessions || {};
  var k = String(convo || A.conversation_id || 'global');
  if (val) A._tx_sessions[k] = true;
  else delete A._tx_sessions[k];
}

/* Query the real MySQL/InnoDB session state to determine whether a
 * transaction is currently active on this connection.  Unlike the JS-side
 * _tx_sessions mirror (which is always blank at the start of a fresh
 * shannon_agent_run() call), this survives across invocations.
 *
 * Uses mysql.agent_tx_lease as the authoritative source — each begin_tx
 * writes a row there and each commit_tx/rollback_tx deletes it.  This
 * replaces the former INNODB_TRX-based check whose semantics were
 * engine-dependent (START TRANSACTION without touching an InnoDB table
 * might not immediately appear in INNODB_TRX). */
function is_real_tx_active() {
  try {
    var rows = query(
      "SELECT 1 FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(A.conversation_id) + "' " +
      "AND session_conn_id = CONNECTION_ID() LIMIT 1"
    );
    return Array.isArray(rows) && rows.length > 0;
  } catch (e) { return false; }
}

function begin_tx_lease(conv_id, plan_id, timeout_minutes) {
  timeout_minutes = timeout_minutes || 30;

  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_tx_lease(conversation_id, session_conn_id, plan_id, expires_at) " +
      "VALUES ('" + esc(conv_id) + "', CONNECTION_ID(), '" + esc(plan_id || '') + "', " +
      "DATE_ADD(NOW(), INTERVAL " + timeout_minutes + " MINUTE)) " +
      "ON DUPLICATE KEY UPDATE " +
      "session_conn_id = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(session_conn_id), session_conn_id), " +
      "plan_id         = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(plan_id), plan_id), " +
      "created_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), NOW(), created_at), " +
      "expires_at      = IF(expires_at < NOW() OR session_conn_id = CONNECTION_ID(), VALUES(expires_at), expires_at)"
    );
  } catch (e) {}

  /* Confirm ownership: the row must exist and belong to THIS connection. */
  try {
    var owned = query(
      "SELECT 1 FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(conv_id) + "' " +
      "AND session_conn_id = CONNECTION_ID() LIMIT 1"
    );
    return Array.isArray(owned) && owned.length > 0;
  } catch (e) { return false; }
}

/** Remove the lease row — only for THIS session (CONNECTION_ID).  A
 *  different session's lease for the same conversation_id must never be
 *  accidentally deleted by a commit/rollback on this connection. */
function clear_tx_lease(conv_id) {
  try {
    sys.exec_sql(
      "DELETE FROM mysql.agent_tx_lease " +
      "WHERE conversation_id='" + esc(conv_id) + "' " +
      "AND session_conn_id = CONNECTION_ID()"
    );
  } catch (e) {}
}

/** At the top of shannon_agent_run, clean up any lease that has exceeded its
 *  timeout.  Returns an array of cleaned-up conversation_ids (for logging). */
function cleanup_expired_tx_leases() {
  try {
    var expired = query(
      "SELECT conversation_id, plan_id, TIMESTAMPDIFF(SECOND, created_at, NOW()) AS age_sec " +
      "FROM mysql.agent_tx_lease WHERE expires_at < NOW() AND session_conn_id = CONNECTION_ID()"
    );
    if (!Array.isArray(expired) || !expired.length) return [];
    for (var i = 0; i < expired.length; i++) {
      try { sys.exec_sql('ROLLBACK'); } catch (e) {}
      clear_tx_lease(expired[i].conversation_id);
      log_rollback_event(expired[i].conversation_id, expired[i].plan_id, 0, '',
                         'expired', t('超时自动回滚：', 'Timeout auto-rollback: ') + expired[i].age_sec + 's');
    }
    return expired;
  } catch (e) { return []; }
}

function finalize_tx_safety_net() {
  if (!tx_active_for(A.conversation_id) && !is_real_tx_active()) return '';
  try {
    var pending = query(
      "SELECT 1 FROM mysql.agent_review_plan " +
      "WHERE conversation_id='" + esc(A.conversation_id) + "' " +
      "AND status='awaiting_approval' LIMIT 1"
    );
    if (Array.isArray(pending) && pending.length > 0) return '';
  } catch (e) {}

  try { sys.exec_sql('ROLLBACK'); } catch (e) {}
  set_tx_active_for(A.conversation_id, false);
  clear_tx_lease(A.conversation_id);
  return '\n' + t('[安全网] 未提交事务已强制回滚。',
                  '[Safety net] Uncommitted transaction force-rolled back.');
}

function classify_statement(sql) {
  var s = String(sql || '');

  var stripped = s, prev;
  do {
    prev = stripped;
    stripped = stripped
      .replace(/^\s*\/\*[\s\S]*?\*\//, '')
      .replace(/^\s*--[^\n]*(\n|$)/, '')
      .replace(/^\s*#[^\n]*(\n|$)/, '');
  } while (stripped !== prev);
  stripped = stripped.trim();

  var upper = stripped.toUpperCase();
  var first = upper.split(/\s+/)[0] || '';

  var _READ  = ['SELECT','SHOW','DESCRIBE','DESC','EXPLAIN','WITH'];
  var _WRITE = ['INSERT','UPDATE','DELETE','REPLACE'];
  var _DDL   = ['CREATE','ALTER','DROP','TRUNCATE','RENAME'];
  var _TCL   = ['BEGIN','START','COMMIT','ROLLBACK','SAVEPOINT'];

  var is_read  = _READ.indexOf(first)  !== -1;
  var is_write = _WRITE.indexOf(first) !== -1;
  var is_ddl   = _DDL.indexOf(first)   !== -1;
  var is_tcl   = _TCL.indexOf(first)   !== -1;

  /* CTE (WITH) that wraps a DML — classify as write, not read */
  if (first === 'WITH' && /\b(INSERT|UPDATE|DELETE|REPLACE)\b/i.test(upper)) {
    is_read  = false;
    is_write = true;
  }

  /* Risk classification */
  var risk = 'low';
  if (is_ddl) {
    risk = 'high';
  } else if (is_write) {
    risk = (first === 'DELETE' && !/\bWHERE\b/.test(upper)) ? 'high' : 'medium';
  }

  var kind;
  if (is_read)              kind = first;  /* SELECT, SHOW, DESCRIBE, ... */
  else if (is_write)        kind = 'DML';
  else if (is_ddl)          kind = 'DDL';
  else if (is_tcl)          kind = 'TCL';
  else if (first === 'SET') kind = 'SET';
  else                      kind = 'OTHER';

  return {
    kind:          kind,
    first_keyword: first,
    is_read:       is_read,
    is_write:      is_write,
    is_ddl:        is_ddl,
    is_tcl:        is_tcl,
    risk:          risk
  };
}

function try_recover_unknown_table(result, original_sql) {
  if (!result || result.indexOf('Unknown table') === -1) return null;
  var m = result.match(/Unknown table\s+'?(?:[\w]+\.)?([\w]+)'?/i);
  if (!m) return null;
  var bad = m[1].toUpperCase(), replacement = TABLE_FALLBACK[bad];
  if (!replacement) return null;
  var pattern = new RegExp('((?:FROM|JOIN)\\s+)(?:information_schema\\.|performance_schema\\.)?' + bad + '\\b', 'gi');
  var new_sql = original_sql.replace(pattern, function(match, prefix) {
    return prefix + replacement;
  });
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
      !/结构|schema|column|字段信息|列信息|列名|create|structure|definition|利润表|现金流量表|资产负债表|报表/i.test(msg)) {
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

    var stmt = classify_statement(sql);
    var result_text;
    if (stmt.is_read) {
      var raw = query(sql);
      result_text = compress(rows_to_text(raw), per_step_limit);
      if (result_text.indexOf('Unknown table') !== -1) {
        var recovery = try_recover_unknown_table(result_text, sql);
        if (recovery)
          result_text = recovery.desc + '\n' + compress(rows_to_text(query(recovery.sql)), per_step_limit);
      }
    } else {
      result_text = t('跳过非只读 SQL（plan_sql 中仅执行 SELECT/SHOW/DESC/EXPLAIN/WITH）：',
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

function validate_result(result, sql) {
  if (!result || String(result).trim().length === 0) return 'empty_result';
  var s = String(result);
  if (/unknown table/i.test(s)) return 'unknown_table';
  if (/unknown column/i.test(s)) return 'unknown_column';
  if (/syntax error/i.test(s)) return 'syntax_error';
  if (/denied/i.test(s) || /access denied/i.test(s)) return 'access_denied';
  return null;
}

function validate_tool_call(tool_obj, policy) {
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
      for (var wi = 0; wi < args.steps.length; wi++) {
        var step_stmt = classify_statement(String(args.steps[wi].sql || ''));
        if (!step_stmt.is_read) {
          return t(
            'plan_sql steps[' + wi + '] 是写操作/DDL（plan_sql 只能包含只读 SQL：SELECT/SHOW/DESC/EXPLAIN/WITH）。' +
            '请把只读探查步骤留在 plan_sql 里，写操作单独用 update_data 提交，以便进入审批流程。',
            'plan_sql steps[' + wi + '] is a write/DDL statement (plan_sql may only contain read-only SQL). ' +
            'Keep read-only steps in plan_sql and submit the write separately via update_data so it goes through the approval workflow.'
          );
        }
      }
      /* Cap how many steps a single plan_sql call may queue for individual
       * step-by-step approval under review_mode. Each step becomes its own
       * awaiting_approval round trip (see build_review_steps); without a
       * cap the model could submit dozens of steps in one shot with no
       * natural checkpoint for the user to reconsider mid-stream. Only
       * relevant under review_mode — the non-review execute_plan() path is
       * independently capped by MAX_PLAN_STEPS and never executes writes. */
      if (policy && policy.review_mode === 'review') {
        var max_steps = policy.max_pending_steps || 3;
        if (args.steps.length > max_steps)
          return t('plan_sql 提交的步骤数（', 'plan_sql submitted ') + args.steps.length +
                 t('）超过审批模式下的单批上限（', ' steps, exceeding the review-mode per-batch limit (') +
                 max_steps + t('）。请拆分为多次更小的 plan_sql 调用。', '). Split into smaller plan_sql calls.');
      }
      break;
    case 'list_tables':
      break; // keyword/top_k are both optional
    case 'describe_table':
      if ((!args.table_name || typeof args.table_name !== 'string') &&
          !(Array.isArray(args.table_names) && args.table_names.length > 0))
        return t('describe_table 缺少 table_name（或 table_names 数组）参数',
                 'describe_table missing table_name (or table_names array) argument');
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
    case 'ml_train':
      if (!args.table_name || typeof args.table_name !== 'string')
        return t('ml_train 缺少 table_name 参数（schema.table 格式）',
                 'ml_train missing table_name argument (schema.table format)');
      if (!args.target_column || typeof args.target_column !== 'string')
        return t('ml_train 缺少 target_column 参数',
                 'ml_train missing target_column argument');
      break;
    case 'ml_predict_row':
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_predict_row 缺少 model_handle 参数',
                 'ml_predict_row missing model_handle argument');
      if (!args.data || typeof args.data !== 'object')
        return t('ml_predict_row 缺少 data 参数（JSON 对象，如 {"col1":val1,"col2":val2}）',
                 'ml_predict_row missing data argument (JSON object, e.g. {"col1":val1,"col2":val2})');
      break;
    case 'ml_predict_table':
      if (!args.table_name || typeof args.table_name !== 'string')
        return t('ml_predict_table 缺少 table_name 参数（schema.table 格式）',
                 'ml_predict_table missing table_name argument (schema.table format)');
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_predict_table 缺少 model_handle 参数',
                 'ml_predict_table missing model_handle argument');
      break;
    case 'ml_explain':
      if (!args.table_name || typeof args.table_name !== 'string')
        return t('ml_explain 缺少 table_name 参数（schema.table 格式）',
                 'ml_explain missing table_name argument (schema.table format)');
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_explain 缺少 model_handle 参数',
                 'ml_explain missing model_handle argument');
      break;
    case 'ml_explain_row':
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_explain_row 缺少 model_handle 参数',
                 'ml_explain_row missing model_handle argument');
      if (!args.data || typeof args.data !== 'object')
        return t('ml_explain_row 缺少 data 参数（JSON 对象）',
                 'ml_explain_row missing data argument (JSON object)');
      break;
    case 'ml_explain_table':
      if (!args.table_name || typeof args.table_name !== 'string')
        return t('ml_explain_table 缺少 table_name 参数（schema.table 格式）',
                 'ml_explain_table missing table_name argument (schema.table format)');
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_explain_table 缺少 model_handle 参数',
                 'ml_explain_table missing model_handle argument');
      break;
    case 'ml_score':
      if (!args.table_name || typeof args.table_name !== 'string')
        return t('ml_score 缺少 table_name 参数（schema.table 格式）',
                 'ml_score missing table_name argument (schema.table format)');
      if (!args.target_column || typeof args.target_column !== 'string')
        return t('ml_score 缺少 target_column 参数',
                 'ml_score missing target_column argument');
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_score 缺少 model_handle 参数',
                 'ml_score missing model_handle argument');
      break;
    case 'ml_model_export':
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_model_export 缺少 model_handle 参数',
                 'ml_model_export missing model_handle argument');
      break;
    case 'ml_model_import':
      if (!args.model_handle || typeof args.model_handle !== 'string')
        return t('ml_model_import 缺少 model_handle 参数',
                 'ml_model_import missing model_handle argument');
      break;
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

/**
 * Shared EXPLAIN FORMAT=JSON tree walker. MySQL's JSON explain plan nests
 * table/access-method nodes under a handful of well-known keys depending on
 * whether the plan involves joins, subqueries, or grouping/ordering. Both
 * parse_explain (human-readable text) and sum_explain_rows (numeric
 * estimate) need to visit the same nodes, so the traversal lives here once.
 */
function walk_explain_tree(node, visitor) {
  if (!node || typeof node !== 'object') return;
  visitor(node);
  ['nested_loop','attached_subqueries','query_block',
   'ordering_operation','grouping_operation'].forEach(function(k) {
    if (!node[k]) return;
    if (Array.isArray(node[k])) node[k].forEach(function(c){ walk_explain_tree(c.table || c, visitor); });
    else walk_explain_tree(node[k], visitor);
  });
}

function parse_explain(json_str) {
  try {
    var plan = JSON.parse(json_str), out = [];
    walk_explain_tree(plan.query_block || plan, function(node) {
      if (node.table_name)             out.push(t('表=', 'table=')   + node.table_name);
      if (node.access_type) {
        out.push(t('访问=', 'access=') + node.access_type);
        if (node.access_type === 'ALL') out.push(t('⚠全表扫描', '⚠full table scan'));
      }
      if (node.rows_examined_per_scan) out.push(t('扫描行≈', 'rows≈') + node.rows_examined_per_scan);
      if (node.using_filesort)  out.push(t('⚠需filesort', '⚠filesort required'));
      if (node.using_temporary) out.push(t('⚠临时表',     '⚠temp table'));
      if (node.key) out.push(t('索引=', 'key=') + node.key);
    });
    return out.length ? out.join(' ') : json_str.substring(0, 150);
  } catch(e) { return json_str.substring(0, 150); }
}

/**
 * Numeric row estimate extracted from the same EXPLAIN JSON tree.
 *
 * For a single-table UPDATE/DELETE (the common review-gated case) there is
 * exactly one table node, so this degenerates to that table's
 * rows_examined_per_scan — exactly "how many rows will this WHERE clause
 * touch" that the review preview was missing. For multi-table plans (JOINs)
 * we report the largest single-table scan rather than summing across
 * tables, since summing overstates the actual result cardinality and would
 * mislead more than 'unknown' did.
 */
function sum_explain_rows(json_str) {
  try {
    var plan = JSON.parse(json_str);
    var found = false, max_single = 0;
    walk_explain_tree(plan.query_block || plan, function(node) {
      if (node.rows_examined_per_scan) {
        var n = Number(node.rows_examined_per_scan) || 0;
        if (n > max_single) max_single = n;
        found = true;
      }
    });
    return found ? max_single : null;
  } catch (e) { return null; }
}

/**
 * EXPLAIN-based best-effort row estimate for a write statement. This is an
 * optimizer estimate (same statistics EXPLAIN always uses), not an exact
 * count — labelled as such wherever it's displayed. Returns null if EXPLAIN
 * fails or the statement shape doesn't produce a usable estimate (e.g. a
 * plain INSERT...VALUES with no scan).
 */
function estimate_affected_rows(sql) {
  try {
    var ex = query("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !Array.isArray(ex) || !ex.length) return null;
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'];
    if (!raw) return null;
    return sum_explain_rows(String(raw));
  } catch (e) { return null; }
}

function execute_tool(tool, args, db) {
  var sql, upper, first;

  if (tool === 'query_db') {
    sql = replace_ph(String(args.sql || ''), db);
    var stmt2 = classify_statement(sql);
    if (!stmt2.is_read)
      return { ok: false,
               response: t('拒绝：query_db 只允许只读语句（SELECT/SHOW/DESC/EXPLAIN/WITH）。',
                           'Rejected: query_db only allows read-only statements (SELECT/SHOW/DESC/EXPLAIN/WITH).'),
               error: 'invalid_read_only_sql' };
    var result = compress(rows_to_table(query(sql)), 1200);
    if (result.indexOf('Unknown table') !== -1) {
      var recovery2 = try_recover_unknown_table(result, sql);
      if (recovery2)
        return { ok: true,
                 response: recovery2.desc + '\n' + compress(rows_to_table(query(recovery2.sql)), 1200),
                 sql: recovery2.sql };
    }
    return { ok: true, response: result, sql: sql };
  }

  if (tool === 'explain_sql') {
    sql = replace_ph(String(args.sql || ''), db);
    var ex = query("EXPLAIN FORMAT=JSON " + sql);
    if (!ex || !ex.length)
      return { ok: false, response: t('EXPLAIN 执行失败', 'EXPLAIN execution failed'), error: 'explain_failed' };
    var raw = ex[0]['EXPLAIN'] || ex[0]['explain'] || JSON.stringify(ex[0]);
    return { ok: true, response: t('执行计划：', 'Execution plan: ') + parse_explain(String(raw)), sql: sql };
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
    return { ok: true,
             response: compress(out_lines.join('\n'), cfg('plan_log_max_tokens', 4000)),
             sql: JSON.stringify(steps2) };
  }

  if (tool === 'begin_tx') {
    if (tx_active_for(A.conversation_id))
      return { ok: false,
               response: t('警告：事务已活跃，禁止重复 begin_tx。',
                           'Warning: transaction already active; duplicate begin_tx forbidden.'),
               error: 'transaction_already_active' };
    if (!begin_tx_lease(A.conversation_id, '', 30)) {
      return { ok: false,
               response: t('拒绝：该会话已有另一个连接持有活跃事务租约，无法开启新事务。',
                           'Rejected: another connection already holds an active transaction lease for this conversation.'),
               error: 'lease_owned_by_other_session' };
    }
    sys.exec_sql('START TRANSACTION');
    set_tx_active_for(A.conversation_id, true);
    return { ok: true, response: t('事务已开启', 'Transaction started') };
  }

  if (tool === 'update_data') {
    if (!tx_active_for(A.conversation_id))
      return { ok: false,
               response: t('拒绝：写操作必须在事务内，请先 begin_tx。',
                           'Rejected: write operations must be inside a transaction; call begin_tx first.'),
               error: 'transaction_required' };
    sql   = replace_ph(String(args.sql || ''), db);
    var stmt3 = classify_statement(sql);
    upper = sql.trim().toUpperCase();
    first = stmt3.first_keyword;
    if ((first==='UPDATE'||first==='DELETE') && !/\bWHERE\b/.test(upper))
      return { ok: false,
               response: t('拒绝：', 'Rejected: ') + first +
                         t(' 必须含 WHERE 条件。', ' must include a WHERE clause.'),
               error: 'missing_where' };
    if (stmt3.is_ddl)
      return { ok: false,
               response: t('拒绝：', 'Rejected: ') + first +
                         t(' 为 DDL 语句。update_data 仅允许 INSERT/UPDATE/DELETE/REPLACE，'
                           + 'DDL 必须走审批流程（即使 review_mode=off 也强制拦截）。',
                           ' is a DDL statement. update_data only allows INSERT/UPDATE/DELETE/REPLACE; '
                           + 'DDL is always blocked regardless of review_mode.'),
               error: 'ddl_blocked_in_update_data' };
    var raw_result = sys.exec_sql(sql);
    var affected = -1;
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    if (raw_result && typeof raw_result.affected_rows !== 'undefined') {
      affected = Number(raw_result.affected_rows);
    }
    return { ok: true, response: t('执行成功', 'Success'), sql: sql, affected_rows: affected };
  }

  if (tool === 'commit_tx') {
    if (!tx_active_for(A.conversation_id))
      return { ok: false,
               response: t('警告：当前无活跃事务。', 'Warning: no active transaction.'),
               error: 'no_active_transaction' };

    clear_tx_lease(A.conversation_id);
    sys.exec_sql('COMMIT');
    set_tx_active_for(A.conversation_id, false);
    return { ok: true, response: t('事务已提交', 'Transaction committed') };
  }

  if (tool === 'rollback_tx') {
    if (tx_active_for(A.conversation_id)) {
      sys.exec_sql('ROLLBACK');
      set_tx_active_for(A.conversation_id, false);
      clear_tx_lease(A.conversation_id);
    }
    return { ok: true, response: t('事务已回滚', 'Transaction rolled back') };
  }

  if (tool === 'list_tables')
    return tool_list_tables(db, args || {});

  if (tool === 'describe_table')
    return tool_describe_table(db, args || {});

  if (tool === 'ml_rag') {
    var rag_opt_for_tool = get_rag_options(get_chat_options());
    var rag_res = ml_rag(String(args.question || A.user_message), args.top_k || rag_opt_for_tool.n_citations || 6, rag_opt_for_tool);
    if (!rag_res || !rag_res.ok) {
      return { ok: false, response: t('ml_rag 执行失败', 'ml_rag execution failed'), error: 'ml_rag_failed' };
    }
    return { ok: true, response: rag_res.text || '', raw: rag_res.raw, rag_meta: rag_res };
  }
  if (tool === 'generate_text')
    return { ok: true, response: ml_generate(String(args.prompt || ''), args.options || {}) };

  /* === ML / AutoML tools === */

  if (tool === 'ml_train') {
    var ml_table  = esc(String(args.table_name || ''));
    var ml_target = esc(String(args.target_column || ''));
    var ml_task   = String(args.task || 'classification').toUpperCase();
    var ml_handle = String(args.model_handle || '');
    var ml_opt    = JSON.stringify({ task: ml_task });
    var ml_sql    = "CALL sys.ML_TRAIN('" + esc(ml_table) + "', '" + esc(ml_target) + "', " +
                    "CAST('" + esc(ml_opt) + "' AS JSON), " +
                    (ml_handle ? "'" + esc(ml_handle) + "'" : 'NULL') + ")";
    try {
      var ml_train_res = query(ml_sql);
      return { ok: true, response: t('ML_TRAIN 执行完成。\n', 'ML_TRAIN completed.\n') +
               compress(rows_to_text(ml_train_res), 2000), sql: ml_sql };
    } catch (e) {
      return { ok: false, response: t('ML_TRAIN 失败：', 'ML_TRAIN failed: ') + String(e),
               error: 'ml_train_failed' };
    }
  }

  if (tool === 'ml_predict_row') {
    var pr_handle = esc(String(args.model_handle || ''));
    var pr_data   = JSON.stringify(args.data || {});
    var pr_opt    = args.options ? JSON.stringify(args.options) : 'NULL';
    var pr_sql    = "SELECT sys.ML_PREDICT_ROW(CAST('" + esc(pr_data) + "' AS JSON), '" +
                    esc(pr_handle) + "', " + pr_opt + ") AS prediction";
    try {
      var pr_res = query(pr_sql);
      return { ok: true, response: t('预测结果：\n', 'Prediction result:\n') +
               compress(rows_to_text(pr_res), 2000), sql: pr_sql };
    } catch (e) {
      return { ok: false, response: t('ML_PREDICT_ROW 失败：', 'ML_PREDICT_ROW failed: ') + String(e),
               error: 'ml_predict_row_failed' };
    }
  }

  if (tool === 'ml_predict_table') {
    var pt_table   = esc(String(args.table_name || ''));
    var pt_handle  = esc(String(args.model_handle || ''));
    var pt_out     = args.output_table ? esc(String(args.output_table)) : '';
    var pt_opt     = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
    if (!pt_out) {
      var pt_parts = pt_table.split('.');
      var pt_suffix = '_predictions_' + String(Date.now() % 100000);
      pt_out = (pt_parts.length > 1 ? pt_parts[0] + '.' : '') + pt_parts[pt_parts.length - 1] + pt_suffix;
    }
    var pt_sql = "CALL sys.ML_PREDICT_TABLE('" + esc(pt_table) + "', '" +
                 esc(pt_handle) + "', '" + esc(pt_out) + "', " + pt_opt + ")";
    try {
      var pt_res = query(pt_sql);
      return { ok: true, response: t('ML_PREDICT_TABLE 执行完成，结果写入 ', 'ML_PREDICT_TABLE completed, output written to ') +
               pt_out + '\n' + compress(rows_to_text(pt_res), 2000), sql: pt_sql };
    } catch (e) {
      return { ok: false, response: t('ML_PREDICT_TABLE 失败：', 'ML_PREDICT_TABLE failed: ') + String(e),
               error: 'ml_predict_table_failed' };
    }
  }

  if (tool === 'ml_explain') {
    var ex_table  = esc(String(args.table_name || ''));
    var ex_target = esc(String(args.target_column || ''));
    var ex_handle = esc(String(args.model_handle || ''));
    var ex_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
    var ex_sql    = "CALL sys.ML_EXPLAIN('" + esc(ex_table) + "', '" + esc(ex_target) + "', '" +
                    esc(ex_handle) + "', " + ex_opt + ")";
    try {
      var ex_res = query(ex_sql);
      return { ok: true, response: t('ML_EXPLAIN 执行完成。\n', 'ML_EXPLAIN completed.\n') +
               compress(rows_to_text(ex_res), 3000), sql: ex_sql };
    } catch (e) {
      return { ok: false, response: t('ML_EXPLAIN 失败：', 'ML_EXPLAIN failed: ') + String(e),
               error: 'ml_explain_failed' };
    }
  }

  if (tool === 'ml_explain_row') {
    var er_handle = esc(String(args.model_handle || ''));
    var er_data   = JSON.stringify(args.data || {});
    var er_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
    var er_sql    = "SELECT sys.ML_EXPLAIN_ROW(CAST('" + esc(er_data) + "' AS JSON), '" +
                    esc(er_handle) + "', " + er_opt + ") AS explanation";
    try {
      var er_res = query(er_sql);
      return { ok: true, response: t('预测解释结果：\n', 'Prediction explanation:\n') +
               compress(rows_to_text(er_res), 3000), sql: er_sql };
    } catch (e) {
      return { ok: false, response: t('ML_EXPLAIN_ROW 失败：', 'ML_EXPLAIN_ROW failed: ') + String(e),
               error: 'ml_explain_row_failed' };
    }
  }

  if (tool === 'ml_explain_table') {
    var et_table  = esc(String(args.table_name || ''));
    var et_handle = esc(String(args.model_handle || ''));
    var et_out    = args.output_table ? esc(String(args.output_table)) : '';
    var et_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
    if (!et_out) {
      var et_parts = et_table.split('.');
      var et_suffix = '_explain_' + String(Date.now() % 100000);
      et_out = (et_parts.length > 1 ? et_parts[0] + '.' : '') + et_parts[et_parts.length - 1] + et_suffix;
    }
    var et_sql = "CALL sys.ML_EXPLAIN_TABLE('" + esc(et_table) + "', '" +
                 esc(et_handle) + "', '" + esc(et_out) + "', " + et_opt + ")";
    try {
      var et_res = query(et_sql);
      return { ok: true, response: t('ML_EXPLAIN_TABLE 执行完成，结果写入 ', 'ML_EXPLAIN_TABLE completed, output written to ') +
               et_out + '\n' + compress(rows_to_text(et_res), 2000), sql: et_sql };
    } catch (e) {
      return { ok: false, response: t('ML_EXPLAIN_TABLE 失败：', 'ML_EXPLAIN_TABLE failed: ') + String(e),
               error: 'ml_explain_table_failed' };
    }
  }

  if (tool === 'ml_score') {
    var sc_table  = esc(String(args.table_name || ''));
    var sc_target = esc(String(args.target_column || ''));
    var sc_handle = esc(String(args.model_handle || ''));
    var sc_metric = esc(String(args.metric || 'balanced_accuracy'));
    var sc_opt    = args.options ? "CAST('" + esc(JSON.stringify(args.options)) + "' AS JSON)" : 'NULL';
    /* Use a session variable for the OUT parameter */
    var sc_sql = "SET @_ml_score_val = 0; CALL sys.ML_SCORE('" + esc(sc_table) + "', '" +
                 esc(sc_target) + "', '" + esc(sc_handle) + "', '" + esc(sc_metric) +
                 "', @_ml_score_val, " + sc_opt + "); SELECT @_ml_score_val AS score;";
    try {
      var sc_res = query(sc_sql);
      return { ok: true, response: t('ML_SCORE 结果：\n', 'ML_SCORE result:\n') +
               compress(rows_to_text(sc_res), 2000), sql: sc_sql };
    } catch (e) {
      return { ok: false, response: t('ML_SCORE 失败：', 'ML_SCORE failed: ') + String(e),
               error: 'ml_score_failed' };
    }
  }

  if (tool === 'ml_model_export') {
    var me_handle = esc(String(args.model_handle || ''));
    var me_out    = args.output_table ? esc(String(args.output_table)) : '';
    if (!me_out) {
      me_out = 'ml_export_' + String(Date.now() % 100000);
    }
    var me_sql = "CALL sys.ML_MODEL_EXPORT('" + esc(me_handle) + "', '" + esc(me_out) + "')";
    try {
      var me_res = query(me_sql);
      return { ok: true, response: t('ML_MODEL_EXPORT 完成，导出到 ', 'ML_MODEL_EXPORT completed, exported to ') +
               me_out + '\n' + compress(rows_to_text(me_res), 2000), sql: me_sql };
    } catch (e) {
      return { ok: false, response: t('ML_MODEL_EXPORT 失败：', 'ML_MODEL_EXPORT failed: ') + String(e),
               error: 'ml_model_export_failed' };
    }
  }

  if (tool === 'ml_model_import') {
    var mi_handle  = esc(String(args.model_handle || ''));
    var mi_content = esc(String(args.model_content || ''));
    var mi_task    = String(args.task || 'classification');
    var mi_meta    = JSON.stringify({ task: mi_task });
    /* Import requires model content — the LLM should guide user to provide it
     * or load from a table. For safety, we only allow import from a known table. */
    if (!mi_content) {
      return { ok: false,
               response: t('ML_MODEL_IMPORT 需要 model_content 参数（从 ml_model_export 导出的表名）。',
                           'ML_MODEL_IMPORT requires model_content parameter (table name from ml_model_export).'),
               error: 'ml_model_import_missing_content' };
    }
    var mi_sql = "CALL sys.ML_MODEL_IMPORT((SELECT MODEL_OBJECT FROM " + esc(mi_content) +
                 " LIMIT 1), CAST('" + esc(mi_meta) + "' AS JSON), '" + esc(mi_handle) + "')";
    try {
      var mi_res = query(mi_sql);
      return { ok: true, response: t('ML_MODEL_IMPORT 完成。\n', 'ML_MODEL_IMPORT completed.\n') +
               compress(rows_to_text(mi_res), 2000), sql: mi_sql };
    } catch (e) {
      return { ok: false, response: t('ML_MODEL_IMPORT 失败：', 'ML_MODEL_IMPORT failed: ') + String(e),
               error: 'ml_model_import_failed' };
    }
  }

  /* === List models (helper) === */
  if (tool === 'ml_list_models') {
    try {
      var lm_rows = query(
        "SELECT MODEL_HANDLE, TASK, TARGET_COLUMN_NAME, TRAIN_TABLE_NAME, " +
        "MODEL_OBJECT_SIZE, BUILD_TIMESTAMP " +
        "FROM ML_SCHEMA_" + esc(String(db || '')).replace(/@.*$/, '') + ".MODEL_CATALOG " +
        "ORDER BY BUILD_TIMESTAMP DESC LIMIT 20"
      );
      return { ok: true, response: t('已训练模型列表：\n', 'Trained models:\n') +
               compress(rows_to_text(lm_rows), 2000) };
    } catch (e) {
      return { ok: false,
               response: t('无法列出模型：', 'Cannot list models: ') + String(e),
               error: 'ml_list_models_failed' };
    }
  }
           response: t('错误：未知工具 "', 'Error: unknown tool "') + tool + '"',
           error: 'unknown_tool' };
}

function get_review_policy(chat_opt) {
  var cfg = (chat_opt && typeof chat_opt === 'object') ? chat_opt : {};
  var mode = String(cfg.review_mode || '').toLowerCase();
  return {
    review_mode: (mode === 'review' || mode === 'true' || mode === '1') ? 'review' : 'off',
    auto_execute_read_only: cfg.auto_execute_read_only !== false,
    require_approval_for_write: cfg.require_approval_for_write !== false,
    require_approval_for_ddl: cfg.require_approval_for_ddl !== false,
    require_approval_for_risky_sql: cfg.require_approval_for_risky_sql !== false,
    max_pending_steps: Math.max(1, Number(cfg.max_pending_steps || 3)),
    /* How long an awaiting_approval plan may sit untouched before it's
     * auto-cancelled (see cleanup_expired_review_plans). Mirrors the tx
     * lease's 30-minute default so a forgotten approval prompt doesn't
     * permanently hijack every subsequent message in the conversation. */
    review_plan_ttl_minutes: Math.max(1, Number(cfg.review_plan_ttl_minutes || 30))
  };
}

function normalize_review_command(text) {
  var s = String(text || '').trim().toLowerCase();
  if (!s) return null;
  if (s === 'approve' || s === 'yes' || s === 'y' || s.indexOf('approve') === 0)
    return 'approve';
  if (s === 'reject' || s === 'no' || s === 'n' || s.indexOf('reject') === 0)
    return 'reject';
  if (s.indexOf('modify:') === 0 || s.indexOf('modify ') === 0 ||
      s.indexOf('修改:') === 0 || s.indexOf('修改 ') === 0)
    return 'modify';
  return null;
}

function parse_review_modify(text) {
  var s = String(text || '').trim();
  var m = s.match(/^(?:modify|修改)\s*[:：]?\s*(.+)$/i);
  return m && m[1] ? m[1].trim() : null;
}

function infer_affected_tables(sql) {
  var tables = [];
  var seen = {};
  var patterns = [
    /\bfrom\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bjoin\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\binto\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bupdate\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\bdelete\s+from\s+`?([a-zA-Z0-9_$.]+)`?/gi,
    /\btable\s+`?([a-zA-Z0-9_$.]+)`?/gi
  ];
  for (var i = 0; i < patterns.length; i++) {
    var re = patterns[i];
    var m;
    while ((m = re.exec(sql))) {
      var table = String(m[1] || '').replace(/^`|`$/g, '').replace(/^([^.]+)\./, '');
      if (!table || seen[table]) continue;
      seen[table] = true;
      tables.push(table);
    }
  }
  return tables;
}

function infer_step_risk(sql, tool) {
  var stmt = classify_statement(sql);
  if (stmt.risk !== 'low') return stmt.risk;
  if (tool === 'update_data') return 'medium';
  return 'low';
}

function build_review_step(tool_obj, args, db, compute_estimate) {
  var sql = '';
  if (tool_obj && tool_obj.tool === 'plan_sql') {
    var steps = Array.isArray(args && args.steps) ? args.steps : [];
    sql = String((steps[0] && steps[0].sql) || '');
  } else if (tool_obj && tool_obj.tool === 'list_tables') {
    /* Pure metadata lookup; synthesize an equivalent SHOW TABLES for display
     * purposes only (classify_statement/infer_affected_tables need *some*
     * SQL-shaped string to reason about — this tool has no user-authored SQL). */
    sql = "SHOW TABLES" + ((args && args.keyword) ? " LIKE '%" + args.keyword + "%'" : '');
  } else if (tool_obj && tool_obj.tool === 'describe_table') {
    var tn_disp = (args && args.table_name) ? args.table_name :
                  (Array.isArray(args && args.table_names) ? args.table_names.join(',') : '');
    sql = "DESC " + tn_disp;
  } else {
    sql = String((args && args.sql) || '');
  }
  sql = replace_ph(sql, db);
  var stmt4 = classify_statement(sql);
  var step = {
    id: 1,
    tool: tool_obj && tool_obj.tool ? tool_obj.tool : 'unknown',
    sql: sql,
    args: args || {},
    affected_tables: infer_affected_tables(sql),
    writes: stmt4.is_write,
    ddl: stmt4.is_ddl,
    risk: stmt4.risk,
    estimated_rows: 'unknown',
    thought: tool_obj && tool_obj.thought ? tool_obj.thought : ''
  };

  /* EXPLAIN-based row estimate — only for write statements (the case the
   * approval preview actually needs to protect against: a WHERE clause
   * that touches far more rows than intended), and only when the caller
   * has confirmed we're actually about to render a review prompt.
   * build_review_step is also called on every single turn purely to
   * evaluate policy (see evaluate_step_policy call site) — running an
   * extra EXPLAIN round-trip there would double the SQL calls per turn
   * for the common auto-execute path, so compute_estimate defaults falsy
   * and only build_review_steps (called exclusively from the pause
   * branch) opts in. */
  if (compute_estimate && stmt4.is_write && !stmt4.is_ddl) {
    var est = estimate_affected_rows(sql);
    step.estimated_rows = (est === null)
      ? 'unknown'
      : t('≈' + est + ' 行（EXPLAIN 估算，非精确值）', '≈' + est + ' rows (EXPLAIN estimate, not exact)');
  }

  return step;
}

function evaluate_step_policy(step, policy) {
  /* DDL (CREATE/ALTER/DROP/TRUNCATE/RENAME) is not supported by the
   * current tool set — reject it immediately regardless of review_mode,
   * so the user never sees a misleading "approve this DDL" prompt for
   * something that can never execute successfully. */
  if (step && step.ddl)
    return { action: 'reject', reason: 'ddl_not_supported',
             message: t('DDL 语句当前不支持通过 agent 执行。',
                        'DDL statements are not supported via the agent.') };
  if (!policy || policy.review_mode !== 'review')
    return { action: 'execute', reason: 'review_disabled' };

  var is_write = !!(step && step.writes);
  var is_risky = !!(step && step.risk === 'high');

  if (is_write && policy.require_approval_for_write)
    return { action: 'pause', reason: 'write_requires_approval' };
  if (is_risky && policy.require_approval_for_risky_sql)
    return { action: 'pause', reason: 'risky_sql_requires_approval' };

  /* Read-only fast path — explicitly gated on !is_write. Previously this
   * branch fired whenever auto_execute_read_only was true regardless of
   * whether the step actually wrote anything, so a write step could slip
   * through mislabeled as 'safe_read_only' whenever an operator had
   * disabled require_approval_for_write for an unrelated reason. */
  if (!is_write && policy.auto_execute_read_only)
    return { action: 'execute', reason: 'safe_read_only' };

  /* Reaching here means the step IS a write or high-risk statement, but
   * policy explicitly exempted it from approval (require_approval_for_write
   * / require_approval_for_risky_sql set to false). Execute it — that's the
   * operator's explicit intent — but under its own accurate reason instead
   * of borrowing 'safe_read_only', so audit logs reflect what actually
   * happened. */
  if (is_write || is_risky)
    return { action: 'execute', reason: 'approval_explicitly_disabled_by_policy' };

  return { action: 'pause', reason: 'review_mode' };
}

function render_review_prompt(plan, step, policy) {
  var lines = [];
  lines.push(t('【审批中断】请确认下一步执行：', '[Approval pause] Please review the next step:'));
  lines.push(t('计划 ID：', 'Plan ID: ') + (plan && plan.plan_id ? plan.plan_id : '')); 
  lines.push(t('步骤：', 'Step: ') + (step && step.id ? step.id : 1) + '/' + ((plan && plan.total_steps) ? plan.total_steps : 1));
  lines.push(t('工具：', 'Tool: ') + (step && step.tool ? step.tool : 'unknown'));
  lines.push(t('SQL：', 'SQL: ') + (step && step.sql ? step.sql : ''));
  lines.push(t('影响表：', 'Affected tables: ') + ((step && step.affected_tables && step.affected_tables.length) ? step.affected_tables.join(', ') : t('未知', 'unknown')));
  lines.push(t('预计影响行数：', 'Estimated rows: ') + (step && step.estimated_rows ? step.estimated_rows : 'unknown'));
  lines.push(t('会写入/修改结构：', 'Will write / modify schema: ') + ((step && (step.writes || step.ddl)) ? t('是', 'yes') : t('否', 'no')));
  lines.push(t('风险等级：', 'Risk: ') + (step && step.risk ? step.risk : 'low'));
  if (plan && plan.description) lines.push(t('计划描述：', 'Plan description: ') + plan.description);
  lines.push('');
  lines.push(t('请回复：Approve / Reject / Modify:SQL', 'Please reply: Approve / Reject / Modify:SQL'));
  return lines.join('\n');
}

function build_review_steps(tool_obj, args, db) {
  if (!tool_obj || !tool_obj.tool) return [];
  if (tool_obj.tool === 'plan_sql') {
    var raw_steps = Array.isArray(args && args.steps) ? args.steps : [];
    var steps = [];
    for (var i = 0; i < raw_steps.length; i++) {
      var step_args = raw_steps[i] || {};
      /* Pick the tool that actually matches the statement kind: a write
       * (INSERT/UPDATE/DELETE/REPLACE) sub-step must route to 'update_data',
       * or execute_tool('query_db', ...) will reject it as non-read-only at
       * execution time — even after the step sailed through review and was
       * Approved. Read-only sub-steps keep 'query_db'. */
      var step_sql  = replace_ph(String(step_args.sql || ''), db);
      var step_stmt = classify_statement(step_sql);
      var step_tool = step_stmt.is_write ? 'update_data' : 'query_db';
      var step = build_review_step({ tool: step_tool, thought: step_args.desc || '' }, step_args, db, true);
      step.id = i + 1;
      step.description = step_args.desc || (t('步骤', 'Step') + ' ' + (i + 1));
      steps.push(step);
    }
    return steps;
  }
  var step = build_review_step(tool_obj, args, db, true);
  step.id = 1;
  step.description = tool_obj.thought || '';
  return [step];
}

function save_review_plan(plan) {
  if (!plan || !plan.plan_id || !plan.conversation_id) return;
  var plan_id = plan.plan_id;
  var conv_id = plan.conversation_id;
  var status = plan.status || 'awaiting_approval';
  var current_step_index = Number(plan.current_step_index || 0);
  var total_steps = Number(plan.steps && plan.steps.length ? plan.steps.length : 0);
  var description = String(plan.description || '');
  var plan_json = JSON.stringify({ plan_id: plan_id, current_step_index: current_step_index, total_steps: total_steps });
  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_review_plan(plan_id,conversation_id,status,current_step_index,total_steps,description,plan_json) VALUES ('" +
        esc(plan_id) + "','" + esc(conv_id) + "','" + esc(status) + "'," +
        current_step_index + "," + total_steps + ", '" + esc(description) + "', '" + esc(plan_json) + "') " +
      "ON DUPLICATE KEY UPDATE status=VALUES(status), current_step_index=VALUES(current_step_index), total_steps=VALUES(total_steps), description=VALUES(description), plan_json=VALUES(plan_json), updated_at=CURRENT_TIMESTAMP"
    );
    sys.exec_sql("DELETE FROM mysql.agent_review_plan_step WHERE plan_id='" + esc(plan_id) + "'");
    for (var i = 0; i < (plan.steps || []).length; i++) {
      var step = plan.steps[i];
      sys.exec_sql(
        "INSERT INTO mysql.agent_review_plan_step(plan_id, step_no, tool, sql_text, args_json, affected_tables, writes, ddl, risk, estimated_rows, status, result_preview, error_text) VALUES (" +
        "'" + esc(plan_id) + "'," + Number(step.id || (i + 1)) + "," +
        "'" + esc(step.tool || '') + "'," +
        "'" + esc(step.sql || '') + "'," +
        "'" + esc(JSON.stringify(step.args || {})) + "'," +
        "'" + esc((step.affected_tables || []).join(',')) + "'," +
        Number(step.writes ? 1 : 0) + "," +
        Number(step.ddl ? 1 : 0) + "," +
        "'" + esc(step.risk || '') + "'," +
        "'" + esc(step.estimated_rows || '') + "'," +
        "'" + esc(step.status || 'pending') + "'," +
        "'" + esc(step.result_preview || '') + "'," +
        "'" + esc(step.error_text || '') + "')"
      );
    }
  } catch (e) {
    /* ignore */
  }
}

function load_review_state(conv_id) {
  if (!conv_id) return null;
  try {
    var rows = query(
      "SELECT plan_id, status, current_step_index, total_steps, description, plan_json " +
      "FROM mysql.agent_review_plan " +
      "WHERE conversation_id='" + esc(conv_id) + "' AND status='awaiting_approval' " +
      "ORDER BY created_at DESC LIMIT 1"
    );
    if (!Array.isArray(rows) || !rows.length || !rows[0].plan_id) return null;
    var plan = rows[0];
    var step_rows = query(
      "SELECT step_no, tool, sql_text, args_json, affected_tables, writes, ddl, risk, estimated_rows, status, result_preview, error_text " +
      "FROM mysql.agent_review_plan_step " +
      "WHERE plan_id='" + esc(plan.plan_id) + "' ORDER BY step_no"
    );
    var steps = [];
    if (Array.isArray(step_rows)) {
      for (var i = 0; i < step_rows.length; i++) {
        var sr = step_rows[i];
        steps.push({
          id: Number(sr.step_no),
          tool: sr.tool,
          sql: sr.sql_text,
          args: try_parse_json(sr.args_json) || {},
          affected_tables: String(sr.affected_tables || '').split(',').filter(function(x){ return x; }),
          writes: !!Number(sr.writes),
          ddl: !!Number(sr.ddl),
          risk: sr.risk,
          estimated_rows: sr.estimated_rows,
          status: sr.status,
          result_preview: sr.result_preview,
          error_text: sr.error_text
        });
      }
    }
    return {
      plan_id: plan.plan_id,
      conversation_id: plan.conversation_id,
      status: plan.status,
      current_step_index: Number(plan.current_step_index || 0),
      total_steps: Number(plan.total_steps || steps.length),
      description: plan.description,
      steps: steps
    };
  } catch (e) {
    return null;
  }
}

function try_parse_json(text) {
  if (!text || typeof text !== 'string') return null;
  try { return JSON.parse(text); } catch (e) { return null; }
}

/**
 * DEPRECATED — kept for backward compatibility with any external callers.
 * New code should use the affected_rows field returned by execute_tool()
 * (especially execute_tool('update_data', ...).affected_rows) instead of
 * the approximate information_schema.TABLE_ROWS statistic.
 */
function capture_table_counts(step, db) {
  if (!step || !Array.isArray(step.affected_tables) || !step.affected_tables.length || !db) return null;
  try {
    var names = step.affected_tables.map(function(t){ return "'" + esc(t) + "'"; }).join(',');
    var rows = query(
      "SELECT TABLE_NAME, TABLE_ROWS FROM information_schema.TABLES " +
      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN (" + names + ")"
    );
    if (!Array.isArray(rows)) return null;
    var counts = {};
    for (var i = 0; i < rows.length; i++) {
      counts[String(rows[i].TABLE_NAME || '')] = Number(rows[i].TABLE_ROWS || 0);
    }
    return counts;
  } catch (e) { return null; }
}

/**
 * DEPRECATED — see capture_table_counts above.
 */
function compare_table_counts(before_counts, after_counts) {
  if (!before_counts || !after_counts) return null;
  var lines = [];
  for (var table in before_counts) {
    if (!Object.prototype.hasOwnProperty.call(before_counts, table)) continue;
    var before = Number(before_counts[table] || 0);
    var after = Number(after_counts[table] || 0);
    if (before !== after) {
      lines.push(table + ': ' + before + ' -> ' + after);
    }
  }
  return lines.length ? lines.join('; ') : null;
}

function get_pending_review_step(plan) {
  if (!plan || !Array.isArray(plan.steps) || plan.steps.length === 0) return null;
  var idx = Number(plan.current_step_index || 0);
  if (idx < 0 || idx >= plan.steps.length) return null;
  return plan.steps[idx];
}

function update_review_step_status(plan_id, step_no, status, result_preview, error_text) {
  if (!plan_id || !step_no || !status) return;
  try {
    sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(status) + "', " +
      "result_preview='" + esc(result_preview || '') + "', " +
      "error_text='" + esc(error_text || '') + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no)
    );
  } catch (e) {}
}

function clear_review_state(conv_id, plan_id, final_status) {
  if (!conv_id || !plan_id) return;
  final_status = final_status || 'completed';
  try {
    sys.exec_sql(
      "UPDATE mysql.agent_review_plan SET status='" + esc(final_status) + "', updated_at=CURRENT_TIMESTAMP " +
      "WHERE plan_id='" + esc(plan_id) + "'"
    );
  } catch (e) {}
}

/**
 * Auto-cancel awaiting_approval plans that have sat untouched past
 * review_plan_ttl_minutes. Without this, a user who gets a pause prompt and
 * then wanders off to a different question stays permanently stuck: every
 * subsequent message hits the load_review_state() check at the top of
 * shannon_agent_run and gets reinterpreted as an Approve/Reject/Modify
 * command with no escape hatch. Mirrors cleanup_expired_tx_leases's
 * pattern/TTL semantics. Runs with no LIMIT, so it also sweeps up any
 * orphaned older plans for the same conversation (e.g. left behind by a
 * crash between saving a new plan and clearing the previous one) — not
 * just the single latest row that load_review_state would have looked at.
 */
function cleanup_expired_review_plans(conv_id, ttl_minutes) {
  if (!conv_id) return;
  ttl_minutes = Math.max(1, Number(ttl_minutes) || 30);
  try {
    var expired = query(
      "SELECT plan_id FROM mysql.agent_review_plan " +
      "WHERE conversation_id='" + esc(conv_id) + "' AND status='awaiting_approval' " +
      "AND updated_at < DATE_SUB(NOW(), INTERVAL " + ttl_minutes + " MINUTE)"
    );
    if (!Array.isArray(expired) || !expired.length) return;
    for (var i = 0; i < expired.length; i++) {
      clear_review_state(conv_id, expired[i].plan_id, 'expired');
      log_rollback_event(conv_id, expired[i].plan_id, 0, '', '',
                         t('审批计划超时未确认，自动取消', 'Approval plan timed out with no response and was auto-cancelled'));
    }
  } catch (e) {}
}

var REVIEW_STEP_TRANSITIONS = {
  'pending':            ['executing', 'rejected', 'error', 'pending'],
  'awaiting_approval':  ['executing', 'rejected', 'error', 'pending'],
  'executing':          ['completed', 'failed'],
  'completed':          [],
  'failed':             ['pending'],
  'rejected':           [],
  'error':              ['pending']
};

function get_step_status(plan_id, step_no) {
  try {
    var rows = query(
      "SELECT status FROM mysql.agent_review_plan_step " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no)
    );
    if (Array.isArray(rows) && rows.length > 0) return String(rows[0].status || 'pending');
  } catch (e) {}
  return 'pending';
}

function claim_review_step(plan_id, step_no, from_status, to_status) {
  /* Validate against the shared transition table — same source of truth
   * that transition_review_step uses via its reverse-lookup. */
  var allowed = REVIEW_STEP_TRANSITIONS[from_status];
  if (!allowed || allowed.indexOf(to_status) === -1) {
    try {
      sys.exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'claim_review_step REFUSED: " + esc(from_status) + " -> " + esc(to_status) + " not in REVIEW_STEP_TRANSITIONS', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  try {
    var raw = sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status='" + esc(from_status) + "'"
    );
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    return (raw && Number(raw.affected_rows) === 1);
  } catch (e) { return false; }
}

function transition_review_step(plan_id, step_no, to_status, result_preview, error_text) {
  /* Reverse-lookup: find all statuses from which to_status is reachable */
  var valid_from = [];
  for (var s in REVIEW_STEP_TRANSITIONS) {
    if (!Object.prototype.hasOwnProperty.call(REVIEW_STEP_TRANSITIONS, s)) continue;
    if (REVIEW_STEP_TRANSITIONS[s].indexOf(to_status) !== -1) valid_from.push(s);
  }

  if (!valid_from.length) {
    try {
      sys.exec_sql(
        "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
        "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
        "'No valid FROM status for transition to " + esc(to_status) + "', 'state_machine')"
      );
    } catch (e) {}
    return false;
  }

  /* Single atomic UPDATE: check AND write in one statement */
  try {
    var raw = sys.exec_sql(
      "UPDATE mysql.agent_review_plan_step SET status='" + esc(to_status) + "', " +
      "result_preview='" + esc(result_preview || '') + "', " +
      "error_text='" + esc(error_text || '') + "' " +
      "WHERE plan_id='" + esc(plan_id) + "' AND step_no=" + Number(step_no) +
      " AND status IN ('" + valid_from.map(esc).join("','") + "')"
    );
    // sys.exec_sql returns an object {affected_rows: N}, not a JSON string
    if (raw && Number(raw.affected_rows) === 1) return true;
  } catch (e) {}

  /* Transition refused — either the current status is not in valid_from
   * (concurrent claim_review_step changed it) or the row doesn't exist.
   * Log for audit and return false. */
  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
      "'" + esc(A.conversation_id || '') + "', '" + esc(plan_id) + "', " + Number(step_no) + ", '', " +
      "'Atomic transition REFUSED to " + esc(to_status) + " (valid from: " + esc(valid_from.join(',')) + ")', 'state_machine')"
    );
  } catch (e) {}
  return false;
}

function append_review_history(conv_id, plan_id, step_no, action, user_cmd, comment, result_preview, rollback_flag) {
  if (!conv_id || !plan_id || !action) return;
  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_review_history(conversation_id, plan_id, step_no, action, user_cmd, comment, result_preview, rollback_flag) VALUES (" +
      "'" + esc(conv_id) + "', '" + esc(plan_id) + "', " + Number(step_no || 0) + ", '" + esc(action) + "', '" + esc(user_cmd || '') + "', '" + esc(comment || '') + "', '" + esc(result_preview || '') + "', " +
      Number(rollback_flag ? 1 : 0) + ")"
    );
  } catch (e) {}
}

function log_rollback_event(conv_id, plan_id, step_no, sql_text, error, rollback_reason) {
  if (!conv_id || !plan_id) return;
  try {
    sys.exec_sql(
      "INSERT INTO mysql.agent_rollback_log(conversation_id, plan_id, step_no, sql_text, error_text, rollback_reason) VALUES (" +
      "'" + esc(conv_id) + "', '" + esc(plan_id) + "', " + Number(step_no || 0) + ", '" + esc(sql_text || '') + "', '" + esc(error || '') + "', '" + esc(rollback_reason || '') + "')"
    );
  } catch (e) {}
}

function execute_review_step(step, db) {
  if (!step || !step.tool) {
    return { ok: false, response: t('审批步骤无效。', 'Invalid approval step.'), error: 'invalid_step' };
  }

  var tx_started = false;
  if (step.writes || step.ddl) {
    if (!tx_active_for(A.conversation_id)) {
      /* Under normal operation transactions do NOT survive across
       * shannon_agent_run() invocations (finalize_tx_safety_net runs on
       * every exit path).  The only way the mirror is stale while a real
       * transaction exists is crash-recovery: the previous invocation
       * threw an unhandled exception before reaching its safety net.
       * Double-check the lease table before starting a new transaction —
       * a spurious START TRANSACTION would implicitly commit the orphan. */
      if (is_real_tx_active()) {
        /* Sync the mirror: the transaction survived a prior invocation. */
        set_tx_active_for(A.conversation_id, true);
      } else {
        var begin_res = execute_tool('begin_tx', {}, db);
        if (!begin_res.ok) {
          return { ok: false, response: begin_res.response || t('开启事务失败。', 'Failed to start transaction.'), error: 'begin_tx_failed' };
        }
        tx_started = true;
      }
    }
  }

  var res;
  try {
    res = execute_tool(step.tool, step.args || {}, db);
  } catch (e) {
    res = { ok: false, response: t('步骤执行异常：', 'Step execution exception: ') + String(e), error: 'exception' };
  }

  if (tx_started) {
    if (res.ok) {
      transition_review_step(
        A.current_plan_id, step.id, 'completed', res.response || ''
      );
      var commit_res = execute_tool('commit_tx', {}, db);
      if (!commit_res.ok) {
        execute_tool('rollback_tx', {}, db);
        return { ok: false, response: commit_res.response || t('提交事务失败。', 'Failed to commit transaction.'), error: 'commit_failed' };
      }
      /* Use the precise affected_rows from the DML execution itself.
       * This is the MySQL-protocol-level row count, not the approximate
       * information_schema.TABLE_ROWS statistic.  For single-statement
       * review steps this is exact; for multi-statement plans the
       * diff_summary remains 'pending_commit' (see below). */
      res.diff_summary = (typeof res.affected_rows !== 'undefined' && res.affected_rows >= 0)
        ? t('影响行数：', 'Rows affected: ') + res.affected_rows
        : t('执行成功（行数未知）', 'Success (row count unknown)');
    } else {
      execute_tool('rollback_tx', {}, db);
    }
  } else if (res && res.ok) {
    /* tx_started is false but a real transaction was active — either
     * from an earlier step in this same invocation's agent loop
     * (explicit multi-statement writes) or crash-recovery.
     * The row count will be finalized when the outer commit_tx fires. */
    res.diff_summary = 'pending_commit';
  }

  return res;
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