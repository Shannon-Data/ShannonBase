//@include lib_ml.js

function save_chat_options(chat_opt) {
  A.cached_chat_opt = chat_opt;
  try {
    if (chat_opt.model_options && chat_opt.model_options.api_key) {
      save_secret_api_key(chat_opt.model_options.api_key);
    }

    var safe = JSON.parse(JSON.stringify(chat_opt));
    if (safe.model_options && safe.model_options.api_key)
      safe.model_options.api_key = '***';
    sys.exec_sql("SET @chat_options = '" + esc(JSON.stringify(safe)) + "'");
  } catch(e) {}
}

function update_chat_history(chat_opt, user_msg, bot_msg) {
  if (!Array.isArray(chat_opt.chat_history)) chat_opt.chat_history = [];
  var entry = {
    user_message: user_msg, chat_bot_message: bot_msg,
    chat_query_id: chat_opt.chat_query_id || gen_query_id()
  };
  if (chat_opt.re_run && chat_opt.chat_history.length > 0)
    chat_opt.chat_history[chat_opt.chat_history.length - 1] = entry;
  else
    chat_opt.chat_history.push(entry);
  /* Single authority for the window size: get_memory_options() folds the
   * legacy history_length knob into short_term.recent_turns, so the default
   * is no longer spelled independently in three files. */
  var max_len = Number(get_memory_options(chat_opt).short_term.recent_turns);
  if (!isFinite(max_len) || max_len < 0) max_len = 3;
  /* slice(-0) is slice(0), i.e. the whole array — so history_length:0
   * ("keep no history") silently let chat_history grow without bound.
   * Slice from an absolute offset instead. */
  if (chat_opt.chat_history.length > max_len)
    chat_opt.chat_history =
      chat_opt.chat_history.slice(chat_opt.chat_history.length - max_len);
  chat_opt.re_run = false;
  chat_opt.chat_query_id = gen_query_id();
  return chat_opt;
}

function chat_history_to_text(chat_opt) {
  if (!Array.isArray(chat_opt.chat_history) || !chat_opt.chat_history.length) return '';
  return chat_opt.chat_history.map(function(h) {
    return t('用户：', 'User: ')      + (h.user_message    || '') + '\n' +
           t('助手：', 'Assistant: ') + (h.chat_bot_message || '');
  }).join('\n');
}

/* Legacy accessor, kept because plugins and older call sites use it.
 *
 * Two fixes over the original: it orders by `seq`, not `created_at` (a
 * TIMESTAMP resolves to whole seconds, so several turns inside one second
 * came back in arbitrary order), and a read failure is no longer indis-
 * tinguishable from an empty conversation -- MEM.short.load() records the
 * failure in mysql.agent_memory_audit and flags A.memory_degraded. */
function get_history(conv_id, n) {
  var mo = get_memory_options(get_chat_options());
  if (n) {
    mo = mem_merge_defaults(mo, { short_term: { recent_turns: Math.ceil(Number(n) / 2) } });
  }
  var loaded = mem_short_load(conv_id, mo);
  return mem_turns_to_text(loaded.turns);
}

/* Rebuild @chat_options.chat_history from the server-side store.
 *
 * Pairing is by turn_no now.  The original walked the rows in order and
 * paired each `user` row with whatever happened to follow it, so a turn whose
 * assistant row was never written -- the exact orphan the old two-INSERT
 * persist_turn could leave behind -- silently mis-paired that user message
 * with the *next* turn's answer.  Turns are written atomically now, but
 * pairing by turn_no also makes the recovery correct for rows written by an
 * older server. */
