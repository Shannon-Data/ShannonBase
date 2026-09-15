//@include lib_memory.js

/* ===========================================================================
 * Layered agent memory.
 *
 *   L0  working    A.* + the prompt scratch string        one CALL, never recalled
 *   L1  short      mysql.agent_memory + rolling summary   per principal:conversation
 *   L2a episodic   mysql.agent_memory.embedding (ML_RAG)  per principal, cross-session
 *   L2b procedural mysql.agent_sql_trace.embedding        per principal, cross-session
 *   L3  semantic   mysql.agent_semantic_fact              per principal, explicit writes
 *
 * Everything the agent loop needs comes out of one entry point,
 * MEM.build_block(), replacing the three independent lookups
 * (get_history / retrieve_few_shot / chat_history_to_text) that previously
 * disagreed about how many turns "history" meant.
 *
 * Four properties this layer is responsible for, none of which the previous
 * code had:
 *   1. Isolation.  Every L2/L3 read carries the principal prefix.  A recall
 *      with no isolation key returns nothing rather than falling back to an
 *      unfiltered scan.
 *   2. Atomicity + ordering.  A turn's user and assistant rows are written by
 *      one INSERT under one monotonic `seq`, so a crash can't leave an orphan
 *      user row and two turns in the same wall-clock second can't come back
 *      in the wrong order (TIMESTAMP only has second resolution).
 *   3. Visible degradation.  Failures write an audit row and set
 *      A.memory_degraded instead of being swallowed by `catch(e){}`.
 *   4. Bounded cost.  Token budgets rather than row counts, an embedding
 *      whitelist rather than two ML_EMBED_ROW calls every single turn, and
 *      expires_at retention.
 * ======================================================================== */

var MEM_DEFAULTS = {
  enabled: true,
  short_term: {
    recent_turns: 5,
    max_tokens: 1500,
    summarize_after_turns: 8,
    summary_max_tokens: 600,
    keep_recent_after_compact: 3
  },
  long_term: {
    episodic_enabled: true,
    episodic_top_k: 3,
    episodic_max_distance: 0.6,
    semantic_enabled: true,
    semantic_top_k: 3,
    semantic_max_distance: 0.6,
    /* Long-term facts are written only by an explicit remember_fact call.
     * There is deliberately no automatic fact extraction: it would need an
     * accuracy evaluation of its own before it could be trusted to put words
     * in the user's mouth across sessions.  Deduplication is likewise exact
     * rather than similarity-based -- the unique key on
     * (principal_prefix, statement) is what collapses a repeat write into a
     * use_count bump. */
    default_ttl_days: 90,
    embed_dim: 384,
    /* Routes whose turns are not worth an embedding: approval prompts and
     * approval bookkeeping are UI chatter, not episodes worth recalling. */
    skip_embed_routes: ['review'],
    /* Ranking.  Recall used to be `ORDER BY distance ASC LIMIT k` and nothing
     * else, even though every other signal it could use was already on the
     * table and already being maintained: confidence is written by
     * remember_fact, use_count and last_used_at are bumped by recall itself,
     * and importance already decides whether a turn is embedded at all.  None
     * of them were ever read back at query time.
     *
     * The defaults below reproduce `ORDER BY distance ASC` exactly -- relevance
     * 1, every other weight 0, diversity off -- and that is deliberate rather
     * than timid.  No evidence yet says what recency or usage should be worth,
     * and a weight vector picked by taste would change what the model sees on
     * every single turn, which is the same objection that keeps automatic fact
     * extraction out of this file (see long_term above).  The mechanism is
     * built, configurable and audited; choosing the weights is a job for a
     * recall-quality set. */
    ranking: {
      weight_relevance:  1.0,
      weight_recency:    0.0,
      weight_importance: 0.0,
      weight_usage:      0.0,
      /* Half-life-ish: at recency_tau_days the recency term is worth 1/e. */
      recency_tau_days:  30,
      /* log(1+n)/log(1+saturation), so the 100th hit is not worth 100x the
       * first and one hot fact cannot dominate the ranking outright. */
      usage_saturation:  20,
      /* MMR: 1.0 is pure ranking order, lower trades relevance for variety. */
      diversity_lambda:  1.0
    }
  },
  procedural: { few_shot_top_k: 3 },
  /* Section sizing is expressed as concrete caps -- short_term.max_tokens,
   * summary_max_tokens, and the per-section top_k values -- rather than as
   * fractions of the overall prompt budget.  A fraction only means something
   * once the schema context has been built, and build_schema_context() is
   * already handed whatever the memory block did not use.
   *
   * max_block_tokens is the ceiling those per-section caps do not add up to
   * on their own (few-shot examples in particular are only bounded by top_k,
   * not by size).  Without it the memory block can silently crowd out the
   * schema context, which is the one input the model cannot do without. */
  budget: { max_block_tokens: 3000 },
  redact: {
    patterns: ['sk-[A-Za-z0-9_-]{6,}',
               'api[_-]?key\\s*[=:]\\s*\\S+',
               'password\\s*[=:]\\s*\\S+',
               'AKIA[0-9A-Z]{16}']
  },
  retention: { enabled: true, purge_batch: 500, max_facts_per_principal: 2000 },
  /* 'scan' matches what upstream MySQL's DISTANCE() actually does today: a
   * linear scan with no reverse index.  Approximate-nearest-neighbour search
   * is upstream's work, not this project's -- implementing it here would mean
   * changing sql/, the handler and the optimizer, and every such divergence
   * raises the cost of merging upstream later.  This switch exists so that
   * when upstream ships ANN, mem_vector_search() is the single place to
   * change; until then the scan is bounded instead (mandatory isolation
   * predicate over an indexed column, hard LIMIT, max_distance cut-off,
   * expires_at retention) and its latency is audited. */
  vector_index: 'scan'
};

function mem_merge_defaults(defaults, user) {
  var out = {};
  var k;
  for (k in defaults) {
    if (!Object.prototype.hasOwnProperty.call(defaults, k)) continue;
    var dv = defaults[k];
    var uv = (user && typeof user === 'object') ? user[k] : undefined;
    if (dv && typeof dv === 'object' && !Array.isArray(dv))
      out[k] = mem_merge_defaults(dv, (uv && typeof uv === 'object') ? uv : {});
    else
      out[k] = (uv === undefined || uv === null) ? dv : uv;
  }
  /* Keep unknown user keys so a forward-compatible option is not silently
   * dropped on an older server. */
  if (user && typeof user === 'object') {
    for (k in user) {
      if (!Object.prototype.hasOwnProperty.call(user, k)) continue;
      if (out[k] === undefined) out[k] = user[k];
    }
  }
  return out;
}

/* Single authority for memory configuration.  `history_length` used to be
 * defaulted to 3 in two files, documented as 5 in a third, and overridden by
 * a hard-coded get_history(conversation_id, 8) at the call site; it now maps
 * onto short_term.recent_turns and nothing else reads it. */
