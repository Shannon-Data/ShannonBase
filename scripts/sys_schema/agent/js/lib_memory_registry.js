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
  /* Which retrievers run, and how their answers are combined.
   *
   * 'hybrid' is the default because the two retrievers fail on disjoint
   * inputs: vector recall cannot find a table name it has no embedding for,
   * and lexical recall cannot find a paraphrase.  It costs one extra indexed
   * query per recall, against the full-text index added in
   * mysql_system_tables.sql, and degrades to vector-only by itself on a
   * datadir that predates that index.
   *
   * min_lex_ratio is the floor that makes the ngram parser usable: relevance
   * is normalised against the best hit in the pool, and anything below this
   * fraction of it is the parser's n-gram noise rather than a match.  See
   * mem_lexical_sql().
   *
   * The weights are equal, and deliberately so: no evidence yet says either
   * retriever deserves more, and picking otherwise is what the recall-quality
   * set exists to decide (sys.shannon_agent_selfcheck('recall', ...)). */
  retrieval: {
    mode:           'hybrid',
    min_lex_ratio:  0.15,
    rrf_k:          60,
    weight_vector:  1.0,
    weight_lexical: 1.0
  },
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
  /* Derivation that does not belong on the turn that created the work.
   * `enabled` controls the enqueue side only: turning it off stops the
   * backlog being recorded, not the queue being emptied, so a backlog
   * recorded before it was switched off still drains.  drain_batch is what
   * bounds the drain -- set it to 0 to stop draining here and leave the queue
   * to whatever an operator runs instead. */
  derive: { enabled: true, enqueue_batch: 200, drain_batch: 10,
            lease_seconds: 300, max_attempts: 5, purge_done_days: 7 },
  /* Per-principal, per-day ceilings.  0 is unlimited, and every one of them
   * defaults to 0: this ships as metering, and an instance that was running
   * fine yesterday must not start refusing work because it was upgraded.  See
   * lib_usage.js, which reads these straight off @chat_options rather than
   * through get_memory_options()'s memo. */
  quota: { enabled: true, max_llm_calls_per_day: 0, max_prompt_tokens_per_day: 0,
           max_artifact_bytes_per_day: 0, max_turns_per_day: 0 },
  /* Graph expansion around whatever L3 recalled.  Off by depth rather than by
   * a flag: depth 1 is the neighbours of a recalled fact, which is the case
   * with an obvious use ("this fact is about these tables"); deeper walks are
   * available but not free, and nothing yet says they pay. */
  graph: { enabled: true, depth: 1, max_nodes: 12 },
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
var MEM_RETRIEVAL_MODES = { vector: 1, lexical: 1, hybrid: 1 };

function mem_retrieval_options(mo) {
  var r = (mo && mo.retrieval) || {};
  var d = MEM_DEFAULTS.retrieval;
  var mode = String(r.mode || d.mode);
  /* An unrecognised mode falls back to the default rather than to "no
   * retriever at all", which is what an unchecked string would mean the
   * moment it reached the branch in mem_hybrid_search(). */
  if (!Object.prototype.hasOwnProperty.call(MEM_RETRIEVAL_MODES, mode)) mode = d.mode;
  return {
    mode:           mode,
    min_lex_ratio:  Math.max(0, Math.min(1, mem_num(r.min_lex_ratio, d.min_lex_ratio))),
    rrf_k:          Math.max(1, mem_num(r.rrf_k, d.rrf_k)),
    weight_vector:  Math.max(0, mem_num(r.weight_vector,  d.weight_vector)),
    weight_lexical: Math.max(0, mem_num(r.weight_lexical, d.weight_lexical))
  };
}

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
 * Lexical retrieval.
 *
 * Vector recall has one failure mode it cannot see its way out of: a token
 * that carries no semantic content.  Ask "what did we decide about
 * fact_sales" and the embedding of "fact_sales" is nearly the embedding of
 * "fact_orders", because the model never learned either name.  The same is
 * true of order ids, error codes, column names and person names -- exactly
 * the terms a database conversation is full of.  A full-text index answers
 * that class of question directly, and answers nothing about paraphrase,
 * which is the class vector recall owns.  Neither subsumes the other, so
 * recall runs both and fuses them.
 *
 * mem_lexical_sql() is the single place the full-text dialect is written
 * down.  The index is declared `WITH PARSER ngram` in
 * scripts/mysql_system_tables.sql; replacing that parser with a better one
 * means changing the parser name there and the AGAINST() clause here.
 * ------------------------------------------------------------------------ */

/* NATURAL LANGUAGE MODE rather than BOOLEAN MODE, for a security reason as
 * much as a ranking one: in BOOLEAN MODE the characters + - > < ( ) ~ * " @
 * are operators, so a user question containing them would be interpreted as
 * a query language rather than as text.  In NATURAL LANGUAGE MODE there are
 * no operators, and esc() is then sufficient to make the question a literal.
 *
 * Two things this query does that a naive MATCH ... AGAINST does not:
 *
 *   1. It ranks rather than filters.  Under the ngram parser a term is split
 *      into overlapping n-grams and matched as an OR, so a term that appears
 *      in no row still matches most rows -- 'zzzznotpresent' shares the
 *      bigrams 'es', 're', 'nt' with ordinary English text.  Used as a WHERE
 *      predicate that is close to useless; used as a score it separates
 *      cleanly, because the true hit outscores the noise by more than an
 *      order of magnitude.  Hence the normalisation below and the ratio
 *      floor the caller applies to it.
 *
 *   2. It normalises inside a bounded pool.  MATCH relevance is unnormalised
 *      and its scale depends on the corpus, so no fixed threshold survives a
 *      growing table.  Dividing by the best score in the pool gives a 0..1
 *      score that means the same thing on day one and day one thousand, and
 *      the pool is capped first so the window function never sorts the whole
 *      principal's history. */
