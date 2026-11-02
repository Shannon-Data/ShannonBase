#!/usr/bin/env bash
#
# Expand an agent SQL template into a standalone .sql file.
#
# Usage:
#   gen_dev_sql.sh OUTPUT [TEMPLATE]
#
# TEMPLATE defaults to ml_agent_dev.sql.in (the dev-only self-check routines).
# Pass ../ml_agent_chat.sql.in to produce the shipped agent schema instead.
# Needs only cmake: //@include is resolved the same way the build does it.

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AGENT_DIR=$(cd "$HERE/.." && pwd)

OUT=${1:-}
if [ -z "$OUT" ]; then
  echo "usage: gen_dev_sql.sh OUTPUT [TEMPLATE]" >&2
  exit 2
fi
TEMPLATE=${2:-$HERE/ml_agent_dev.sql.in}

mkdir -p "$(dirname "$OUT")"

cmake -D "JS_SRC_DIR=$AGENT_DIR/js" \
      -D "JS_INCLUDE_DIRS=$AGENT_DIR/js;$HERE/js" \
      -D "ENTRY=shannon_agent_run.js" \
      -D "TEMPLATE=$TEMPLATE" \
      -D "OUTPUT=$OUT" \
      -P "$AGENT_DIR/run_genagentjsfile.cmake"