function get_memory_options(chat_opt) {
  if (A._mem_opt) return A._mem_opt;
  var co   = chat_opt || get_chat_options() || {};
  var user = (co.memory_options && typeof co.memory_options === 'object')
    ? co.memory_options : {};
  var merged = mem_merge_defaults(MEM_DEFAULTS, user);

  /* Backward compatibility: history_length is the pre-existing public knob. */
  if ((!user.short_term || user.short_term.recent_turns === undefined) &&
      co.history_length !== undefined && co.history_length !== null) {
    var hl = Number(co.history_length);
    if (isFinite(hl) && hl >= 0) merged.short_term.recent_turns = hl;
  }
  A._mem_opt = merged;
  return merged;
}

function mem_principal_prefix() {
  var p = scoped_principal_prefix(A.conversation_id);
  if (p) return p;
  return current_principal_prefix();
}

function mem_mark_degraded(reason) {
  A.memory_degraded = true;
  A.memory_degraded_reason = A.memory_degraded_reason || String(reason || '');
}

/* Audit: recall / write / forget / purge / compact / degraded.  elapsed_ms on
 * recall rows is the data that decides whether ANN is ever worth asking
 * upstream for -- see design doc section 6.4. */
function mem_log_audit(tier, op, store, detail, hit_count, elapsed_ms, vector_mode) {
  try {
    var prefix = mem_principal_prefix();
    if (!prefix) return;
    query_checked(
      "INSERT INTO mysql.agent_memory_audit" +
      "(principal_prefix, conversation_id, op, tier, store, detail, hit_count, elapsed_ms, vector_mode)" +
      " VALUES ('" + esc(prefix) + "','" + esc(A.conversation_id || '') + "','" +
      esc(String(op || '')) + "','" + esc(String(tier || '')) + "','" +
      esc(String(store || '').substring(0, 128)) + "','" +
      esc(String(detail || '').substring(0, 512)) + "'," +
      Number(hit_count || 0) + "," + Number(elapsed_ms || 0) + ",'" +
      esc(String(vector_mode || '')) + "')"
    );
  } catch (e) { /* audit must never break the turn */ }
}

/* Redaction runs before anything is persisted.  Previously only
 * chat_options.api_key was masked, so a key pasted into the user's own
 * message landed verbatim in mysql.agent_memory and then in its embedding. */
function mem_redact(text, opt) {
  var s = String(text == null ? '' : text);
  if (!s) return s;
  var mo = opt || get_memory_options(get_chat_options());
  var pats = (mo.redact && Array.isArray(mo.redact.patterns)) ? mo.redact.patterns : [];
  for (var i = 0; i < pats.length; i++) {
    try {
      s = s.replace(new RegExp(pats[i], 'gi'), '[REDACTED]');
    } catch (e) { /* a bad user-supplied pattern must not break persistence */ }
  }
  return s;
}

/* ------------------------------------------------------------------------
 * Ranking and diversity.
 *
 * Everything here is deliberately free of new storage: the four signals below
 * are columns that already exist and are already written.  What was missing
 * was reading them back at recall time.
 * --------------------------------------------------------------------- */

/* MMR needs more candidates than it returns, or there is nothing to choose
 * between.  The scan is linear either way, so a wider pool costs one memcmp
 * per extra row, not an extra query. */
var MEM_MMR_POOL_FACTOR = 3;
var MEM_MMR_POOL_MAX    = 50;

function mem_num(v, dflt) {
  var n = Number(v);
  return isFinite(n) ? n : Number(dflt || 0);
}

/* Weights come from @chat_options and therefore from the client.  They reach
 * SQL only through mem_num(), so a string in the options JSON becomes a
 * number or the default -- never a fragment of the WHERE clause.  The column
 * names and expressions in `signals` are code constants from the call sites
 * below, never client input. */
function mem_ranking_options(mo) {
  var rk = (mo && mo.long_term && mo.long_term.ranking) ? mo.long_term.ranking : {};
  return {
    w_rel:  mem_num(rk.weight_relevance,  1),
    w_rec:  mem_num(rk.weight_recency,    0),
    w_imp:  mem_num(rk.weight_importance, 0),
    w_use:  mem_num(rk.weight_usage,      0),
    tau_h:  Math.max(1, mem_num(rk.recency_tau_days, 30) * 24),
    usat:   Math.max(1, mem_num(rk.usage_saturation, 20)),
    lambda: Math.min(1, Math.max(0, mem_num(rk.diversity_lambda, 1)))
  };
}

/* With only the relevance term, ORDER BY score DESC and ORDER BY distance ASC
 * are the same permutation for any positive weight.  Saying so here is what
 * lets the default path emit exactly the SQL it emitted before ranking
 * existed, instead of a derived table that computes a monotone rewrite of the
 * order it already had. */
function mem_rank_active(rk) {
  return !!(rk.w_rec || rk.w_imp || rk.w_use);
}

/* Every term is normalised to 0..1 so the weights are comparable to one
 * another; a signal whose column the caller did not supply contributes
 * nothing rather than defaulting to some invented value.
 *
 * INVARIANT: every expression reached through `sig` -- sig.recency,
 * sig.importance.expr, sig.usage and the names in sig.columns -- is
 * interpolated into SQL verbatim, so it must be a code constant from a call
 * site in this file.  Only the weights in `rk` come from @chat_options, and
 * they pass through mem_num() first.  If `sig` ever becomes configurable it
 * needs an identifier whitelist, not esc_ident(): these are expressions, not
 * identifiers.
 *
 * The relevance term is clamped because normalisation is otherwise a claim
 * rather than a fact: cosine distance runs 0..2, so 1-distance goes negative
 * once distance exceeds 1.  With the default max_distance of 0.6 it stays in
 * 0.4..1, but max_distance is an operator-settable option, and above 1.0 an
 * unclamped negative relevance would let a barely-related row outrank a
 * closer one on recency alone -- the exact failure the admission floor is
 * there to prevent. */
function mem_rank_sql(rk, sig) {
  var terms = [rk.w_rel + '*GREATEST(0,1-distance)'];
  sig = sig || {};
  if (rk.w_rec && sig.recency)
    terms.push(rk.w_rec + '*COALESCE(EXP(-GREATEST(TIMESTAMPDIFF(HOUR,' +
               sig.recency + ',NOW()),0)/' + rk.tau_h + '),0)');
  if (rk.w_imp && sig.importance) {
    var imax = Math.max(1, mem_num(sig.importance.max, 100));
    terms.push(rk.w_imp + '*COALESCE(LEAST(GREATEST(' + sig.importance.expr +
               ',0),' + imax + ')/' + imax + ',0)');
  }
  if (rk.w_use && sig.usage)
    terms.push(rk.w_use + '*COALESCE(LN(1+LEAST(GREATEST(' + sig.usage + ',0),' +
               rk.usat + '))/LN(1+' + rk.usat + '),0)');
  return terms.join(' + ');
}

