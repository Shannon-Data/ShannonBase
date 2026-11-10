//@include lib_lang.js
function save_secret_api_key(real_key) {
  if (!real_key || real_key === '***') return;
  try {
    exec_sql("SET @chat_api_key = '" +
                 esc(current_principal_prefix() + ':' + real_key) + "'");
  } catch (e) {}
}

function load_secret_api_key() {
  try {
    var rows = query("SELECT @chat_api_key AS k");
    if (!Array.isArray(rows) || !rows.length || !rows[0].k) return '';
    var stored = String(rows[0].k);
    var sep    = stored.indexOf(':');
    // No separator: written by an older server, before the binding existed.
    if (sep <= 0) return '';
    if (stored.substring(0, sep) !== current_principal_prefix()) return '';
    return stored.substring(sep + 1);
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
var LOW_SIGNAL_KEYWORDS = {
  'id':1,'name':1,'time':1,'date':1,'status':1,'type':1,'data':1,'value':1,
  '时间':1,'状态':1,'名称':1,'类型':1,'数据':1,'编号':1,'信息':1,'记录':1
};

// Reset the per-turn infer_candidate_tables() memo cache.
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
  if (intent && intent.kind === 'write') {

    return t('这是一个变更请求。确认目标对象后，请用 update_data（DML）或 run_ddl（DDL）把变更提交执行，'
             + '不要只把 SQL 写给用户让他自己去跑。需要人工确认时，审批流程会自动暂停并把该步骤展示给用户，'
             + '所以提交变更是安全的，也是让用户能够审批的唯一方式。',
             'This is a change request. Once the target is confirmed, submit the change through '
             + 'update_data (DML) or run_ddl (DDL) -- do not just print the SQL for the user to run. '
             + 'Where confirmation is required the approval flow pauses and shows the user the step, '
             + 'so proposing the change is both safe and the only way the user gets to approve it.');
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

function prompt_token_budget() {
  var co  = get_chat_options();
  var mo  = (co && co.model_options) ? co.model_options : {};
  var win = model_context_window();
  var reply = Number(mo.max_tokens || 1200);
  if (!isFinite(reply) || reply <= 0) reply = 1200;
  var budget = Math.floor((win - reply) * 0.85);
  return Math.max(1000, budget);
}

// The engine-heap ceiling, in characters.
function prompt_char_budget() {
  return Math.floor(engine_heap_bytes() * 3 / 16);
}

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

// ml_generate() is the only path to a model, so it is the only place that has to count.
// Which dialect this provider speaks, or null for "no tool channel".
function llm_tool_dialect() {
  var co = get_chat_options();
  var mo = (co && co.model_options) ? co.model_options : {};
  var p  = String(mo.provider || '').toLowerCase();
  if (p === 'openai' || p === 'deepseek' || p === 'dashscope' || p === 'qianfan')
    return 'openai';
  if (p === 'anthropic') return 'anthropic';
  return null;
}

// The catalogue, in the provider's shape, from the registry.
function llm_tools_extra_body(dialect) {
  var man = tool_manifest();
  var tools = [];
  for (var i = 0; i < man.length; i++) {
    var m = man[i];
    if (m.annotations && m.annotations.hidden) continue;
    var spec = get_tool_spec(m.name);
    if (spec && !tool_category_expanded(spec.category, A.user_message) &&
        !(A.expanded_tools && A.expanded_tools[m.name])) continue;
    var desc = (A.lang === 'zh') ? m.description.zh : m.description.en;

    var example = m.example;
    if (example && (example.zh !== undefined || example.en !== undefined))
      example = A.lang === 'zh' ? example.zh : example.en;
    desc = String(desc || m.name) + '\nExample args: ' + JSON.stringify(example || {});
    var schema = m.input_schema || { type: 'object' };
    if (dialect === 'anthropic') {
      tools.push({ name: m.name, description: desc, input_schema: schema });
    } else {
      tools.push({ type: 'function',
                   function: { name: m.name, description: desc, parameters: schema } });
    }
  }
  if (!tools.length) return null;

  return (dialect === 'anthropic') ? { tools: tools } : { tools: tools, tool_choice: 'auto' };
}

function llm_parse_native_tool_call(raw_json, dialect) {
  if (!raw_json) return null;
  var doc;
  try { doc = JSON.parse(raw_json); } catch (e) { return null; }
  if (!doc || typeof doc !== 'object') return null;

  if (dialect === 'anthropic') {
    var blocks = doc.content;
    if (!Array.isArray(blocks)) return null;
    for (var i = 0; i < blocks.length; i++) {
      var b = blocks[i];
      if (b && b.type === 'tool_use' && typeof b.name === 'string') {
        return { tool: b.name,
                 args: (b.input && typeof b.input === 'object') ? b.input : {},
                 thought: llm_text_blocks(blocks),
                 native: true };
      }
    }
    return null;
  }

  var choices = doc.choices;
  if (!Array.isArray(choices) || !choices.length) return null;
  var msg = choices[0].message;
  if (!msg || !Array.isArray(msg.tool_calls) || !msg.tool_calls.length) return null;
  var call = msg.tool_calls[0];
  var fn = call && call.function;
  if (!fn || typeof fn.name !== 'string') return null;
  // OpenAI sends the arguments as a JSON *string*, and a model can still produce a malformed one.
  var args = {};
  if (typeof fn.arguments === 'string' && fn.arguments.trim()) {
    try { args = JSON.parse(fn.arguments); } catch (e2) { args = {}; }
  } else if (fn.arguments && typeof fn.arguments === 'object') {
    args = fn.arguments;
  }
  return { tool: fn.name, args: args,
           thought: String(msg.content == null ? '' : msg.content), native: true };
}

function llm_text_blocks(blocks) {
  var out = [];
  for (var i = 0; i < blocks.length; i++)
    if (blocks[i] && blocks[i].type === 'text' && typeof blocks[i].text === 'string')
      out.push(blocks[i].text);
  return out.join(' ').trim();
}

function parse_generate_envelope(raw) {
  var s = String(raw == null ? '' : raw);
  if (s.charAt(0) !== '{') return { text: s, finish_reason: '', prompt_tokens: -1, completion_tokens: -1, raw: '' };
  try {
    var o = JSON.parse(s);
    if (o && typeof o === 'object' && typeof o.text === 'string') {
      return { text: o.text,
               finish_reason: String(o.finish_reason || ''),
               prompt_tokens: Number(o.prompt_tokens === undefined ? -1 : o.prompt_tokens),
               completion_tokens: Number(o.completion_tokens === undefined ? -1 : o.completion_tokens),
               raw: (typeof o.raw === 'string') ? o.raw : '' };
    }
  } catch (e) { /* not our envelope */ }
  return { text: s, finish_reason: '', prompt_tokens: -1, completion_tokens: -1, raw: '' };
}

function llm_note_call(prompt, out, ms, env) {
  if (!A.cost)
    A.cost = { llm_calls: 0, prompt_tokens: 0, completion_tokens: 0, llm_ms: 0 };
  A.cost.llm_calls++;
  var pt = (env && env.prompt_tokens     >= 0) ? env.prompt_tokens     : est_tok(prompt);
  var ct = (env && env.completion_tokens >= 0) ? env.completion_tokens : est_tok(out);
  if (env && env.prompt_tokens > 0) {
    var raw_estimate = est_tok_raw(prompt);
    if (raw_estimate > 0) A.token_ratio = Math.max(Number(A.token_ratio || 1),
      Math.min(4, env.prompt_tokens / raw_estimate));
  }
  A.cost.prompt_tokens     += pt;
  A.cost.completion_tokens += ct;
  A.cost.llm_ms            += Number(ms || 0);
  if (env && env.finish_reason === 'length')
    A.cost.truncated_calls = Number(A.cost.truncated_calls || 0) + 1;
}

// Operator control over where the prompt goes.
var EGRESS_KEYS = ['provider', 'endpoint', 'api_key',
                  'workspace_id', 'region', 'api_config'];

function apply_egress_policy(o) {
  var base = get_operator_policy();
  if (!o) return o;

  if (_pol_has(base, 'allow_remote_provider') && !_pol_bool(base.allow_remote_provider)) {
    // Drop everything that could name an off-box destination.
    for (var ek = 0; ek < EGRESS_KEYS.length; ek++) delete o[EGRESS_KEYS[ek]];
    return o;
  }

  if (_pol_has(base, 'allowed_providers')) {
    var allowed = String(base.allowed_providers || '').toLowerCase().split(',');
    var want = String(o.provider || '').toLowerCase().trim();
    var ok = false;
    for (var i = 0; i < allowed.length; i++)
      if (allowed[i].trim() && allowed[i].trim() === want) { ok = true; break; }
    if (!ok)
      throw new Error(t('该 provider 不在操作员允许的清单内：',
                        'Provider is not in the operator allow-list: ') + (o.provider || '(none)'));
  }

  var endpoint_locked =
      (_pol_has(base, 'allow_endpoint_override') && !_pol_bool(base.allow_endpoint_override)) ||
      (_pol_has(base, 'allowed_providers') && !_pol_has(base, 'allow_endpoint_override'));
  if (endpoint_locked) delete o.endpoint;

  return o;
}

// The same ceiling, for the options blob handed to a batch routine.
function apply_egress_policy_nested(o) {
  if (!o || typeof o !== 'object') return o;
  for (var i = 0; i < EGRESS_KEYS.length; i++) delete o[EGRESS_KEYS[i]];
  if (o.model_options && typeof o.model_options === 'object')
    o.model_options = apply_egress_policy(o.model_options);
  return o;
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

  o = apply_egress_policy(o);

  // The options are assembled on their own, and the prompt is joined in once at the end.
  var opts = "JSON_OBJECT(" +
    "'task','"             + esc(o.task)             + "'," +
    "'model_id','"         + esc(o.model_id)         + "'," +
    "'language','"         + esc(o.language)         + "'," +
    "'temperature',"       + Number(o.temperature)   + "," +
    "'max_tokens',"        + Number(o.max_tokens)    + "," +
    "'top_p',"             + Number(o.top_p)         + "," +
    "'repeat_penalty',"    + Number(o.repeat_penalty)    + "," +
    "'frequency_penalty'," + Number(o.frequency_penalty) + "," +
    "'presence_penalty',"  + Number(o.presence_penalty);

  if (o.provider)           opts += ",'provider','"           + esc(o.provider)           + "'";
  if (o.endpoint)           opts += ",'endpoint','"           + esc(o.endpoint)           + "'";
  if (o.api_key)            opts += ",'api_key','"            + esc(o.api_key)            + "'";
  if (o.workspace_id)       opts += ",'workspace_id','"       + esc(o.workspace_id)       + "'";
  if (o.region)             opts += ",'region','"             + esc(o.region)             + "'";
  if (o.api_config)         opts += ",'api_config','"         + esc(o.api_config)         + "'";

  if (o.deepseek_thinking !== undefined && o.deepseek_thinking !== '')
    opts += ",'deepseek_thinking','" + esc(String(o.deepseek_thinking)) + "'";
  if (o.reasoning_effort)
    opts += ",'reasoning_effort','" + esc(o.reasoning_effort) + "'";

  // The call's wall clock.
  var budget_timeout = llm_attempt_timeout_ms(o.provider);
  var call_timeout = isFinite(Number(o.timeout_ms)) && Number(o.timeout_ms) > 0 ? Number(o.timeout_ms) : budget_timeout;
  if (budget_timeout) call_timeout = Math.min(call_timeout, budget_timeout);
  if (call_timeout)
    opts += ",'timeout_ms'," + Math.round(call_timeout);

  // Ask for the envelope rather than the bare text, so the loop can see why generation stopped.
  var dialect    = (extra && extra.tools === true) ? llm_tool_dialect() : null;
  var extra_body = dialect ? llm_tools_extra_body(dialect) : null;
  if (extra_body) {
    opts += ",'extra_body','" + esc(JSON.stringify(extra_body)) + "'";
    opts += ",'verbose','raw'";
  } else {
    dialect = null;
    opts += ",'verbose','1'";
  }

  opts += ")";

  return ml_generate_call(function () {
    var remaining = A.runtime_active ? runtime_sql_timeout() : (call_timeout || 30000);
    return "SELECT sys.ML_GENERATE('" + esc(prompt) + "',JSON_SET(" + opts +
      ", '$.timeout_ms'," + Math.max(1, Math.min(call_timeout || remaining, remaining)) + ")) AS result";
  }, prompt, dialect);
}

var LLM_MAX_ATTEMPTS = 3;

function llm_attempt_timeout_ms(provider) {
  var until = Number(A.turn_deadline_at || 0);
  if (!until) return 0;
  var p = String(provider || '').toLowerCase();
  var backend_default = (p && p !== 'ollama' && p !== 'onnx') ? 120000 : 30000;
  var left = until - Date.now();
  var per = Math.max(1, Math.floor(left / LLM_MAX_ATTEMPTS));
  return Math.min(per, backend_default);
}

function classify_llm_error(msg) {
  var m = String(msg || '').toLowerCase();
  if (!m) return { kind: 'unknown', retryable: true };
  if (m.indexOf('maximum statement execution time exceeded') !== -1)
    return { kind: 'deadline', retryable: false };
  if (/cancelled|canceled|kill query|query execution was interrupted/.test(m))
    return { kind: 'cancelled', retryable: false };
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

function llm_backoff_sleep(attempt) {
  var secs = Math.min(4, 0.5 * Math.pow(2, attempt));
  if (A.runtime_active) secs = Math.min(secs, runtime_sql_timeout() / 1000);
  if (secs <= 0) return;
  try { query("SELECT SLEEP(" + secs + ")"); } catch (e) {}
}

function ml_generate_call(sql, prompt, dialect) {
  if (A.runtime_active) {
    var ctx = get_tx_context();
    if (!ctx.known || (ctx.active && ctx.owner !== TX_OWNER_CALLER)) {
      A.last_llm_status = { ok: false, kind: 'transaction_open', error: 'Close the agent transaction before inference.' };
      return '';
    }
  }
  // The seam the loop evaluation drives the agent through.
  if (A.eval_script) return eval_script_respond(prompt, dialect);

  var attempts = LLM_MAX_ATTEMPTS;
  var last_err = '';
  var last_kind = '';

  for (var attempt = 0; attempt < attempts; attempt++) {
    var t0 = Date.now();
    if (A.runtime_active && runtime_sql_timeout() <= 0) {
      last_kind = 'deadline'; last_err = 'Agent deadline exceeded'; break;
    }
    var rows = query(typeof sql === 'function' ? sql() : sql);
    var ms = Date.now() - t0;

    var err = (rows && !Array.isArray(rows) && rows.error) ? String(rows.error) : '';
    if (!err && (!rows || !Array.isArray(rows) || !rows.length || rows[0].result == null))
      err = 'model returned no result row';

    if (!err) {
      var env = parse_generate_envelope(String(rows[0].result));
      llm_note_call(prompt, env.text, ms, env);
      if (env.finish_reason === 'cancelled') {
        A.last_llm_status = { ok: false, kind: 'cancelled', error: 'Generation cancelled.', attempts: attempt + 1 };
        A.last_think = '';
        return '';
      }
      var raw = env.text;
      // A structured call, when the provider made one.
      var native_call = dialect ? llm_parse_native_tool_call(env.raw, dialect) : null;
      A.last_llm_status = { ok: true, kind: '', error: '', attempts: attempt + 1,
                            finish_reason: env.finish_reason,
                            tool_call: native_call };
      if (native_call) A.native_tool_calls = Number(A.native_tool_calls || 0) + 1;
      var think_m = raw.match(/<think>([\s\S]*?)<\/think>/i);
      A.last_think = think_m ? think_m[1].trim() : '';
      return raw.replace(/<think>[\s\S]*?<\/think>\s*/gi, '').trim();
    }

    var cls = classify_llm_error(err);
    last_err = err;
    last_kind = cls.kind;
    llm_note_failure(prompt, ms, cls.kind, err);

    if (!cls.retryable || attempt === attempts - 1) break;

    if (A.turn_deadline_at && Date.now() >= Number(A.turn_deadline_at)) break;
    llm_backoff_sleep(attempt);
  }

  // Out of attempts.
  A.last_llm_status = { ok: false, kind: last_kind, error: last_err, attempts: attempts };
  A.last_think = '';
  return '';
}

function llm_note_failure(prompt, ms, kind, err) {
  if (!A.cost)
    A.cost = { llm_calls: 0, prompt_tokens: 0, completion_tokens: 0, llm_ms: 0 };
  if (A.cost.llm_errors === undefined) A.cost.llm_errors = 0;
  A.cost.llm_errors++;
  A.cost.llm_ms += Number(ms || 0);

  try {
    mem_log_audit('model', 'llm_error', 'ml_generate',
                  kind + ': ' + String(err).substring(0, 400), 0, Number(ms || 0), '');
  } catch (e) {}
}

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

  opt = apply_egress_policy_nested(opt);
  try {
    query_checked("SET @_rag_out = NULL");
    query_checked(
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

var RAG_BLOCKED_SCHEMAS = ['mysql', 'sys', 'information_schema', 'performance_schema'];

function rag_store_blocked(spec) {
  var s = String(spec == null ? '' : spec).trim().replace(/`/g, '');
  if (!s) return false;
  var dot = s.indexOf('.');
  if (dot <= 0) {

    return /^agent_/i.test(s);
  }
  var schema = s.substring(0, dot).toLowerCase();
  return RAG_BLOCKED_SCHEMAS.indexOf(schema) !== -1;
}

// Splits a caller-supplied vector_store into what may be searched and what may not.
function rag_filter_vector_store(list) {
  if (!Array.isArray(list)) return { allowed: list, blocked: [] };
  var allowed = [], blocked = [];
  for (var i = 0; i < list.length; i++) {
    if (rag_store_blocked(list[i])) blocked.push(String(list[i]));
    else allowed.push(list[i]);
  }
  return { allowed: allowed, blocked: blocked };
}

function rag_table_blocked(tb) {
  if (!tb) return false;
  if (typeof tb === 'string') return rag_store_blocked(tb);
  return rag_store_blocked(String(tb.schema_name || '') + '.' + String(tb.table_name || ''));
}

function rag_blocked_message(blocked) {
  return t('拒绝：不能把向量检索指向系统库或 agent 自身的表（' + blocked.join('、') + '）。' +
           '这些表按 principal 隔离，绕过隔离键即可读到其他用户的数据。',
           'Rejected: vector retrieval cannot target a system schema or the agent\'s own tables (' +
           blocked.join(', ') + '). Those tables are isolated per principal, and reaching them ' +
           'this way bypasses the isolation key.');
}

function get_rag_options(chat_opt) {
  var user_rag = (chat_opt && chat_opt.rag_options &&
                  typeof chat_opt.rag_options === 'object') ? chat_opt.rag_options : {};

  chat_opt=chat_opt || {};
  var legacy = {};
  if (chat_opt.retrieve_top_k !== undefined) legacy.n_citations = chat_opt.retrieve_top_k;
  if (chat_opt.retrieval_options) legacy.retrieval_options = chat_opt.retrieval_options;
  if (chat_opt.vector_store_columns) legacy.vector_store_columns = chat_opt.vector_store_columns;
  if (chat_opt.embed_model_id) legacy.embed_model_id = chat_opt.embed_model_id;
  if (chat_opt.schema_name) legacy.schema = [String(chat_opt.schema_name)];
  if (Array.isArray(chat_opt.tables) && chat_opt.tables.length) {
    legacy.vector_store=chat_opt.tables.map(function(tb) {
      return typeof tb==='string' ? tb : String(tb.schema_name)+'.'+String(tb.table_name);
    });
  }

  var defaults = { n_citations: 6, distance_metric: 'COSINE', skip_generate: 1 };

  var merged = Object.assign({}, defaults, legacy, user_rag);
  if (!merged.embed_model_id) merged.embed_model_id = get_embed_model_id(merged);

  /* The caller's vector_store is a request, not a capability. */
  var filtered = rag_filter_vector_store(merged.vector_store);
  if (filtered.blocked.length) {
    merged.vector_store  = filtered.allowed;
    merged.blocked_stores = filtered.blocked;
  }
  return merged;
}