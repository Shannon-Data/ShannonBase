//@include lib_tools.js
//@include shannon_agent_run.js
// Loop evaluation: does the harness behave, given a model that behaves in a stated way.
function eval_script_respond(prompt, dialect) {
  var sc = A.eval_script;
  // Counted, not kept.
  sc.calls++;
  // A script that keeps being asked for turns is a loop that is not terminating.
  if (sc.calls > 16)
    throw new Error('eval script runaway: model called ' + sc.calls +
                    ' times (turns scripted: ' + sc.turns.length + ')');

  var i = Math.min(sc.pos, sc.turns.length - 1);
  var turn = sc.turns[i] || { text: '(script exhausted)' };
  sc.pos++;

  if (turn.error) {
    A.last_llm_status = { ok: false, kind: classify_llm_error(turn.error).kind,
                          error: turn.error, attempts: 1 };
    A.last_think = '';
    return '';
  }

  var text = '';
  var native_call = null;

  if (turn.native) {

    var d = dialect || 'openai';
    var raw = (d === 'anthropic')
      ? JSON.stringify({ content: [ { type: 'text', text: turn.thought || '' },
                                    { type: 'tool_use', name: turn.native,
                                      input: turn.args || {} } ] })
      : JSON.stringify({ choices: [ { message: {
              content: turn.thought || null,
              tool_calls: [ { id: 'call_1', type: 'function',
                              function: { name: turn.native,
                                          arguments: JSON.stringify(turn.args || {}) } } ] } } ] });
    native_call = llm_parse_native_tool_call(raw, d);
    text = turn.thought || '';
  } else if (turn.tool) {
    text = JSON.stringify({ tool: turn.tool, args: turn.args || {},
                            thought: turn.thought || 'scripted' });
  } else {
    text = String(turn.text == null ? '' : turn.text);
  }

  A.last_llm_status = { ok: true, kind: '', error: '', attempts: 1,
                        finish_reason: turn.finish_reason || 'stop',
                        tool_call: native_call };
  A.last_think = '';
  return text;
}

