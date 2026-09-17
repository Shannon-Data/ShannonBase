//@include lib_lang.js

function save_secret_api_key(real_key) {
  if (!real_key || real_key === '***') return;
  try {
    sys.exec_sql("SET @chat_api_key = '" + esc(real_key) + "'");
  } catch (e) {}
}

function load_secret_api_key() {
  try {
    var rows = query("SELECT @chat_api_key AS k");
    if (Array.isArray(rows) && rows.length && rows[0].k) return String(rows[0].k);
  } catch (e) {}
  return '';
}

function get_chat_options() {
  if (A.cached_chat_opt) return A.cached_chat_opt;
  try {
    var rows = query("SELECT @chat_options AS opt");
    if (!rows || !Array.isArray(rows) || !rows.length || !rows[0].opt) return {};
    A.cached_chat_opt = JSON.parse(rows[0].opt);
    if (A.cached_chat_opt.model_options) {
      var cur_key = A.cached_chat_opt.model_options.api_key;
      if (!cur_key || cur_key === '***') {
        var real_key = load_secret_api_key();
        if (real_key) A.cached_chat_opt.model_options.api_key = real_key;
      }
    }
    return A.cached_chat_opt;
  } catch(e) { return {}; }
}

// `analyze_intent` is defined centrally in lib_lang.js for single-point modification.

/**
 * Stopword-ish tokens that are so common across schemas (id/name/time/status
 * fields, generic verbs) that a raw substring hit carries almost no signal.
 * Kept small and cheap — this is not meant to be a real IDF corpus, just a
 * denylist for the highest-frequency offenders that were previously dragging
 * half the schema into Tier1 (see build_schema_context_fallback).
 */
var LOW_SIGNAL_KEYWORDS = {
  'id':1,'name':1,'time':1,'date':1,'status':1,'type':1,'data':1,'value':1,
  '时间':1,'状态':1,'名称':1,'类型':1,'数据':1,'编号':1,'信息':1,'记录':1
};

/**
 * Score candidate tables against the user message using a lightweight
 * per-keyword IDF: a keyword that matches N distinct tables out of T total
 * contributes weight ~log(T/N) per match, instead of every match counting
 * equally.  This keeps generic column names (id/status/时间...) from
 * inflating the candidate set to near-schema-size once table count grows
 * into the hundreds/thousands — the failure mode that previously forced
 * Tier1 to truncate by scan order rather than by relevance.
 *
 * Returns an array of {table, score, name_hit} sorted by score descending
 * (score > 0 only). Table-name / comment substring hits get an additional
 * flat bonus since a keyword literally naming the table is strong signal
 * regardless of corpus frequency.
 */
/**
 * Reset the per-turn infer_candidate_tables() memo cache. Call this once at
 * the top of shannon_agent_run(), before the cache is read/written anywhere
 * else, so scores from a previous user message never leak into the current
 * turn. Cheap by design — the cache itself is only ever populated (and thus
 * only ever needs clearing) within a single shannon_agent_run() invocation,
 * since the whole module (including `var A = {...}`) is re-evaluated fresh
 * on every call; this exists mainly to give within-turn callers (e.g.
 * build_schema_context() followed by a list_tables tool call using the same
 * keyword) an explicit, named reset point rather than reaching into A
 * directly, and to make the cache's lifetime an intentional contract rather
 * than an accident of A's re-initialization.
 */
function clear_shared_idf_cache() {
  A.idf_cache = {};
}

