# ShannonBase Agent — self-checks / 自检程序

Development and operator tooling. **Not part of the agent's interface**: nothing
here answers a question a user of `sys.shannon_chat` has, and two of the three
write rows. For the agent itself see [HOW_TO_USE.md](HOW_TO_USE.md).

## The write opt-in

Everything here that writes is gated on one session variable:

```sql
SET @shannon_agent_selfcheck_allow_writes = 1;
```

Without it the entry point returns a one-row explanation and does nothing. That
covers the memory self-check, the recall self-check, and `shannon_agent_loopcheck`
— the last of which runs a whole agent turn for real, so it writes conversation
rows, a SQL trace and usage counters, and its policy cases execute a real
`DROP TABLE` through the real tool path. One knob for "let the self-checks write",
not one per entry point.

`mysql-test/t/shannon_agent_loop.test` asserts the refusal before opting in,
because a gate nobody tests is a gate that quietly stops being one.

A caveat on the word "internal". All of these are declared exactly like
`sys.shannon_chat` — `DEFINER='mysql.sys'@'localhost' … SQL SECURITY INVOKER` in
`sys` — so the gate above is what stops them mutating anything, not a privilege.
They remain callable by anyone who can call the agent; the read-only
`selfcheck('tools')` needs no opt-in and is meant to be callable. Restricting
*execution* is a separate privileges decision that has not been made.

## sys.shannon_agent_selfcheck('tools', NULL, NULL)

Read-only, and the one an operator may legitimately want: it verifies that the
registry, the rendered catalogue, `validate_tool_call()` and `execute_tool()`
still agree, and returns a single `OK` row when they do. It is also what
`mysql-test/t/shannon_agent_tool_contract.test` asserts, so the contract cannot
silently drift from the implementation.

It additionally checks that the *budgeted* catalogue still names every tool, that
an on-demand category re-expands when the message calls for it, and that
`describe_tool` returns a full entry — the three ways progressive disclosure could
quietly cost a capability.

## sys.shannon_agent_loopcheck(<case>)

Takes one case name, or `NULL` for every case. It runs the agent for real
against a scripted model — no LLM, no network — and asserts on the loop's own
signals: which tools ran, why
the turn ended, and whether the answer carried an incompleteness note. Never on
generated prose, which a script cannot make realistic. It is its own routine
rather than another kind of `shannon_agent_selfcheck` because each routine body
is a separate full copy of the agent closure sharing one engine heap with its
runtime data, and the self-check body carries more of it. `mysql-test/t/shannon_agent_loop.test`
runs one `CALL` per case so a regression names itself.

## sys.shannon_agent_selfcheck('recall', …) — retrieval quality / 检索质量评测

`sys.shannon_agent_selfcheck('recall', …)` carries a labelled set of 28 documents and 38 queries (47 judged pairs), built to discriminate rather than merely to be large: distinctive identifiers where embeddings carry no signal, paraphrases sharing no token with their answer, and near-miss distractors (`fact_orders` beside `fact_sales`) that punish a retriever for matching loosely.

```sql
SET @shannon_agent_selfcheck_allow_writes = 1;
CALL sys.shannon_agent_selfcheck('recall', 'seed',  'run1');
CALL sys.shannon_agent_selfcheck('recall', 'score:lexical:3', 'run1');
-- with an embedding model present:
CALL sys.shannon_agent_selfcheck('recall', 'score:lexical,vector,hybrid:5', 'run1');
CALL sys.shannon_agent_selfcheck('recall', 'cleanup', 'run1');
```

It reports `recall@k` and MRR per query kind, so a retriever that is strong on identifiers and weak on paraphrase shows up as exactly that rather than being averaged into one number. The lexical baseline recorded in `mysql-test/r/shannon_agent_storage.result` is `recall@3 = 1.00` on identifiers against `0.45` on paraphrase — that asymmetry is the evidence that the two legs are complementary, and it is what the `retrieval.*` and `long_term.ranking.*` weights should be chosen against.

Seeding writes real rows to `mysql.agent_semantic_fact` under the calling principal, scoped by `scope='rqs:<label>'`, so `cleanup` removes exactly what `seed` added.