/* ------------------------------------------------------------- the cases */
// The cases.
function eval_cases() {
  return [
    { name: 'finish_clean',
      turns: [ { text: 'The orders table has 3 rows.' } ],
      expect: { stop_reason: 'finish', tools: [], note: false } },

    { name: 'cancelled_generation',
      turns: [ { error: 'Query execution was interrupted' },
               { tool: 'query_db', args: {sql:'SELECT 1'} } ],
      expect: { stop_reason: 'cancelled', tools: [], note: true } },

    { name: 'one_tool_then_answer',
      turns: [ { tool: 'query_db', args: { sql: 'SELECT COUNT(*) FROM eval_orders' } },
               { text: 'There are 3 orders.' } ],
      expect: { stop_reason: 'finish', tools: ['query_db'], note: false } },

    { name: 'native_tool_call',
      turns: [ { native: 'query_db', args: { sql: 'SELECT COUNT(*) FROM eval_orders' } },
               { text: 'There are 3 orders.' } ],
      expect: { stop_reason: 'finish', tools: ['query_db'], note: false } },

    { name: 'read_ceiling_then_recover',
      turns: [ { tool: 'query_db', args: { sql: 'SELECT * FROM eval_orders' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM eval_orders LIMIT 10' } },
               { text: 'Here are the orders.' } ],
      expect: { stop_reason: 'finish', tools: ['query_db','query_db'], note: false } },

    { name: 'repeat_detected',
      turns: [ { tool: 'query_db', args: { sql: 'SELECT COUNT(*) FROM eval_orders' } },
               { tool: 'query_db', args: { sql: 'SELECT COUNT(*) FROM eval_orders' } },
               { text: 'The orders table has 3 rows.' } ],
      expect: { stop_reason: 'loop_detected', note: true } },

    { name: 'error_budget',
      turns: [ { tool: 'query_db', args: { sql: 'SELECT * FROM no_tbl_1 LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM no_tbl_2 LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM no_tbl_3 LIMIT 1' } },
               { text: 'None of the tables I tried exist.' } ],
      expect: { stop_reason: 'error_budget', note: true } },

    { name: 'max_turns',
      turns: [ { tool: 'query_db', args: { sql: 'SELECT 1 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 2 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 3 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 4 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 5 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 6 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 7 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 8 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 9 AS a LIMIT 1' } },
               { tool: 'query_db', args: { sql: 'SELECT 10 AS a LIMIT 1' } },
               { text: 'Here is what the ten steps established.' } ],
      expect: { stop_reason: 'max_turns', note: true } },

    { name: 'truncated_answer',
      turns: [ { text: 'The orders table contains', finish_reason: 'length' } ],
      expect: { stop_reason: 'truncated', note: true } },

    { name: 'model_unreachable',
      turns: [ { error: 'HTTP 401 unauthorized: invalid api key' } ],
      expect: { stop_reason: 'llm_error', note: false } },

    { name: 'policy_read_capped', manual: true,
      turns: [ { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 100' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 99' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 98' } },
               { text: 'I could not read the orders.' } ],
      expect: { stop_reason: 'error_budget', tools: ['query_db','query_db','query_db'],
                note: true } },

    { name: 'policy_read_uncapped', manual: true,
      turns: [ { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 100' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 99' } },
               { tool: 'query_db', args: { sql: 'SELECT * FROM eval_db.eval_orders LIMIT 98' } },
               { text: 'Here are the orders.' } ],
      expect: { stop_reason: 'finish', tools: ['query_db','query_db','query_db'],
                note: false } },

    // tools:[] is the assertion.
    { name: 'policy_ddl_refused', manual: true,
      turns: [ { tool: 'run_ddl', args: { sql: 'DROP TABLE eval_db.policy_victim' } },
               { text: 'Policy would not let me drop it.' } ],
      expect: { stop_reason: 'finish', tools: [], note: false } },

    { name: 'policy_ddl_allowed', manual: true,
      turns: [ { tool: 'run_ddl', args: { sql: 'DROP TABLE eval_db.policy_victim' } },
               { text: 'Dropped.' } ],
      expect: { stop_reason: 'finish', tools: ['run_ddl'], note: false } },

    // The shipped default, with nothing switched off.
    { name: 'policy_ddl_needs_approval', manual: true,
      turns: [ { tool: 'run_ddl', args: { sql: 'DROP TABLE eval_db.policy_victim' } },
               { text: 'Dropped.' } ],
      expect: { stop_reason: 'awaiting_approval', tools: [], note: false } },

    // The caller has pointed @chat_options.rag_options.vector_store at the agent's own memory.
    { name: 'rag_store_forbidden', manual: true,
      turns: [ { tool: 'ml_rag', args: { question: 'what is data skew' } },
               { text: 'I could not use that knowledge base.' } ],
      expect: { stop_reason: 'finish', tools: ['ml_rag'], note: false } }
  ];
}

/* ------------------------------------------------------------- the driver */
// The same write opt-in the memory and recall self-checks use.
function loopcheck_writes_allowed() {
  var rows = query("SELECT COALESCE(@shannon_agent_selfcheck_allow_writes, 0) AS v");
  return !!(Array.isArray(rows) && rows.length && Number(rows[0].v) === 1);
}

function shannon_loop_selfcheck(which) {

  if (!loopcheck_writes_allowed())
    return ['loop self-check runs the agent for real and writes to mysql.agent_*, ' +
            'and its policy cases execute DDL; ' +
            'SET @shannon_agent_selfcheck_allow_writes = 1 to allow it'];

  var out = [];
  var cases = eval_cases();
  var only  = (which && String(which).trim()) ? String(which).trim() : '';

  for (var i = 0; i < cases.length; i++) {
    var c = cases[i];
    if (only && c.name !== only) continue;

    if (!only && c.manual) continue;
    var problems = eval_run_case(c);
    for (var p = 0; p < problems.length; p++) out.push(c.name + ': ' + problems[p]);
  }
  if (only && !out.length) {
    var known = false;
    for (var k = 0; k < cases.length; k++) if (cases[k].name === only) known = true;
    if (!known) return ['unknown loop case: ' + only];
  }
  return out.length ? out : ['OK'];
}

function eval_run_case(c) {
  var problems = [];
  var problems_calls = -1;
  var conv = 'evalloop_' + c.name + '_' + Date.now();

  /* The scripted model, plus the record of what the loop did with it. */
  A.eval_script = { turns: c.turns, pos: 0, calls: 0 };
  A.eval_tools  = [];

  var probe = function (tool, args, ctx, result) { A.eval_tools.push(tool); return result; };
  register_hook('post_tool_use', probe);

  var answer = '';
  try {
    answer = String(shannon_agent_run('eval: ' + c.name, conv) || '');
  } catch (e) {
    problems.push('threw: ' + String(e));
  }
  problems_calls = A.eval_script ? A.eval_script.calls : -1;

  var got_stop  = String(A.stop_reason || '');
  var got_tools = A.eval_tools.slice(0);
  var got_note  = !!stop_reason_note(got_stop) &&
                  answer.indexOf(stop_reason_note(got_stop).substring(0, 12)) !== -1;

  var kept = [];
  for (var h = 0; h < HOOKS.post_tool_use.length; h++)
    if (HOOKS.post_tool_use[h] !== probe) kept.push(HOOKS.post_tool_use[h]);
  HOOKS.post_tool_use = kept;
  A.eval_script = null;

  // Hand the engine back everything this case accumulated.
  A.eval_tools      = null;
  A.cost            = null;
  A.last_llm_status = null;
  A.last_think      = '';
  A.cached_chat_opt = null;
  A.operator_policy = null;

  if (c.expect.stop_reason && got_stop !== c.expect.stop_reason)
    problems.push('stop_reason expect=' + c.expect.stop_reason + ' got=' + got_stop +
                  ' (model calls=' + problems_calls + ')');

  if (c.expect.tools) {
    if (got_tools.join(',') !== c.expect.tools.join(','))
      problems.push('tools expect=[' + c.expect.tools.join(',') + '] got=[' + got_tools.join(',') + ']');
  }

  if (c.expect.note === true && !got_note)
    problems.push('incomplete answer carried no note (stop_reason=' + got_stop + ')');
  if (c.expect.note === false && got_note)
    problems.push('complete answer carried an incompleteness note');

  return problems;
}