function infer_candidate_tables(db, intent, user_msg, limit) {
  if (!db) return [];
  limit = limit || 20;
  var text = String(user_msg || '');

  A.idf_cache = A.idf_cache || {};
  var cache_key = db + '\u0001' + limit + '\u0001' + text;
  if (A.idf_cache.hasOwnProperty(cache_key)) return A.idf_cache[cache_key];

  var raw_kw = text.match(/[A-Za-z0-9_]+|[\u4e00-\u9fff]{1,4}/g) || [];
  if (!raw_kw.length) return (A.idf_cache[cache_key] = []);

  var seen = {}, keywords = [];
  for (var i = 0; i < raw_kw.length && keywords.length < 10; i++) {
    var k = String(raw_kw[i]).toLowerCase();
    if (k.length < 1 || seen[k]) continue;
    /* Filter noise tokens produced by the [\u4e00-\u9fff]{1,4} regex
     * splitting a long Chinese sentence into arbitrary 1-4 char chunks:
     *   - single Chinese characters (年, 月, 的) carry zero search signal
     *   - 1–2 digit numbers (1, 6, 24) are too generic for LIKE filtering
     *     (4-digit numbers like 2024 are meaningful and pass through) */
    if (/^[\u4e00-\u9fff]$/.test(k)) continue;
    if (/^\d{1,2}$/.test(k)) continue;
    seen[k] = true;
    keywords.push(k);
  }
  if (!keywords.length) return (A.idf_cache[cache_key] = []);

  var total_tables_rows = query(
    "SELECT COUNT(*) AS cnt FROM information_schema.TABLES" +
    " WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE'"
  );
  var total_tables = (Array.isArray(total_tables_rows) && total_tables_rows.length) ?
    Number(total_tables_rows[0].cnt) || 1 : 1;

  var scores = {};   // table -> accumulated score
  var name_hit = {}; // table -> true if keyword hit the table name itself

  for (var ki = 0; ki < keywords.length; ki++) {
    var kw = keywords[ki];
    var ek = esc_like(kw);
    var is_low_signal = !!LOW_SIGNAL_KEYWORDS[kw];

    // Table-name / TABLE_COMMENT hits: strongest signal, flat bonus, always counted.
    var tbl_rows = query(
      "SELECT TABLE_NAME FROM information_schema.TABLES" +
      " WHERE TABLE_SCHEMA='" + esc(db) + "' AND TABLE_TYPE='BASE TABLE' AND" +
      " (TABLE_NAME LIKE '%" + ek + "%' OR TABLE_COMMENT LIKE '%" + ek + "%') LIMIT 30"
    );
    if (Array.isArray(tbl_rows)) {
      for (var ti = 0; ti < tbl_rows.length; ti++) {
        var tn0 = String(tbl_rows[ti].TABLE_NAME || '');
        if (!tn0) continue;
        scores[tn0] = (scores[tn0] || 0) + 3.0; // table-name match is high-confidence
        name_hit[tn0] = true;
      }
    }

    if (is_low_signal) continue; // don't let generic column names spam the column-level pass

    var col_rows = query(
      "SELECT TABLE_NAME, COUNT(*) AS hit_cnt FROM information_schema.COLUMNS" +
      " WHERE TABLE_SCHEMA='" + esc(db) + "' AND" +
      " (COLUMN_NAME LIKE '%" + ek + "%' OR COLUMN_COMMENT LIKE '%" + ek + "%')" +
      " GROUP BY TABLE_NAME LIMIT 200"
    );
    if (!Array.isArray(col_rows) || !col_rows.length) continue;
    var n_matched = col_rows.length;
    // idf: rarer keywords across the schema carry more weight; +1 avoids log(0)/div0.
    var idf = Math.log((total_tables + 1) / (n_matched + 1)) + 1;
    for (var ci = 0; ci < col_rows.length; ci++) {
      var tn = String(col_rows[ci].TABLE_NAME || '');
      if (!tn) continue;
      scores[tn] = (scores[tn] || 0) + idf;
    }
  }

  var ranked = Object.keys(scores).map(function(tn) {
    return { table: tn, score: scores[tn], name_hit: !!name_hit[tn] };
  });
  ranked.sort(function(a, b) { return b.score - a.score; });
  return (A.idf_cache[cache_key] = ranked.slice(0, limit));
}

