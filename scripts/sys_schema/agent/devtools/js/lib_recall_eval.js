//@include lib_memory_registry.js

/* ===========================================================================
 * The recall-quality set.
 *
 * Four separate decisions were all blocked on the same missing artifact:
 *
 *   - the ranking weights in MEM_DEFAULTS.long_term.ranking, which are set to
 *     relevance-only because nothing said what recency or usage are worth;
 *   - retrieval.mode and the vector/lexical fusion weights;
 *   - retrieval.min_lex_ratio, the floor that makes the ngram parser usable;
 *   - whether a retrieval-backed skill store can ship at all, since a missed
 *     skill fails harder than a missed memory.
 *
 * None of them can be argued from taste, because every one of them changes
 * what the model is shown on every turn.  They need a set of questions with
 * known answers and a number.  This is that set.
 *
 * It is built to be discriminating rather than merely large.  Three kinds of
 * query, chosen so that a retriever that only does one thing scores badly:
 *
 *   lexical   a distinctive token -- a table name, a column, an error code,
 *             an id.  Embeddings carry almost no signal for these: fact_sales
 *             and fact_orders embed to nearly the same point.
 *   semantic  a paraphrase sharing no token with its answer.  Full-text
 *             search cannot reach these at all.
 *   mixed     both signals present, which is the ordinary case.
 *
 * A fusion that is worth its extra query has to beat both single retrievers
 * on the whole set, not just on the half that suits it.
 *
 * Runs lexical-only without an embedding model, which is the normal state of
 * a CI machine -- see the `modes` argument to recall_eval_score().
 * ======================================================================== */

/* Documents.  `tag` is the identity the scorer matches on; it is stored in
 * the subject column rather than inside the statement, so it cannot itself
 * become a lexical match for a query. */
function recall_eval_docs() {
  return [
    /* --- distinctive identifiers: lexical should win ------------------- */
    { tag: 'r01', text: '销售事实表 fact_sales 存放在 sales_db，按 tx_date 做范围分区' },
    { tag: 'r02', text: '维度表 dim_customer 的主键是 customer_sk，不是 customer_id' },
    { tag: 'r03', text: 'orders 表的 status 列上有索引 idx_status，写入热点集中在 status=pending' },
    { tag: 'r04', text: 'The revenue_items table stores amounts in cents, not in the base currency unit' },
    { tag: 'r05', text: 'ER_LOCK_WAIT_TIMEOUT in the batch_load job is usually innodb_lock_wait_timeout set to 5 seconds' },
    { tag: 'r06', text: 'Report job nightly_rollup runs at 02:15 UTC and needs stg_orders loaded first' },
    { tag: 'r07', text: '列 cost_items.cogs_amount 只有在 cost_type 等于 cogs 时才有意义' },
    { tag: 'r08', text: 'customer 88123 是内部测试账号，做统计时必须排除掉' },

    /* --- paraphrase targets: vector should win ------------------------- */
    { tag: 'r09', text: '用户希望所有回答都用中文，并且先给结论再给细节' },
    { tag: 'r10', text: 'The user prefers large result sets summarised rather than printed in full' },
    { tag: 'r11', text: '团队约定：任何会删除数据的操作，都要先在测试库演练一遍' },
    { tag: 'r12', text: 'Schema changes are only applied during the Friday evening maintenance window' },
    { tag: 'r13', text: '报表口径以财务部给出的定义为准，不要自己推断字段含义' },
    { tag: 'r14', text: 'When a query becomes slow, look at the execution plan before adding an index' },

    /* --- mixed --------------------------------------------------------- */
    { tag: 'r15', text: 'shop 库的 orders 与 customers 通过 customer_id 关联，一对多' },
    { tag: 'r16', text: 'The RAPID secondary engine must be loaded before ml_train can use a table' },
    { tag: 'r17', text: '分析月度收入时要用 tx_date，而不是 created_at —— 后者是写入时间' },
    { tag: 'r18', text: 'Deleted rows are kept in orders_archive for 90 days before they are purged' },
    { tag: 'r19', text: '员工表 hr.employee 含敏感字段 salary，查询前需要脱敏' },
    { tag: 'r20', text: 'The staging tables named stg_ are truncated and rebuilt every night' },
    { tag: 'r21', text: '库存表 inventory 的 quantity 可能为负数，表示超卖' },
    { tag: 'r22', text: 'Primary key lookups on shop.orders are fast; scanning by status is not' },
    { tag: 'r23', text: '财务季度从二月开始，和自然季度不一样' },
    { tag: 'r24', text: 'The embedding model used across this instance is multilingual-e5-small' },

    /* --- near misses ---------------------------------------------------
     * Distractors, not filler.  Each one is deliberately close to a real
     * answer along exactly one axis, so a retriever that matches loosely is
     * punished rather than rewarded:
     *   r25 embeds almost identically to r01 (fact_orders / fact_sales) and
     *       is lexically distinct -- the case for keeping a lexical leg;
     *   r26 shares r02's shape and its *_sk vocabulary;
     *   r27 shares r06's rollup vocabulary with a different schedule;
     *   r28 makes "which tables are rebuilt nightly" genuinely have two
     *       answers, so recall@k can distinguish finding one from both. */
    { tag: 'r25', text: '明细事实表 fact_orders 存放在 ops_db，和 fact_sales 不是同一张表' },
    { tag: 'r26', text: 'The dim_product dimension is keyed by product_sk, mirroring dim_customer' },
    { tag: 'r27', text: 'Report job weekly_rollup runs Monday morning and is unrelated to nightly_rollup' },
    { tag: 'r28', text: 'stg_customers is also a staging table rebuilt every night' }
  ];
}

