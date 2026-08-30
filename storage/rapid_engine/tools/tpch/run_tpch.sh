#!/usr/bin/env bash
# TPC-H A/B runner: InnoDB (legacy opt) vs Rapid FORCED (hypergraph).
# Usage: run_tpch.sh [q1 q2 ...]   (default: all 22)
#   MAXMS   server-side max_execution_time per query (default 180000)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL="${MYSQL_CLIENT:-/home/lihao/workshop/shannon-bin/bin/mysql}"
CONN=(-h127.0.0.1 -P3306 -uroot -p123456 -N)
DB=tpch
MAXMS="${MAXMS:-180000}"
OUT="${OUT:-$HERE/results/run_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"

m() { "$MYSQL" "${CONN[@]}" "$@" 2>/dev/null; }

kill_leftovers() {
  local ids
  ids=$(m -e "SELECT id FROM information_schema.processlist WHERE db='$DB' AND command='Query' AND info LIKE 'select%'")
  for id in $ids; do m -e "KILL QUERY $id" >/dev/null 2>&1; done
}

QUERIES=("$@")
[ ${#QUERIES[@]} -eq 0 ] && QUERIES=($(for i in $(seq 1 22); do echo q$i; done))

run_one() {  # label  preamble  qfile  outfile
  local label="$1" pre="$2" qf="$3" of="$4"
  local start end dur
  start=$(date +%s.%N)
  "$MYSQL" "${CONN[@]}" "$DB" 2>"$of.err" >"$of" <<SQL
SET SESSION max_execution_time=$MAXMS;
$pre
$(cat "$qf")
SQL
  local rc=$?
  end=$(date +%s.%N)
  dur=$(awk "BEGIN{printf \"%.2f\", $end-$start}")
  local note=""
  if grep -qi "max_execution_time exceeded\|Query execution was interrupted" "$of.err"; then note=" (TIMEOUT ${MAXMS}ms)"; fi
  printf '%-5s %-7s %9s s  rc=%d rows=%s%s\n' "$(basename "$qf" .sql)" "$label" "$dur" "$rc" "$(wc -l <"$of")" "$note" | tee -a "$SUMMARY"
  if [ -s "$of.err" ] && grep -qv "Using a password" "$of.err"; then
    grep -v "Using a password" "$of.err" | sed 's/^/       ERR: /' | tee -a "$SUMMARY"
  fi
  kill_leftovers
}

INNODB_PRE="SET SESSION optimizer_switch='hypergraph_optimizer=off'; SET SESSION use_secondary_engine=OFF;"
RAPID_PRE="SET SESSION optimizer_switch='hypergraph_optimizer=on'; SET SESSION use_secondary_engine=FORCED;"

# SIDES=both|rapid|innodb -- rapid-only skips the slow InnoDB baseline while iterating.
SIDES="${SIDES:-both}"

for q in "${QUERIES[@]}"; do
  qf="$HERE/queries/$q.sql"
  [ -f "$qf" ] || { echo "missing $qf"; continue; }
  [ "$SIDES" != rapid ]  && run_one innodb "$INNODB_PRE" "$qf" "$OUT/$q.innodb"
  [ "$SIDES" != innodb ] && run_one rapid  "$RAPID_PRE"  "$qf" "$OUT/$q.rapid"
  if [ -s "$OUT/$q.innodb" ] && [ -s "$OUT/$q.rapid" ]; then
    if diff -q <(sort "$OUT/$q.innodb") <(sort "$OUT/$q.rapid") >/dev/null; then
      echo "      DIFF: identical" | tee -a "$SUMMARY"
    else
      echo "      DIFF: MISMATCH" | tee -a "$SUMMARY"
      diff <(sort "$OUT/$q.innodb") <(sort "$OUT/$q.rapid") | head -6 | sed 's/^/        /' | tee -a "$SUMMARY"
    fi
  fi
done
echo "results in $OUT"
