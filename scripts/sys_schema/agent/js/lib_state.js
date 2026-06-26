/* Module-level shared context. This entire script is re-executed on every
 * function call, so there is no issue of "dirty data residue from the previous
 * call" — it is equivalent to the local variables originally inside
 * shannon_agent_run, just hoisted to the module top level so that sibling
 * helper functions can also access them via closure. */
var A = {
  user_message: '',
  conversation_id: '',
  lang: 'en',
  last_think: '',
  cached_chat_opt: null,
  tx_active: false
};