/* Queries.  `kind` records what the query is meant to exercise, so the report
 * can say *where* a retriever is losing rather than only that it is. */
function recall_eval_queries() {
  return [
    { q: 'fact_sales',                              kind: 'lexical',  rel: ['r01'] },
    { q: 'customer_sk',                             kind: 'lexical',  rel: ['r02'] },
    { q: 'idx_status',                              kind: 'lexical',  rel: ['r03'] },
    { q: 'ER_LOCK_WAIT_TIMEOUT',                    kind: 'lexical',  rel: ['r05'] },
    { q: 'nightly_rollup',                          kind: 'lexical',  rel: ['r06'] },
    { q: 'cogs_amount',                             kind: 'lexical',  rel: ['r07'] },
    { q: '88123',                                   kind: 'lexical',  rel: ['r08'] },
    { q: 'orders_archive',                          kind: 'lexical',  rel: ['r18'] },
    { q: 'multilingual-e5-small',                   kind: 'lexical',  rel: ['r24'] },
    { q: 'stg_orders',                              kind: 'lexical',  rel: ['r06', 'r20'] },

    { q: '回答应该用什么语言',                        kind: 'semantic', rel: ['r09'] },
    { q: 'should I print every row of a big result', kind: 'semantic', rel: ['r10'] },
    { q: '删数据之前要先做什么',                      kind: 'semantic', rel: ['r11', 'r18'] },
    { q: 'when am I allowed to run an ALTER TABLE',  kind: 'semantic', rel: ['r12'] },
    { q: '字段含义不确定的时候怎么办',                 kind: 'semantic', rel: ['r13'] },
    { q: 'my query got slower, what should I check first', kind: 'semantic', rel: ['r14'] },
    { q: '有没有不能给普通人看的数据',                 kind: 'semantic', rel: ['r19'] },
    { q: 'why would a stock count be below zero',    kind: 'semantic', rel: ['r21'] },

    { q: 'orders 和 customers 怎么 join',            kind: 'mixed',    rel: ['r15', 'r22'] },
    { q: 'ml_train 之前要做什么准备',                 kind: 'mixed',    rel: ['r16'] },
    { q: '按月统计收入应该用哪个日期列',               kind: 'mixed',    rel: ['r17', 'r01'] },
    { q: 'revenue_items 的金额单位是什么',            kind: 'mixed',    rel: ['r04'] },
    { q: 'dim_customer 主键',                        kind: 'mixed',    rel: ['r02'] },
    { q: '财务季度是怎么算的',                        kind: 'mixed',    rel: ['r23'] },
    { q: 'staging 表什么时候重建',                    kind: 'mixed',    rel: ['r20'] },
    { q: 'salary 字段能直接查吗',                     kind: 'mixed',    rel: ['r19'] },
    { q: 'pending 订单为什么查得慢',                  kind: 'mixed',    rel: ['r03', 'r22'] },
    { q: 'RAPID 引擎',                               kind: 'mixed',    rel: ['r16'] },

    /* Queries that only the distractors make meaningful. */
    { q: 'fact_orders 在哪个库',                      kind: 'lexical',  rel: ['r25'] },
    { q: 'product_sk',                               kind: 'lexical',  rel: ['r26'] },
    { q: 'weekly_rollup 什么时候跑',                  kind: 'lexical',  rel: ['r27'] },
    { q: 'stg_customers',                            kind: 'lexical',  rel: ['r28'] },
    { q: '哪些表是每晚重建的',                        kind: 'semantic', rel: ['r20', 'r28'] },
    { q: 'which tables should statistics leave out',  kind: 'semantic', rel: ['r08'] },
    { q: '事实表都放在哪里',                          kind: 'mixed',    rel: ['r01', 'r25'] },
    { q: 'surrogate key columns',                    kind: 'mixed',    rel: ['r02', 'r26'] },
    { q: 'rollup 作业有哪些',                         kind: 'mixed',    rel: ['r06', 'r27'] },
    { q: '超卖',                                      kind: 'mixed',    rel: ['r21'] }
  ];
}