function build_task_header(intent) {
  if (intent && intent.kind === 'diagnose') {
    return t('请优先使用 information_schema / performance_schema 做故障诊断，避免凭经验猜表名。',
             'Prioritize information_schema / performance_schema for diagnostics and avoid guessing table names.');
  }
  if (intent && intent.kind === 'analytics') {
    return t('请先确认主表和聚合维度，再生成 GROUP BY / 聚合 SQL。',
             'Identify the primary table and aggregation dimensions before generating GROUP BY / aggregate SQL.');
  }
  if (intent && intent.kind === 'schema') {
    return t('请先确认目标表和字段，再生成查询。',
             'Identify the target table and columns before generating the query.');
  }
  if (intent && intent.kind === 'ml') {
    return t('这是一个机器学习任务。请先确认数据表结构和目标列。\n'
             + '⚠️ 训练前必须确保表已通过 ALTER TABLE db.table SECONDARY_LOAD 加载到 RAPID 引擎。\n'
             + '   先用 check_secondary_load 检查表是否已加载，\n'
             + '   若未加载，用 run_ddl 执行 ALTER TABLE db.table SECONDARY_LOAD 完成加载后继续。\n'
             + '步骤：1) describe_table 确认列结构\n'
             + '      2) check_secondary_load 检查是否已 SECONDARY_LOAD\n'
             + '      3) ml_train 训练模型（model_handle 为模型名称如 census_model）\n'
             + '      4) 训练完成后用 ml_list_models 查看模型列表。\n'
             + '如果是预测任务，请确认 model_handle 对应的已训练模型；'
             + '如果用户未指定模型句柄，先用 ml_list_models 列出可用模型。',
             'This is an ML task. First inspect the table structure and target column.\n'
             + '⚠️ Before training, verify the table is loaded into RAPID via ALTER TABLE db.table SECONDARY_LOAD.\n'
             + '   Use check_secondary_load to verify if the table is loaded.\n'
             + '   If not loaded, run ALTER TABLE db.table SECONDARY_LOAD via run_ddl, then continue.\n'
             + 'Steps: 1) describe_table to confirm column structure\n'
             + '       2) check_secondary_load to verify SECONDARY_LOAD status\n'
             + '       3) ml_train to train the model (model_handle is the model name, e.g. census_model)\n'
             + '       4) ml_list_models to list available models after training.\n'
             + 'For prediction: confirm the model_handle of a trained model. '
             + 'If no model handle is specified, use ml_list_models to list available models.');
  }
  return t('请先确认目标表和字段，再生成查询。',
           'Identify the target table and columns before generating the query.');
}

/* ------------------------------------------------- context budget, by model
 *
 * The budget used to be three hardcoded constants -- PROMPT_TOK_LIMIT
 * 102800, MAX_SCHEMA_CHARS 4000, MAX_TURNS 10 -- which were wrong in both
 * directions at once. 4000 characters of schema is about 1.3k tokens, so a
 * wide star schema ran out of room long before the model did and the failure
 * looked like "the model does not know that column". Meanwhile 102800 tokens
 * is more text than the engine can physically hold (see below), so the
 * budget could never be reached anyway: the heap gave out first.
 *
 * Two different ceilings apply, and the effective budget is the lower.
 *
 * The model's: whatever it can attend to, minus what the completion needs.
 * Taken from model_options.context_window when the caller sets it, otherwise
 * inferred from the model id, otherwise a deliberately small default -- an
 * unknown model is more likely to be a local 8k one than a frontier model,
 * and guessing high produces a provider error while guessing low only
 * produces a shorter prompt.
 *
 * The engine's: the prompt is a JavaScript string in the 512KB JerryScript
 * heap, and by the time it reaches the model it exists about three times
 * over -- the string, the esc() copy, and the assembled SQL. The limit is
 * therefore expressed in characters, not tokens, because that is the unit
 * the heap charges in: est_tok() counts a CJK character as roughly a token
 * and four ASCII characters as one, so an identical token count can differ
 * fourfold in bytes. This is the ceiling that actually binds on a long
 * Chinese conversation. */
var MODEL_CONTEXT_DEFAULT = 8192;

var MODEL_CONTEXT_TABLE = [
  { re: /deepseek/i,                    window: 65536  },
  { re: /qwen3|qwen-?3|qwen3\.5/i,      window: 32768  },
  { re: /qwen/i,                        window: 32768  },
  { re: /gpt-?4o|gpt-?4\.1|gpt-?5/i,    window: 128000 },
  { re: /gpt-?4/i,                      window: 8192   },
  { re: /claude/i,                      window: 200000 },
  { re: /gemini/i,                      window: 1000000 },
  { re: /llama-?3|llama3/i,             window: 8192   }
];

function model_context_window() {
  var co = get_chat_options();
  var mo = (co && co.model_options) ? co.model_options : {};
  var explicit = Number(mo.context_window || co.context_window || 0);
  if (isFinite(explicit) && explicit > 0) return explicit;
  var id = String(mo.model_id || '');
  for (var i = 0; i < MODEL_CONTEXT_TABLE.length; i++)
    if (MODEL_CONTEXT_TABLE[i].re.test(id)) return MODEL_CONTEXT_TABLE[i].window;
  return MODEL_CONTEXT_DEFAULT;
}

