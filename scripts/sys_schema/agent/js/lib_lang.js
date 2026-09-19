//@include lib_state.js

function detect_lang(text) {
  return /[\u4e00-\u9fff\u3400-\u4dbf]/.test(String(text || '')) ? 'zh' : 'en';
}

var TABLE_LIST_PATTERN_SRC =
  '有哪些表|所有表|列出.*表|show.?tables|list.*tables|what.*tables';

function t(zh, en) { return A.lang === 'zh' ? zh : en; }

var ESC_MAP = {
  '\\': '\\\\', '\u0000': '\\0', '\n': '\\n',
  '\r': '\\r', '\x1a': '\\Z', "'": "''"
};
function esc(s) {
  return String(s == null ? '' : s)
    .replace(/[\\\u0000\n\r\x1a']/g, function (c) { return ESC_MAP[c]; });
}

/* Escape an identifier for use between backticks. */
function esc_ident(s) {
  return String(s == null ? '' : s).replace(/`/g, '``');
}

function esc_like(s) {
  return esc(s)
    .replace(/%/g, '\\%')
    .replace(/_/g, '\\_');
}

function valid_ident(s) {
  return /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(String(s || ''));
}

function esc_qualified_ident(s) {
  var parts = String(s || '').split('.');
  if (!parts.length || parts.length > 2) return null;
  for (var i = 0; i < parts.length; i++) {
    if (!valid_ident(parts[i])) return null;
  }
  return parts.map(function(p) { return '`' + p + '`'; }).join('.');
}

/* Validate a "DBName.TableName.ColumnName" reference, the argument shape the
 * batch ML routines (ML_EMBED_TABLE / ML_GENERATE_TABLE / ML_RAG_TABLE) take
 * for their input and output columns. */
function valid_table_column_ref(s) {
  var parts = String(s || '').split('.');
  if (parts.length !== 3) return false;
  for (var i = 0; i < parts.length; i++) {
    if (!valid_ident(parts[i])) return false;
  }
  return true;
}

/* A structural change, recognised before the introspection branch below.
 *
 * That branch matches bare nouns -- 索引, 字段, 表 -- so "add an index on
 * approver" classified as 'schema', and build_task_header() then told the
 * model to confirm the columns and *generate a query*. It did exactly that:
 * it printed the CREATE INDEX for the user to run, instead of proposing it
 * through run_ddl where the approval gate lives. A noun does not say what the
 * user wants done with it; the verb does, so both have to be present. */
var _STRUCT_CHANGE_RE =
  /(创建|新建|建立|新增|添加|增加|删除|删掉|去掉|移除|清空|修改|更改|改名|重命名|重建)\s*(一个|一条|一列|个)?\s*(索引|主键|外键|字段|列|表|视图|分区|约束)|加\s*(一个|一条|个)?\s*(索引|主键|外键|字段|列|分区|约束)|\b(create|add|drop|alter|rename|modify)\s+(index|table|column|key|view|partition|constraint)\b/;

/* Does this reply hand the user a statement to run, rather than running it?
 *
 * Deliberately narrow: a fenced block (or a line that starts with one) whose
 * first keyword changes something. Prose that merely mentions ALTER, or a
 * SELECT shown to explain a result, is not this -- the point is a change the
 * user asked for that was described instead of proposed. */
function looks_like_handed_back_sql(text) {
  var s = String(text || '');
  var m = s.match(/```(?:sql)?\s*([\s\S]*?)```/i);
  var body = m ? m[1] : s;
  return /(^|\n)\s*(ALTER|CREATE|DROP|RENAME|TRUNCATE|INSERT|UPDATE|DELETE|REPLACE)\s+/i.test(body);
}

function classify_request(text) {
  var t = String(text || '').toLowerCase();
  if (_STRUCT_CHANGE_RE.test(t))
    return 'write';
  if (/有哪些表|所有表|列出.*表|show.?tables|list.*tables|字段|列信息|结构|describe.*table|索引|外键|schema/.test(t))
    return 'schema';
  if (/锁|死锁|线程|慢查询|事务|进程|连接|buffer|redo|表空间|变量|状态/.test(t))
    return 'diagnose';
  if (/group\s+by|统计|汇总|排名|占比|趋势|同比|环比|sum\(|avg\(|count\(/.test(t))
    return 'analytics';
  /* 改成/改为/删掉 are how the request usually arrives in Chinese; the
   * original list only had their formal equivalents. Safety never depended on
   * this -- the approval gate classifies the statement, not the sentence --
   * but the task header does. */
  if (/insert|update|delete|drop|alter|truncate|rename|创建|新建|删除|删掉|移除|清空|修改|更改|改成|改为|新增|添加|增加|写入/.test(t))
    return 'write';
  if (/训练|预测|模型|评分|评估|解释|导出|导入|train|predict|model|score|evaluate|explain|export|import|回归|分类|异常检测|推荐|forecast|anomaly|recommend|classif|regression/i.test(t))
    return 'ml';
  return 'general';
}

function analyze_intent(text) {
  var kind = classify_request(text);
  var t = String(text || '').toLowerCase();
  return {
    kind: kind,
    need_join: /join|关联|多表|跨表|联合/.test(t),
    need_time_filter: /最近|过去|今天|昨天|本月|上月|近\d+天|last\s+\d|过去\s*\d|within/.test(t),
    need_agg: /group\s+by|统计|汇总|排名|占比|趋势|同比|环比|sum\(|avg\(|count\(/.test(t)
  };
}

/* The engine heap, in bytes, as the host sizes it.
 *
 * Every ceiling in this file -- and the prompt's character budget in
 * lib_ml.js -- exists because of that heap, and each was written as a
 * constant chosen against a 512KB one. A build that raises the heap would
 * then leave the agent exactly as constrained as before, having paid the
 * memory for nothing, so the number is asked for rather than assumed.
 *
 * The fallback is the size the server shipped before the accessor existed.
 * A server built from an older tree must still run this script, and being
 * wrong in the conservative direction leaves it merely as restricted as it
 * used to be. */
var ENGINE_HEAP_BYTES_FALLBACK = 512 * 1024;
var _engine_heap_bytes = 0;

function engine_heap_bytes() {
  if (_engine_heap_bytes > 0) return _engine_heap_bytes;
  var h = 0;
  try { h = Number(sys.engine_heap_bytes()); } catch (e) { h = 0; }
  if (!isFinite(h) || h <= 0) h = ENGINE_HEAP_BYTES_FALLBACK;
  _engine_heap_bytes = h;
  return h;
}

/* Ceilings every fetch is subject to.
 *
 * sys.fetch_all() materialises rows into the per-thread JerryScript heap,
 * whose size is a build-time decision (SHANNONBASE_JERRY_HEAP_KB; see
 * engine_heap_bytes() below, and kJerryHeapBytes in sql/sp_head.cc for what
 * the two sides have to agree on). Exhausting it used to terminate the whole
 * server, because jerry-core answers heap exhaustion with jerry_port_fatal()
 * and the port's default implementation calls exit() for out-of-memory. That
 * is now overridden to raise a SQL error instead, but an error is still a
 * failed turn, so the fetch is bounded here as well and comes back short
 * rather than failing.
 *
 * The byte ceiling is the one that matters: a hundred rows of LONGTEXT
 * overrun the heap just as surely as a million narrow ones, and no row count
 * can see that coming. FETCH_MAX_ROWS is a second, coarser net, set high
 * enough that schema introspection over a large information_schema still
 * fits under it.
 *
 * The truncation is never silent: fetch_all marks a short result with
 * __truncated and reports __total_rows, and every caller that renders rows
 * for the model passes that on. */
var FETCH_MAX_ROWS = 5000;

/* Three eighths of the heap -- 192KB of 512KB, which is what this was tuned
 * to -- and it follows the heap from there. */
function fetch_max_bytes() {
  return Math.floor(engine_heap_bytes() * 3 / 8);
}

function query(sql, caps) {
  // sys.exec_sql returns a cursor ({columns, __cursor_id}) for SELECT,
  // or {affected_rows} for DML.  fetch_all materializes rows into the JS
  // heap, bounded by the two ceilings above; it sets __truncated on the
  // returned array when it stopped early.
  // For genuinely unlimited SELECTs, use sys.send_result_set(cursor) in a
  // PROCEDURE to stream rows to the client without JS heap pressure.
  var max_rows  = (caps && caps.max_rows  !== undefined) ? caps.max_rows  : FETCH_MAX_ROWS;
  var max_bytes = (caps && caps.max_bytes !== undefined) ? caps.max_bytes : fetch_max_bytes();
  try {
    var rs = sys.exec_sql(sql);
    if (typeof rs.__cursor_id === 'number') {
      return sys.fetch_all(rs, max_rows, max_bytes);
    }
    return rs;
  } catch (e) { return { error: String(e) }; }
}

/* True when the last fetch stopped at a ceiling rather than at the end of
 * the result. Callers use it to tell the model it is looking at a prefix. */
function rows_truncated(rows) {
  return !!(rows && rows.__truncated);
}

function rows_total(rows) {
  if (!rows) return 0;
  return (rows.__total_rows !== undefined) ? Number(rows.__total_rows)
                                           : (Array.isArray(rows) ? rows.length : 0);
}

/* Strict variant for execution paths whose success/failure affects the
 * agent state machine. Best-effort metadata helpers may keep using query(),
 * but tools must never translate a SQL exception into {ok:true}.
 *
 * IMPORTANT -- sys.exec_sql() does NOT throw on a SQL error.  It returns
 * { error: "<message>" }.  A bare
 *
 *     try { sys.exec_sql(dml); } catch (e) { ...handle failure... }
 *
 * therefore never runs its catch block for access-denied, syntax errors,
 * constraint violations or anything else the server rejects: the write
 * silently does nothing and the caller reports success.  Any statement whose
 * outcome matters must go through query_checked(), which turns that error
 * value back into a throw.  For DML it still returns { affected_rows: N } on
 * success, so it is a drop-in replacement for sys.exec_sql(). */
function query_checked(sql, caps) {
  var rows = query(sql, caps);
  if (rows && !Array.isArray(rows) && rows.error) {
    throw new Error(String(rows.error));
  }
  return rows;
}

function rows_to_text(rows, limit) {
  limit = limit || 150;
  if (!Array.isArray(rows)) {
    if (rows && rows.error)
      return t('执行出错：', 'Error: ') + rows.error;
    if (rows && rows.affected_rows !== undefined) {
      if (rows.columns) {
        return t('（查询结果为空）', '(No results)');
      }
      return t('执行成功，影响行数：', 'Success, rows affected: ') + rows.affected_rows;
    }
    return JSON.stringify(rows);
  }
  if (rows.length === 0) return t('（查询结果为空）', '(No results)');

  var cols = Object.keys(rows[0]);
  var cap  = Math.min(rows.length, limit);
  var out  = [t('共 ', 'Total ') + rows.length + t(' 条：', ' rows:')];
  for (var i = 0; i < cap; i++) {
    out.push(t('行', 'Row') + (i + 1) + ':');
    for (var c = 0; c < cols.length; c++) {
      var v = rows[i][cols[c]];
      out.push('  ' + cols[c] + ': ' + (v === null ? 'NULL' : String(v)));
    }
  }
  if (rows.length > cap)
    out.push(t('（仅展示前 ', '(Showing first ') + cap + t(' 条）', ' rows)'));
  return out.join('\n');
}

function compress(text, max_chars) {
  max_chars = max_chars || 700;
  if (!text || text.length <= max_chars) return text;
  return text.substring(0, max_chars) +
         t('\n…[截断，原长 ', '\n…[truncated, original ') +
         text.length +
         t(' 字符]', ' chars]');
}

/**
 *   Total N rows:
 *   col1 | col2 | col3
 *   ---- | ---- | ----
 *   val1 | val2 | val3
 */
function rows_to_table(rows, limit) {
  limit = limit || 150;
  if (!Array.isArray(rows)) {
    if (rows && rows.error)
      return t('执行出错：', 'Error: ') + rows.error;
    if (rows && rows.affected_rows !== undefined) {
      if (rows.columns)
        return t('（查询结果为空）', '(No results)');
      return t('执行成功，影响行数：', 'Success, rows affected: ') + rows.affected_rows;
    }
    return JSON.stringify(rows);
  }
  if (rows.length === 0) return t('（查询结果为空）', '(No results)');

  var cols = Object.keys(rows[0]);
  var cap  = Math.min(rows.length, limit);

  // Calculate column widths (min header width vs max data width, capped at 40)
  var widths = [];
  for (var c = 0; c < cols.length; c++) {
    var w = Math.min(String(cols[c]).length, 40);
    for (var i = 0; i < cap; i++) {
      var v = rows[i][cols[c]];
      var vs = (v === null ? 'NULL' : String(v));
      w = Math.max(w, Math.min(vs.length, 40));
    }
    widths.push(w);
  }

  var out = [t('共 ', 'Total ') + rows.length + t(' 条：', ' rows:')];

  // Header
  var header = [];
  for (c = 0; c < cols.length; c++) {
    header.push(pad_right(String(cols[c]), widths[c]));
  }
  out.push(header.join(' | '));

  // Separator
  var sep = [];
  for (c = 0; c < cols.length; c++) {
    sep.push(repeat_str('-', widths[c]));
  }
  out.push(sep.join('-|-'));

  // Data rows
  for (var r = 0; r < cap; r++) {
    var line = [];
    for (c = 0; c < cols.length; c++) {
      var val = rows[r][cols[c]];
      line.push(pad_right(val === null ? 'NULL' : String(val), widths[c]));
    }
    out.push(line.join(' | '));
  }

  if (rows.length > cap)
    out.push(t('（仅展示前 ', '(Showing first ') + cap + t(' 条）', ' rows)'));

  return out.join('\n');
}

function pad_right(s, len) {
  s = String(s);
  if (s.length >= len) return s.substring(0, len);
  return s + repeat_str(' ', len - s.length);
}

function repeat_str(ch, n) {
  return new Array(n + 1).join(ch);
}

function think_suffix() {
  return A.last_think ? '\n[think]\n' + A.last_think : '';
}

function strip_think_tags(text) {
  if (!text) return text;
  return text.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
}

function est_tok(s) {
  s = String(s || '');
  if (!s.length) return 0;

  var cjk_count = 0;
  /* \u3000-\u303f: CJK \u3400-\u9fff: CJK Unified Ideographs Extension A
  \uff00-\uffef: Fullwidth and Halfwidth Forms */
  var re = /[\u3000-\u303f\u3400-\u9fff\uff00-\uffef]/g;
  var m = s.match(re);
  if (m) cjk_count = m.length;

  var other_count = s.length - cjk_count;

  return Math.ceil(cjk_count / 1.5 + other_count / 3);
}

function gen_query_id() { return Math.random().toString(36).substring(2, 10); }

function current_principal_prefix() {
  try {
    var rows = query_checked(
      "SELECT LOWER(SUBSTRING(SHA2(CURRENT_USER(),256),1,16)) AS principal_prefix"
    );
    if (Array.isArray(rows) && rows.length && rows[0].principal_prefix)
      return String(rows[0].principal_prefix);
  } catch (e) {}
  return '';
}

/* Keep the external conversation UUID separate from the internal persistence
 * key. A 16-hex principal prefix + ':' + 47-hex conversation hash fits the
 * existing VARCHAR(64) key while preventing one SQL principal from opening
 * another principal's memory/review state by guessing conversation_id. */
function principal_scope_conversation_id(raw_id) {
  var raw = String(raw_id || '');
  var prefix = current_principal_prefix();
  if (!prefix) {
    /* Fail isolated rather than falling back to an unscoped shared key. */
    return 'isolated-' + gen_query_id() + '-' + gen_query_id();
  }

  var scoped_re = /^([0-9a-f]{16}):([0-9a-f]{47})$/i;
  var m = raw.match(scoped_re);
  if (m && String(m[1]).toLowerCase() === prefix) return raw.toLowerCase();

  try {
    var rows = query_checked(
      "SELECT CONCAT('" + esc(prefix) + "',':'," +
      "LOWER(SUBSTRING(SHA2('" + esc(raw) + "',256),1,47))) AS scoped_id"
    );
    if (Array.isArray(rows) && rows.length && rows[0].scoped_id)
      return String(rows[0].scoped_id);
  } catch (e) {}

  return 'isolated-' + gen_query_id() + '-' + gen_query_id();
}

function scoped_principal_prefix(scoped_id) {
  var m = String(scoped_id || '').match(/^([0-9a-f]{16}):/i);
  return m ? String(m[1]).toLowerCase() : '';
}