function recall_eval_scope(label) { return 'rqs:' + String(label || 'default'); }

/* Seed the set as ordinary long-term facts, so retrieval sees exactly the
 * table shape it sees in production rather than a fixture that happens to be
 * easier.  scope carries the run label, which is also what confines scoring
 * and cleanup to this run's rows. */
function recall_eval_seed(label, opt) {
  var mo    = opt || get_memory_options(get_chat_options());
  var scope = recall_eval_scope(label);
  var docs  = recall_eval_docs();
  var ok = 0, failed = 0;
  for (var i = 0; i < docs.length; i++) {
    var res = mem_long_write_fact(docs[i].text,
                                  { scope: scope, subject: docs[i].tag, confidence: 80 }, mo);
    if (res && res.ok) ok++; else failed++;
  }
  return { seeded: ok, failed: failed, total: docs.length };
}

function recall_eval_cleanup(label) {
  var prefix = mem_principal_prefix();
  if (!prefix) return 0;
  var scope = recall_eval_scope(label);
  try {
    var r = query_checked("DELETE FROM mysql.agent_semantic_fact" +
                          " WHERE principal_prefix='" + esc(prefix) + "'" +
                          "   AND scope='" + esc(scope) + "'");
    return (r && r.affected_rows) ? Number(r.affected_rows) : 0;
  } catch (e) { return 0; }
}

