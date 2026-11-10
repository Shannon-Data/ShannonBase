//@include lib_tool_schema.js
var HOOKS = { pre_tool_use: [], post_tool_use: [] };

// options.enforce=true marks a mandatory security policy.
function register_hook(event, fn, options) {
  if (!HOOKS[event] || typeof fn !== 'function') return false;
  if (event === 'pre_tool_use' && options && options.enforce === true)
    fn._shannon_enforce = true;
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
      if (list[i]._shannon_enforce === true) {
        return { deny: true, result: {
          ok: false, error: 'mandatory_hook_failed',
          response: t('强制安全策略执行失败，拒绝工具调用。',
                      'Mandatory security policy failed; tool call denied.')
        } };
      }
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
  if (spec.requiresTx && spec.transactional === false)
    throw new Error('requiresTx conflicts with transactional:false: '+spec.name);
  if (!TOOL_REGISTRY[spec.name]) TOOL_ORDER.push(spec.name);
  if (spec.args && spec.args.additionalProperties === undefined)
    spec.args.additionalProperties = false;
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

// Approval metadata, derived from the spec instead of the old parallel ML_WRITE_TOOLS constant.
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

// Does this tool need to be gated behind the approval workflow at all?
function tool_is_memory_scoped(name) {
  var s = get_tool_spec(name);
  return !!(s && s.scope === 'memory' && !s.requiresTx);
}

var TOOL_CATEGORY_POLICY = {};

function register_tool_category(policy) {
  if (!policy || !policy.name) return;
  TOOL_CATEGORY_POLICY[policy.name] = policy;
}

function tool_category_expanded(category, message) {
  var p = TOOL_CATEGORY_POLICY[category];
  if (!p || p.expand !== 'ondemand') return true;
  if (!p.match) return true;
  return p.match.test(String(message || ''));
}

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

// Prompt tool catalogue, generated from the registry.
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

// Machine-readable contract export.
function tool_manifest() {
  var names = tool_names(), out = [];
  for (var i = 0; i < names.length; i++) {
    var s = TOOL_REGISTRY[names[i]];
    out.push({
      name:        s.name,
      category:    s.category,
      example:     s.example || ts_sample(s.args),
      description: { zh: s.doc.zh, en: s.doc.en },
      input_schema: s.args || { type: 'object' },
      annotations: {
        read_only:    !!s.readOnly,
        write:        !!s.write,
        ddl:          !!s.ddl,
        risk:         s.risk || 'low',
        scope:        s.scope,
        requires_tx:  !!s.requiresTx,
        transactional:s.transactional !== false,
        trusted:      !!s.trusted,
        template_version:s.trusted ? s.trusted.version : null,
        hidden:       !!s.hidden,
        capabilities: s.capabilities || []
      }
    });
  }
  return out;
}