/* How many tokens the prompt may occupy: the window less the completion
 * reservation less a margin, because est_tok() is an estimate and being
 * slightly over is a provider error rather than a slightly worse answer. */
function prompt_token_budget() {
  var co  = get_chat_options();
  var mo  = (co && co.model_options) ? co.model_options : {};
  var win = model_context_window();
  var reply = Number(mo.max_tokens || 1200);
  if (!isFinite(reply) || reply <= 0) reply = 1200;
  var budget = Math.floor((win - reply) * 0.85);
  return Math.max(1000, budget);
}

/* The engine-heap ceiling, in characters. Roughly a third of the heap, which
 * leaves room for the two transient copies the call makes plus everything
 * else the routine is holding. */
function prompt_char_budget() {
  return 96 * 1024;
}

/* Schema context scales with the budget instead of being pinned at 4000
 * characters, but never grows past what the heap ceiling can carry. */
function schema_char_budget() {
  var by_tokens = prompt_token_budget() * 0.35 * 3; /* ~3 chars per token */
  var cap = Math.floor(prompt_char_budget() * 0.35);
  return Math.max(2000, Math.min(Math.floor(by_tokens), cap));
}

function cfg(key, default_val) {
  var co = get_chat_options();
  if (!co || co[key] === undefined || co[key] === null) return default_val;
  var v = Number(co[key]);
  return (isFinite(v) && v > 0) ? v : default_val;
}

/* ml_generate() is the only path to a model, so it is the only place that has
 * to count.  The token figures are est_tok() estimates, not the backend's
 * accounting: sys.ML_GENERATE returns generated text and no usage block, and
 * an estimate that is always present beats an exact number that only one
 * provider reports.  est_tok() is the same estimator the prompt budget is
 * enforced with, so the recorded cost and the budget agree. */
/* ML_GENERATE with verbose=1 answers with
 * {"text":..,"finish_reason":..,"prompt_tokens":..,"completion_tokens":..}.
 *
 * Falls back to treating the whole thing as the answer when it is not that
 * envelope, which covers two real cases: a server built before the option
 * existed, and a model whose answer happens to be JSON. The finish_reason
 * is then empty, which the caller reads as "unknown" -- the same thing it
 * reads when a provider declines to report one. */
function parse_generate_envelope(raw) {
  var s = String(raw == null ? '' : raw);
  if (s.charAt(0) !== '{') return { text: s, finish_reason: '', prompt_tokens: -1, completion_tokens: -1 };
  try {
    var o = JSON.parse(s);
    if (o && typeof o === 'object' && typeof o.text === 'string') {
      return { text: o.text,
               finish_reason: String(o.finish_reason || ''),
               prompt_tokens: Number(o.prompt_tokens === undefined ? -1 : o.prompt_tokens),
               completion_tokens: Number(o.completion_tokens === undefined ? -1 : o.completion_tokens) };
    }
  } catch (e) { /* not our envelope */ }
  return { text: s, finish_reason: '', prompt_tokens: -1, completion_tokens: -1 };
}

/* Token accounting prefers what the provider reported and falls back to
 * est_tok() only where it reported nothing. The estimate was previously the
 * only source, and it is the same estimator the prompt budget uses, so a
 * systematic bias in it was invisible: budget and bill were wrong together
 * and therefore agreed. */
function llm_note_call(prompt, out, ms, env) {
  if (!A.cost)
    A.cost = { llm_calls: 0, prompt_tokens: 0, completion_tokens: 0, llm_ms: 0 };
  A.cost.llm_calls++;
  var pt = (env && env.prompt_tokens     >= 0) ? env.prompt_tokens     : est_tok(prompt);
  var ct = (env && env.completion_tokens >= 0) ? env.completion_tokens : est_tok(out);
  A.cost.prompt_tokens     += pt;
  A.cost.completion_tokens += ct;
  A.cost.llm_ms            += Number(ms || 0);
  if (env && env.finish_reason === 'length')
    A.cost.truncated_calls = Number(A.cost.truncated_calls || 0) + 1;
}

