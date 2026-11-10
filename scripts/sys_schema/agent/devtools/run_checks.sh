#!/usr/bin/env bash
#
# Run the agent's development checks against a server.
#
# Installs the dev-only routines (sys.shannon_agent_selfcheck,
# sys.shannon_agent_loopcheck) from the source tree, runs the syntax check,
# then exercises the tool contract, the SQL-mode gate, memory, the review
# state machine and the loop. Writes to mysql.agent_*; use --read-only for
# the non-mutating subset.
#
# Usage:
#   ./run_checks.sh [--socket PATH | --host H --port P] [-u USER] [-p PASS]
#                   [--mysql PATH] [--jerry PATH] [--read-only]
#                   [--skip-syntax] [--with-recall] [--keep]
#
# Exit status: 0 when every check passes, 1 otherwise.

set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$HERE/../../../.." && pwd)

CONN=()
MYSQL_ARG=""
JERRY_ARG=""
READ_ONLY=0
SKIP_SYNTAX=0
WITH_RECALL=0
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --socket)  CONN+=(--socket "${2:-}"); shift 2 ;;
    --host)    CONN+=(--host "${2:-}"); shift 2 ;;
    --port)    CONN+=(--port "${2:-}"); shift 2 ;;
    -u|--user) CONN+=(--user "${2:-}"); shift 2 ;;
    -p|--password)
      MYSQL_PWD="${2:-}"; export MYSQL_PWD; shift 2 ;;
    --mysql)   MYSQL_ARG="${2:-}"; shift 2 ;;
    --jerry)   JERRY_ARG="${2:-}"; shift 2 ;;
    --read-only)  READ_ONLY=1; shift ;;
    --skip-syntax) SKIP_SYNTAX=1; shift ;;
    --with-recall) WITH_RECALL=1; shift ;;
    --keep)    KEEP=1; shift ;;
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "run_checks.sh: unknown option: $1" >&2; exit 2 ;;
  esac
done