/* The derived table must carry the scoring columns as well as the caller's
 * own select list, without naming either of them twice: a duplicate column in
 * a derived table is an error, not a warning. */
function mem_inner_list(select_list, cols) {
  var have = {}, parts = String(select_list).split(',');
  for (var i = 0; i < parts.length; i++) have[parts[i].trim().toLowerCase()] = 1;
  var out = String(select_list);
  for (var c = 0; cols && c < cols.length; c++) {
    var name = String(cols[c]).trim();
    if (!name || have[name.toLowerCase()]) continue;
    have[name.toLowerCase()] = 1;
    out += ', ' + name;
  }
  return out;
}

/* Token set for lexical similarity.  Same split lib_ml.js uses for keyword
 * work, so identifiers and CJK both produce something usable. */
function mem_tokens(text) {
  var raw = String(text == null ? '' : text).toLowerCase()
              .match(/[a-z0-9_]+|[\u4e00-\u9fff]{1,2}/g) || [];
  var set = {}, n = 0;
  for (var i = 0; i < raw.length; i++)
    if (!Object.prototype.hasOwnProperty.call(set, raw[i])) { set[raw[i]] = 1; n++; }
  return { set: set, size: n };
}

function mem_lex_sim(a, b) {
  if (!a.size || !b.size) return 0;
  var small = (a.size <= b.size) ? a : b;
  var large = (small === a) ? b : a;
  var inter = 0;
  for (var k in small.set)
    if (Object.prototype.hasOwnProperty.call(small.set, k) && large.set[k]) inter++;
  return inter / (a.size + b.size - inter);
}

/* Maximal marginal relevance: repeatedly take the candidate with the best
 * (lambda * relevance - (1-lambda) * similarity to what is already picked).
 *
 * The similarity is lexical, not cosine, and that is a cost decision rather
 * than an oversight.  Candidates come back without their embeddings, so a
 * vector MMR would mean one extra ML_EMBED_ROW per candidate on every recall
 * -- the same per-turn embedding cost mem_should_embed() exists to avoid.
 * The redundancy this is here to remove is the same question asked five times
 * producing five near-identical turns, which is lexical redundancy, and
 * Jaccard detects it perfectly well. */
function mem_diversify(items, text_of, rel_of, lambda, k) {
  k = Number(k);
  if (!(lambda < 1) || !items || items.length <= 1) return (items || []).slice(0, k);
  var toks = [], i;
  for (i = 0; i < items.length; i++) toks.push(mem_tokens(text_of(items[i])));
  var picked = [], used = {};
  while (picked.length < k && picked.length < items.length) {
    var best = -1, best_v = null;
    for (i = 0; i < items.length; i++) {
      if (used[i]) continue;
      var pen = 0;
      for (var p = 0; p < picked.length; p++) {
        var sim = mem_lex_sim(toks[i], toks[picked[p]]);
        if (sim > pen) pen = sim;
      }
      var v = lambda * Number(rel_of(items[i])) - (1 - lambda) * pen;
      if (best_v === null || v > best_v) { best_v = v; best = i; }
    }
    if (best < 0) break;
    used[best] = 1;
    picked.push(best);
  }
  var out = [];
  for (i = 0; i < picked.length; i++) out.push(items[picked[i]]);
  return out;
}

/* ------------------------------------------------------------------------
 * Vector retrieval: the single adaptation point.
 *
 * Two backends, one audited entry point:
 *   'rag' — hand the search to sys.ML_RAG.  Used for episodic recall over
 *           mysql.agent_memory, which gets citations, segment overlap and
 *           max_distance handling for free.
 *   'sql' — a direct principal-filtered DISTANCE query.  Used for
 *           mysql.agent_semantic_fact, where the caller needs structured
 *           columns back (fact_id, confidence) that ML_RAG's citation shape
 *           cannot carry.
 *
 * Both are linear scans today, because upstream MySQL's DISTANCE() has no
 * reverse index.  That is upstream's work, not this project's: what is done
 * here instead is bounding the scan (a mandatory isolation predicate over an
 * indexed column, a hard LIMIT, and a max_distance cut-off) and recording
 * elapsed_ms so the decision to chase ANN can be made from data.  When
 * upstream ANN lands, this function is the only thing that changes.
 * --------------------------------------------------------------------- */