function recover_chat_history_from_memory(conv_id, max_turns) {
  max_turns = max_turns || 3;
  try {
    /* Select the last max_turns *turns*, not the last max_turns*2 rows: a
     * turn is two rows only when both legs were written, and a legacy orphan
     * row (the pre-atomic persist_turn could leave one) would otherwise eat
     * the budget of a whole turn and recover one fewer than asked for. */
    var rows = query(
      "SELECT role, content, turn_no, seq FROM mysql.agent_memory" +
      " WHERE conversation_id='" + esc(conv_id) + "'" +
      "   AND turn_no IN (SELECT t FROM (" +
      "         SELECT DISTINCT turn_no AS t FROM mysql.agent_memory" +
      "          WHERE conversation_id='" + esc(conv_id) + "'" +
      "          ORDER BY turn_no DESC LIMIT " + Number(max_turns) +
      "       ) w)" +
      " ORDER BY seq ASC"
    );
    if (!Array.isArray(rows) || !rows.length) return [];

    var by_turn = {}, order = [];
    for (var i = 0; i < rows.length; i++) {
      var tn = String(rows[i].turn_no || 0);
      if (!Object.prototype.hasOwnProperty.call(by_turn, tn)) {
        by_turn[tn] = { user_message: '', chat_bot_message: '',
                        chat_query_id: gen_query_id() };
        order.push(tn);
      }
      if (rows[i].role === 'user') by_turn[tn].user_message     = rows[i].content || '';
      else                         by_turn[tn].chat_bot_message = rows[i].content || '';
    }

    var history = [];
    for (var o = 0; o < order.length && history.length < max_turns; o++) {
      var entry = by_turn[order[o]];
      if (entry.user_message) history.push(entry);
    }
    return history;
  } catch(e) { return []; }
}

/* Persist one turn.
 *
 * Delegates to MEM.short.append_turn(), which writes the user and assistant
 * rows in a single INSERT under one monotonic `seq`.  The previous
 * implementation issued two independent INSERTs wrapped in bare
 * `try{...}catch(e){}`, so a failure between them left an orphan user row and
 * a failure of either left no trace at all.
 *
 * `thought` still carries a route marker at most call sites ('review:approve',
 * 'catalog:<sql>', 'hw_mode:<mode>'), so when no explicit route is passed we
 * derive it from that prefix rather than editing a dozen call sites.  The
 * route decides whether the turn is worth embedding: approval prompts are UI
 * chatter, and embedding them doubled ML_EMBED_ROW cost for no recall value. */
/* One id per invocation, minted on first use.  It is what joins this turn's
 * rows in mysql.agent_memory to its cost row in mysql.agent_memory_audit.
 * mysql.agent_sql_trace is still joined by (conversation_id, turn_no), and
 * its turn_no is the loop iteration rather than the conversation turn --
 * closing that last gap needs a column on the trace table, which is bootstrap
 * schema and a separate decision. */
function current_turn_id() {
  if (!A.turn_id) A.turn_id = gen_query_id() + gen_query_id();
  return A.turn_id;
}

/* What did this turn cost?  Written once per turn rather than per audit row,
 * so "tokens, latency and model calls for turn X" is one row to read and no
 * existing audit row changes shape. */
function log_turn_cost(route) {
  var c = A.cost;
  if (!c || !c.llm_calls) return;
  mem_log_audit('L1', 'cost', 'ml_generate',
                'turn=' + current_turn_id() + ' route=' + String(route || '') +
                ' prompt_tokens=' + c.prompt_tokens +
                ' completion_tokens=' + c.completion_tokens +
                ' mem_block_tokens=' + Number(A.mem_block_tokens || 0),
                c.llm_calls, c.llm_ms, '');
}

function persist_turn(conv_id, user_msg, bot_msg, thought, route) {
  if (!route) {
    var th = String(thought || '');
    if      (th.indexOf('review:')  === 0) route = 'review';
    else if (th.indexOf('catalog:') === 0) route = 'catalog';
    else if (th.indexOf('hw_mode:') === 0) route = 'rag';
    else                                   route = 'agent_loop';
  }
  MEM.short.append_turn(conv_id, user_msg, bot_msg, thought, { route: route });
  log_turn_cost(route);

  /* Surface degradation to the caller regardless of whether this branch
   * happened to call save_chat_options() before or after persisting. */
  if (A.memory_degraded) {
    try {
      sys.exec_sql(
        "SET @chat_options = JSON_SET(COALESCE(@chat_options, JSON_OBJECT())," +
        " '$.memory_degraded', TRUE," +
        " '$.memory_degraded_reason', '" + esc(A.memory_degraded_reason || '') + "')"
      );
    } catch (e) {}
  }
}