function mem_lexical_sql(table, column, question, isolation_column, docs,
                         extra_where, select_list, pool) {
  var doc_list = [];
  for (var d = 0; d < docs.length; d++) doc_list.push("'" + esc(docs[d]) + "'");
  return "SELECT " + select_list + ", lex, lex / NULLIF(MAX(lex) OVER (), 0) AS lex_score" +
         " FROM (SELECT " + select_list + "," +
         " MATCH(`" + esc_ident(column) + "`) AGAINST ('" +
         esc(String(question).substring(0, 1024)) + "' IN NATURAL LANGUAGE MODE) AS lex" +
         " FROM " + table +
         " WHERE `" + esc_ident(isolation_column) + "` IN (" + doc_list.join(',') + ")" +
         (extra_where ? " AND " + extra_where : '') +
         " HAVING lex > 0 ORDER BY lex DESC LIMIT " + Number(pool) + ") c" +
         " ORDER BY lex DESC";
}

var MEM_LEX_POOL_FACTOR = 4;
var MEM_LEX_POOL_MAX    = 100;

/* filters carries the same isolation contract as mem_vector_search():
 * documents[] is mandatory, and an empty one means no recall rather than an
 * unfiltered scan. */
function mem_lexical_search(tier, table, columns, question, filters, topK, opt) {
  var mo    = opt || get_memory_options(get_chat_options());
  var vmode = mo.vector_index || 'scan';
  var docs  = (filters && filters.documents) ? filters.documents : [];
  var out   = { ok: false, rows: [], hits: 0 };

  if (!docs.length || !docs[0]) {
    mem_log_audit(tier, 'degraded', table, 'missing_isolation_key', 0, 0, vmode);
    mem_mark_degraded('missing_isolation_key');
    return out;
  }
  var q = String(question || '').trim();
  if (!q) return { ok: true, rows: [], hits: 0 };

  var ro   = mem_retrieval_options(mo);
  var pool = Math.min(Number(topK) * MEM_LEX_POOL_FACTOR, MEM_LEX_POOL_MAX);
  var t0   = Date.now();
  var sql  = mem_lexical_sql(table, columns.segment, q,
                             filters.isolation_column, docs,
                             filters.extra_where, filters.select_list, pool);
  var rows = query(sql);
  if (!Array.isArray(rows)) {
    /* A missing full-text index is the one failure worth distinguishing: it
     * means the server predates the index rather than that the search failed,
     * and hybrid recall should fall back to vector-only silently rather than
     * marking the whole memory layer degraded on every single turn. */
    var err = (rows && rows.error) ? String(rows.error) : 'query_failed';
    var no_index = err.indexOf('1191') !== -1 ||
                   err.indexOf("Can't find FULLTEXT") !== -1 ||
                   err.indexOf('fulltext index') !== -1;
    mem_log_audit(tier, no_index ? 'skip' : 'degraded', table,
                  (no_index ? 'no_fulltext_index: ' : '') + err.substring(0, 180),
                  0, Date.now() - t0, vmode);
    if (!no_index) mem_mark_degraded('lexical_search_failed');
    return out;
  }

  /* The ratio floor is what turns the ngram parser's OR-of-n-grams into a
   * usable list.  It is relative, not absolute, because MATCH relevance has
   * no fixed scale. */
  var keep = [];
  for (var i = 0; i < rows.length && keep.length < Number(topK); i++) {
    if (mem_num(rows[i].lex_score, 0) >= ro.min_lex_ratio) keep.push(rows[i]);
  }
  mem_log_audit(tier, 'recall', table, 'lexical', keep.length, Date.now() - t0, vmode);
  return { ok: true, rows: keep, hits: keep.length };
}

/* Reciprocal rank fusion.
 *
 * Fusing on rank rather than on score is the whole point: a cosine distance
 * and a MATCH relevance are not comparable quantities, and any attempt to put
 * them on one scale needs a calibration set that does not exist.  Rank needs
 * none -- 1/(k + rank) says only "this list put it near the top", which is
 * true of both retrievers in the same units.
 *
 * k damps the top of each list.  At the usual k = 60 the difference between
 * rank 1 and rank 2 is small, so a document both retrievers rank highly beats
 * a document one retriever ranks first and the other does not return at all.
 * That is the behaviour worth having: agreement between two independent
 * retrievers is stronger evidence than confidence within one.
 *
 * `lists` is an array of { items, weight }, each already ordered best-first.
 * key_of maps an item to its identity; the first list to produce a key owns
 * the item that survives, so the richer representation (the vector hit, which
 * carries a distance) wins over the lexical row for the same text. */
