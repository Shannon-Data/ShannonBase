//@include lib_memory_registry.js

/* ===========================================================================
 * Metering and quota, per principal per day.
 *
 * The only quota that existed was max_facts_per_principal -- a row cap on one
 * table.  Nothing counted model calls, tokens, or stored bytes, so the
 * expensive resources were the unmetered ones.  A principal could spend an
 * instance's entire model budget and the only record of it was a token count
 * inside an audit row's free-text detail column, which nothing aggregates.
 *
 * Counters are advisory, not transactional: they are written outside the
 * user's transaction (a rolled-back conversation still cost the model call it
 * made) and a counter update that fails never fails the turn.  What they buy
 * is a number an operator can query and a ceiling that stops a runaway loop,
 * not billing-grade accounting.
 * ======================================================================== */

var USAGE_DEFAULTS = {
  enabled: true,
  /* 0 means unlimited.  Everything is unlimited by default: this ships as
   * metering, and a quota nobody configured should not start refusing work on
   * an instance that was running fine yesterday.  An operator who wants a
   * ceiling sets one. */
  max_llm_calls_per_day:      0,
  max_prompt_tokens_per_day:  0,
  max_artifact_bytes_per_day: 0,
  max_turns_per_day:          0
};

/* Read straight out of the passed chat_options rather than through
 * get_memory_options().
 *
 * That function memoises into A._mem_opt and returns the memo regardless of
 * what it is handed, which is correct for the agent loop -- one options object
 * per call -- and wrong for a function whose whole job is to answer a question
 * about the options it was given.  Going through it made usage_options(x)
 * silently ignore x whenever anything had already resolved the options for
 * this call, which is almost always. */
function usage_options(chat_opt) {
  var co  = (chat_opt && typeof chat_opt === 'object') ? chat_opt : {};
  var mo  = (co.memory_options && typeof co.memory_options === 'object') ? co.memory_options : {};
  var u   = (mo.quota && typeof mo.quota === 'object') ? mo.quota : {};
  var d   = USAGE_DEFAULTS;
  return {
    enabled:                    u.enabled !== false,
    max_llm_calls_per_day:      Math.max(0, mem_num(u.max_llm_calls_per_day,      d.max_llm_calls_per_day)),
    max_prompt_tokens_per_day:  Math.max(0, mem_num(u.max_prompt_tokens_per_day,  d.max_prompt_tokens_per_day)),
    max_artifact_bytes_per_day: Math.max(0, mem_num(u.max_artifact_bytes_per_day, d.max_artifact_bytes_per_day)),
    max_turns_per_day:          Math.max(0, mem_num(u.max_turns_per_day,          d.max_turns_per_day))
  };
}

var USAGE_COLUMNS = ['turns', 'llm_calls', 'prompt_tokens', 'completion_tokens',
                     'embed_calls', 'tool_calls', 'artifact_bytes', 'llm_ms'];

/* Add to today's counters.  One upsert, no read: the row may not exist yet
 * and two sessions may add to it at once, which is exactly what ON DUPLICATE
 * KEY UPDATE with col = col + VALUES(col) is for. */
function usage_add(delta) {
  if (!delta) return false;
  var prefix = mem_principal_prefix();
  if (!prefix) return false;

  var cols = ['principal_prefix', 'usage_date'], vals = ["'" + esc(prefix) + "'", 'CURDATE()'], ups = [];
  for (var i = 0; i < USAGE_COLUMNS.length; i++) {
    var c = USAGE_COLUMNS[i];
    var v = Math.max(0, Math.floor(mem_num(delta[c], 0)));
    if (!v) continue;
    cols.push(c); vals.push(String(v));
    ups.push(c + '=' + c + '+VALUES(' + c + ')');
  }
  if (!ups.length) return false;

  try {
    query_checked("INSERT INTO mysql.agent_usage (" + cols.join(',') + ")" +
                  " VALUES (" + vals.join(',') + ")" +
                  " ON DUPLICATE KEY UPDATE " + ups.join(','));
    return true;
  } catch (e) {
    /* Metering must never be the reason a turn fails. */
    return false;
  }
}

function usage_today() {
  var out = { turns: 0, llm_calls: 0, prompt_tokens: 0, completion_tokens: 0,
              embed_calls: 0, tool_calls: 0, artifact_bytes: 0, llm_ms: 0 };
  var prefix = mem_principal_prefix();
  if (!prefix) return out;
  var rows = query("SELECT " + USAGE_COLUMNS.join(',') + " FROM mysql.agent_usage" +
                   " WHERE principal_prefix='" + esc(prefix) + "' AND usage_date=CURDATE()");
  if (!Array.isArray(rows) || !rows.length) return out;
  for (var i = 0; i < USAGE_COLUMNS.length; i++)
    out[USAGE_COLUMNS[i]] = Number(rows[0][USAGE_COLUMNS[i]] || 0);
  return out;
}