function mem_vector_search(tier, mode, table, columns, question, filters, topK, opt) {
  var mo   = opt || get_memory_options(get_chat_options());
  var vmode = mo.vector_index || 'scan';
  var docs = (filters && filters.documents) ? filters.documents : [];

  /* No isolation key means no recall.  There is deliberately no code path
   * that queries these tables without a principal predicate. */
  if (!docs.length || !docs[0]) {
    mem_log_audit(tier, 'degraded', table, 'missing_isolation_key', 0, 0, vmode);
    mem_mark_degraded('missing_isolation_key');
    return { ok: false, rows: [], text: '', citations: [], hits: 0 };
  }

  var rk     = mem_ranking_options(mo);
  var ranked = mem_rank_active(rk) && !!(filters && filters.rank);
  var pool   = (rk.lambda < 1)
    ? Math.min(Number(topK) * MEM_MMR_POOL_FACTOR, MEM_MMR_POOL_MAX)
    : Number(topK);

  var t0 = Date.now();
  var res;
  if (mode === 'rag') {
    var rag_opt = {
      vector_store:         [table],
      vector_store_columns: columns,
      document_name:        docs,
      n_citations:          pool,
      distance_metric:      'COSINE',
      retrieval_options:    { max_distance: filters.max_distance },
      skip_generate:        1
    };
    var rag = ml_rag(question, pool, rag_opt);
    res = { ok: !!(rag && rag.ok), rows: [],
            text: (rag && rag.text) || '',
            citations: (rag && rag.citations) || [],
            hits: (rag && rag.hits) || 0 };
    /* ML_RAG ranks by distance alone and cannot be handed the recency or
     * importance columns, because a citation carries only
     * {segment, distance, document_name, segment_number, metadata}.  Diversity
     * is therefore the only part of the ranking this backend can take.
     * Closing that asymmetry means moving episodic recall onto the 'sql'
     * backend, which would also give up ML_RAG's segment-overlap handling --
     * a trade worth making on evidence, not in passing.
     *
     * Under skip_generate the text ML_RAG returns is exactly its kept segments
     * joined by a blank line, so rebuilding it from the surviving citations
     * reproduces the format the caller already parses. */
    if (res.ok && rk.lambda < 1 && res.citations.length > 1) {
      var kept = mem_diversify(
        res.citations,
        function (c) { return c && c.segment; },
        /* Clamped for the same reason as the SQL relevance term: MMR weighs
         * this against a 0..1 similarity penalty, so a negative relevance
         * would make lambda mean something different per row. */
        function (c) { return Math.max(0, 1 - mem_num(c && c.distance, 0)); },
        rk.lambda, Number(topK));
      var segs = [];
      for (var kc = 0; kc < kept.length; kc++)
        if (kept[kc] && kept[kc].segment) segs.push(String(kept[kc].segment));
      res.citations = kept;
      res.text      = segs.join('\n\n');
      res.hits      = kept.length;
    }
  } else {
    var embed_expr =
      "sys.ML_EMBED_ROW('" + esc(String(question).substring(0, 1800)) + "'," +
      "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
    var doc_list = [];
    for (var d = 0; d < docs.length; d++) doc_list.push("'" + esc(docs[d]) + "'");
    var scan =
      ", DISTANCE(`" + esc_ident(columns.segment_embedding) + "`, " + embed_expr +
      ", 'COSINE') AS distance" +
      " FROM " + table +
      " WHERE `" + esc_ident(filters.isolation_column) + "` IN (" + doc_list.join(',') + ")" +
      "   AND `" + esc_ident(columns.segment_embedding) + "` IS NOT NULL" +
      (filters.extra_where ? " AND " + filters.extra_where : '');
    var sql;
    if (!ranked) {
      /* Byte for byte what this emitted before ranking existed.  See
       * mem_rank_active(). */
      sql =
        "SELECT " + filters.select_list + scan +
        " HAVING distance <= " + Number(filters.max_distance) +
        " ORDER BY distance ASC LIMIT " + Number(pool);
    } else {
      /* The derived table is what holds ML_EMBED_ROW to a single call: a
       * select alias is not visible to a sibling select expression, so
       * scoring beside DISTANCE() in one SELECT would mean writing the
       * embedding expression out a second time and embedding the question
       * twice per recall.
       *
       * distance still gates admission -- score only reorders what already
       * cleared max_distance, so a stale fact cannot be boosted in on usage
       * alone -- and ties fall back to distance for a deterministic order. */
      sql =
        "SELECT " + filters.select_list + ", distance, " +
        mem_rank_sql(rk, filters.rank) + " AS score" +
        " FROM (SELECT " + mem_inner_list(filters.select_list, filters.rank.columns) +
        scan + ") mem_cand" +
        " WHERE distance <= " + Number(filters.max_distance) +
        " ORDER BY score DESC, distance ASC LIMIT " + Number(pool);
    }
    var rows = query(sql);
    if (!Array.isArray(rows)) {
      mem_log_audit(tier, 'degraded', table,
                    (rows && rows.error) ? String(rows.error).substring(0, 200) : 'query_failed',
                    0, Date.now() - t0, vmode);
      mem_mark_degraded('vector_search_failed');
      return { ok: false, rows: [], text: '', citations: [], hits: 0 };
    }
    if (rk.lambda < 1 && rows.length > 1)
      rows = mem_diversify(
        rows,
        function (r) { return r[columns.segment]; },
        function (r) {
          return (r.score === undefined || r.score === null)
            ? Math.max(0, 1 - mem_num(r.distance, 0)) : mem_num(r.score, 0);
        },
        rk.lambda, Number(topK));
    else if (rows.length > Number(topK))
      rows = rows.slice(0, Number(topK));
    res = { ok: true, rows: rows, text: '', citations: [], hits: rows.length };
  }

  /* The audit detail says which ranking actually ran, so a recall that looks
   * wrong can be told apart from a recall that was ranked differently. */
  mem_log_audit(tier, 'recall', table,
                mode + (ranked ? '+rank' : '') + ((rk.lambda < 1) ? '+mmr' : ''),
                res.hits, Date.now() - t0, vmode);
  return res;
}

/* ------------------------------------------------------------------------
 * L1: short-term (per conversation)
 * --------------------------------------------------------------------- */

/* Should this turn's rows carry an embedding?
 *
 * Previously every single turn issued two ML_EMBED_ROW calls -- including the
 * "please reply Approve / Reject" prompts, which are pure UI chatter and were
 * doubling embedding cost for zero recall value. */
function mem_should_embed(route, importance, mo) {
  if (!mo.long_term.episodic_enabled) return false;
  if (importance > 0) return true;
  var skip = mo.long_term.skip_embed_routes || [];
  var r = String(route || '');
  for (var i = 0; i < skip.length; i++)
    if (r.indexOf(skip[i]) === 0) return false;
  return true;
}

function mem_embed_expr(text, mo) {
  return "sys.ML_EMBED_ROW('" + esc(String(text || '').substring(0, 1800)) + "'," +
         "JSON_OBJECT('model_id','" + esc(get_embed_model_id()) + "','truncate',true))";
}

/* Did this INSERT lose to a concurrent writer on the same conversation,
 * rather than fail for a reason doing it again cannot fix?
 *
 * Three ways to lose: the other writer read the same MAX(seq) and reached
 * uk_conv_seq first, or the two INSERT ... SELECT statements deadlocked over
 * the range they both scan to find that MAX, or one waited out its lock
 * timeout.  All three mean "someone else went first", which is exactly the
 * situation a retry is for -- and none of them mean the turn should be
 * thrown away. */
function mem_is_write_conflict(err) {
  var s = String(err || '');
  if (/deadlock/i.test(s) || /lock wait timeout/i.test(s)) return true;
  return /duplicate entry/i.test(s) && /uk_conv_seq/i.test(s);
}

/* Two writers on one conversation is the realistic case (the agent loop plus
 * a review step committing its own turn), so three attempts covers it without
 * turning a genuinely wedged table into a long stall. */
var MEM_WRITE_RETRIES = 3;

/* Insert one turn, retrying only a lost race.  The statement derives
 * its own base from MAX(seq) each time it runs, so re-running it *is* the
 * retry -- there is nothing to recompute here.  Auditing the conflict without
 * retrying, which is what this path used to do, made the loss visible but
 * still lost the turn. */
function mem_insert_turn(stmt) {
  var err = '';
  for (var i = 0; i < MEM_WRITE_RETRIES; i++) {
    try { query_checked(stmt); return { ok: true, err: '', retries: i }; }
    catch (e) {
      err = String(e);
      if (!mem_is_write_conflict(err)) break;
    }
  }
  return { ok: false, err: err, retries: MEM_WRITE_RETRIES };
}

/* One statement writes both rows of a turn.
 *
 * `seq` is allocated inside the statement from the conversation's current
 * MAX(seq), so the pair is contiguous and monotonic without a separate
 * counter round trip.  uk_conv_seq (conversation_id, seq) turns a concurrent
 * writer into a duplicate-key error instead of two interleaved turns silently
 * sharing an ordering position -- and mem_insert_turn then retries it, since
 * a turn that is merely late must not be a turn that is lost. */
