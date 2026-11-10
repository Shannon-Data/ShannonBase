# ShannonBase Agent — self-checks / 自检程序

Development and CI tooling. **Not part of the agent's interface and not part of
the shipped `sys` schema**: nothing here answers a question a user of
`sys.shannon_chat` has, two of the three write rows, and the two routines were
together about half of the generated agent schema. For the agent itself see
[HOW_TO_USE.md](HOW_TO_USE.md).

## Installing them

The routines live in [devtools/](devtools/) and are installed on demand:

```sh
scripts/sys_schema/agent/devtools/install_dev_routines.sh \
    --socket /tmp/mysql.sock -u root
```

Everything below is then callable exactly as shown. `devtools/run_checks.sh`
installs them and runs every check in one go; `devtools/check_syntax.sh` needs
no server. The MTR suite installs them per test through
`mysql-test/suite/agent/include/agent_dev_routines.inc`.

## The write opt-in

Everything here that writes is gated on one session variable:

```sql
SET @shannon_agent_selfcheck_allow_writes = 1;
```

Without it the entry point returns a one-row explanation and does nothing. That
covers the memory self-check, the recall self-check, and `shannon_agent_loopcheck`
— the last of which runs a whole agent turn for real, so it writes conversation
rows and a SQL trace, and its policy cases execute a real
`DROP TABLE` through the real tool path. One knob for "let the self-checks write",
not one per entry point.

`mysql-test/suite/agent/t/shannon_loop.test` asserts the refusal before opting in,
because a gate nobody tests is a gate that quietly stops being one.

A caveat on the word "internal". These are declared exactly like
`sys.shannon_chat` — `DEFINER='mysql.sys'@'localhost' … SQL SECURITY INVOKER` in
`sys` — so once installed, the gate above is what stops them mutating anything,
not a privilege: they are callable by anyone who can call the agent. That is one
more reason they are not installed by default; on a release server they simply
do not exist. Restricting *execution* is a separate privileges decision that has
not been made.

## sys.shannon_agent_selfcheck('tools', NULL, NULL)

Read-only, and the one an operator may legitimately want after an upgrade, once
the dev routines are installed: it verifies that the
registry, the rendered catalogue, `validate_tool_call()` and `execute_tool()`
still agree, and returns a single `OK` row when they do. It is also what
`mysql-test/suite/agent/t/shannon_tool_contract.test` asserts, so the contract cannot
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
runtime data, and the self-check body carries more of it. `mysql-test/suite/agent/t/shannon_loop.test`
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

It reports `recall@k` and MRR per query kind, so a retriever that is strong on identifiers and weak on paraphrase shows up as exactly that rather than being averaged into one number. The lexical baseline recorded in `mysql-test/suite/agent/r/shannon_storage.result` is `recall@3 = 1.00` on identifiers against `0.45` on paraphrase — that asymmetry is the evidence that the two legs are complementary, and it is what the `retrieval.*` and `long_term.ranking.*` weights should be chosen against.

The vector and hybrid legs need an ONNX model under `llm-models/`. `mysql-test/suite/agent/t/shannon_embedding.test` runs the whole seed/stats/score/cleanup sequence against `multilingual-e5-small` and is the run to compare a model or a fusion change against:

```sh
./mtr --mysqld=--llm-dir=/path/to/dir-containing-llm-models agent.shannon_embedding
```

Seeding writes real rows to `mysql.agent_semantic_fact` under the calling principal, scoped by `scope='rqs:<label>'`, so `cleanup` removes exactly what `seed` added.



## Fixed business transaction integration

`agent.shannon_trusted` creates its own `trusted_business` InnoDB fixtures and
calls `shannon_agent_selfcheck('trusted', op, label)` with write opt-in. The
operations are `contracts`, `flow`, `caller`, `loop`, `permission` and `race`.
Run the MTR case to provision its tables, trigger, restricted user and concurrent
connections; the generic selfcheck runner does not provision these fixtures.

The case verifies required transaction admission, operator authorization,
strict arguments, template/version/target/verification binding, mandatory
approval, and denial of Modify-to-SQL. It checks version/status conflicts,
actual affected rows, same-transaction InnoDB verification, rollback after a
real trigger breaks the postcondition, caller-controlled durability, request
replay and correlated audit. Two real connections compete for the same version:
one completes and one fails the business precondition. The restricted user's
SQL write is refused without changing the business row.