function mem_fuse_rrf(lists, key_of, rrf_k, topK) {
  var order = [], bykey = {};
  for (var l = 0; l < lists.length; l++) {
    var items  = lists[l].items || [];
    var weight = mem_num(lists[l].weight, 1);
    if (weight <= 0) continue;
    for (var i = 0; i < items.length; i++) {
      var key = String(key_of(items[i]) || '');
      if (!key) continue;
      var contribution = weight / (Number(rrf_k) + i + 1);
      if (!Object.prototype.hasOwnProperty.call(bykey, key)) {
        bykey[key] = { item: items[i], score: contribution, sources: [lists[l].name] };
        order.push(key);
      } else {
        bykey[key].score += contribution;
        bykey[key].sources.push(lists[l].name);
      }
    }
  }
  var merged = [];
  for (var o = 0; o < order.length; o++) merged.push(bykey[order[o]]);
  /* Ties broken by first-seen order, which is the first list's ranking, so
   * the result is deterministic rather than dependent on sort stability. */
  merged.sort(function (a, b) { return b.score - a.score; });

  var out = [];
  for (var m = 0; m < merged.length && out.length < Number(topK); m++) {
    var it = merged[m].item;
    it.fused_score   = merged[m].score;
    it.fused_sources = merged[m].sources.join('+');
    out.push(it);
  }
  return out;
}

/* Hybrid recall: the entry point every L2a/L3 lookup goes through.
 *
 * Vector-only is still reachable (retrieval.mode = 'vector') and is what runs
 * when the table has no full-text index, so an older datadir degrades to
 * exactly the previous behaviour rather than to an error. */