function mem_short_append_turn(conv_id, user_msg, bot_msg, thought, meta) {
  var mo = get_memory_options(get_chat_options());
  if (!mo.enabled) return false;

  meta = meta || {};
  var route      = String(meta.route || '');
  var importance = Number(meta.importance || 0);
  var intent     = analyze_intent(user_msg);
  var user_meta  = JSON.stringify({
    intent: intent.kind, need_join: intent.need_join,
    need_time_filter: intent.need_time_filter, need_agg: intent.need_agg
  });

  var safe_user = mem_redact(user_msg, mo);
  var safe_bot  = mem_redact(bot_msg,  mo);
  var safe_tht  = mem_redact(thought,  mo);

  var prefix = mem_principal_prefix();
  var embed  = mem_should_embed(route, importance, mo);
  var ttl    = Number(mo.long_term.default_ttl_days || 0);
  var expires = (ttl > 0) ? "DATE_ADD(NOW(), INTERVAL " + ttl + " DAY)" : 'NULL';
  var dim     = Number(mo.long_term.embed_dim || 384);

  var meta_json = JSON.stringify({
    conversation_id: conv_id, route: route, turn_id: current_turn_id(),
    cost: (A.cost && A.cost.llm_calls) ? A.cost : null
  });

  /* One row of the pair.  with_embed governs BOTH the vector expression and
   * the embed_model_id / embed_dim provenance columns, so a row can never
   * claim a model and dimension it did not actually store. */
  function row(seq_expr, role, content, thought_text, with_embed) {
    return "SELECT '" + esc(conv_id) + "', b.base+" + seq_expr + ", b.turn+1, '" + role + "'," +
           "'" + esc(content) + "','" + esc(thought_text) + "'," +
           "'" + esc(route) + "'," + importance + "," +
           "SHA2('" + esc(content) + "',256)," +
           (with_embed ? "'" + esc(get_embed_model_id()) + "'," + dim : "NULL,NULL") + "," +
           "'" + esc(prefix) + "'," +
           "CAST('" + esc(meta_json) + "' AS JSON)," + expires + "," +
           (with_embed ? mem_embed_expr(content, mo) : 'NULL') +
           " FROM (SELECT COALESCE(MAX(seq),0) AS base, COALESCE(MAX(turn_no),0) AS turn" +
           "         FROM mysql.agent_memory WHERE conversation_id='" + esc(conv_id) + "') b";
  }

  var cols = "(conversation_id, seq, turn_no, role, content, thought, route, importance," +
             " content_hash, embed_model_id, embed_dim, document_name, meta, expires_at, embedding)";

  function turn_stmt(with_embed) {
    return "INSERT INTO mysql.agent_memory " + cols + " " +
           row('1', 'user',      safe_user, user_meta, with_embed) + " UNION ALL " +
           row('2', 'assistant', safe_bot,  safe_tht,  with_embed);
  }

  var r = mem_insert_turn(turn_stmt(embed));
  if (r.ok) {
    mem_log_audit('L1', 'write', 'mysql.agent_memory',
                  r.retries ? route + ' write_conflict_retries=' + r.retries : route,
                  2, 0, mo.vector_index);
    return true;
  }

  /* Never silently write a broken vector: if the embedding leg is what
   * failed (model unavailable, dimension mismatch), retry the turn without
   * it so the conversation is still remembered, and say so in the audit.
   * A lost seq race is not that failure -- retrying it without the vector
   * would just lose the race again. */
  var err = r.err;
  if (embed && !mem_is_write_conflict(err)) {
    var r2 = mem_insert_turn(turn_stmt(false));
    if (r2.ok) {
      mem_log_audit('L1', 'degraded', 'mysql.agent_memory',
                    'embedding_skipped: ' + err.substring(0, 160), 2, 0, mo.vector_index);
      mem_mark_degraded('embedding_unavailable');
      return true;
    }
    err = r2.err;
  }

  mem_log_audit('L1', 'degraded', 'mysql.agent_memory', err.substring(0, 200), 0, 0, mo.vector_index);
  mem_mark_degraded(mem_is_write_conflict(err) ? 'short_term_write_conflict'
                                              : 'short_term_write_failed');
  return false;
}

/* Load the short-term window: rolling summary plus the most recent turns,
 * trimmed by token budget rather than by row count. */
function mem_short_load(conv_id, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var out = { summary: '', covered_upto_seq: 0, turns: [], tokens: 0, degraded: false };
  if (!mo.enabled || !conv_id) return out;

  var sum_rows = query(
    "SELECT summary, covered_upto_seq, summary_tokens FROM mysql.agent_conversation_summary" +
    " WHERE conversation_id='" + esc(conv_id) + "'"
  );
  if (Array.isArray(sum_rows) && sum_rows.length) {
    out.summary          = String(sum_rows[0].summary || '');
    out.covered_upto_seq = Number(sum_rows[0].covered_upto_seq || 0);
  } else if (sum_rows && sum_rows.error) {
    out.degraded = true;
    mem_mark_degraded('summary_read_failed');
  }

  var want = Math.max(0, Number(mo.short_term.recent_turns || 0)) * 2;
  if (!want) return out;

  /* ORDER BY seq, never created_at: TIMESTAMP resolves to whole seconds, so
   * several turns inside one second came back in arbitrary order. */
  var rows = query(
    "SELECT role, content, turn_no, seq FROM (" +
    "  SELECT role, content, turn_no, seq FROM mysql.agent_memory" +
    "  WHERE conversation_id='" + esc(conv_id) + "' AND seq > " + Number(out.covered_upto_seq) +
    "  ORDER BY seq DESC LIMIT " + want +
    ") m ORDER BY m.seq ASC"
  );
  if (!Array.isArray(rows)) {
    out.degraded = true;
    mem_mark_degraded('short_term_read_failed');
    mem_log_audit('L1', 'degraded', 'mysql.agent_memory',
                  (rows && rows.error) ? String(rows.error).substring(0, 200) : 'read_failed',
                  0, 0, mo.vector_index);
    return out;
  }

  /* Accumulate backwards from the newest turn and stop at the token budget --
   * a fixed row count let one huge pasted result set blow the prompt. */
  var budget = Number(mo.short_term.max_tokens || 1500);
  var used = 0, kept = [];
  for (var i = rows.length - 1; i >= 0; i--) {
    var cost = est_tok(String(rows[i].content || '')) + 4;
    if (used + cost > budget && kept.length) break;
    used += cost;
    kept.unshift({ role: String(rows[i].role || ''), content: String(rows[i].content || ''),
                   turn_no: Number(rows[i].turn_no || 0), seq: Number(rows[i].seq || 0) });
  }
  out.turns  = kept;
  out.tokens = used;
  return out;
}

function mem_turns_to_text(turns) {
  if (!turns || !turns.length) return '';
  var lines = [];
  for (var i = 0; i < turns.length; i++) {
    lines.push((turns[i].role === 'user' ? t('用户', 'User') : t('助手', 'Assistant')) +
               '：' + turns[i].content);
  }
  return lines.join('\n');
}

