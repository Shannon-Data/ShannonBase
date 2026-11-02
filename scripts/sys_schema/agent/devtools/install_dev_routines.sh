#!/usr/bin/env bash
#
# Generate and install the dev-only agent routines into a running server.
#
# Creates sys.shannon_agent_selfcheck and sys.shannon_agent_loopcheck. They are
# not part of the shipped sys schema; this is what the MTR suite and
# run_checks.sh use to get them.
#
# Usage:
#   install_dev_routines.sh [--socket PATH | --host H --port P] [-u USER]
#                           [-p PASS] [--mysql PATH] [--keep]
#
# Prints nothing on success.

set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$HERE/../../../.." && pwd)

CONN=()
MYSQL_ARG=""
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --socket)  CONN+=(--socket="${2:-}"); shift 2 ;;
    --host)    CONN+=(--host="${2:-}"); shift 2 ;;
    --port)    CONN+=(--port="${2:-}"); shift 2 ;;
    -u|--user) CONN+=(--user="${2:-}"); shift 2 ;;
    -p|--password) MYSQL_PWD="${2:-}"; export MYSQL_PWD; shift 2 ;;
    --mysql)   MYSQL_ARG="${2:-}"; shift 2 ;;
    --keep)    KEEP=1; shift ;;
    -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install_dev_routines.sh: unknown option: $1" >&2; exit 2 ;;
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
  echo "install_dev_routines.sh: no mysql client found; pass --mysql PATH" >&2
  exit 2
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/agent_devsql.XXXXXX")
cleanup() { [ "$KEEP" -eq 1 ] && echo "kept: $TMP" >&2 || rm -rf "$TMP"; }
trap cleanup EXIT

if ! "$HERE/gen_dev_sql.sh" "$TMP/ml_agent_dev.sql" > "$TMP/gen.log" 2>&1; then
  echo "install_dev_routines.sh: template expansion failed" >&2
  sed 's/^/  /' "$TMP/gen.log" >&2
  exit 1
fi

# The client understands $$ quoting; mysqltest does not, which is why the tests
# call this script instead of sourcing the generated file. utf8mb4 is explicit:
# the routine bodies carry non-ASCII text, and their creation charset is what
# the engine reads those literals back with.
"$MYSQL" "${CONN[@]}" --batch --default-character-set=utf8mb4 < "$TMP/ml_agent_dev.sql"
