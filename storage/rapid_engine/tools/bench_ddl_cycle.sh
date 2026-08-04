#!/usr/bin/env bash
# =============================================================================
# ShannonBase Rapid Engine — DDL-Heavy Load/Unload Stress Script
# =============================================================================
# Purpose:
#   Stress-test the CREATE TABLE → SECONDARY_LOAD → SECONDARY_UNLOAD → DROP TABLE
#   lifecycle.  This exercises:
#     - Populator / log coordinator (change propagation)
#     - Table / RpdTable / MemoryPool / ART construction & destruction
#     - load_indexes_caches_impl (mysql.indexes full scan)
#     - EmbeddingManager DDL event processing (ONNX inference)
#
# Usage:
#   CYCLES=100   TABLES_PER_CYCLE=5   ROWS_PER_TABLE=100K   ./bench_ddl_cycle.sh
#
#   # With perf:
#   PERF_RECORD=1  MYSQLD_PID=$(pgrep mysqld)  CYCLES=50  ./bench_ddl_cycle.sh
#
# Prerequisites:
#   Same as bench_pure_select.sh
#
# Output:
#   results/ddl_bench_<timestamp>/summary.txt
#   results/ddl_bench_<timestamp>/perf.data + flamegraph.svg (if PERF_RECORD=1)
# =============================================================================

set -euo pipefail

# ─── Config file loading ─────────────────────────────────────────────────────
# Priority: env vars > bench.local.conf > bench.conf
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "${SCRIPT_DIR}/bench.conf" ]]; then
  set -a; source "${SCRIPT_DIR}/bench.conf"; set +a
fi
if [[ -f "${SCRIPT_DIR}/bench.local.conf" ]]; then
  set -a; source "${SCRIPT_DIR}/bench.local.conf"; set +a
fi

# ─── Configuration ────────────────────────────────────────────────────────────
MYSQL_CLIENT="${MYSQL_CLIENT:-mysql}"
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-}"
MYSQL_SOCKET="${MYSQL_SOCKET:-}"
AWK="$(command -v gawk || command -v awk)"

TEST_DB="${TEST_DB:-rapid_ddl_bench}"

# Number of CREATE→LOAD→UNLOAD→DROP cycles
CYCLES="${CYCLES:-50}"

# Tables created per cycle
TABLES_PER_CYCLE="${TABLES_PER_CYCLE:-5}"

# Rows per table
ROWS_PER_TABLE="${ROWS_PER_TABLE:-100000}"

# Pause between cycles (seconds, 0 = no pause)
CYCLE_PAUSE="${CYCLE_PAUSE:-0.5}"

PERF_RECORD="${PERF_RECORD:-0}"
PERF_EVENTS="${PERF_EVENTS:-cycles:u,instructions:u,branches:u,branch-misses:u,cache-misses:u,cache-references:u}"
PERF_FREQ="${PERF_FREQ:-997}"
PERF_SUDO="${PERF_SUDO:-0}"
MYSQLD_PID="${MYSQLD_PID:-}"

TS=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${RESULT_DIR:-results/ddl_bench_${TS}}"

# ─── Helpers ──────────────────────────────────────────────────────────────────
die()  { echo "[FATAL] $*" >&2; exit 1; }
info() { echo "[INFO]  $(date +%H:%M:%S) $*"; }

mysql_cmd() {
  local opts=(-u "$MYSQL_USER" -h "$MYSQL_HOST" -P "$MYSQL_PORT" --init-command="SET USE_SECONDARY_ENGINE=FORCED;")
  [[ -n "$MYSQL_SOCKET" ]] && opts+=(-S "$MYSQL_SOCKET")
  [[ -n "$MYSQL_PASS"   ]] && opts+=("-p${MYSQL_PASS}")
  "$MYSQL_CLIENT" "${opts[@]}" "$@"
}