/* Rolling summary.  Idempotent by construction: the UPDATE is a CAS on
 * covered_upto_seq, so two concurrent sessions cannot both advance it and a
 * repeated call is a no-op rather than a second version bump. */
function mem_short_compact(conv_id, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.enabled || !conv_id) return false;

  var keep = Math.max(0, Number(mo.short_term.keep_recent_after_compact || 3)) * 2;
  var after = Number(mo.short_term.summarize_after_turns || 8) * 2;

  var head = query(
    "SELECT COALESCE(MAX(seq),0) AS max_seq, COUNT(*) AS n FROM mysql.agent_memory" +
    " WHERE conversation_id='" + esc(conv_id) + "'"
  );
  if (!Array.isArray(head) || !head.length) return false;
  var max_seq = Number(head[0].max_seq || 0);
  var n_rows  = Number(head[0].n || 0);
  if (n_rows < after) return false;

  var cur = query(
    "SELECT summary, covered_upto_seq FROM mysql.agent_conversation_summary" +
    " WHERE conversation_id='" + esc(conv_id) + "'"
  );
  var have    = Array.isArray(cur) && cur.length;
  var covered = have ? Number(cur[0].covered_upto_seq || 0) : 0;
  var prev    = have ? String(cur[0].summary || '') : '';

  /* Summarize everything older than the window we keep verbatim.  Raw rows
   * are never deleted -- summarizing only lowers their recall weight. */
  var target = max_seq - keep;
  if (target <= covered) return false;
  /* Compaction costs an LLM call, and `target` advances with every turn, so
   * without this the summarizer would re-run on every single turn once the
   * conversation passed the threshold.  Wait until a full window's worth of
   * new material has accumulated instead. */
  if ((target - covered) < after) return false;

  var rows = query(
    "SELECT role, content FROM mysql.agent_memory" +
    " WHERE conversation_id='" + esc(conv_id) + "'" +
    "   AND seq > " + covered + " AND seq <= " + target +
    " ORDER BY seq ASC LIMIT 200"
  );
  if (!Array.isArray(rows) || !rows.length) return false;

  var transcript = mem_turns_to_text(rows.map(function(r) {
    return { role: String(r.role || ''), content: String(r.content || '') };
  }));

  var summary = '';
  try {
    summary = ml_generate(
      t('把下面的对话压缩成不超过 ' + Number(mo.short_term.summary_max_tokens || 600) +
        ' token 的要点摘要，保留用户偏好、已确认的表名/列名和已完成的操作，去掉寒暄：\n' +
        (prev ? '【已有摘要】\n' + prev + '\n\n' : '') + transcript,
        'Compress the conversation below into a bullet summary of at most ' +
        Number(mo.short_term.summary_max_tokens || 600) +
        ' tokens. Keep user preferences, confirmed table/column names and completed actions; ' +
        'drop pleasantries:\n' +
        (prev ? '[Existing summary]\n' + prev + '\n\n' : '') + transcript),
      { max_tokens: Number(mo.short_term.summary_max_tokens || 600) }
    );
  } catch (e) {
    mem_log_audit('L1', 'degraded', 'mysql.agent_conversation_summary',
                  'summarize_failed: ' + String(e).substring(0, 160), 0, 0, mo.vector_index);
    return false;
  }
  if (!summary) return false;
  summary = mem_redact(summary, mo);

  var tokens = est_tok(summary);
  try {
    if (!have) {
      query_checked(
        "INSERT IGNORE INTO mysql.agent_conversation_summary" +
        "(conversation_id, summary, covered_upto_seq, summary_tokens, version)" +
        " VALUES ('" + esc(conv_id) + "','" + esc(summary) + "'," + target + "," + tokens + ",1)"
      );
    } else {
      query_checked(
        "UPDATE mysql.agent_conversation_summary SET summary='" + esc(summary) + "'," +
        " summary_tokens=" + tokens + ", covered_upto_seq=" + target + ", version=version+1" +
        " WHERE conversation_id='" + esc(conv_id) + "' AND covered_upto_seq=" + covered
      );
    }
    mem_log_audit('L1', 'compact', 'mysql.agent_conversation_summary',
                  'covered_upto_seq=' + target, 0, 0, mo.vector_index);
    return true;
  } catch (e) {
    mem_log_audit('L1', 'degraded', 'mysql.agent_conversation_summary',
                  String(e).substring(0, 200), 0, 0, mo.vector_index);
    return false;
  }
}

/* ------------------------------------------------------------------------
 * L2a: episodic recall over past turns, via sys.ML_RAG.
 *
 * mysql.agent_memory is unreachable to ML_RAG's auto-discovery by design (it
 * lives in the `mysql` schema, which auto-discovery excludes, and its vector
 * column is named `embedding`, not `segment_embedding`).  Recall therefore
 * passes vector_store and vector_store_columns explicitly -- and, critically,
 * a document_name filter pinned to this principal.  Without that filter the
 * explicit configuration would make every principal's memory searchable by
 * every other principal, which is why document_name is injected here and is
 * not overridable from @chat_options.
 * --------------------------------------------------------------------- */
function mem_long_recall_turns(question, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.enabled || !mo.long_term.episodic_enabled)
    return { ok: false, text: '', citations: [] };
  var prefix = mem_principal_prefix();
  var res = mem_vector_search(
    'L2a', 'rag', 'mysql.agent_memory',
    { segment: 'content', segment_embedding: 'embedding' },
    question,
    { documents: [prefix], max_distance: Number(mo.long_term.episodic_max_distance || 0.6) },
    Number(mo.long_term.episodic_top_k || 3), mo);
  return res;
}

/* ------------------------------------------------------------------------
 * L3: long-term semantic facts
 * --------------------------------------------------------------------- */
function mem_long_recall_facts(question, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.enabled || !mo.long_term.semantic_enabled) return [];
  var prefix = mem_principal_prefix();
  var res = mem_vector_search(
    'L3', 'sql', 'mysql.agent_semantic_fact',
    { segment: 'statement', segment_embedding: 'embedding' },
    question,
    { documents: [prefix],
      isolation_column: 'principal_prefix',
      select_list: 'fact_id, statement, confidence, scope',
      extra_where: '(expires_at IS NULL OR expires_at > NOW())',
      /* Signals this table has always carried and recall never read:
       * confidence comes from remember_fact, and use_count/last_used_at are
       * maintained a few lines below by this very function. */
      rank: { columns: ['use_count', 'last_used_at', 'created_at'],
              recency:    'COALESCE(last_used_at, created_at)',
              importance: { expr: 'confidence', max: 100 },
              usage:      'use_count' },
      max_distance: Number(mo.long_term.semantic_max_distance || 0.6) },
    Number(mo.long_term.semantic_top_k || 3), mo);
  if (!res.ok || !res.rows.length) return [];

  var facts = [], ids = [];
  for (var i = 0; i < res.rows.length; i++) {
    facts.push({ fact_id: Number(res.rows[i].fact_id),
                 statement: String(res.rows[i].statement || ''),
                 confidence: Number(res.rows[i].confidence || 0),
                 scope: String(res.rows[i].scope || '') });
    ids.push(Number(res.rows[i].fact_id));
  }
  try {
    query_checked(
      "UPDATE mysql.agent_semantic_fact SET use_count=use_count+1, last_used_at=NOW()" +
      " WHERE principal_prefix='" + esc(prefix) + "' AND fact_id IN (" + ids.join(',') + ")"
    );
  } catch (e) { /* usage stats are best effort */ }
  return facts;
}

