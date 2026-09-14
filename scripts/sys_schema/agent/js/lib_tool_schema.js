//@include lib_lang.js

/* JSON Schema subset validator for ToolSpec.args.
 *
 * Deliberately a *subset*: type / enum / required / properties / items /
 * minLength / maxLength / pattern / minimum / maximum / minItems / maxItems /
 * anyOf.  That is everything the tool argument shapes in this agent actually
 * need, and nothing that would drag a full JSON Schema engine into a
 * JerryScript stored program.
 *
 * Two deliberate omissions:
 *   - additionalProperties is never enforced.  LLMs routinely emit extra keys
 *     (top_k, desc, ...) that the old hand-written switch silently ignored;
 *     turning those into hard validation failures would change the agent
 *     loop's retry/error_count semantics, not just the message text.
 *   - Semantic checks (is this SQL read-only? is the table loaded?) stay in
 *     the tool handler or in spec.validate, never here.
 *
 * Fail-fast, single-error return, matching the old validate_tool_call()
 * behaviour of returning the first problem it found.
 */
function ts_type_of(v) {
  if (v === null || v === undefined) return 'null';
  if (Array.isArray(v)) return 'array';
  return typeof v;
}

function ts_is_integer(v) {
  return ts_type_of(v) === 'number' && isFinite(v) && Math.floor(v) === v;
}

/* Returns null when the value conforms, else {path, keyword, expected}. */
function ts_validate(schema, value, path) {
  if (!schema) return null;
  path = path || '';

  if (schema.type) {
    var types  = Array.isArray(schema.type) ? schema.type : [schema.type];
    var actual = ts_type_of(value);
    var type_ok = false;
    for (var i = 0; i < types.length; i++) {
      if (types[i] === actual) { type_ok = true; break; }
      if (types[i] === 'integer' && ts_is_integer(value)) { type_ok = true; break; }
    }
    if (!type_ok)
      return { path: path, keyword: 'type', expected: types.join('|') };
  }

  if (schema.enum) {
    var hit = false;
    for (var e = 0; e < schema.enum.length; e++)
      if (schema.enum[e] === value) { hit = true; break; }
    if (!hit)
      return { path: path, keyword: 'enum', expected: schema.enum.join('|') };
  }

  if (ts_type_of(value) === 'string') {
    if (typeof schema.minLength === 'number' && value.trim().length < schema.minLength)
      return { path: path, keyword: 'minLength', expected: String(schema.minLength) };
    if (typeof schema.maxLength === 'number' && value.length > schema.maxLength)
      return { path: path, keyword: 'maxLength', expected: String(schema.maxLength) };
    if (schema.pattern && !(new RegExp(schema.pattern)).test(value))
      return { path: path, keyword: 'pattern', expected: schema.pattern };
  }

  if (ts_type_of(value) === 'number') {
    if (typeof schema.minimum === 'number' && value < schema.minimum)
      return { path: path, keyword: 'minimum', expected: String(schema.minimum) };
    if (typeof schema.maximum === 'number' && value > schema.maximum)
      return { path: path, keyword: 'maximum', expected: String(schema.maximum) };
  }

  if (Array.isArray(value)) {
    if (typeof schema.minItems === 'number' && value.length < schema.minItems)
      return { path: path, keyword: 'minItems', expected: String(schema.minItems) };
    if (typeof schema.maxItems === 'number' && value.length > schema.maxItems)
      return { path: path, keyword: 'maxItems', expected: String(schema.maxItems) };
    if (schema.items) {
      for (var a = 0; a < value.length; a++) {
        var item_err = ts_validate(schema.items, value[a], path + '[' + a + ']');
        if (item_err) return item_err;
      }
    }
  }

  if (ts_type_of(value) === 'object') {
    var req = schema.required || [];
    for (var r = 0; r < req.length; r++) {
      var rv = value[req[r]];
      /* An empty string is "missing" for our purposes: every required string
       * argument here is a SQL statement, a table name or a question, and the
       * old switch treated '' exactly the same as absent. */
      if (rv === undefined || rv === null ||
          (typeof rv === 'string' && rv.trim().length === 0))
        return { path: (path ? path + '.' : '') + req[r],
                 keyword: 'required', expected: req[r] };
    }
    var props = schema.properties || {};
    for (var k in props) {
      if (!Object.prototype.hasOwnProperty.call(props, k)) continue;
      if (value[k] === undefined) continue;
      var prop_err = ts_validate(props[k], value[k], path ? path + '.' + k : k);
      if (prop_err) return prop_err;
    }
    /* additionalProperties intentionally not enforced -- see header. */
  }

  if (schema.anyOf) {
    var any_ok = false;
    for (var o = 0; o < schema.anyOf.length; o++)
      if (!ts_validate(schema.anyOf[o], value, path)) { any_ok = true; break; }
    if (!any_ok)
      return { path: path, keyword: 'anyOf', expected: String(schema.anyOf.length) };
  }

  return null;
}

/* Bilingual message lookup: exact "path#keyword" > wildcard "*#keyword" >
 * path-only > generic fallback.  Every message the old hand-written switch
 * produced is carried over verbatim into the owning spec's `messages` map, so
 * user-visible validation text does not regress. */
function ts_error_message(spec, err) {
  var m = null;
  if (err && spec && spec.messages) {
    m = spec.messages[err.path + '#' + err.keyword] ||
        spec.messages['*#' + err.keyword] ||
        spec.messages[err.path];
  }
  if (m) return t(m.zh, m.en);
  var where = (err && err.path) ? 'args.' + err.path : 'args';
  var kw    = err ? err.keyword : 'schema';
  return t((spec && spec.name ? spec.name + ' ' : '') + where +
             ' 参数不合法（' + kw + '）',
           (spec && spec.name ? spec.name + ' ' : '') + 'invalid ' + where +
             ' (' + kw + ')');
}

/* Derive a Prompt-ready args skeleton from a schema, used when a spec does
 * not supply a hand-written `example`. */
function ts_sample(schema) {
  if (!schema) return {};
  if (schema.enum) return schema.enum[0];
  if (schema.type === 'object' || schema.properties) {
    var o = {};
    var props = schema.properties || {};
    for (var k in props) {
      if (!Object.prototype.hasOwnProperty.call(props, k)) continue;
      o[k] = ts_sample(props[k]);
    }
    return o;
  }
  if (schema.type === 'array')   return [ts_sample(schema.items)];
  if (schema.type === 'string')  return (schema.minLength >= 3) ? '...' : '';
  if (schema.type === 'integer' || schema.type === 'number') return 0;
  if (schema.type === 'boolean') return true;
  return null;
}