function mem_hybrid_search(tier, mode, table, columns, question, filters, topK, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var ro = mem_retrieval_options(mo);

  var vec = { ok: false, rows: [], text: '', citations: [], hits: 0 };
  if (ro.mode !== 'lexical')
    vec = mem_vector_search(tier, mode, table, columns, question, filters, topK, opt);

  if (ro.mode === 'vector' || !filters.isolation_column)
    return vec;

  /* The RAG backend reaches agent_memory through sys.ML_RAG, which is a
   * DEFINER routine and therefore needs no grants of its own.  The lexical
   * leg is a direct SELECT as the invoker, so on a principal with no grant on
   * mysql.agent_memory it fails where the vector leg succeeded.  That is a
   * skip, not a degradation -- vector-only recall is still a correct answer. */
  var lex = mem_lexical_search(tier, table, columns, question, filters, topK, opt);
  if (!lex.ok || !lex.rows.length) {
    if (vec.ok) vec.retrieval_mode = 'vector_only';
    return vec;
  }
  if (!vec.ok || (!vec.hits && !vec.rows.length)) {
    /* Lexical-only: still a real answer, and the only one available when the
     * embedding model is absent -- which is the normal state of a CI run. */
    var lex_out = { ok: true, rows: lex.rows, text: '', citations: [],
                    hits: lex.rows.length, retrieval_mode: 'lexical_only' };
    if (mode === 'rag') {
      var segs = [];
      for (var i = 0; i < lex.rows.length; i++) {
        var seg = String(lex.rows[i][columns.segment] || '');
        if (seg) { segs.push(seg); lex_out.citations.push({ segment: seg, distance: null }); }
      }
      lex_out.text = segs.join('\n\n');
      lex_out.hits = lex_out.citations.length;
    }
    mem_log_audit(tier, 'recall', table, 'lexical_only', lex_out.hits, 0, mo.vector_index);
    return lex_out;
  }

  /* Both legs returned.  Fuse on the text itself: it is the only identity the
   * two sides share, because an ML_RAG citation carries no primary key. */
  var seg_col = columns.segment;
  function key_of(x) {
    if (x === null || x === undefined) return '';
    var v = (x.segment !== undefined && x.segment !== null) ? x.segment : x[seg_col];
    return String(v === undefined || v === null ? '' : v).trim();
  }

  var vector_items = (mode === 'rag') ? vec.citations : vec.rows;
  var fused = mem_fuse_rrf(
    [ { name: 'vector',  items: vector_items, weight: ro.weight_vector },
      { name: 'lexical', items: lex.rows,     weight: ro.weight_lexical } ],
    key_of, ro.rrf_k, topK);

  var res = { ok: true, rows: [], text: '', citations: [], hits: fused.length,
              retrieval_mode: 'hybrid' };
  if (mode === 'rag') {
    var texts = [];
    for (var f = 0; f < fused.length; f++) {
      var text = key_of(fused[f]);
      if (!text) continue;
      texts.push(text);
      /* A fused item is whichever object the first list produced.  A lexical
       * row is not a citation, so give it the citation shape the caller
       * already parses rather than making every consumer type-test. */
      res.citations.push(fused[f].segment !== undefined ? fused[f]
                                                        : { segment: text, distance: null,
                                                            fused_score: fused[f].fused_score,
                                                            fused_sources: fused[f].fused_sources });
    }
    res.text = texts.join('\n\n');
    res.hits = res.citations.length;
  } else {
    res.rows = fused;
    res.hits = fused.length;
  }
  mem_log_audit(tier, 'recall', table, 'hybrid_rrf', res.hits, 0, mo.vector_index);
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

  var meta_obj = {
    conversation_id: conv_id, route: route, turn_id: current_turn_id(),
    cost: (A.cost && A.cost.llm_calls) ? A.cost : null
  };
  /* Why the decision is recorded and not just acted on: `embedding IS NULL`
   * is the only thing the backlog sweep can see, and it cannot tell a vector
   * the model failed to produce from one mem_should_embed() deliberately did
   * not ask for.  Without this marker the sweep enqueues the approval
   * chatter that the skip policy exists to keep out, and the drain then pays
   * exactly the ML_EMBED_ROW cost the skip was saving -- one turn later,
   * where nobody is looking for it.  Only written when the answer is "no", so
   * the common row carries nothing extra. */
  if (!embed) meta_obj.embed_skipped = true;
  var meta_json = JSON.stringify(meta_obj);

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
  var res = mem_hybrid_search(
    'L2a', 'rag', 'mysql.agent_memory',
    { segment: 'content', segment_embedding: 'embedding' },
    question,
    { documents: [prefix],
      /* The lexical leg needs these three; the vector leg reaches the same
       * table through sys.ML_RAG, which injects document_name itself. */
      isolation_column: 'document_name',
      select_list:      'content',
      /* No role filter: the legs must see the same candidate rows or the
       * fusion is comparing two different corpora.  The vector leg is
       * narrower by construction -- ML_RAG's query requires a non-null
       * embedding -- and that asymmetry is the point, since the lexical leg
       * can still reach a turn whose embedding was never written. */
      extra_where:      '(expires_at IS NULL OR expires_at > NOW())',
      max_distance: Number(mo.long_term.episodic_max_distance || 0.6) },
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
  var res = mem_hybrid_search(
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

/* Turn a fact's subject/predicate/object into graph edges.
 *
 * Those three columns, and the index over them, have existed since the table
 * was created; nothing ever wrote them, so every row was NULL and
 * forget_memory's `predicate` filter could only ever match nothing.  They are
 * written now, and the same triple is mirrored into agent_memory_edge so the
 * structure is walkable rather than only filterable.
 *
 * Best effort by design: a fact whose edges cannot be written is still a
 * fact, and the statement -- not the triple -- is what recall reads. */
function mem_link_fact_triple(prefix, statement, prov, mo) {
  if (!mo.graph || mo.graph.enabled === false) return;
  prov = prov || {};
  var rows = query("SELECT fact_id FROM mysql.agent_semantic_fact" +
                   " WHERE principal_prefix='" + esc(prefix) + "'" +
                   "   AND statement='" + esc(statement) + "' LIMIT 1");
  if (!Array.isArray(rows) || !rows.length) return;
  var fid = String(rows[0].fact_id);

  if (prov.subject && prov.predicate && prov.object) {
    mem_graph_link('fact', fid, 'asserts', 'entity', String(prov.subject), 1,
                   { predicate: String(prov.predicate), object: String(prov.object) }, mo);
    mem_graph_link('entity', String(prov.subject), String(prov.predicate),
                   'entity', String(prov.object), 1, null, mo);
  }
  /* scope is where the fact applies -- a schema or table name.  It is the one
   * structured field remember_fact callers actually fill in. */
  if (prov.scope) mem_graph_link('fact', fid, 'scoped_to', 'table', String(prov.scope), 1, null, mo);
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
    mem_link_fact_triple(prefix, text, prov, mo);
    return { ok: true };
  } catch (e) {
    /* Retry without the vector rather than losing the fact entirely. */
    if (embed !== 'NULL') {
      try {
        query_checked(sql.replace(embed, 'NULL'));
        mem_log_audit('L3', 'degraded', 'mysql.agent_semantic_fact',
                      'embedding_skipped: ' + String(e).substring(0, 160), 1, 0, mo.vector_index);
        mem_mark_degraded('embedding_unavailable');
        mem_link_fact_triple(prefix, text, prov, mo);
        /* The row exists and has no vector, so episodic recall cannot see it.
         * Record that rather than leaving it to be noticed, or not, later. */
        mem_derive_enqueue_missing(mo);
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

  /* Same maintenance slot, opposite direction: retention removes rows that
   * have outlived their use, and this records the rows that were never
   * finished.  Both are bounded and both are cheap once there is nothing to
   * do -- an indexed anti-join that writes zero rows. */
  mem_derive_enqueue_missing(mo);
  return total;
}

/* ------------------------------------------------------------------------
 * Asynchronous derivation.
 *
 * Embedding used to happen on the user's turn: persisting a turn called
 * sys.ML_EMBED_ROW inline, so the reply waited on the embedding model, and a
 * model that was unavailable cost that turn its vector permanently -- the
 * write was retried without the embedding, audited as degraded, and never
 * looked at again.  A row with no vector is invisible to episodic recall
 * forever.
 *
 * The queue makes that recoverable.  Anything that should carry a vector and
 * does not is enqueued, and MEM.derive.drain() fills it in later, out of
 * band.  Two consequences worth stating:
 *
 *   - The backlog is derived, not tracked.  mem_derive_enqueue_missing() asks
 *     the table which rows lack a vector rather than trusting that every
 *     failure remembered to enqueue itself, so a crash between the write and
 *     the enqueue costs nothing.  uk_task makes the repeat enqueue a no-op.
 *   - The drain runs here, in JavaScript, on the maintenance slot every
 *     invocation already passes through.  It was a stored procedure
 *     (sys.shannon_agent_derive) for one reason: two concurrent LANGUAGE
 *     JAVASCRIPT calls used to abort the server, so a scheduler EVENT firing
 *     mid-conversation was fatal.  That is fixed -- the engine context is
 *     per-thread now -- and the procedure it forced was a system object
 *     nothing ever called: the EVENT that would have driven it was only ever
 *     a suggestion in its own header comment, so in practice the queue was
 *     filled and never emptied.  Draining from the code that does the
 *     enqueuing also keeps the two halves from drifting: they agree about
 *     the kind names, the columns and the 1800-character bound because they
 *     are written next to each other.
 * --------------------------------------------------------------------- */

/* Enqueue every row of this principal's that should have a vector and does
 * not.  INSERT IGNORE against uk_task, so calling it on every turn costs one
 * indexed anti-join and writes nothing once the backlog is empty. */
function mem_derive_enqueue_missing(opt) {
  var mo = opt || get_memory_options(get_chat_options());
  if (!mo.derive || mo.derive.enabled === false) return 0;
  if (!mo.long_term.episodic_enabled && !mo.long_term.semantic_enabled) return 0;
  var prefix = mem_principal_prefix();
  if (!prefix) return 0;

  var batch = Number(mo.derive.enqueue_batch || 200);
  var total = 0;

  /* Each statement is its own try: a principal may hold grants on one of
   * these tables and not the other, and a backlog that cannot be enqueued is
   * not a reason to skip the one that can. */
  var jobs = [
    { kind: 'embed_memory', table: 'agent_memory',
      enabled: mo.long_term.episodic_enabled,
      select: "SELECT document_name, 'embed_memory', 'agent_memory', id," +
              " JSON_OBJECT('source_column','content','target_column','embedding')" +
              " FROM mysql.agent_memory" +
              " WHERE document_name='" + esc(prefix) + "' AND embedding IS NULL" +
              "   AND content IS NOT NULL AND content <> ''" +
              /* A row the writer chose not to embed is not a gap to fill.
               * Rows written before the marker existed carry no opinion and
               * are still swept, which is the conservative direction: the
               * worst case is embedding something that would have been
               * skipped, not leaving a real gap unfilled. */
              "   AND COALESCE(meta->>'$.embed_skipped','') <> 'true'" +
              "   AND (expires_at IS NULL OR expires_at > NOW())" +
              " ORDER BY id DESC LIMIT " + batch },
    { kind: 'embed_fact', table: 'agent_semantic_fact',
      enabled: mo.long_term.semantic_enabled,
      select: "SELECT principal_prefix, 'embed_fact', 'agent_semantic_fact', fact_id," +
              " JSON_OBJECT('source_column','statement','target_column','embedding')" +
              " FROM mysql.agent_semantic_fact" +
              " WHERE principal_prefix='" + esc(prefix) + "' AND embedding IS NULL" +
              "   AND (expires_at IS NULL OR expires_at > NOW())" +
              " ORDER BY fact_id DESC LIMIT " + batch }
  ];

  for (var j = 0; j < jobs.length; j++) {
    if (!jobs[j].enabled) continue;
    try {
      var r = query_checked(
        "INSERT IGNORE INTO mysql.agent_derive_queue" +
        " (principal_prefix, kind, target_table, target_id, payload) " + jobs[j].select);
      var n = (r && r.affected_rows) ? Number(r.affected_rows) : 0;
      total += n;
      if (n) mem_log_audit('L2', 'enqueue', 'mysql.agent_derive_queue',
                           jobs[j].kind, n, 0, mo.vector_index);
    } catch (e) {
      mem_log_audit('L2', 'degraded', 'mysql.agent_derive_queue',
                    jobs[j].kind + ': ' + String(e).substring(0, 160), 0, 0, mo.vector_index);
    }
  }
  return total;
}

/* How much work is outstanding for this principal?  Reported to the caller so
 * "recall found nothing" can be told apart from "recall has not been built
 * yet", which are the same symptom and completely different problems. */
/* Work the backlog down, bounded, on the maintenance slot.
 *
 * Scoped to one principal, unlike the stored procedure this replaces: that
 * one ran as a DBA over every row in the table, while this runs inside
 * somebody's session and must not touch anybody else's rows.  Each principal
 * therefore drains its own backlog, which is also the only backlog its own
 * recall cares about.
 *
 * The lease token is what makes a claim exclusive.  Every statement after
 * the claim addresses rows by lease_owner and never by state alone, so two
 * sessions draining at the same moment cannot take the same task -- which is
 * no longer hypothetical, because two sessions can now run JavaScript at the
 * same time.
 *
 * Nothing here is allowed to leave a task in 'running': the last step
 * resolves whatever this token still holds, either to 'done' or back to the
 * queue with backoff.  A crash is covered by the lease instead, reclaimed by
 * step 1 of the next drain.
 */
function mem_derive_drain(opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var d  = mo.derive || {};
  /* Deliberately not gated on d.enabled: that switch governs recording the
   * backlog, and a backlog recorded before it was switched off still has to
   * drain.  drain_batch = 0 is the switch for this half. */
  var batch = Math.max(0, Math.min(Number(d.drain_batch || 0), 200));
  if (!batch) return 0;
  var prefix = mem_principal_prefix();
  if (!prefix) return 0;

  var lease    = Math.max(30, Number(d.lease_seconds || 300));
  var attempts = Math.max(1, Number(d.max_attempts || 5));
  var days     = Math.max(0, Number(d.purge_done_days || 7));
  var model    = get_embed_model_id();
  var dim      = Number(mo.long_term.embed_dim || 384);
  var p        = esc(prefix);
  /* Unique per drain: a token reused across two drains would let the later
   * one resolve tasks the earlier one still holds. */
  var token = esc(('js-' + prefix + '-' + Date.now() + '-' +
                   Math.floor(Math.random() * 1000000)).substring(0, 64));

  /* LEFT(...,1800) and the same JSON_OBJECT as mem_embed_expr(), so a row
   * embedded here and a row embedded inline get the same vector rather than
   * two vectors of the same text that do not quite match. */
  function embed_of(col) {
    return "sys.ML_EMBED_ROW(LEFT(" + col + ",1800)," +
           "JSON_OBJECT('model_id','" + esc(model) + "','truncate',true))";
  }

  var claimed = 0, err = '';
  try {
    /* 1. A drainer that died holding tasks released nothing; the lease is
     *    what gives them back. */
    query_checked(
      "UPDATE mysql.agent_derive_queue SET state='pending', lease_owner=''," +
      " lease_expires_at=NULL WHERE principal_prefix='" + p + "' AND state='running'" +
      "   AND lease_expires_at IS NOT NULL AND lease_expires_at < NOW()");

    /* 2. Claim. */
    var c = query_checked(
      "UPDATE mysql.agent_derive_queue SET state='running', lease_owner='" + token + "'," +
      " lease_expires_at=NOW() + INTERVAL " + lease + " SECOND, attempts=attempts+1" +
      " WHERE principal_prefix='" + p + "' AND state='pending' AND available_at <= NOW()" +
      " ORDER BY task_id LIMIT " + batch);
    claimed = (c && c.affected_rows) ? Number(c.affected_rows) : 0;
  } catch (e) {
    mem_log_audit('L2', 'degraded', 'mysql.agent_derive_queue',
                  'claim: ' + String(e).substring(0, 160), 0, 0, mo.vector_index);
    return 0;
  }
  if (!claimed) return 0;

  var embedded = 0;
  try {
    /* 3. A task whose target retention has already deleted is complete, not
     *    stuck -- otherwise it retries a vanished row to max_attempts. */
    query_checked(
      "UPDATE mysql.agent_derive_queue q LEFT JOIN mysql.agent_memory m ON m.id=q.target_id" +
      " SET q.state='done', q.lease_owner='', q.lease_expires_at=NULL" +
      " WHERE q.lease_owner='" + token + "' AND q.kind='embed_memory' AND m.id IS NULL");
    query_checked(
      "UPDATE mysql.agent_derive_queue q" +
      " LEFT JOIN mysql.agent_semantic_fact f ON f.fact_id=q.target_id" +
      " SET q.state='done', q.lease_owner='', q.lease_expires_at=NULL" +
      " WHERE q.lease_owner='" + token + "' AND q.kind='embed_fact' AND f.fact_id IS NULL");

    /* 4. The derivation, one set-based statement per kind.  embed_model_id
     *    and embed_dim move with the vector for the same reason they do on
     *    the inline path: a row must not claim a model it did not store. */
    var e1 = query_checked(
      "UPDATE mysql.agent_memory m JOIN mysql.agent_derive_queue q" +
      "   ON q.lease_owner='" + token + "' AND q.kind='embed_memory' AND q.target_id=m.id" +
      " SET m.embedding=" + embed_of('m.content') + ", m.embed_model_id='" + esc(model) + "'," +
      "     m.embed_dim=" + dim +
      " WHERE m.embedding IS NULL AND m.content IS NOT NULL AND m.content <> ''");
    embedded += (e1 && e1.affected_rows) ? Number(e1.affected_rows) : 0;

    var e2 = query_checked(
      "UPDATE mysql.agent_semantic_fact f JOIN mysql.agent_derive_queue q" +
      "   ON q.lease_owner='" + token + "' AND q.kind='embed_fact' AND q.target_id=f.fact_id" +
      " SET f.embedding=" + embed_of('f.statement') +
      " WHERE f.embedding IS NULL AND f.statement <> ''");
    embedded += (e2 && e2.affected_rows) ? Number(e2.affected_rows) : 0;

    /* 5. Done is read back off the target, not assumed from the UPDATE
     *    returning: a row whose embedding came back NULL is not done. */
    query_checked(
      "UPDATE mysql.agent_derive_queue q JOIN mysql.agent_memory m ON m.id=q.target_id" +
      " SET q.state='done', q.lease_owner='', q.lease_expires_at=NULL, q.last_error=NULL" +
      " WHERE q.lease_owner='" + token + "' AND q.kind='embed_memory' AND m.embedding IS NOT NULL");
    query_checked(
      "UPDATE mysql.agent_derive_queue q JOIN mysql.agent_semantic_fact f ON f.fact_id=q.target_id" +
      " SET q.state='done', q.lease_owner='', q.lease_expires_at=NULL, q.last_error=NULL" +
      " WHERE q.lease_owner='" + token + "' AND q.kind='embed_fact' AND f.embedding IS NOT NULL");
  } catch (e) {
    /* The usual failure is the embedding model not being loaded, which is a
     * reason to back off, not to lose the task. */
    err = String(e).substring(0, 400);
  }

  /* 6. Whatever this token still holds did not derive.  Park it past
   *    max_attempts so a task that can never succeed stops being retried,
   *    and back off exponentially until then. */
  try {
    query_checked(
      "UPDATE mysql.agent_derive_queue" +
      " SET state=IF(attempts >= " + attempts + ",'failed','pending')," +
      "     lease_owner='', lease_expires_at=NULL," +
      "     last_error=" + (err ? "'" + esc(err) + "'" : 'NULL') + "," +
      "     available_at=NOW() + INTERVAL LEAST(POW(2,attempts),3600) SECOND" +
      " WHERE lease_owner='" + token + "' AND state='running'");
  } catch (e) {
    mem_log_audit('L2', 'degraded', 'mysql.agent_derive_queue',
                  'park: ' + String(e).substring(0, 160), 0, 0, mo.vector_index);
  }

  /* 7. Completed tasks are a log, and a log needs an end. */
  if (days > 0) {
    try {
      query_checked(
        "DELETE FROM mysql.agent_derive_queue WHERE principal_prefix='" + p + "'" +
        "  AND state='done' AND updated_at < NOW() - INTERVAL " + days + " DAY LIMIT 200");
    } catch (e) {}
  }

  if (embedded || err)
    mem_log_audit('L2', err ? 'degraded' : 'write', 'mysql.agent_derive_queue',
                  'drain claimed=' + claimed + ' embedded=' + embedded +
                  (err ? ' err=' + err.substring(0, 120) : ''),
                  embedded, 0, mo.vector_index);
  return embedded;
}

function mem_derive_backlog(opt) {
  var prefix = mem_principal_prefix();
  if (!prefix) return { pending: 0, failed: 0 };
  var rows = query(
    "SELECT SUM(state='pending') AS pending, SUM(state='failed') AS failed" +
    " FROM mysql.agent_derive_queue WHERE principal_prefix='" + esc(prefix) + "'");
  if (!Array.isArray(rows) || !rows.length) return { pending: 0, failed: 0 };
  return { pending: Number(rows[0].pending || 0), failed: Number(rows[0].failed || 0) };
}

/* ------------------------------------------------------------------------
 * The memory graph.
 *
 * Distance answers "what reads like this question".  It cannot answer "which
 * tables does the fact I just recalled actually name", because that is a
 * join.  agent_semantic_fact has carried subject/predicate/object columns and
 * an index over them since it was created, and nothing ever wrote them, so
 * the structure the agent was told to record had nowhere to go.  These edges
 * are where it goes.
 * --------------------------------------------------------------------- */

var MEM_GRAPH_MAX_DEPTH = 3;

function mem_graph_link(src_kind, src_id, rel, dst_kind, dst_id, weight, meta, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var prefix = mem_principal_prefix();
  if (!prefix) return false;
  if (!src_kind || !src_id || !rel || !dst_kind || !dst_id) return false;

  var ttl     = Number(mo.long_term.default_ttl_days || 0);
  var expires = (ttl > 0) ? "DATE_ADD(NOW(), INTERVAL " + ttl + " DAY)" : 'NULL';
  var w       = mem_num(weight, 1);
  try {
    query_checked(
      "INSERT INTO mysql.agent_memory_edge" +
      " (principal_prefix, src_kind, src_id, rel, dst_kind, dst_id, weight, meta, expires_at)" +
      " VALUES ('" + esc(prefix) + "','" + esc(String(src_kind).substring(0, 16)) + "'," +
      "'" + esc(String(src_id).substring(0, 191)) + "'," +
      "'" + esc(String(rel).substring(0, 64)) + "'," +
      "'" + esc(String(dst_kind).substring(0, 16)) + "'," +
      "'" + esc(String(dst_id).substring(0, 191)) + "'," + w + "," +
      (meta ? "CAST('" + esc(JSON.stringify(meta)) + "' AS JSON)" : 'NULL') + "," + expires + ")" +
      /* A repeated assertion is the same edge, not a second one: reinforce it
       * and push its expiry out rather than duplicating it. */
      " ON DUPLICATE KEY UPDATE weight=VALUES(weight), meta=VALUES(meta)," +
      " expires_at=VALUES(expires_at)");
    return true;
  } catch (e) {
    mem_log_audit('L3', 'degraded', 'mysql.agent_memory_edge',
                  String(e).substring(0, 200), 0, 0, mo.vector_index);
    return false;
  }
}

/* Walk out from a set of seed nodes.
 *
 * The seed rows are CAST, and that is load-bearing rather than tidy: a
 * recursive CTE takes its column types from the non-recursive branch alone,
 * so an uncast seed of SELECT 'table','orders' fixes id at CHAR(6) and every
 * longer name found by the recursive branch is silently dropped -- the query
 * returns zero rows and reports no error at all.
 *
 * Edges are followed in both directions.  "fact 7 mentions table orders" and
 * "which facts mention orders" are the same edge asked from two ends, and
 * storing each edge once means the walk, not the writer, has to handle it. */
function mem_graph_expand(seeds, depth, limit, opt) {
  var mo = opt || get_memory_options(get_chat_options());
  var prefix = mem_principal_prefix();
  if (!prefix || !seeds || !seeds.length) return [];

  depth = Math.max(1, Math.min(Number(depth || 1), MEM_GRAPH_MAX_DEPTH));
  limit = Math.max(1, Number(limit || 20));

  var seed_sql = [];
  for (var i = 0; i < seeds.length; i++) {
    if (!seeds[i] || !seeds[i].kind || !seeds[i].id) continue;
    seed_sql.push(
      "SELECT CAST('" + esc(String(seeds[i].kind).substring(0, 16)) + "' AS CHAR(16)) AS kind," +
      " CAST('" + esc(String(seeds[i].id).substring(0, 191)) + "' AS CHAR(191)) AS id," +
      " CAST('' AS CHAR(64)) AS rel, 0 AS depth");
  }
  if (!seed_sql.length) return [];

  var t0 = Date.now();
  var rows = query(
    "WITH RECURSIVE walk (kind, id, rel, depth) AS (" +
    seed_sql.join(" UNION ALL ") +
    " UNION ALL " +
    " SELECT n.kind, n.id, n.rel, w.depth + 1 FROM walk w JOIN (" +
    "   SELECT src_kind AS from_kind, src_id AS from_id, dst_kind AS kind, dst_id AS id, rel" +
    "     FROM mysql.agent_memory_edge" +
    "    WHERE principal_prefix='" + esc(prefix) + "'" +
    "      AND (expires_at IS NULL OR expires_at > NOW())" +
    "   UNION ALL" +
    "   SELECT dst_kind, dst_id, src_kind, src_id, rel" +
    "     FROM mysql.agent_memory_edge" +
    "    WHERE principal_prefix='" + esc(prefix) + "'" +
    "      AND (expires_at IS NULL OR expires_at > NOW())" +
    " ) n ON n.from_kind = w.kind AND n.from_id = w.id" +
    " WHERE w.depth < " + depth +
    ")" +
    " SELECT kind, id, rel, MIN(depth) AS depth FROM walk WHERE depth > 0" +
    " GROUP BY kind, id, rel ORDER BY depth ASC, kind, id LIMIT " + limit);

  if (!Array.isArray(rows)) {
    mem_log_audit('L3', 'degraded', 'mysql.agent_memory_edge',
                  (rows && rows.error) ? String(rows.error).substring(0, 180) : 'walk_failed',
                  0, Date.now() - t0, mo.vector_index);
    return [];
  }
  var out = [];
  for (var r = 0; r < rows.length; r++)
    out.push({ kind: String(rows[r].kind || ''), id: String(rows[r].id || ''),
               rel: String(rows[r].rel || ''), depth: Number(rows[r].depth || 0) });
  mem_log_audit('L3', 'recall', 'mysql.agent_memory_edge',
                'graph depth=' + depth, out.length, Date.now() - t0, mo.vector_index);
  return out;
}

function mem_graph_to_text(nodes) {
  if (!nodes || !nodes.length) return '';
  var lines = [];
  for (var i = 0; i < nodes.length; i++)
    lines.push('- ' + nodes[i].rel + ' → ' + nodes[i].kind + ':' + nodes[i].id +
               ' (hop ' + nodes[i].depth + ')');
  return lines.join('\n');
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
      var seeds  = [];
      for (var i = 0; i < facts.length; i++) {
        flines.push('- ' + facts[i].statement +
                    (facts[i].scope ? ' [' + facts[i].scope + ']' : '') +
                    ' (confidence ' + facts[i].confidence + ')');
        seeds.push({ kind: 'fact', id: String(facts[i].fact_id) });
      }
      mem_block_section(sections, flines.join('\n'), 3);

      /* What the recalled facts are about, rather than what they say.  Seeded
       * from the facts recall just chose, so the walk is bounded by the same
       * top_k and never runs on a turn that recalled nothing. */
      if (mo.graph && mo.graph.enabled !== false) {
        var nodes = mem_graph_expand(seeds, mo.graph.depth, mo.graph.max_nodes, mo);
        var gtext = mem_graph_to_text(nodes);
        if (gtext)
          mem_block_section(sections,
            t('【相关实体】', '[Related Entities]') + '\n' + gtext, 6);
      }
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
  /* Retrieval internals, exported for the same reason the ranking helpers
   * are: each generated JerryScript root has its own global scope, so a bare
   * mem_*() call only resolves inside a root that happens to include this
   * file.  MEM is the handle every caller already holds. */
  search:  { vector: mem_vector_search, lexical: mem_lexical_search,
             hybrid: mem_hybrid_search, fuse: mem_fuse_rrf,
             options: mem_retrieval_options, lexical_sql: mem_lexical_sql },
  derive:  { enqueue_missing: mem_derive_enqueue_missing, backlog: mem_derive_backlog,
             drain: mem_derive_drain },
  graph:   { link: mem_graph_link, expand: mem_graph_expand, to_text: mem_graph_to_text },
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
