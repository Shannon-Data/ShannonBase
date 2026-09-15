//@include lib_tool_registry.js
//@include lib_memory_registry.js

/* Memory as tools.
 *
 * scope:'memory' marks these as writing only to mysql.agent_* — never to user
 * data — and requiresTx:false says they have no transaction to join.
 * evaluate_step_policy() uses that pair to let remember_fact through
 * require_approval_for_write, which exists to protect user tables.
 * forget_memory keeps risk:'high', so it still lands in the approval gate. */

var MEMORY_TOOLS_GROUP = {
  zh: '【记忆工具】跨会话的长期事实，按 SQL principal 隔离。',
  en: '[Memory Tools] Cross-session long-term facts, isolated per SQL principal.'
};

/* ----------------------------------------------------------- remember_fact */
register_tool({
  name: 'remember_fact', order: 200, category: 'memory', group: MEMORY_TOOLS_GROUP,
  scope: 'memory', requiresTx: false,
  write: true, ddl: false, risk: 'low', transactional: false,
  doc: { zh: '记住一条长期事实/偏好（如"用户偏好中文回答"、"事实表在 sales_db"）\n' +
             '写入 agent_semantic_fact，按 principal 隔离，重复写入只累加使用次数而不产生重复行\n' +
             '只在用户明确表达了一个跨会话稳定的偏好或事实时才调用，不要拿它记录一次性的查询结果',
         en: 'Persist a long-term fact or preference (e.g. "user prefers Chinese answers", ' +
             '"fact tables live in sales_db")\n' +
             'Stored in agent_semantic_fact, isolated per principal; a repeat write bumps a ' +
             'usage counter instead of creating a duplicate row\n' +
             'Only call it for a stable cross-session preference or fact — never to record a ' +
             'one-off query result' },
  example: { zh: { statement: '用户偏好中文回答' },
             en: { statement: 'the user prefers concise answers' } },
  /* subject/predicate/object are optional structure alongside the sentence,
   * not a replacement for it.  The columns, and the idx_principal_pred index
   * over them, have existed since the table was created, and forget_memory
   * has always offered a `predicate` filter -- but no caller ever supplied
   * the values, so every one of those rows was NULL and that filter could
   * only ever match nothing.
   *
   * Dedup deliberately stays on the statement: uk_fact is
   * (principal_prefix, statement(191)) and mem_long_write_fact()'s ON
   * DUPLICATE KEY UPDATE is what turns a repeat write into a use_count bump.
   * Adding a second unique key over (principal_prefix, subject, predicate)
   * would give one INSERT two keys to violate, and ON DUPLICATE KEY UPDATE
   * acts on whichever it hits first -- so "same subject+predicate replaces"
   * and "same sentence dedups" would silently fight.  Picking between them
   * is a schema decision with a migration attached, not a passenger on this
   * change. */
  args: { type: 'object', required: ['statement'],
          properties: { statement:  { type: 'string', minLength: 4, maxLength: 512 },
                        scope:      { type: 'string', maxLength: 64 },
                        subject:    { type: 'string', maxLength: 128 },
                        predicate:  { type: 'string', maxLength: 64 },
                        object:     { type: 'string', maxLength: 512 },
                        confidence: { type: 'integer', minimum: 0, maximum: 100 } } },
  displaySql: display_remember_fact,
  messages: {
    'statement#required':  { zh: 'remember_fact 缺少 statement 参数（要记住的事实，自然语言一句话）',
                             en: 'remember_fact missing statement argument (the fact to remember, one sentence)' },
    'statement#type':      { zh: 'remember_fact 缺少 statement 参数（要记住的事实，自然语言一句话）',
                             en: 'remember_fact missing statement argument (the fact to remember, one sentence)' },
    'statement#minLength': { zh: 'remember_fact 的 statement 过短，请写成完整的一句话。',
                             en: 'remember_fact statement is too short; write a complete sentence.' },
    'statement#maxLength': { zh: 'remember_fact 的 statement 超过 512 字符，请精简为一句话。',
                             en: 'remember_fact statement exceeds 512 characters; condense it to one sentence.' },
    'confidence#maximum':  { zh: 'remember_fact 的 confidence 必须在 0-100 之间',
                             en: 'remember_fact confidence must be between 0 and 100' },
    'confidence#minimum':  { zh: 'remember_fact 的 confidence 必须在 0-100 之间',
                             en: 'remember_fact confidence must be between 0 and 100' }
  },
  handler: impl_remember_fact
});

function impl_remember_fact(args, ctx) {
  var res = MEM.long.write_fact(String(args.statement || ''),
                                { scope:      args.scope || '',
                                  subject:    args.subject,
                                  predicate:  args.predicate,
                                  object:     args.object,
                                  confidence: args.confidence },
                                get_memory_options(ctx.chat_opt));
  if (!res.ok)
    return { ok: false, error: res.error || 'remember_fact_failed',
             response: t('记忆写入失败：', 'Failed to persist the fact: ') + (res.error || '') };
  return { ok: true,
           response: t('已记住：', 'Remembered: ') + String(args.statement || '') +
                     (res.degraded ? t('（未生成向量，语义召回不可用）',
                                       ' (stored without a vector; semantic recall unavailable)') : ''),
           memory_degraded: !!res.degraded };
}