/**
 * Persist one executed SQL step to mysql.agent_sql_trace, and — for
 * successful, read-only query_db steps only — also embed `desc` (the
 * model's stated reason/intent for that query, the closest thing we have
 * to "what question was this SQL answering") into a new `embedding` column
 * on this same row.
 *
 * This table (not mysql.agent_memory) is now the single source of truth
 * for retrieve_few_shot()'s vector search: agent_memory.thought never
 * actually contained the raw SQL text (see tool_log's `thought=<free text>`
 * format in shannon_agent_run.js), so a prior version of retrieve_few_shot
 * regex-matched against a field that could never contain what it was
 * looking for and silently always returned ''. Keeping (desc, sql) + their
 * embedding together on one row, in the one table that already stores the
 * real sql_text, removes that cross-table/format mismatch entirely.
 *
 * Embedding is intentionally gated to `tool === 'query_db' && !is_write`
 * with a non-empty desc and no visible error in `result`:
 *   - write/DDL/ML/tx-control steps aren't useful text-to-SQL examples and
 *     would just add embedding cost with no few-shot value;
 *   - a failed query is actively harmful as a "learned" example, since it
 *     would teach the model to reproduce SQL that didn't work.
 * This keeps embedding calls bounded to roughly the same steps the old
 * `thought LIKE '%query_db%'` filter was trying (and failing) to select.
 */
function log_sql_trace(conv_id, turn_no, step_no, route, tool, sql, desc, result) {
  try {
    var stmt = classify_statement(sql);
    var is_write = stmt.is_write ? 1 : 0;

    var result_str = String(result || '');
    var has_error =
      result_str.indexOf('执行出错')     !== -1 ||
      result_str.indexOf('Error: ')      !== -1 ||
      result_str.indexOf('Unknown table') !== -1;

    var should_embed =
      tool === 'query_db' && !is_write && !has_error &&
      String(desc || '').trim().length > 0;

    var embed_col_sql = 'NULL';
    if (should_embed) {
      var safe_desc = String(desc).trim().substring(0, 1800);
      embed_col_sql =
        "sys.ML_EMBED_ROW('" + esc(safe_desc) + "'," +
        "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
    }

    /* turn_id, not just turn_no.  The two count different things: turn_no
     * here is the agent-loop iteration, while a conversation turn is one
     * user message, so "which tools did this turn run" had no join key at
     * all.  agent_memory.turn_id is generated from the same value, so the
     * ledger and the memory rows now meet. */
    sys.exec_sql(
      "INSERT INTO mysql.agent_sql_trace " +
      "(conversation_id, turn_no, step_no, route, tool, sql_text, desc_text, result_preview, is_write, embedding, turn_id) " +
      "VALUES ('" + esc(conv_id) + "'," + Number(turn_no) + "," + Number(step_no) + "," +
      "'" + esc(route) + "','" + esc(tool || '') + "'," +
      "'" + esc(String(sql || '')) + "','" + esc(String(desc || '')) + "'," +
      "'" + esc(result_str.substring(0, 1200)) + "'," + is_write + "," +
      embed_col_sql + ",'" + esc(current_turn_id()) + "')"
    );
  } catch (e) {}
}

/**
 * Vector-retrieve a small number of past (intent -> working SQL) examples
 * to use as few-shot grounding for the current question.
 *
 * Queries mysql.agent_sql_trace directly (see log_sql_trace) rather than
 * mysql.agent_memory: sql_text/desc_text/embedding all live on the same row
 * there, for exactly the query_db steps that succeeded, so there is no
 * text-format assumption to keep in sync with how shannon_agent_run.js
 * happens to build tool_log/thought this month.
 */