/* Write (or refresh) one long-term fact.  The unique key on
 * (principal_prefix, statement) makes a repeat write an ON DUPLICATE bump of
 * use_count rather than a second row. */
function mem_long_write_fact(statement, prov, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var prefix = mem_principal_prefix();
  if (!prefix) {
    mem_log_audit('L3', 'degraded', 'mysql.agent_semantic_fact', 'missing_isolation_key', 0, 0, mo.vector_index);
    mem_mark_degraded('missing_isolation_key');
    return { ok: false, error: 'missing_isolation_key' };
  }
  var text = mem_redact(String(statement || '').trim(), mo);
  if (!text) return { ok: false, error: 'empty_statement' };

  prov = prov || {};
  var confidence = Number(prov.confidence);
  if (!isFinite(confidence) || confidence < 0 || confidence > 100) confidence = 80;
  var ttl     = Number(mo.long_term.default_ttl_days || 0);
  var expires = (ttl > 0) ? "DATE_ADD(NOW(), INTERVAL " + ttl + " DAY)" : 'NULL';
  var embed   = mo.long_term.semantic_enabled
    ? mem_embed_expr(text, mo) : 'NULL';

  var sql =
    "INSERT INTO mysql.agent_semantic_fact" +
    "(principal_prefix, scope, statement, subject, predicate, object, embedding," +
    " confidence, source_conversation_id, source_turn_no, expires_at)" +
    " VALUES ('" + esc(prefix) + "','" + esc(String(prov.scope || '')) + "','" + esc(text) + "'," +
    (prov.subject   ? "'" + esc(String(prov.subject))   + "'" : 'NULL') + "," +
    (prov.predicate ? "'" + esc(String(prov.predicate)) + "'" : 'NULL') + "," +
    (prov.object    ? "'" + esc(String(prov.object))    + "'" : 'NULL') + "," +
    embed + "," + confidence + ",'" + esc(A.conversation_id || '') + "'," +
    Number(prov.turn_no || 0) + "," + expires +
    ") ON DUPLICATE KEY UPDATE use_count=use_count+1, last_used_at=NOW()," +
    " confidence=VALUES(confidence), scope=VALUES(scope), expires_at=VALUES(expires_at)";
  try {
    query_checked(sql);
    mem_log_audit('L3', 'write', 'mysql.agent_semantic_fact', text.substring(0, 120), 1, 0, mo.vector_index);
    return { ok: true };
  } catch (e) {
    /* Retry without the vector rather than losing the fact entirely. */
    if (embed !== 'NULL') {
      try {
        query_checked(sql.replace(embed, 'NULL'));
        mem_log_audit('L3', 'degraded', 'mysql.agent_semantic_fact',
                      'embedding_skipped: ' + String(e).substring(0, 160), 1, 0, mo.vector_index);
        mem_mark_degraded('embedding_unavailable');
        return { ok: true, degraded: true };
      } catch (e2) { e = e2; }
    }
    mem_log_audit('L3', 'degraded', 'mysql.agent_semantic_fact', String(e).substring(0, 200), 0, 0, mo.vector_index);
    mem_mark_degraded('semantic_write_failed');
    return { ok: false, error: String(e) };
  }
}

function mem_long_forget(filter, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var prefix = mem_principal_prefix();
  if (!prefix) return { ok: false, error: 'missing_isolation_key', removed: 0 };

  var where = "principal_prefix='" + esc(prefix) + "'";
  filter = filter || {};
  if (filter.fact_id)   where += " AND fact_id=" + Number(filter.fact_id);
  if (filter.scope)     where += " AND scope='" + esc(String(filter.scope)) + "'";
  if (filter.predicate) where += " AND predicate='" + esc(String(filter.predicate)) + "'";
  if (filter.contains)  where += " AND statement LIKE '%" + esc_like(String(filter.contains)) + "%'";
  /* Refuse an unqualified "forget everything" issued by the model: only an
   * explicit all:true from the caller may wipe a principal's facts. */
  if (!filter.fact_id && !filter.scope && !filter.predicate && !filter.contains && !filter.all)
    return { ok: false, error: 'forget_requires_filter', removed: 0 };

  try {
    var res = query_checked("DELETE FROM mysql.agent_semantic_fact WHERE " + where +
                           " LIMIT " + Number(mo.retention.purge_batch || 500));
    var removed = (res && typeof res.affected_rows !== 'undefined') ? Number(res.affected_rows) : 0;
    mem_log_audit('L3', 'forget', 'mysql.agent_semantic_fact', where.substring(0, 200), removed, 0, mo.vector_index);
    return { ok: true, removed: removed };
  } catch (e) {
    mem_log_audit('L3', 'degraded', 'mysql.agent_semantic_fact', String(e).substring(0, 200), 0, 0, mo.vector_index);
    return { ok: false, error: String(e), removed: 0 };
  }
}

/* Retention.  Runs alongside cleanup_expired_tx_leases() at the top of a
 * call, batched so it can never turn into an unbounded delete. */
function mem_long_purge_expired(opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.retention || mo.retention.enabled === false) return 0;
  var prefix = mem_principal_prefix();
  if (!prefix) return 0;
  var batch = Number(mo.retention.purge_batch || 500);
  var total = 0;
  try {
    var r1 = query_checked(
      "DELETE FROM mysql.agent_memory WHERE document_name='" + esc(prefix) + "'" +
      " AND expires_at IS NOT NULL AND expires_at < NOW() LIMIT " + batch);
    total += (r1 && r1.affected_rows) ? Number(r1.affected_rows) : 0;
    var r2 = query_checked(
      "DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='" + esc(prefix) + "'" +
      " AND expires_at IS NOT NULL AND expires_at < NOW() LIMIT " + batch);
    total += (r2 && r2.affected_rows) ? Number(r2.affected_rows) : 0;
    if (total) mem_log_audit('L2', 'purge', 'mysql.agent_memory', 'expired', total, 0, mo.vector_index);

    /* Quota.  Long-term facts have no natural end of life -- a principal that
     * keeps remembering things would otherwise grow its slice of the vector
     * scan without bound.  Evict the stalest first: never-recalled facts age
     * from their creation date, recalled ones from their last use. */
    var cap = Number(mo.retention.max_facts_per_principal || 0);
    if (cap > 0) {
      var cnt = query("SELECT COUNT(*) AS c FROM mysql.agent_semantic_fact" +
                      " WHERE principal_prefix='" + esc(prefix) + "'");
      var have = (Array.isArray(cnt) && cnt.length) ? Number(cnt[0].c) : 0;
      if (have > cap) {
        var excess = Math.min(have - cap, batch);
        var r3 = query_checked(
          "DELETE FROM mysql.agent_semantic_fact WHERE principal_prefix='" + esc(prefix) + "'" +
          " ORDER BY COALESCE(last_used_at, created_at) ASC, use_count ASC, fact_id ASC" +
          " LIMIT " + excess);
        var evicted = (r3 && r3.affected_rows) ? Number(r3.affected_rows) : 0;
        total += evicted;
        if (evicted)
          mem_log_audit('L3', 'purge', 'mysql.agent_semantic_fact',
                        'over_quota cap=' + cap, evicted, 0, mo.vector_index);
      }
    }
  } catch (e) {
    mem_log_audit('L2', 'degraded', 'purge', String(e).substring(0, 200), 0, 0, mo.vector_index);
  }
  return total;
}

