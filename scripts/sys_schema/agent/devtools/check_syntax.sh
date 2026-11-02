#!/usr/bin/env bash
#
# Offline syntax check for the agent's JerryScript sources.
#
# Expands the same //@include templates the build uses, then parses every
# source file and every generated routine body with the JerryScript CLI
# (--parse-only). No server and no build required; needs cmake and jerry.
#
# Usage:
#   ./check_syntax.sh [--jerry PATH] [--keep]
#
# Exit status: 0 when everything parses, 1 otherwise.

set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AGENT_DIR=$(cd "$HERE/.." && pwd)
REPO_ROOT=$(cd "$HERE/../../../.." && pwd)

JERRY_ARG=""
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --jerry) JERRY_ARG="${2:-}"; shift 2 ;;
    --keep)  KEEP=1; shift ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "check_syntax.sh: unknown option: $1" >&2; exit 2 ;;
  esac
done

find_jerry() {
  local c
  if [ -n "$JERRY_ARG" ]; then printf '%s\n' "$JERRY_ARG"; return 0; fi
  if [ -n "${JERRY:-}" ]; then printf '%s\n' "$JERRY"; return 0; fi
  if [ -n "${MYSQL_BINDIR:-}" ]; then
    for c in "$MYSQL_BINDIR/runtime_output_directory/jerry" "$MYSQL_BINDIR/jerry"; do
      [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
  fi
  for c in "$REPO_ROOT"/cmake_build/runtime_output_directory/jerry \
           "$REPO_ROOT"/build*/runtime_output_directory/jerry; do
    if [ -x "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  command -v jerry 2>/dev/null && return 0
  return 1
}

JERRY=$(find_jerry) || {
  echo "check_syntax.sh: no jerry CLI found." >&2
  echo "  build one with: make -j5 jerry" >&2
  echo "  or pass: --jerry /path/to/jerry" >&2
  exit 2
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/agent_syntax.XXXXXX")
cleanup() { [ "$KEEP" -eq 1 ] && echo "kept: $TMP" || rm -rf "$TMP"; }
trap cleanup EXIT

CHECKS=0
FAILED=0

# The server executes a routine body as (function(){ ... })(), so parse it the
# same way instead of as a script (a top-level return is legal in a body).
check_js() {
  local label=$1 file=$2 wrapped="$TMP/wrapped.js"
  CHECKS=$((CHECKS + 1))
  { printf '(function() {\n'; cat "$file"; printf '\n})()\n'; } > "$wrapped"
  if "$JERRY" --parse-only "$wrapped" > "$TMP/jerry.out" 2>&1; then
    return 0
  fi
  FAILED=$((FAILED + 1))
  echo "FAIL  $label"
  if command -v node >/dev/null 2>&1; then
    echo "      $(node --check "$wrapped" 2>&1 | head -5 | sed 's/^/      /')"
  fi
  return 1
}

# gen_sql <template> <output>: expand //@include the way the build does.
gen_sql() {
  local template=$1 out=$2
  "$HERE/gen_dev_sql.sh" "$out" "$template" > "$TMP/gen.log" 2>&1
}

check_sql_bodies() {
  local sql=$1 name start end
  if [ ! -s "$sql" ]; then
    echo "FAIL  $sql was not generated"
    FAILED=$((FAILED + 1))
    return
  fi
  while IFS=$'\t' read -r name start end; do
    [ -n "$name" ] || continue
    sed -n "${start},${end}p" "$sql" > "$TMP/body.js"
    check_js "$name" "$TMP/body.js"
  done < <(awk '
    /^CREATE DEFINER=.*(FUNCTION|PROCEDURE) / {
      name = $0
      sub(/^CREATE DEFINER=[^ ]+ (FUNCTION|PROCEDURE) /, "", name)
      sub(/\(.*$/, "", name)
    }
    /^AS \$\$/ { start = NR + 1; inbody = 1; next }
    inbody && /^\$\$;/ { print name "\t" start "\t" NR - 1; inbody = 0 }
  ' "$sql")
}

echo "jerry: $JERRY"
echo "sources: $AGENT_DIR/js, $AGENT_DIR/devtools/js"

for f in "$AGENT_DIR"/js/*.js; do
  check_js "${f#"$REPO_ROOT"/}" "$f"
done

for f in "$AGENT_DIR"/devtools/js/*.js; do
  check_js "${f#"$REPO_ROOT"/}" "$f"
done

if gen_sql "$AGENT_DIR/ml_agent_chat.sql.in" "$TMP/runtime.sql"; then
  check_sql_bodies "$TMP/runtime.sql"
else
  echo "FAIL  expanding ml_agent_chat.sql.in"
  sed 's/^/      /' "$TMP/gen.log"
  FAILED=$((FAILED + 1))
fi

if gen_sql "$AGENT_DIR/devtools/ml_agent_dev.sql.in" "$TMP/devtools.sql"; then
  check_sql_bodies "$TMP/devtools.sql"
else
  echo "FAIL  expanding devtools/ml_agent_dev.sql.in"
  sed 's/^/      /' "$TMP/gen.log"
  FAILED=$((FAILED + 1))
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo "OK: $CHECKS files/bodies parsed"
  exit 0
fi
echo "FAILED: $FAILED of $CHECKS files/bodies did not parse"
exit 1
