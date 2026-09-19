//@include lib_tool_schema.js

/* Tool registry: the single source of truth for a tool's name, argument
 * schema, policy/approval metadata, execution body and Prompt documentation.
 *
 * JerryScript constraints this file has to respect:
 *   - register_tool() runs at script load, once per CALL, in every execution
 *     root.  It must be idempotent and must never touch the database.
 *   - spec.handler must be a `function` declaration.  `var f = function(){}`
 *     is still undefined when the top-level register_tool() call runs.
 */
/* ------------------------------------------------------------------- hooks
 *
 * Extension points around tool execution, so that a deployment can add a
 * rule without editing this file.
 *
 * The problem they solve: every policy the agent enforces used to be a
 * hardcoded branch inside evaluate_step_policy() or one of the
 * check_*_policy() functions. An organisation with its own rule -- "never
 * touch the billing schema during business hours", "log every statement
 * against these tables to our own audit sink" -- had no way to express it
 * except by patching lib_tools.js, which is also the file that gets
 * regenerated into a ~350KB stored program body on every build. That is a
 * merge conflict once a release and a strong incentive to fork.
 *
 * Two events, both synchronous, both inside execute_tool() so that no
 * caller can reach a tool around them:
 *
 *   pre_tool_use(tool, args, ctx)
 *       Returning nothing lets the call proceed. Returning
 *       {deny: true, response: "..."} refuses it, and the refusal reaches
 *       the model as an ordinary tool failure it can react to. Returning
 *       {args: {...}} substitutes the arguments -- which is how a hook
 *       narrows a query rather than rejecting it.
 *
 *   post_tool_use(tool, args, ctx, result)
 *       Returning nothing keeps the result. Returning an object replaces
 *       it. Runs for failures as well as successes, because an audit sink
 *       that only sees successes is not an audit sink.
 *
 * A hook that throws is ignored, deliberately: a bug in an extension must
 * not be able to take the agent down, and a hook that cannot run is a
 * weaker guarantee than one that can but is still better than an outage.
 * Denials are the exception -- a hook that means to refuse must return,
 * not throw, and that difference is what keeps "the rule said no" distinct
 * from "the rule crashed".
 */
var HOOKS = { pre_tool_use: [], post_tool_use: [] };

function register_hook(event, fn) {
  if (!HOOKS[event] || typeof fn !== 'function') return false;
  HOOKS[event].push(fn);
  return true;
}

function run_pre_tool_hooks(tool, args, ctx) {
  var list = HOOKS.pre_tool_use;
  var cur = args;
  for (var i = 0; i < list.length; i++) {
    var out;
    try { out = list[i](tool, cur, ctx); }
    catch (e) {
      try {
        mem_log_audit('hook', 'pre_tool_use_error', tool, String(e).substring(0, 400), 0, 0, '');
      } catch (e2) {}
      continue;
    }
    if (!out) continue;
    if (out.deny) {
      return { deny: true,
               result: { ok: false,
                         response: out.response ||
                           t('该操作被本实例的策略钩子拒绝。',
                             'This operation was refused by an instance policy hook.'),
                         error: out.error || 'denied_by_hook' } };
    }
    if (out.args) cur = out.args;
  }
  return { deny: false, args: cur };
}

function run_post_tool_hooks(tool, args, ctx, result) {
  var list = HOOKS.post_tool_use;
  var cur = result;
  for (var i = 0; i < list.length; i++) {
    var out;
    try { out = list[i](tool, args, ctx, cur); }
    catch (e) {
      try {
        mem_log_audit('hook', 'post_tool_use_error', tool, String(e).substring(0, 400), 0, 0, '');
      } catch (e2) {}
      continue;
    }
    if (out) cur = out;
  }
  return cur;
}

var TOOL_REGISTRY = {};   /* name -> spec */
var TOOL_ORDER    = [];   /* registration order, before sorting by spec.order */

function register_tool(spec) {
  if (!spec || !spec.name) return;
  if (!TOOL_REGISTRY[spec.name]) TOOL_ORDER.push(spec.name);
  spec.category = spec.category || 'core';
  spec.scope    = spec.scope    || 'db';
  spec.messages = spec.messages || {};
  spec.doc      = spec.doc      || { zh: spec.name, en: spec.name };
  spec.order    = (typeof spec.order === 'number') ? spec.order : 1000;
  TOOL_REGISTRY[spec.name] = spec;
}