/* One query, one retrieval mode, top-k tags in rank order. */
function recall_eval_run_one(mode, question, scope, topK, mo) {
  var prefix = mem_principal_prefix();
  var opt = mem_merge_defaults(mo, { retrieval: { mode: mode },
                                     /* The pool has to be at least topK or a
                                      * mode is being scored on a shorter list
                                      * than its rivals. */
                                     long_term: { semantic_top_k: topK } });
  var filters = {
    documents:        [prefix],
    isolation_column: 'principal_prefix',
    select_list:      'fact_id, statement, subject',
    extra_where:      "scope='" + esc(scope) + "'",
    max_distance:     Number(mo.long_term.semantic_max_distance || 0.6)
  };
  var res;
  if (mode === 'lexical')
    res = mem_lexical_search('L3', 'mysql.agent_semantic_fact',
                             { segment: 'statement', segment_embedding: 'embedding' },
                             question, filters, topK, opt);
  else
    res = mem_hybrid_search('L3', 'mysql.agent_semantic_fact',
                            { segment: 'statement', segment_embedding: 'embedding' },
                            question, filters, topK, opt);

  var tags = [];
  var rows = (res && res.rows) || [];
  for (var i = 0; i < rows.length; i++) tags.push(String(rows[i].subject || ''));
  return { ok: !!(res && res.ok), tags: tags };
}

/* recall@k and MRR, macro-averaged over queries and reported per query kind.
 *
 * recall@k rather than precision: the memory block shows the model a handful
 * of facts, and the cost of one irrelevant fact among them is a few tokens,
 * while the cost of a missing one is a wrong answer.  MRR is reported
 * alongside because a relevant fact at rank 1 and at rank 3 are not equally
 * useful when the block is later trimmed to a token budget.
 *
 * `modes` is a comma-separated list. 'lexical' alone needs no embedding
 * model, which is what makes this runnable in CI. */
function recall_eval_score(label, modes, topK, opt) {
  var mo    = opt || get_memory_options(get_chat_options());
  var scope = recall_eval_scope(label);
  var qs    = recall_eval_queries();
  topK      = Math.max(1, Number(topK || 3));

  var mode_list = String(modes || 'lexical').split(',');
  var out = [];

  for (var m = 0; m < mode_list.length; m++) {
    var mode = String(mode_list[m]).trim();
    if (!mode) continue;

    var agg = {}, kinds = ['lexical', 'semantic', 'mixed', 'all'];
    for (var k = 0; k < kinds.length; k++)
      agg[kinds[k]] = { n: 0, recall: 0, mrr: 0, empty: 0 };

    for (var i = 0; i < qs.length; i++) {
      var run   = recall_eval_run_one(mode, qs[i].q, scope, topK, mo);
      var found = 0, first_rank = 0;
      for (var r = 0; r < run.tags.length; r++) {
        var is_rel = false;
        for (var j = 0; j < qs[i].rel.length; j++)
          if (qs[i].rel[j] === run.tags[r]) { is_rel = true; break; }
        if (!is_rel) continue;
        found++;
        if (!first_rank) first_rank = r + 1;
      }
      var recall = found / qs[i].rel.length;
      var rr     = first_rank ? (1 / first_rank) : 0;

      var buckets = [qs[i].kind, 'all'];
      for (var b = 0; b < buckets.length; b++) {
        var a = agg[buckets[b]];
        a.n++; a.recall += recall; a.mrr += rr;
        if (!run.tags.length) a.empty++;
      }
    }

    for (var kk = 0; kk < kinds.length; kk++) {
      var ag = agg[kinds[kk]];
      if (!ag.n) continue;
      out.push([mode + '.' + kinds[kk],
                'queries=' + ag.n +
                ' recall@' + topK + '=' + recall_eval_round(ag.recall / ag.n) +
                ' mrr=' + recall_eval_round(ag.mrr / ag.n) +
                ' empty=' + ag.empty]);
    }
  }
  return out;
}

/* Two decimals, without toFixed: JerryScript has it, but rounding here keeps
 * the printed value identical across engines and makes the mysql-test result
 * file stable. */
function recall_eval_round(x) {
  var v = Math.round(Number(x) * 100) / 100;
  var s = String(v);
  if (s.indexOf('.') === -1) return s + '.00';
  var frac = s.split('.')[1];
  return (frac.length === 1) ? s + '0' : s;
}