function ml_generate(prompt, extra) {
  var chat_opt   = get_chat_options();
  var model_opts = (chat_opt && chat_opt.model_options) ? chat_opt.model_options : {};
  var o = Object.assign({
    task: 'generation', model_id: 'Qwen3.5-2B-ONNX',
    language: A.lang,
    temperature: 0.25, max_tokens: 1200,
    top_p: 0.95, repeat_penalty: 1.1,
    frequency_penalty: 0.0, presence_penalty: 0.0
  }, model_opts, extra || {});

  var sql =
    "SELECT sys.ML_GENERATE('" + esc(prompt) + "'," +
    "JSON_OBJECT(" +
    "'task','"             + esc(o.task)             + "'," +
    "'model_id','"         + esc(o.model_id)         + "'," +
    "'language','"         + esc(o.language)         + "'," +
    "'temperature',"       + Number(o.temperature)   + "," +
    "'max_tokens',"        + Number(o.max_tokens)    + "," +
    "'top_p',"             + Number(o.top_p)         + "," +
    "'repeat_penalty',"    + Number(o.repeat_penalty)    + "," +
    "'frequency_penalty'," + Number(o.frequency_penalty) + "," +
    "'presence_penalty',"  + Number(o.presence_penalty);

  if (o.provider)           sql += ",'provider','"           + esc(o.provider)           + "'";
  if (o.endpoint)           sql += ",'endpoint','"           + esc(o.endpoint)           + "'";
  if (o.api_key)            sql += ",'api_key','"            + esc(o.api_key)            + "'";
  if (o.workspace_id)       sql += ",'workspace_id','"       + esc(o.workspace_id)       + "'";
  if (o.region)             sql += ",'region','"             + esc(o.region)             + "'";
  if (o.api_config)         sql += ",'api_config','"         + esc(o.api_config)         + "'";

  if (o.deepseek_thinking !== undefined && o.deepseek_thinking !== '')
    sql += ",'deepseek_thinking','" + esc(String(o.deepseek_thinking)) + "'";
  if (o.reasoning_effort)
    sql += ",'reasoning_effort','" + esc(o.reasoning_effort) + "'";

  if (o.timeout_ms)
    sql += ",'timeout_ms'," + Number(o.timeout_ms);

  /* Ask for the envelope rather than the bare text, so the loop can see
   * why generation stopped. A server that predates the option ignores it
   * and returns plain text, which ml_generate_call() handles -- that is
   * why the parse below falls back instead of failing. */
  sql += ",'verbose','1'";

  sql += ")) AS result";

  return ml_generate_call(sql, prompt);
}

/* ------------------------------------------------------- model call retry
 *
 * What this replaces: a single `query(sql)`. query() turns a SQL error into
 * `{error: "..."}` rather than throwing, and the old code then read
 * `rows[0].result`, found nothing, and returned ''. Three things followed
 * from that. An unavailable model was indistinguishable from a model that
 * answered with whitespace, so the loop treated infrastructure failure as
 * "the model has nothing to say" and ended the turn. The error text --
 * which says whether it was a rate limit, a bad key, or a prompt over the
 * context window -- was discarded without ever being logged. And a failure
 * that would have succeeded half a second later was final.
 *
 * So: classify, then decide. Retrying a 401 is pointless and retrying a
 * context-length error just spends the same tokens again, whereas a 429 or
 * a dropped connection is exactly what backoff is for. A prompt that is too
 * long is reported as such so the caller can shrink it and come back --
 * that is what the loop's compaction does with it.
 *
 * Errors arrive as free text from whichever provider produced them, so the
 * classifier matches on substrings and defaults to retryable-once: a
 * transient failure misread as permanent ends the user's turn, while a
 * permanent one misread as transient costs a second of backoff. */
var LLM_MAX_ATTEMPTS = 3;