mysql_exec() { mysql_cmd -N -s -e "$1"; }
mysql_exec_db() {
  if [[ $# -gt 0 ]]; then
    mysql_cmd -N -s -e "$1" "$TEST_DB"
  else
    mysql_cmd -N -s "$TEST_DB"
  fi
}

# ─── Setup ────────────────────────────────────────────────────────────────────
mkdir -p "$RESULT_DIR"
SUMMARY_FILE="${RESULT_DIR}/summary.txt"
TIMING_CSV="${RESULT_DIR}/timings.csv"
echo "cycle,table,phase,elapsed_ms" > "$TIMING_CSV"

info "Output: $RESULT_DIR"

# Prompt for password if not set
if [[ -z "$MYSQL_PASS" ]] && [[ -t 0 ]]; then
  read -rsp "MySQL password for ${MYSQL_USER}@${MYSQL_HOST}: " MYSQL_PASS
  echo ""
fi

mysql_exec "SELECT 1" > /dev/null || die "Cannot connect"

# Perf
PERF_PID=""
if [[ "$PERF_RECORD" == "1" ]]; then
  command -v perf &>/dev/null || die "perf not found"
  [[ -z "$MYSQLD_PID" ]] && MYSQLD_PID=$(pgrep -f 'mysqld' | head -1)
  [[ -z "$MYSQLD_PID" ]] && die "Cannot find mysqld PID"
  if [[ "$PERF_SUDO" == "1" ]]; then
    PERF_CMD="sudo perf"
  else
    PERF_CMD="perf"
  fi
  info "perf record on PID $MYSQLD_PID ..."
  $PERF_CMD record -o "${RESULT_DIR}/perf.data" -F "$PERF_FREQ" -e "$PERF_EVENTS" -g -p "$MYSQLD_PID" --call-graph dwarf &
  PERF_PID=$!
  sleep 2

  # Verify perf is still alive
  if ! kill -0 "$PERF_PID" 2>/dev/null; then
    wait "$PERF_PID" 2>/dev/null || true
    PERF_EXIT=$?
    PERF_PID=""
    echo "[WARN]  perf record failed to start (exit code: $PERF_EXIT)"
    echo "[WARN]  Likely causes:"
    echo "[WARN]    - perf_event_paranoid too high: check /proc/sys/kernel/perf_event_paranoid"
    echo "[WARN]    - Try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid"
    echo "[WARN]    - Or: PERF_SUDO=1 PERF_RECORD=1 ... ./bench_ddl_cycle.sh"
    echo "[WARN]  Continuing without perf — no perf.data / flamegraph will be generated."
    rm -f "${RESULT_DIR}/perf.data"
  fi
fi

# ─── Create test database ─────────────────────────────────────────────────────
mysql_exec "DROP DATABASE IF EXISTS ${TEST_DB};"
mysql_exec "CREATE DATABASE ${TEST_DB};"

# Pre-create a template DDL file for efficiency
CREATE_SQL="${RESULT_DIR}/create_template.sql"
cat > "$CREATE_SQL" <<SQLEOF
CREATE TABLE __TABLE__ (
  id      BIGINT NOT NULL AUTO_INCREMENT,
  col_a   BIGINT NOT NULL DEFAULT 0,
  col_b   VARCHAR(64) NOT NULL DEFAULT '',
  col_c   DECIMAL(12,4) NOT NULL DEFAULT 0,
  col_d   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  INDEX idx_a (col_a)
) ENGINE=InnoDB SECONDARY_ENGINE=RAPID;
SQLEOF

INSERT_SQL="${RESULT_DIR}/insert_template.sql"
cat > "$INSERT_SQL" <<SQLEOF
INSERT INTO __TABLE__ (col_a, col_b, col_c, col_d)
SELECT
  FLOOR(RAND() * 100000),
  CONCAT('val_', LPAD(seq, 8, '0')),
  RAND() * 99999.9999,
  FROM_UNIXTIME(UNIX_TIMESTAMP('2022-01-01') + FLOOR(RAND() * 315360000))
FROM (
  SELECT ones.n + 10*tens.n + 100*hundreds.n + 1000*thousands.n AS seq FROM
    (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) ones,
    (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) tens,
    (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) hundreds,
    (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) thousands
  LIMIT __ROWS__
) gen;
SQLEOF

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Main Loop                                                                  ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info "Starting ${CYCLES} cycles × ${TABLES_PER_CYCLE} tables × ${ROWS_PER_TABLE} rows"
info ""

TOTAL_TABLES=0
CYCLE_TIMES=()

for ((cycle=1; cycle<=CYCLES; cycle++)); do
  CYCLE_START=$(date +%s%N)

  TABLE_NAMES=()
  for ((t=0; t<TABLES_PER_CYCLE; t++)); do
    TABLE_NAMES+=("ddl_t_${cycle}_${t}")
  done

  # ── Phase A: CREATE + INSERT + SECONDARY_LOAD ──
  for tbl in "${TABLE_NAMES[@]}"; do
    T0=$(date +%s%N)

    # CREATE TABLE
    sed "s/__TABLE__/${tbl}/g" "$CREATE_SQL" | mysql_exec_db
    T1=$(date +%s%N)

    # INSERT data
    sed -e "s/__TABLE__/${tbl}/g" -e "s/__ROWS__/${ROWS_PER_TABLE}/g" "$INSERT_SQL" | mysql_exec_db
    T2=$(date +%s%N)

    # SECONDARY_LOAD into Rapid
    mysql_exec_db "ALTER TABLE ${tbl} SECONDARY_LOAD;" 2>/dev/null || true
    T3=$(date +%s%N)

    echo "${cycle},${tbl},create,$(( (T1-T0)/1000000 ))"  >> "$TIMING_CSV"
    echo "${cycle},${tbl},insert,$(( (T2-T1)/1000000 ))"  >> "$TIMING_CSV"
    echo "${cycle},${tbl},load,$(( (T3-T2)/1000000 ))"    >> "$TIMING_CSV"

    ((TOTAL_TABLES++))
  done

  # Brief query on loaded tables (optional — verify data is queryable)
  if [[ "${RUN_QUERY_CHECK:-0}" == "1" ]]; then
    for tbl in "${TABLE_NAMES[@]}"; do
      mysql_exec_db "SELECT COUNT(*) FROM ${tbl};" > /dev/null || true
    done
  fi

  # ── Phase B: SECONDARY_UNLOAD + DROP ──
  for tbl in "${TABLE_NAMES[@]}"; do
    T4=$(date +%s%N)

    mysql_exec_db "ALTER TABLE ${tbl} SECONDARY_UNLOAD;" 2>/dev/null || true
    T5=$(date +%s%N)

    mysql_exec_db "DROP TABLE IF EXISTS ${tbl};"
    T6=$(date +%s%N)

    echo "${cycle},${tbl},unload,$(( (T5-T4)/1000000 ))" >> "$TIMING_CSV"
    echo "${cycle},${tbl},drop,$(( (T6-T5)/1000000 ))"   >> "$TIMING_CSV"
  done

  CYCLE_END=$(date +%s%N)
  CYCLE_MS=$(( (CYCLE_END - CYCLE_START) / 1000000 ))
  CYCLE_TIMES+=("$CYCLE_MS")

  info "Cycle ${cycle}/${CYCLES}  —  ${CYCLE_MS}ms  (total tables: ${TOTAL_TABLES})"

  [[ "$CYCLE_PAUSE" != "0" ]] && sleep "$CYCLE_PAUSE"
done

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Summary                                                                    ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info ""
info "=== Summary ==="

{
  echo "============================================================"
  echo "DDL Cycle Benchmark Summary"
  echo "============================================================"
  echo "Cycles:            ${CYCLES}"
  echo "Tables per cycle:  ${TABLES_PER_CYCLE}"
  echo "Rows per table:    ${ROWS_PER_TABLE}"
  echo "Total tables:      ${TOTAL_TABLES}"
  echo ""

  # Per-phase averages from CSV
  echo "--- Per-Phase Latency (avg ms) ---"
  for phase in create insert load unload drop; do
    "$AWK" -F, -v ph="$phase" '$4==ph { sum+=$5; n++ } END { if(n>0) printf "  %-10s  avg=%-8.1f  n=%d\n", ph, sum/n, n }' "$TIMING_CSV"
  done
  echo ""

  echo "--- Cycle Latency (ms) ---"
  printf '%d\n' "${CYCLE_TIMES[@]}" | "$AWK" '
    { a[++n]=$1; sum+=$1; if(n==1||$1<min) min=$1; if(n==1||$1>max) max=$1 }
    END {
      for (i=1; i<=n; i++) for (j=i+1; j<=n; j++) if (a[i]>a[j]) { tmp=a[i]; a[i]=a[j]; a[j]=tmp }
      printf "  avg=%-8.1f  min=%-8.1f  p50=%-8.1f  p95=%-8.1f  max=%-8.1f\n",
        sum/n, min, a[int(n*0.50)+1], a[int(n*0.95)+1], max
    }'
  echo ""
  echo "Results: $RESULT_DIR"
} | tee "$SUMMARY_FILE"

# ─── Stop perf ────────────────────────────────────────────────────────────────
if [[ -n "$PERF_PID" ]]; then
  info "Stopping perf (PID $PERF_PID) ..."
  kill -TERM "$PERF_PID" 2>/dev/null || true
  waited=0
  while kill -0 "$PERF_PID" 2>/dev/null && [[ $waited -lt 50 ]]; do
    sleep 0.1; ((waited++))
  done
  if kill -0 "$PERF_PID" 2>/dev/null; then
    kill -KILL "$PERF_PID" 2>/dev/null || true
    wait "$PERF_PID" 2>/dev/null || true
  fi
  wait "$PERF_PID" 2>/dev/null || true

  # Fixup header via perf inject
  perf_data="${RESULT_DIR}/perf.data"
  perf_fixed="${RESULT_DIR}/perf_fixed.data"
  if ${PERF_CMD:-perf} inject -i "$perf_data" -o "$perf_fixed" 2>/dev/null; then
    if ${PERF_CMD:-perf} script -i "$perf_fixed" > /dev/null 2>&1; then
      mv "$perf_fixed" "$perf_data"
    else
      rm -f "$perf_fixed"
    fi
  fi

  if command -v stackcollapse-perf.pl &>/dev/null && command -v flamegraph.pl &>/dev/null; then
    ${PERF_CMD:-perf} script -i "$perf_data" 2>/dev/null \
      | stackcollapse-perf.pl \
      | flamegraph.pl --title "ShannonBase DDL Cycle Benchmark" \
      > "${RESULT_DIR}/flamegraph.svg" 2>/dev/null || true
    [[ -s "${RESULT_DIR}/flamegraph.svg" ]] && info "Flamegraph: ${RESULT_DIR}/flamegraph.svg"
  fi
fi

# ─── Cleanup ──────────────────────────────────────────────────────────────────
if [[ "${KEEP_DATA:-0}" != "1" ]]; then
  info "Dropping ${TEST_DB} ..."
  mysql_exec "DROP DATABASE IF EXISTS ${TEST_DB};" || true
fi

info "Done."
