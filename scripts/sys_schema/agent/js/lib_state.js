// Module-level shared context.
var A = {
  user_message: '',
  conversation_id: '',
  lang: 'en',

  current_db: '',
  last_think: '',
  cached_chat_opt: null,
  tx_active: false,

  turn_id: '',
  // Filled by llm_note_call() in lib_ml.js -- see the comment there on why these are estimates.
  cost: null,
  // Tool calls in this execution.
  tool_calls: 0,
  mem_block_tokens: 0,
  // Per-invocation memo cache for infer_candidate_tables (lib_ml.js).
  idf_cache: {}
};