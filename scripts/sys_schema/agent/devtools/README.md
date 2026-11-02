# Agent devtools

Development-time checks for the ShannonBase agent. **Not part of the agent's
interface and not installed into the release `sys` schema** — the two routines
they create live here, not in `ml_agent_chat.sql.in`, because they write
(memory rows, review plans, conversation rows, a real `DROP TABLE` in the loop
policy cases) and are roughly half the size of the agent schema.

For what each check proves, see [../SELFCHECK.md](../SELFCHECK.md).

## Run it

```sh
# syntax only, no server needed
./agent/devtools/check_syntax.sh

# everything, against a dev server
./agent/devtools/run_checks.sh --socket /tmp/mysql.sock -u root

# the non-mutating subset
./agent/devtools/run_checks.sh --socket /tmp/mysql.sock -u root --read-only
```

Both scripts are self-contained: they expand the `//@include` templates with
`cmake -P run_genagentjsfile.cmake` into a temporary directory, so no build is
required. `check_syntax.sh` needs a JerryScript CLI (auto-detected at
`cmake_build/runtime_output_directory/jerry` or `$MYSQL_BINDIR/jerry`); build it
with `make -j5 jerry`. `run_checks.sh` needs the `mysql` client and a server.

## What runs

| step | proves |
|---|---|
| `check_syntax.sh` | every source file and every generated routine body parses in the same engine the server uses |
| `tools` | tool registry, JSON schema, approval metadata and rendered prompt agree |
| `sqlmode` | the SQL-mode gate accepts a default session |
| `memory/seed`, `memory/verify` | turns are complete, ordered, deduplicated, redacted and scoped to the principal |
| `review/*` | approvals are compare-and-set; a refused or abandoned step never executes |
| `loop` | read ceiling, repeat detector, error budget, compaction, deadline and stop reasons fire as specified |
| `recall` (opt-in, `--with-recall`) | lexical retrieval baseline |

`run_checks.sh` exits non-zero if any of them fails. It mirrors the value-level
expectations in `mysql-test/suite/agent`, but it is a fast pre-check, not a
replacement: the MTR suite is still what gates a merge.

## Options

- `--jerry PATH`, `--mysql PATH` — override auto-detection.
- `--socket`, `--host`, `--port`, `-u/--user`, `-p/--password` — connection.
  The password is passed through `MYSQL_PWD`, not argv.
- `--read-only` — syntax, install, `tools` and `sqlmode` only.
- `--skip-syntax` — skip the offline syntax step.
- `--with-recall` — also seed and score the retrieval corpus.
- `--keep` — keep the temporary directory.

## Build integration

`ml_agent_dev.sql` is always generated into the build tree
(`make gen_agent_devtools`). It is only embedded in the `sys` schema when
`-DSHANNON_AGENT_DEVTOOLS=ON`, which defaults to ON for `WITH_DEBUG` and
`CMAKE_BUILD_TYPE=Debug` builds.
