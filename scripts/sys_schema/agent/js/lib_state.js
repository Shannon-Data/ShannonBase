/* Module-level shared context. This entire script is re-executed on every
 * function call, so there is no issue of "dirty data residue from the previous
 * call" — it is equivalent to the local variables originally inside
 * shannon_agent_run, just hoisted to the module top level so that sibling
 * helper functions can also access them via closure. */
var A = {
  user_message: '',
  conversation_id: '',
  lang: 'en',
  /* DATABASE() for this call, published by shannon_agent_run so the policy
   * gates can see it -- validate_tool_call() is not handed a database. */
  current_db: '',
  last_think: '',
  cached_chat_opt: null,
  tx_active: false,
  /* Minted on first use by current_turn_id(); ties this turn's row in
   * mysql.agent_memory to its cost audit row. */
  turn_id: '',
  /* Filled by llm_note_call() in lib_ml.js -- see the comment there on why
   * these are estimates. */
  cost: null,
  /* Incremented by execute_tool(); folded into mysql.agent_usage once per
   * turn by USAGE.record_turn(). */
  tool_calls: 0,
  mem_block_tokens: 0,
  /* Per-invocation memo cache for infer_candidate_tables (lib_ml.js). Reset
   * via clear_shared_idf_cache() at the top of shannon_agent_run so stale
   * scores never leak across turns/messages, while still letting repeated
   * lookups against the same (db, message) within a single turn — e.g.
   * build_schema_context() followed by a list_tables tool call — reuse the
   * already-computed IDF-weighted candidate list instead of re-scanning
   * information_schema. */
  idf_cache: {}
};