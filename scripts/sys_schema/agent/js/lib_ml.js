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
  return t('请先确认目标表和字段，再生成查询。',
           'Identify the target table and columns before generating the query.');
}

function cfg(key, default_val) {
  var co = get_chat_options();
  if (!co || co[key] === undefined || co[key] === null) return default_val;
  var v = Number(co[key]);
  return (isFinite(v) && v > 0) ? v : default_val;
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

  sql += ")) AS result";

  var rows = query(sql);
  var raw = (rows && Array.isArray(rows) && rows.length && rows[0].result != null)
        ? String(rows[0].result) : '';
  var think_m = raw.match(/<think>([\s\S]*?)<\/think>/i);
  A.last_think = think_m ? think_m[1].trim() : '';
  return raw.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
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
    return {
      text: String(obj.text || raw),
      ok: true,
      raw: raw,
      found: obj.found || obj.hit_count || obj.n_citations || false,
      hits: obj.hit_count || obj.n_citations || 0,
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