function classify_llm_error(msg) {
  var m = String(msg || '').toLowerCase();
  if (!m) return { kind: 'unknown', retryable: true };
  if (m.indexOf('context length') !== -1 || m.indexOf('context_length') !== -1 ||
      m.indexOf('too many tokens') !== -1 || m.indexOf('maximum context') !== -1 ||
      m.indexOf('prompt is too long') !== -1 || m.indexOf('input too long') !== -1)
    return { kind: 'context_length', retryable: false };
  if (m.indexOf('rate limit') !== -1 || m.indexOf('rate_limit') !== -1 ||
      m.indexOf('429') !== -1 || m.indexOf('too many requests') !== -1 ||
      m.indexOf('quota') !== -1 || m.indexOf('overloaded') !== -1)
    return { kind: 'rate_limit', retryable: true };
  if (m.indexOf('timeout') !== -1 || m.indexOf('timed out') !== -1 ||
      m.indexOf('connection') !== -1 || m.indexOf('network') !== -1 ||
      m.indexOf('unreachable') !== -1 || m.indexOf('temporarily') !== -1 ||
      m.indexOf('503') !== -1 || m.indexOf('502') !== -1 || m.indexOf('504') !== -1)
    return { kind: 'transient', retryable: true };
  if (m.indexOf('unauthorized') !== -1 || m.indexOf('forbidden') !== -1 ||
      m.indexOf('api key') !== -1 || m.indexOf('api_key') !== -1 ||
      m.indexOf('401') !== -1 || m.indexOf('403') !== -1 ||
      m.indexOf('invalid model') !== -1 || m.indexOf('not found') !== -1)
    return { kind: 'permanent', retryable: false };
  return { kind: 'unknown', retryable: true };
}

/* Backoff without a timer: JerryScript has no setTimeout, and the engine is
 * single-threaded inside the routine anyway, so the wait is the server's. */
function llm_backoff_sleep(attempt) {
  var secs = Math.min(4, 0.5 * Math.pow(2, attempt));
  try { query("SELECT SLEEP(" + secs + ")"); } catch (e) {}
}

function ml_generate_call(sql, prompt) {
  var attempts = LLM_MAX_ATTEMPTS;
  var last_err = '';
  var last_kind = '';

  for (var attempt = 0; attempt < attempts; attempt++) {
    var t0 = Date.now();
    var rows = query(sql);
    var ms = Date.now() - t0;

    var err = (rows && !Array.isArray(rows) && rows.error) ? String(rows.error) : '';
    if (!err && (!rows || !Array.isArray(rows) || !rows.length || rows[0].result == null))
      err = 'model returned no result row';

    if (!err) {
      var env = parse_generate_envelope(String(rows[0].result));
      var raw = env.text;
      llm_note_call(prompt, raw, ms, env);
      A.last_llm_status = { ok: true, kind: '', error: '', attempts: attempt + 1,
                            finish_reason: env.finish_reason };
      var think_m = raw.match(/<think>([\s\S]*?)<\/think>/i);
      A.last_think = think_m ? think_m[1].trim() : '';
      return raw.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
    }

    var cls = classify_llm_error(err);
    last_err = err;
    last_kind = cls.kind;
    llm_note_failure(prompt, ms, cls.kind, err);

    if (!cls.retryable || attempt === attempts - 1) break;
    llm_backoff_sleep(attempt);
  }

  /* Out of attempts. The caller gets '' as before -- every call site
   * already handles an empty completion -- but A.last_llm_status now says
   * why, so the loop can tell "the model is unreachable" from "the model
   * had nothing to add" and say so to the user instead of apologising
   * generically. */
  A.last_llm_status = { ok: false, kind: last_kind, error: last_err, attempts: attempts };
  A.last_think = '';
  return '';
}

/* A failed call still consumed a request slot and still took time, so it is
 * still metered; it is counted separately so that a principal's error rate
 * is visible next to its spend. */
function llm_note_failure(prompt, ms, kind, err) {
  if (!A.cost)
    A.cost = { llm_calls: 0, prompt_tokens: 0, completion_tokens: 0, llm_ms: 0 };
  if (A.cost.llm_errors === undefined) A.cost.llm_errors = 0;
  A.cost.llm_errors++;
  A.cost.llm_ms += Number(ms || 0);
  /* Recorded where the rest of the agent's diagnostics live, so that a run
   * of model failures is visible next to the retrievals and writes of the
   * same turn rather than only in the server error log. */
  try {
    mem_log_audit('model', 'llm_error', 'ml_generate',
                  kind + ': ' + String(err).substring(0, 400), 0, Number(ms || 0), '');
  } catch (e) {}
}

/* Human-readable reason for the user when the model could not be reached at
 * all, rather than the generic "please try again". */