find_mysql() {
  local c
  if [ -n "$MYSQL_ARG" ]; then printf '%s\n' "$MYSQL_ARG"; return 0; fi
  if [ -n "${MYSQL:-}" ] && [ -x "$MYSQL" ]; then printf '%s\n' "$MYSQL"; return 0; fi
  if [ -n "${MYSQL_BINDIR:-}" ]; then
    for c in "$MYSQL_BINDIR/runtime_output_directory/mysql" "$MYSQL_BINDIR/mysql"; do
      [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
  fi
  for c in "$REPO_ROOT"/cmake_build/runtime_output_directory/mysql \
           "$REPO_ROOT"/build*/runtime_output_directory/mysql; do
    if [ -x "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  command -v mysql 2>/dev/null && return 0
  return 1
}

MYSQL=$(find_mysql) || {
  echo "run_checks.sh: no mysql client found; pass --mysql /path/to/mysql" >&2
  exit 2
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/agent_checks.XXXXXX")
MEM_LABEL="devmem$(date +%s)$$"
REV_LABELS=("devrev1$(date +%s)$$" "devrev2$(date +%s)$$" "devrev3$(date +%s)$$")
RCL_LABEL="devrcl$(date +%s)$$"
INSTALLED=0

cleanup() {
  if [ "$INSTALLED" -eq 1 ]; then
    run_sql "SET @shannon_agent_selfcheck_allow_writes=1;
             CALL sys.shannon_agent_selfcheck('memory','cleanup','$MEM_LABEL');
             CALL sys.shannon_agent_selfcheck('review','cleanup','${REV_LABELS[0]}');
             CALL sys.shannon_agent_selfcheck('review','cleanup','${REV_LABELS[1]}');
             CALL sys.shannon_agent_selfcheck('review','cleanup','${REV_LABELS[2]}');
             CALL sys.shannon_agent_selfcheck('recall','cleanup','$RCL_LABEL');" \
      >/dev/null 2>&1
  fi
  [ "$KEEP" -eq 1 ] && echo "kept: $TMP" || rm -rf "$TMP"
}
trap cleanup EXIT

PASS=0
FAIL=0
CURRENT=""

run_sql() { "$MYSQL" "${CONN[@]}" --batch --raw --skip-column-names -e "$1"; }

declare -A KV
kv_load() {
  KV=()
  local k v
  while IFS=$'\t' read -r k v; do
    [ -n "${k:-}" ] && KV["$k"]="${v:-}"
  done
}

ok()   { PASS=$((PASS + 1)); echo "ok    $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL  $*"; }

expect() {
  local key=$1 want=$2 got=${KV[$1]:-<missing>}
  if [ "$got" = "$want" ]; then
    return 0
  fi
  bad "$CURRENT: $key = '$got', want '$want'"
  return 1
}

call() {
  local sql=$1
  CURRENT=$2
  local out
  out=$(run_sql "SET @shannon_agent_selfcheck_allow_writes=1; $sql" 2>"$TMP/err.log")
  if [ $? -ne 0 ]; then
    bad "$CURRENT: call failed -- $(tail -3 "$TMP/err.log" | tr '\n' ' ')"
    return 1
  fi
  printf '%s\n' "$out"
}

# ok_for_section <lines...>: report only if this section added no failures.
BEGIN_FAIL=$FAIL
section_ok() {
  if [ "$FAIL" -eq "$BEGIN_FAIL" ]; then
    ok "$*"
  fi
  BEGIN_FAIL=$FAIL
}

# 1. syntax, from the source tree, no install needed
if [ "$SKIP_SYNTAX" -eq 0 ]; then
  CURRENT="syntax"
  if [ -n "$JERRY_ARG" ]; then
    "$HERE/check_syntax.sh" --jerry "$JERRY_ARG" > "$TMP/syntax.log" 2>&1
  else
    "$HERE/check_syntax.sh" > "$TMP/syntax.log" 2>&1
  fi
  if [ $? -eq 0 ]; then
    ok "syntax: $(tail -1 "$TMP/syntax.log")"
  else
    bad "syntax: see below"
    sed 's/^/      /' "$TMP/syntax.log"
  fi
else
  echo "skip  syntax"
fi

# 2. install the dev-only routines
CURRENT="install"
if "$HERE/install_dev_routines.sh" "${CONN[@]}" --mysql "$MYSQL" > "$TMP/install.log" 2>&1; then
  INSTALLED=1
  ok "install: sys.shannon_agent_selfcheck, sys.shannon_agent_loopcheck"
else
  bad "install: $(tail -3 "$TMP/install.log" | tr '\n' ' ')"
  echo
  echo "Every remaining check needs the routines; stopping here."
  exit 1
fi

# 3. the tool contract is self-asserting: one OK row or a problem per row
CURRENT="tools"
out=$(run_sql "CALL sys.shannon_agent_selfcheck('tools', NULL, NULL)" 2>"$TMP/err.log")
if [ $? -eq 0 ] && [ "$out" = $'result\tOK' ]; then
  ok "tools: registry / schema / prompt agree"
else
  bad "tools: ${out:-$(tail -3 "$TMP/err.log" | tr '\n' ' ')}"
fi

# 4. the SQL-mode gate on a default session
CURRENT="sqlmode"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('sqlmode', NULL, NULL)" sqlmode)"
expect ok 1
expect message_names_mode 0
expect message_is_actionable 0
section_ok "sqlmode: default mode accepted"

if [ "$READ_ONLY" -eq 1 ]; then
  echo
  echo "read-only: skipped memory, review, loop, recall"
  echo "passed: $PASS, failed: $FAIL"
  [ "$FAIL" -eq 0 ]
  exit
fi

# Runtime recovery and retries require the current native bridge.
CURRENT="runtime/entry"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('runtime','entry',NULL)" runtime/entry)"
expect failed_recovery_blocked true
expect no_new_work true
expect unknown_owner_blocked true
expect caller_preserved true
section_ok "runtime/entry: uncertain recovery stops new work"

CURRENT="runtime/retry"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('runtime','retry','$MEM_LABEL')" runtime/retry)"
expect admitted true
expect pending_blocked true
expect changed_blocked true
expect replayed true
section_ok "runtime/retry: keyed retries are suppressed or replay saved results"

# 5. memory: ordering, dedup, redaction, isolation
CURRENT="memory/seed"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('memory','seed','$MEM_LABEL')" memory/seed)"
expect conversation_scoped yes
expect degraded no
section_ok "memory/seed: turns and facts written"

CURRENT="memory/verify"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('memory','verify','$MEM_LABEL')" memory/verify)"
expect total_rows 10
expect incomplete_turns 0
expect rows_without_isolation_key 0
expect rows_leaking_secret 0
expect rows_redacted 1
expect min_seq 1
expect max_seq 10
expect distinct_seq 10
section_ok "memory/verify: rows complete, ordered, redacted, scoped"

# 6. the review state machine: one approval, one execution
CURRENT="review/transitions"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('review','transitions','${REV_LABELS[0]}')" review/transitions)"
expect approve_once won
expect approve_twice lost
expect completed_status completed
expect resurrect_completed lost
expect illegal_edge lost
expect final_status completed
section_ok "review/transitions: approvals are compare-and-set"

CURRENT="review/rejected"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('review','rejected','${REV_LABELS[1]}')" review/rejected)"
expect reject won
expect execute_rejected lost
expect final_status rejected
section_ok "review/rejected: a refused step stays refused"

CURRENT="review/stuck"
kv_load <<< "$(call "CALL sys.shannon_agent_selfcheck('review','stuck','${REV_LABELS[2]}')" review/stuck)"
expect before executing
expect recovered 1
expect after indeterminate
expect fresh_recovered 0
expect fresh_status executing
section_ok "review/stuck: abandoned steps become indeterminate, fresh ones are left alone"

# 7. the loop, driven by a scripted model; every row must be OK
CURRENT="loop"
out=$(call "CALL sys.shannon_agent_loopcheck(NULL)" loopcheck)
loop_bad=$(printf '%s\n' "$out" | grep -v $'^result\tOK$' || true)
if [ -z "$loop_bad" ] && [ -n "$out" ]; then
  ok "loop: all cases behaved as specified"
else
  bad "loop: $loop_bad"
fi

# 8. recall is opt-in: it seeds a labelled corpus and needs a few seconds
if [ "$WITH_RECALL" -eq 1 ]; then
  CURRENT="recall"
  call "CALL sys.shannon_agent_selfcheck('recall','seed','$RCL_LABEL')" recall/seed >/dev/null &&
    call "CALL sys.shannon_agent_selfcheck('recall','score:lexical:3','$RCL_LABEL')" recall/score >/dev/null &&
    ok "recall: lexical baseline produced (see MTR for thresholds)" ||
    bad "recall: see above"
fi

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
