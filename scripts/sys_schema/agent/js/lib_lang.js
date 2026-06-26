//@include lib_state.js

function detect_lang(text) {
  return /[\u4e00-\u9fff\u3400-\u4dbf]/.test(String(text || '')) ? 'zh' : 'en';
}

function t(zh, en) { return A.lang === 'zh' ? zh : en; }

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/\\/g, '\\\\')
    .replace(/'/g,  "\\'");
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

function est_tok(s) { return Math.ceil((s || '').length / 3); }
function gen_query_id() { return Math.random().toString(36).substring(2, 10); }