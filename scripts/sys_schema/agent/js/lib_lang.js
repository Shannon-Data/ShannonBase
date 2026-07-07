//@include lib_state.js

function detect_lang(text) {
  return /[\u4e00-\u9fff\u3400-\u4dbf]/.test(String(text || '')) ? 'zh' : 'en';
}

function t(zh, en) { return A.lang === 'zh' ? zh : en; }

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/\\/g, '\\\\')
    .replace(/\u0000/g, '\\0')
    .replace(/\n/g, '\\n')
    .replace(/\r/g, '\\r')
    .replace(/\x1a/g, '\\Z')
    .replace(/'/g,  "\\'")
    .replace(/"/g,  '\\"');
}

function classify_request(text) {
  var t = String(text || '').toLowerCase();
  if (/有哪些表|所有表|列出.*表|show.?tables|list.*tables|字段|列信息|结构|describe.*table|索引|外键|schema/.test(t))
    return 'schema';
  if (/锁|死锁|线程|慢查询|事务|进程|连接|buffer|redo|表空间|变量|状态/.test(t))
    return 'diagnose';
  if (/group\s+by|统计|汇总|排名|占比|趋势|同比|环比|sum\(|avg\(|count\(/.test(t))
    return 'analytics';
  if (/insert|update|delete|创建|删除|修改|写入/.test(t))
    return 'write';
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

function query(sql) {
  try   { return JSON.parse(sys.exec_sql(sql)); }
  catch (e) { return { error: String(e) }; }
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