/* ----------------------------------------------------------- recall_memory */
register_tool({
  name: 'recall_memory', order: 201, category: 'memory',
  scope: 'memory', requiresTx: false,
  readOnly: true, write: false, ddl: false, transactional: false,
  doc: { zh: '按语义检索自己此前记住的长期事实与历史对话片段（仅限当前 SQL 用户）\n' +
             '当用户提到"上次""之前说过""我的偏好"而当前上下文里没有答案时使用',
         en: 'Semantically recall this principal\'s own long-term facts and past conversation ' +
             'snippets\n' +
             'Use it when the user refers to "last time" / "as I said before" / "my preference" and ' +
             'the current context does not contain the answer' },
  example: { question: '...' },
  args: { type: 'object', required: ['question'],
          properties: { question: { type: 'string', minLength: 2 },
                        top_k:    { type: 'integer', minimum: 1, maximum: 20 } } },
  messages: {
    '*#required': { zh: 'recall_memory 缺少 question 参数', en: 'recall_memory missing question argument' },
    '*#type':     { zh: 'recall_memory 缺少 question 参数', en: 'recall_memory missing question argument' }
  },
  handler: impl_recall_memory
});

function impl_recall_memory(args, ctx) {
  var mo = get_memory_options(ctx.chat_opt);
  var q  = String(args.question || '');
  if (args.top_k) {
    mo = mem_merge_defaults(mo, { long_term: { episodic_top_k: Number(args.top_k),
                                               semantic_top_k: Number(args.top_k) } });
  }
  var lines = [];

  var facts = MEM.long.recall_facts(q, mo);
  for (var i = 0; i < facts.length; i++)
    lines.push(t('事实：', 'Fact: ') + facts[i].statement +
               ' (confidence ' + facts[i].confidence + ')');

  var ep = MEM.long.recall_turns(q, mo);
  if (ep && ep.ok && ep.text && String(ep.text).trim())
    lines.push(t('历史片段：\n', 'Past turns:\n') + compress(String(ep.text), 1200));

  if (!lines.length)
    return { ok: true, response: t('没有检索到相关的长期记忆。', 'No relevant long-term memory found.'),
             memory_degraded: !!A.memory_degraded };
  return { ok: true, response: lines.join('\n'), memory_degraded: !!A.memory_degraded };
}

/* ------------------------------------------------------------ forget_memory
 * hidden:true — deleting memory is an operator action, not something the
 * model should volunteer.  It stays registered (and validated, and gated by
 * risk:'high') so an explicit call still works and still needs approval. */
register_tool({
  name: 'forget_memory', order: 202, category: 'memory', hidden: true,
  scope: 'memory', requiresTx: false,
  write: true, ddl: false, risk: 'high', transactional: false,
  doc: { zh: '删除此前记住的长期事实（按 fact_id / scope / predicate / 关键词过滤）',
         en: 'Delete previously remembered long-term facts (filter by fact_id / scope / ' +
             'predicate / keyword)' },
  example: { contains: '...' },
  args: { type: 'object',
          properties: { fact_id:   { type: 'integer', minimum: 1 },
                        scope:     { type: 'string' },
                        predicate: { type: 'string' },
                        contains:  { type: 'string' },
                        all:       { type: 'boolean' } } },
  displaySql: display_forget_memory,
  handler: impl_forget_memory
});

/* Approval previews.  These tools take a sentence and a filter, not SQL, so
 * without a rendering the review prompt would show an empty "SQL:" line for
 * the one memory operation that is destructive. */
function display_remember_fact(args) {
  var cols = ['principal_prefix', 'scope', 'statement'];
  var vals = ['<this principal>',
              "'" + String(args.scope || '') + "'",
              "'" + String(args.statement || '').substring(0, 200) + "'"];
  var trip = ['subject', 'predicate', 'object'];
  for (var i = 0; i < trip.length; i++) {
    if (args[trip[i]] === undefined || args[trip[i]] === null || args[trip[i]] === '') continue;
    cols.push(trip[i]);
    vals.push("'" + String(args[trip[i]]).substring(0, 128) + "'");
  }
  return "INSERT INTO mysql.agent_semantic_fact(" + cols.join(', ') + ") " +
         "VALUES (" + vals.join(', ') + ")";
}

function display_forget_memory(args) {
  var conds = ['principal_prefix = <this principal>'];
  if (args.fact_id)   conds.push('fact_id = ' + Number(args.fact_id));
  if (args.scope)     conds.push("scope = '" + String(args.scope) + "'");
  if (args.predicate) conds.push("predicate = '" + String(args.predicate) + "'");
  if (args.contains)  conds.push("statement LIKE '%" + String(args.contains) + "%'");
  if (args.all)       conds.push('<all facts for this principal>');
  return 'DELETE FROM mysql.agent_semantic_fact WHERE ' + conds.join(' AND ');
}

function impl_forget_memory(args, ctx) {
  var res = MEM.long.forget(args || {}, get_memory_options(ctx.chat_opt));
  if (!res.ok) {
    if (res.error === 'forget_requires_filter')
      return { ok: false, error: res.error,
               response: t('forget_memory 需要指定过滤条件（fact_id / scope / predicate / contains），' +
                           '或显式传 all=true 才会清空。',
                           'forget_memory needs a filter (fact_id / scope / predicate / contains), ' +
                           'or an explicit all=true to wipe everything.') };
    return { ok: false, error: res.error || 'forget_memory_failed',
             response: t('记忆删除失败：', 'Failed to forget: ') + (res.error || '') };
  }
  return { ok: true, response: t('已删除 ', 'Forgot ') + res.removed +
                               t(' 条长期记忆。', ' long-term memory entries.') };
}