function get_tool_spec(name) {
  var key = String(name === null || name === undefined ? '' : name);
  return Object.prototype.hasOwnProperty.call(TOOL_REGISTRY, key)
    ? TOOL_REGISTRY[key] : null;
}

function tool_names() {
  return TOOL_ORDER.slice(0).sort(function(a, b) {
    var d = TOOL_REGISTRY[a].order - TOOL_REGISTRY[b].order;
    return d !== 0 ? d : (a < b ? -1 : (a > b ? 1 : 0));
  });
}

/* Approval metadata, derived from the spec instead of the old parallel
 * ML_WRITE_TOOLS constant.  Returns null for tools that neither write nor
 * change schema -- exactly the contract build_review_step() relied on.
 *
 * Restricted to `category === 'ml'` on purpose: build_review_step() used
 * ML_WRITE_TOOLS both as "is this a write?" and as "should I synthesize a
 * CALL sys.ML_* preview instead of reading args.sql?".  Non-ML write tools
 * (update_data, run_ddl) carry real SQL in args.sql and must keep going down
 * the classify_statement() path. */
function tool_review_meta(name) {
  var s = get_tool_spec(name);
  if (!s) return null;
  if (s.category !== 'ml') return null;
  if (!s.write && !s.ddl) return null;
  return {
    risk:          s.risk || (s.ddl ? 'high' : 'medium'),
    table_arg:     s.tableArg,
    fallback_arg:  s.fallbackArg,
    transactional: s.transactional !== false
  };
}

/* Does this tool need to be gated behind the approval workflow at all?
 * Memory-scoped tools write to mysql.agent_* only, never to user data, and
 * have no transaction to join -- see evaluate_step_policy(). */
function tool_is_memory_scoped(name) {
  var s = get_tool_spec(name);
  return !!(s && s.scope === 'memory' && !s.requiresTx);
}

/* Which categories get their full documentation in every prompt, and which
 * are collapsed to a signature until the conversation needs them.
 *
 * The catalogue is emitted in full on every single turn, in the language the
 * user is speaking.  At 33 tools that is ~6.4k characters of Chinese or ~9k
 * of English -- charged on turn one, on turn ten, and on a turn that only
 * wanted SHOW TABLES.  Sixteen of the tools are the ML family, and they are
 * roughly half of it.
 *
 * So the default is: everything the model needs to do SQL at all stays in
 * full, and the rest collapses to one line naming the tool and its arguments.
 * A collapsed tool is still callable -- the signature carries the argument
 * names -- and describe_tool returns the full entry when the model wants the
 * detail before calling.
 *
 * A category is expanded when the current message matches its `match` test,
 * so asking about training a model brings the ML tools back in full on the
 * same turn, not the turn after. */
var TOOL_CATEGORY_POLICY = {};

function register_tool_category(policy) {
  if (!policy || !policy.name) return;
  TOOL_CATEGORY_POLICY[policy.name] = policy;
}

/* No policy means "always expanded": a new category is visible by default and
 * has to opt into being collapsed.  Failing open is right here -- the cost of
 * a wrongly-expanded category is prompt tokens, the cost of a wrongly-hidden
 * one is a capability the model cannot find. */
function tool_category_expanded(category, message) {
  var p = TOOL_CATEGORY_POLICY[category];
  if (!p || p.expand !== 'ondemand') return true;
  if (!p.match) return true;
  return p.match.test(String(message || ''));
}

/* One line standing in for a full entry: the name and its argument names,
 * with optional ones marked by a trailing '?'.
 *
 * No description.  That is a considered omission rather than a saving taken
 * too far: the prose first line averaged longer than the signature itself, so
 * including it gave back most of what collapsing the category saved, and for
 * a family whose members are called ml_train, ml_predict_row and ml_score the
 * group header plus the argument names already say what a sentence would.
 *
 * A signature is enough to call the tool.  describe_tool is there for when it
 * is not -- and it is the cheaper failure: an unnecessary describe_tool costs
 * one round trip, whereas a tool the model cannot see at all costs a
 * capability it has no way to discover it is missing. */