function llm_failure_message() {
  var st = A.last_llm_status;
  if (!st || st.ok) return '';
  if (st.kind === 'rate_limit')
    return t('模型服务限流，已重试 ' + st.attempts + ' 次仍未成功。请稍后再试。',
             'The model service is rate-limiting requests; ' + st.attempts +
             ' attempts failed. Please retry shortly.');
  if (st.kind === 'context_length')
    return t('本次对话的上下文超出了模型窗口，且压缩后仍然过长。请缩小问题范围或开启新会话。',
             'This conversation exceeded the model context window even after compaction. ' +
             'Narrow the question or start a new conversation.');
  if (st.kind === 'permanent')
    return t('模型调用被拒绝（凭据或模型配置问题）：' + String(st.error).substring(0, 200),
             'The model call was rejected (credentials or model configuration): ' +
             String(st.error).substring(0, 200));
  return t('模型服务暂时不可用（已重试 ' + st.attempts + ' 次）：' +
           String(st.error).substring(0, 200),
           'The model service is unavailable (' + st.attempts + ' attempts): ' +
           String(st.error).substring(0, 200));
}

function ml_rag(question, topK, opt_override) {
  topK = topK || 6;
  var opt = Object.assign(
    { n_citations: topK, distance_metric: 'COSINE', skip_generate: 1 },
    opt_override || {}
  );
  try {
    sys.exec_sql("SET @_rag_out = NULL");
    sys.exec_sql(
      "CALL sys.ML_RAG(" +
      "'" + esc(question) + "'," +
      "@_rag_out," +
      "'" + esc(JSON.stringify(opt)) + "')"
    );
  } catch (e) {
    return { text: '', ok: false, raw: '', error: String(e) };
  }
  var rows = query("SELECT @_rag_out AS result");
  if (!rows || !Array.isArray(rows) || !rows.length || rows[0].result == null)
    return { text: '', ok: false, raw: '' };
  var raw = String(rows[0].result);
  try {
    var obj = JSON.parse(raw);
    /* ML_RAG reports what it retrieved as `citations` plus a
     * `processing_info` block -- it has never emitted `found`, `hit_count` or
     * `n_citations`.  Reading only those three meant hits was always 0 and
     * heatwave_dispatch scored every successful retrieval as a miss, then
     * threw the citations away and answered from the bare model instead.
     * Prefer the real fields and keep the old names as fallbacks. */
    var info = (obj.processing_info && typeof obj.processing_info === 'object')
      ? obj.processing_info : {};
    var citation_list = Array.isArray(obj.citations) ? obj.citations : [];
    var hits = Number(
      info.citations_returned !== undefined    ? info.citations_returned :
      info.total_citations_found !== undefined ? info.total_citations_found :
      obj.hit_count !== undefined              ? obj.hit_count :
      obj.n_citations !== undefined            ? obj.n_citations :
                                                 citation_list.length
    );
    if (!isFinite(hits)) hits = citation_list.length;
    return {
      text: String(obj.text || raw),
      ok: true,
      raw: raw,
      found: (obj.found === true) || hits > 0,
      hits: hits,
      citations: citation_list,
      /* skip_generate mode returns retrieved context, not a written answer. */
      retrieved_only: !!(opt && opt.skip_generate),
      score: obj.score || 0
    };
  } catch(e) {
    return { text: raw, ok: true, raw: raw, found: false };
  }
}

/* Note: get_embed_model_id is defined in lib_schema.js, but it can be called directly here——
 * After the entire script is concatenated, function declarations are hoisted in their entirety,
 * so cross-file calls do not depend on include order. */
function get_rag_options(chat_opt) {
  var user_rag = (chat_opt && chat_opt.rag_options &&
                  typeof chat_opt.rag_options === 'object') ? chat_opt.rag_options : {};

  var legacy = {};
  if (!chat_opt.rag_options) {
    if (chat_opt.retrieve_top_k)    legacy.n_citations = chat_opt.retrieve_top_k;
    if (chat_opt.retrieval_options) legacy.retrieval_options = chat_opt.retrieval_options;
    if (chat_opt.embed_model_id)    legacy.embed_model_id = chat_opt.embed_model_id;
    if (Array.isArray(chat_opt.tables) && chat_opt.tables.length) {
      legacy.vector_store = chat_opt.tables.map(function(tb) {
        return tb.schema_name + '.' + tb.table_name;
      });
    }
  }

  var defaults = { n_citations: 6, distance_metric: 'COSINE', skip_generate: 1 };

  var merged = Object.assign({}, defaults, legacy, user_rag);
  if (!merged.embed_model_id) merged.embed_model_id = get_embed_model_id(merged);
  return merged;
}