/* Checked once at the top of a call, against today's totals rather than
 * against this turn's projected cost: the agent cannot know in advance how
 * many model calls a question will take, so the ceiling is enforced on entry
 * and overshoot within one turn is accepted.
 *
 * Across turns the overshoot used to be unbounded, because the counter was
 * only written when a turn finished: N turns entering together all read the
 * same total and all passed.  usage_reserve_turn() writes `turns` the
 * moment the turn starts instead, so the window a concurrent arrival can
 * slip through is one statement wide rather than a whole turn, and what
 * bounds how many can be in it is how many turns the account can have in
 * flight -- which is MAX_USER_CONNECTIONS, enforced by the server at
 * connect time.
 *
 * These ceilings are deliberately not the instance's admission control.
 * A turn is one connection, so "how many at once" and "how often" are
 * account attributes (MAX_USER_CONNECTIONS, MAX_QUERIES_PER_HOUR) and CPU
 * is a resource group; reimplementing any of that here would be a weaker
 * copy of something the server already enforces before this code runs.
 * What is left for these counters is the one cost the server has no
 * counterpart for -- money spent at a model provider -- plus metering that
 * makes the spend queryable per principal. */
function usage_check_quota(chat_opt) {
  var q = usage_options(chat_opt);
  if (!q.enabled) return { ok: true };
  if (!q.max_llm_calls_per_day && !q.max_prompt_tokens_per_day &&
      !q.max_artifact_bytes_per_day && !q.max_turns_per_day)
    return { ok: true };

  var used = usage_today();
  var checks = [
    { limit: q.max_turns_per_day,          used: used.turns,          name: 'turns' },
    { limit: q.max_llm_calls_per_day,      used: used.llm_calls,      name: 'llm_calls' },
    { limit: q.max_prompt_tokens_per_day,  used: used.prompt_tokens,  name: 'prompt_tokens' },
    { limit: q.max_artifact_bytes_per_day, used: used.artifact_bytes, name: 'artifact_bytes' }
  ];
  for (var i = 0; i < checks.length; i++) {
    if (checks[i].limit > 0 && checks[i].used >= checks[i].limit) {
      var msg = t('已达今日配额上限（' + checks[i].name + '：' +
                  checks[i].used + '/' + checks[i].limit + '）。请明日再试，' +
                  '或由管理员调整 @chat_options.memory_options.quota。',
                  'Daily quota reached (' + checks[i].name + ': ' +
                  checks[i].used + '/' + checks[i].limit + '). Try again tomorrow, or have an ' +
                  'operator raise @chat_options.memory_options.quota.');
      mem_log_audit('L1', 'quota', 'mysql.agent_usage',
                    checks[i].name + ' ' + checks[i].used + '/' + checks[i].limit, 0, 0, '');
      return { ok: false, reason: checks[i].name, message: msg,
               used: checks[i].used, limit: checks[i].limit };
    }
  }
  return { ok: true };
}

/* Claim the turn against the daily ceiling at the moment it is admitted,
 * not when it finishes.  `turns` is the only counter that can be known up
 * front; the cost counters are settled by usage_record_turn() below, which
 * is why this one does not double-count them. */
function usage_reserve_turn() {
  return usage_add({ turns: 1 });
}

/* The agent loop's own ending, recorded once per finished loop.
 *
 * Neither `turns` nor the model-cost counters are settled here any more, and
 * for the same reason in both cases -- this is reached only by Route D.
 * `turns` is claimed on entry by usage_reserve_turn(), so a turn that ended
 * badly still counts; model cost is settled by log_turn_cost(), which every
 * route reaches through persist_turn(). What is left here is the loop's own
 * shape: its tool calls and how it stopped. */
function usage_record_turn(tool_calls) {
  var c = A.cost || {};
  /* The turn's ending, recorded where the turn's cost is recorded.
   *
   * agent_usage counts what was spent; without this it could not say what
   * the spending achieved. The ratio that matters operationally is how
   * often turns end in something other than 'finish' -- that number moving
   * after a prompt or tool-catalogue change is the earliest signal that the
   * change made the agent worse, and it was previously not recorded
   * anywhere at all. Kept in the audit trail rather than as a new column,
   * so the counter table keeps its fixed shape. */
  try {
    mem_log_audit('loop', 'turn_end', String(A.stop_reason || 'unknown'),
                  'turns=' + Number(A.turn_count || 0) +
                  ' tools=' + Number(tool_calls || 0) +
                  ' llm_calls=' + Number(c.llm_calls || 0) +
                  ' llm_errors=' + Number(c.llm_errors || 0) +
                  ' truncated=' + Number(c.truncated_calls || 0) +
                  ' compactions=' + Number(A.compactions || 0),
                  0, Number(c.llm_ms || 0), '');
  } catch (e) {}
  return usage_add({ tool_calls: Number(tool_calls || 0) });
}

var USAGE = { add: usage_add, today: usage_today, check: usage_check_quota,
              reserve: usage_reserve_turn,
              record_turn: usage_record_turn, options: usage_options };