/* ------------------------------------------------------------------------
 * The loop's only memory entry point.
 * --------------------------------------------------------------------- */
/* Sections are collected with a priority, then admitted against
 * budget.max_block_tokens cheapest-to-lose first, and finally emitted in
 * reading order.  Priority is what the loop cannot work without: the recent
 * turns answer "what were we just doing", the summary answers it for older
 * turns, and the recalled material is enrichment.  A dropped section is
 * audited rather than silently missing. */
function mem_block_section(sections, text, prio) {
  if (text) sections.push({ text: text, prio: prio, ord: sections.length });
}

function mem_block_assemble(sections, mo) {
  var cap = Number((mo.budget && mo.budget.max_block_tokens) || 0);
  var by_prio = sections.slice(0).sort(function(a, b) {
    return (a.prio - b.prio) || (a.ord - b.ord);
  });

  var keep = {}, used = 0, dropped = 0;
  for (var i = 0; i < by_prio.length; i++) {
    var cost = est_tok(by_prio[i].text);
    if (cap > 0 && used + cost > cap && used > 0) { dropped++; continue; }
    keep[by_prio[i].ord] = true;
    used += cost;
  }

  var out = [];
  for (var j = 0; j < sections.length; j++)
    if (keep[sections[j].ord]) out.push(sections[j].text);

  A.mem_block_tokens = used;
  if (dropped)
    mem_log_audit('L1', 'degraded', 'build_block',
                  'over_budget cap=' + cap + ' dropped_sections=' + dropped,
                  0, 0, mo.vector_index);
  return out.join('\n\n');
}

function mem_build_block(question, conv_id, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  A.mem_block_tokens = 0;
  if (!mo.enabled) return '';

  var sections = [];

  var short = mem_short_load(conv_id, mo);
  if (short.summary) {
    mem_block_section(sections,
      t('【会话摘要】（已覆盖 seq ≤ ' + short.covered_upto_seq + '）',
        '[Conversation Summary] (covers seq <= ' + short.covered_upto_seq + ')') + '\n' +
      compress(short.summary, Number(mo.short_term.summary_max_tokens || 600) * 3), 2);
  }
  var recent = mem_turns_to_text(short.turns);
  mem_block_section(sections, t('【近期对话】', '[Recent Turns]') + '\n' +
                    (recent || t('（无历史）', '(No history)')), 1);

  if (mo.long_term.episodic_enabled) {
    var ep = mem_long_recall_turns(question, mo);
    if (ep && ep.ok && ep.text && String(ep.text).trim())
      mem_block_section(sections, t('【相关历史片段】', '[Related Past Turns]') + '\n' +
                        compress(String(ep.text), 800), 4);
  }

  if (mo.long_term.semantic_enabled) {
    var facts = mem_long_recall_facts(question, mo);
    if (facts.length) {
      var flines = [t('【长期事实】', '[Long-term Facts]')];
      for (var i = 0; i < facts.length; i++)
        flines.push('- ' + facts[i].statement +
                    (facts[i].scope ? ' [' + facts[i].scope + ']' : '') +
                    ' (confidence ' + facts[i].confidence + ')');
      mem_block_section(sections, flines.join('\n'), 3);
    }
  }

  var few_shot = retrieve_few_shot(question, Number(mo.procedural.few_shot_top_k || 3));
  mem_block_section(sections, few_shot, 5);

  return mem_block_assemble(sections, mo);
}

/* Server-side conversation-scoped scratch state.  Deliberately separate from
 * @chat_options, which is a *request* object the client can read and forge:
 * the agent loop's history now comes from mysql.agent_memory via
 * MEM.short.load(), so a fabricated @chat_options.chat_history no longer
 * steers the prompt. */
function mem_session_key(key) {
  return '@_shannon_mem_' + String(key).replace(/[^A-Za-z0-9_]/g, '_');
}
function mem_session_get(key) {
  var rows = query("SELECT " + mem_session_key(key) + " AS v");
  return (Array.isArray(rows) && rows.length && rows[0].v != null) ? String(rows[0].v) : '';
}
function mem_session_set(key, val) {
  try { query_checked("SET " + mem_session_key(key) + " = '" + esc(String(val)) + "'"); }
  catch (e) {}
}
function mem_session_del(key) {
  try { query_checked("SET " + mem_session_key(key) + " = NULL"); } catch (e) {}
}

var MEM = {
  session: { get: mem_session_get, set: mem_session_set, del: mem_session_del },
  short:   { load: mem_short_load, append_turn: mem_short_append_turn,
             compact: mem_short_compact, to_text: mem_turns_to_text },
  long:    { recall_turns: mem_long_recall_turns, recall_facts: mem_long_recall_facts,
             write_fact: mem_long_write_fact, forget: mem_long_forget,
             purge_expired: mem_long_purge_expired },
  /* Exported for the self-check: rank.sql() and rank.diversify() are pure
   * functions of their arguments, so the ranking can be asserted exactly,
   * without depending on the particular cosine distances an embedding model
   * happens to produce.
   *
   * inner_list and merge_defaults are here for reach rather than purity.
   * JerryScript gives each generated root its own global scope, so a bare
   * mem_*() call only resolves when the caller happens to share a root with
   * this file; going through MEM is the one access path every caller already
   * has to hold to do anything else here.  lib_memory.js guards its
   * mem_diversify() use with a typeof check for the same reason -- these
   * exports are what let the self-check state the assumption instead of
   * relying on the current include order. */
  rank:    { options: mem_ranking_options, active: mem_rank_active,
             sql: mem_rank_sql, diversify: mem_diversify, tokens: mem_tokens,
             similarity: mem_lex_sim, inner_list: mem_inner_list },
  merge_defaults: mem_merge_defaults,
  build_block: mem_build_block,
  redact:      mem_redact,
  audit:       mem_log_audit
};