/* L2b procedural recall.
 *
 * Near-duplicate few-shots are the most visible redundancy in the whole
 * memory block: ask the same question on five days and the model is shown
 * essentially the same example five times, spending the budget of three
 * distinct examples to say one thing.  So this path takes the same MMR the
 * L2a/L3 recalls take.
 *
 * It does not take the score terms.  agent_sql_trace has no confidence or
 * use_count to read back, and this query -- unlike the two in
 * lib_memory_registry.js -- has no max_distance floor, so reordering on
 * anything other than distance could promote a genuinely unrelated example.
 *
 * mem_diversify() lives in lib_memory_registry.js, which includes this file
 * rather than the other way round.  Both generated roots happen to contain
 * it today (lib_tools.js -> lib_router.js -> lib_memory_registry.js), but a
 * root that took lib_memory.js alone would not, and JerryScript gives each
 * root its own global scope -- so this asks rather than assumes, and falls
 * back to plain distance order when it is absent. */
function retrieve_few_shot(question, topK) {
  topK = topK || 3;
  try {
    var embed_expr =
      "sys.ML_EMBED_ROW('" + esc(question) + "'," +
      "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
    var principal_prefix = scoped_principal_prefix(A.conversation_id);
    if (!principal_prefix) return '';
    var can_mmr = (typeof mem_diversify === 'function' &&
                   typeof mem_ranking_options === 'function');
    var lambda  = 1;
    if (can_mmr) {
      try { lambda = mem_ranking_options(get_memory_options(get_chat_options())).lambda; }
      catch (e) { lambda = 1; can_mmr = false; }
    }
    var fetch_k = (can_mmr && lambda < 1) ? Math.min(topK * 3, 50) : topK;
    var sim_rows = query(
      "SELECT desc_text, sql_text FROM mysql.agent_sql_trace" +
      " WHERE tool='query_db' AND is_write=0" +
      "   AND conversation_id LIKE '" + esc(principal_prefix) + ":%'" +
      "   AND conversation_id != '" + esc(A.conversation_id) + "'" +
      "   AND embedding IS NOT NULL" +
      " ORDER BY DISTANCE(embedding, " + embed_expr + ", 'cosine') ASC LIMIT " + fetch_k
    );
    if (!Array.isArray(sim_rows) || !sim_rows.length) return '';
    if (can_mmr && lambda < 1 && sim_rows.length > 1) {
      /* The query returned them best-first and the rows carry no distance
       * column, so rank position is the only relevance signal there is;
       * 1/(1+i) makes it monotone decreasing within 0..1, which is the range
       * MMR weighs the similarity penalty against.  Precomputed onto the row
       * rather than derived inside the callback, because the callback would
       * otherwise have to search the very array being replaced. */
      for (var ri = 0; ri < sim_rows.length; ri++) sim_rows[ri].rank_rel = 1 / (1 + ri);
      sim_rows = mem_diversify(
        sim_rows,
        /* Both halves matter: two traces can share a description and differ
         * in SQL, or share SQL and differ in what they were asked for. */
        function (r) { return String(r.desc_text || '') + ' ' + String(r.sql_text || ''); },
        function (r) { return r.rank_rel; },
        lambda, topK);
    } else if (sim_rows.length > topK) {
      sim_rows = sim_rows.slice(0, topK);
    }
    var lines = [t('【Few-Shot 参考（向量检索）】', '[Few-Shot References (vector search)]')];
    for (var i = 0; i < sim_rows.length; i++) {
      var desc = String(sim_rows[i].desc_text || '').trim();
      var sql  = String(sim_rows[i].sql_text  || '').trim();
      if (!sql) continue; // defensive: should never happen given the WHERE/embedding gate, but never emit a bare Q with no SQL
      lines.push('Q' + (i+1) + ': ' + (desc || t('（无描述）', '(no description)')).substring(0, 80));
      lines.push('SQL: ' + sql.substring(0, 150));
    }
    return lines.length > 1 ? lines.join('\n') : '';
  } catch(e) { return ''; }
}