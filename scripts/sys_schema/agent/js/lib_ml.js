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

function infer_candidate_tables(db, intent, user_msg) {
  if (!db) return [];
  var keywords = String(user_msg || '').match(/[A-Za-z0-9_]+/g) || [];
  if (!keywords.length) return [];
  var filters = [];
  for (var i = 0; i < Math.min(keywords.length, 6); i++) {
    var k = String(keywords[i]).toLowerCase();
    if (k.length < 2) continue;
    filters.push("COLUMN_NAME LIKE '%" + esc(k) + "%'");
  }
  if (!filters.length) return [];
  var sql = "SELECT DISTINCT TABLE_NAME FROM information_schema.COLUMNS " +
            "WHERE TABLE_SCHEMA='" + esc(db) + "' AND (" + filters.join(' OR ') + ") LIMIT 20";
  var rows = query(sql);
  return Array.isArray(rows) ? rows.map(function(r) {
    return String(r.TABLE_NAME || '');
  }).filter(function(x) { return x; }) : [];
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