function tool_signature_line(spec) {
  var props = (spec.args && spec.args.properties) || {};
  var req   = (spec.args && spec.args.required) || [];
  var parts = [];
  for (var k in props) {
    if (!Object.prototype.hasOwnProperty.call(props, k)) continue;
    var required = false;
    for (var r = 0; r < req.length; r++) if (req[r] === k) { required = true; break; }
    parts.push(k + (required ? '' : '?'));
  }
  return '   ' + spec.name + '(' + parts.join(', ') + ')';
}

/* Prompt tool catalogue, generated from the registry.  Replaces the two
 * hand-maintained bilingual lists that build_system_prompt() carried (which
 * had already drifted from the implementation, and from HOW_TO_USE.md).
 *
 * `group` on a spec emits a section header before its first tool, preserving
 * the old prompt's "[ML/AutoML Tools]" style grouping.
 *
 * `opts.expand_all` renders every category in full regardless of policy.  The
 * contract self-check uses it to prove the registry and the prompt still
 * agree about the whole catalogue, and describe_tool uses it for one tool. */
function render_tool_docs(lang, opts) {
  var is_zh   = (lang === 'zh');
  var o       = opts || {};
  var message = (o.message !== undefined) ? o.message : A.user_message;
  var lines = [ is_zh
    ? '【可用工具】每次只输出一个合法 JSON，禁止在 JSON 前后添加任何文字：'
    : '[Available Tools] Output exactly one valid JSON per turn; no surrounding text:' ];

  var names = tool_names();
  var shown = 0;
  var last_group = '';
  var collapsed = [];
  for (var i = 0; i < names.length; i++) {
    var s = TOOL_REGISTRY[names[i]];
    if (s.hidden) continue;
    if (o.only && o.only !== s.name) continue;

    var group = s.group ? (is_zh ? s.group.zh : s.group.en) : '';

    if (!o.expand_all && !o.only && !tool_category_expanded(s.category, message)) {
      collapsed.push({ spec: s, group: group });
      continue;
    }

    if (group && group !== last_group) {
      lines.push('');
      lines.push(group);
      last_group = group;
    }

    /* `example` may be one object for both languages, or {zh, en} when the
     * placeholder text itself is prose the model will read. */
    var ex_obj = s.example;
    if (ex_obj && (ex_obj.zh !== undefined || ex_obj.en !== undefined))
      ex_obj = is_zh ? ex_obj.zh : ex_obj.en;
    var ex = ex_obj ? JSON.stringify(ex_obj) : JSON.stringify(ts_sample(s.args));
    shown++;
    lines.push(shown + '. {"thought":"...","tool":"' + s.name + '","args":' + ex + '}');
    var doc = is_zh ? s.doc.zh : s.doc.en;
    var doc_lines = String(doc || '').split('\n');
    for (var d = 0; d < doc_lines.length; d++)
      lines.push('   → ' + doc_lines[d]);
  }

  if (collapsed.length) {
    lines.push('');
    lines.push(is_zh
      ? '【其余工具（当前话题未展开）】签名如下，可直接调用；需要完整参数说明时先调用 ' +
        'describe_tool：'
      : '[Other tools (not expanded for this topic)] Signatures below are enough to call them; ' +
        'call describe_tool first if you need the full argument documentation:');
    var last_collapsed_group = '';
    for (var c = 0; c < collapsed.length; c++) {
      if (collapsed[c].group && collapsed[c].group !== last_collapsed_group) {
        lines.push(collapsed[c].group);
        last_collapsed_group = collapsed[c].group;
      }
      lines.push(tool_signature_line(collapsed[c].spec));
    }
  }
  return lines.join('\n');
}

/* Machine-readable contract export.  Used by the contract self-check today;
 * the shape is deliberately MCP/OpenAI-tools flavoured so a protocol adapter
 * can be added later without reopening every spec. */
function tool_manifest() {
  var names = tool_names(), out = [];
  for (var i = 0; i < names.length; i++) {
    var s = TOOL_REGISTRY[names[i]];
    out.push({
      name:        s.name,
      category:    s.category,
      description: { zh: s.doc.zh, en: s.doc.en },
      input_schema: s.args || { type: 'object' },
      annotations: {
        read_only:    !!s.readOnly,
        write:        !!s.write,
        ddl:          !!s.ddl,
        risk:         s.risk || 'low',
        scope:        s.scope,
        hidden:       !!s.hidden,
        capabilities: s.capabilities || []
      }
    });
  }
  return out;
}
