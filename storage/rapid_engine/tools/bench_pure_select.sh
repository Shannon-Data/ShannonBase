#!/usr/bin/env bash
# =============================================================================
# ShannonBase Rapid Engine — Pure SELECT Benchmark Script
# =============================================================================
# Purpose:
#   Profile the query execution path (vectorized scan/agg, IMCU pruning,
#   SIMD aggregates) in isolation — NO DDL in the hot loop.
#
# Usage:
#   # Quick smoke test (small data, 1 thread, 10s per query type):
#   SCALE=100K  DURATION=10  THREADS=1  ./bench_pure_select.sh
#
#   # Serious profiling run (10M rows, 4 threads, 60s, collect flamegraph):
#   SCALE=10M  DURATION=60  THREADS=4  PERF_RECORD=1  ./bench_pure_select.sh
#
#   # CPU profiling with specific events:
#   PERF_RECORD=1  PERF_EVENTS="cycles:u,instructions:u"  ./bench_pure_select.sh
#
# Prerequisites:
#   - MySQL client binary in PATH (or set MYSQL_CLIENT)
#   - Running ShannonBase server with Rapid engine loaded
#   - Server has enough disk for the test data
#   - For PERF_RECORD=1: perf must be installed, and you need
#     sudo or /proc/sys/kernel/perf_event_paranoid <= 1
#
# Output:
#   - results/bench_<timestamp>/summary.txt   — latency / throughput summary
#   - results/bench_<timestamp>/raw.csv       — per-query raw timings
#   - results/bench_<timestamp>/perf.data     — perf record (if PERF_RECORD=1)
#   - results/bench_<timestamp>/flamegraph.svg — flamegraph (if PERF_RECORD=1)
# =============================================================================

set -euo pipefail

# ─── Config file loading ─────────────────────────────────────────────────────
# Priority: env vars > bench.local.conf > bench.conf
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Layer 1: defaults from bench.conf (shipped with repo)
if [[ -f "${SCRIPT_DIR}/bench.conf" ]]; then
  set -a; source "${SCRIPT_DIR}/bench.conf"; set +a
fi

# Layer 2: local overrides (git-ignored, safe for passwords / custom paths)
if [[ -f "${SCRIPT_DIR}/bench.local.conf" ]]; then
  set -a; source "${SCRIPT_DIR}/bench.local.conf"; set +a
fi

# Layer 3: environment variables — already in scope, they win automatically
# because we use ${VAR:-default} below.

# ─── Configuration (override via env or config file) ─────────────────────────
MYSQL_CLIENT="${MYSQL_CLIENT:-mysql}"
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-}"
MYSQL_SOCKET="${MYSQL_SOCKET:-}"
AWK="$(command -v gawk || command -v awk)"

# Test database (created & dropped by this script — DO NOT use an existing one)
TEST_DB="${TEST_DB:-rapid_bench}"

# Data scale: number of rows in the main test table.
# Supports suffixes: K (×1,000), M (×1,000,000), G (×1,000,000,000)
SCALE="${SCALE:-1M}"

# How long to run each query category (seconds)
DURATION="${DURATION:-30}"

# Number of concurrent clients (via mysqlslap or custom loops)
THREADS="${THREADS:-1}"

# Collect perf data (0=no, 1=yes)
PERF_RECORD="${PERF_RECORD:-0}"
PERF_EVENTS="${PERF_EVENTS:-cycles:u,instructions:u,branches:u,branch-misses:u,cache-misses:u,cache-references:u}"
PERF_FREQ="${PERF_FREQ:-997}"
PERF_SUDO="${PERF_SUDO:-0}"
PERF_MAX_SIZE="${PERF_MAX_SIZE:-2G}"      # hard cap for perf.data size; dwarf call-graph ~200MB/min/core @ 1KHz
PERF_CALL_GRAPH="${PERF_CALL_GRAPH:-fp}"   # "fp" (small, needs -fno-omit-frame-pointer) or "dwarf" (accurate, large)
MYSQLD_PID="${MYSQLD_PID:-}"

# Output directory
TS=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="${RESULT_DIR:-results/bench_${TS}}"

# ─── Helper functions ─────────────────────────────────────────────────────────
die()  { echo "[FATAL] $*" >&2; exit 1; }
info() { echo "[INFO]  $*"; }

parse_scale() {
  local s="$1"
  s="${s//_/}"                        # remove underscores
  if   [[ "$s" =~ ^([0-9]+)G$ ]]; then echo $((${BASH_REMATCH[1]} * 1000000000))
  elif [[ "$s" =~ ^([0-9]+)M$ ]]; then echo $((${BASH_REMATCH[1]} * 1000000))
  elif [[ "$s" =~ ^([0-9]+)K$ ]]; then echo $((${BASH_REMATCH[1]} * 1000))
  else echo "$s"
  fi
}

mysql_cmd() {
  local opts=(-u "$MYSQL_USER" -h "$MYSQL_HOST" -P "$MYSQL_PORT" --init-command="SET USE_SECONDARY_ENGINE=FORCED;")
  [[ -n "$MYSQL_SOCKET" ]] && opts+=(-S "$MYSQL_SOCKET")
  [[ -n "$MYSQL_PASS"   ]] && opts+=("-p${MYSQL_PASS}")
  "$MYSQL_CLIENT" "${opts[@]}" "$@"
}

mysql_exec() {
  mysql_cmd -N -s -e "$1"
}

mysql_exec_db() {
  mysql_cmd -N -s -e "$1" "$TEST_DB"
}

# Run a single query N times and output avg/min/max/p50/p95/p99 (ms)
bench_query() {
  local label="$1" query="$2" runs="${3:-100}"
  local tmpfile
  tmpfile=$(mktemp)
  echo "$query" > "$tmpfile"
  info "  [$label] running ${runs}x ..."
  # Use a tight client loop — lowest overhead
  for ((i=0; i<runs; i++)); do
    local start end
    start=$(date +%s%N)
    mysql_exec_db "$query" > /dev/null 2>&1 || { echo "ERR,$i" >> "${RAW_CSV}"; continue; }
    end=$(date +%s%N)
    local ms=$(( (end - start) / 1000000 ))
    echo "${label},${ms}" >> "${RAW_CSV}"
  done
  rm -f "$tmpfile"

  # Stats from raw csv
  "$AWK" -F, -v label="$label" '
    $1 == label && $2+0 == $2 {
      a[++n] = $2; sum += $2
      if (n==1 || $2<min) min=$2
      if (n==1 || $2>max) max=$2
    }
    END {
      if (n==0) { printf "%-45s  runs=0\n", label; exit }
      for (i=1; i<=n; i++) for (j=i+1; j<=n; j++) if (a[i]>a[j]) { tmp=a[i]; a[i]=a[j]; a[j]=tmp }
      printf "%-45s  runs=%-6d  avg=%-8.2f  min=%-8.2f  p50=%-8.2f  p95=%-8.2f  p99=%-8.2f  max=%-8.2f\n",
        label, n, sum/n, min, a[int(n*0.50)+1], a[int(n*0.95)+1], a[int(n*0.99)+1], max
    }' "${RAW_CSV}" | tee -a "${SUMMARY_FILE}"
}

# Run a query in a loop for DURATION seconds, count QPS
bench_throughput() {
  local label="$1" query="$2"
  info "  [$label] throughput test (${DURATION}s) ..."
  local count=0 start end elapsed
  start=$(date +%s%N)
  end=$(( start + DURATION * 1000000000 ))
  while [[ $(date +%s%N) -lt $end ]]; do
    mysql_exec_db "$query" > /dev/null 2>&1 || true
    (( ++count ))
  done
  local actual_end
  actual_end=$(date +%s%N)
  elapsed=$(( (actual_end - start) / 1000000000 ))
  local qps=0
  [[ $elapsed -gt 0 ]] && qps=$(( count / elapsed ))
  printf "%-45s  qps=%-8d  elapsed=%ds\n" "$label" "$qps" "$elapsed" | tee -a "${SUMMARY_FILE}"
}

# ─── Parse scale ──────────────────────────────────────────────────────────────
NUM_ROWS=$(parse_scale "$SCALE")
info "Scale: $SCALE → $NUM_ROWS rows"
BATCH_SIZE=10000   # rows per INSERT batch

# ─── Setup ────────────────────────────────────────────────────────────────────
mkdir -p "$RESULT_DIR"
SUMMARY_FILE="${RESULT_DIR}/summary.txt"
RAW_CSV="${RESULT_DIR}/raw.csv"
echo "label,latency_ms" > "$RAW_CSV"   # CSV header

info "Output directory: $RESULT_DIR"

# Prompt for password if not set
if [[ -z "$MYSQL_PASS" ]] && [[ -t 0 ]]; then
  read -rsp "MySQL password for ${MYSQL_USER}@${MYSQL_HOST}: " MYSQL_PASS
  echo ""
fi

info "Connecting to ${MYSQL_USER}@${MYSQL_HOST}:${MYSQL_PORT} ..."
mysql_exec "SELECT 1" > /dev/null || die "Cannot connect to MySQL"

# ─── Perf setup ───────────────────────────────────────────────────────────────
PERF_PID=""
if [[ "$PERF_RECORD" == "1" ]]; then
  if ! command -v perf &>/dev/null; then
    die "perf not found — install linux-tools or set PERF_RECORD=0"
  fi
  if [[ -z "$MYSQLD_PID" ]]; then
    MYSQLD_PID=$(pgrep -f 'mysqld' | head -1)
    if [[ -z "$MYSQLD_PID" ]]; then
      die "Cannot find mysqld PID — set MYSQLD_PID= explicitly"
    fi
  fi
  if [[ "$PERF_SUDO" == "1" ]]; then
    PERF_CMD="sudo perf"
  else
    PERF_CMD="perf"
  fi
fi
# (perf record is started just before Phase 4 benchmarks — after schema/data/warmup)

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Phase 1 — Schema & Data                                                     ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info "=== Phase 1: Creating schema & populating data ==="

mysql_exec "DROP DATABASE IF EXISTS ${TEST_DB};"
mysql_exec "CREATE DATABASE ${TEST_DB};"

mysql_exec "USE ${TEST_DB};
-- ── Main test table: wide rows, mix of integer / decimal / varchar ──
CREATE TABLE t_wide (
  id      BIGINT NOT NULL AUTO_INCREMENT,
  grp_low  INT NOT NULL,             -- low cardinality: 10 distinct values
  grp_mid  INT NOT NULL,             -- medium cardinality: ~1K distinct
  grp_high BIGINT NOT NULL,          -- high cardinality: near-unique
  val_int  BIGINT NOT NULL DEFAULT 0,
  val_dec  DECIMAL(18,4) NOT NULL DEFAULT 0,
  val_str  VARCHAR(128) NOT NULL DEFAULT '',
  ts       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  flag     TINYINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  INDEX idx_grp_low  (grp_low),
  INDEX idx_grp_mid  (grp_mid),
  INDEX idx_grp_high (grp_high),
  INDEX idx_ts       (ts)
) ENGINE=InnoDB SECONDARY_ENGINE=RAPID;

-- ── Narrow table: few columns, test pure scan bandwidth ──
CREATE TABLE t_narrow (
  id      BIGINT NOT NULL AUTO_INCREMENT,
  val_a   BIGINT NOT NULL DEFAULT 0,
  val_b   BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id)
) ENGINE=InnoDB SECONDARY_ENGINE=RAPID;

-- ── String-heavy table: test varchar / dictionary paths ──
CREATE TABLE t_strings (
  id      BIGINT NOT NULL AUTO_INCREMENT,
  s_short VARCHAR(32)  NOT NULL DEFAULT '',
  s_long  VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (id),
  INDEX idx_s_short (s_short)
) ENGINE=InnoDB SECONDARY_ENGINE=RAPID;"

info "Tables created. Populating t_wide with ${NUM_ROWS} rows ..."

# Generate bulk INSERT statements efficiently.
# Use a stored procedure to avoid round-trips.
mysql_exec_db "
DELIMITER //
CREATE PROCEDURE pop_wide(IN total_rows BIGINT)
BEGIN
  DECLARE i BIGINT DEFAULT 0;
  DECLARE batch_end BIGINT;
  SET autocommit = 0;
  WHILE i < total_rows DO
    SET batch_end = i + ${BATCH_SIZE};
    IF batch_end > total_rows THEN SET batch_end = total_rows; END IF;

    INSERT INTO t_wide (grp_low, grp_mid, grp_high, val_int, val_dec, val_str, ts, flag)
    SELECT
      (seq + i) % 10,                         -- grp_low:  0..9
      (seq + i) % 1024,                       -- grp_mid:  0..1023
      (seq + i),                              -- grp_high: unique
      FLOOR(RAND() * 1000000),                -- val_int
      RAND() * 99999.9999,                    -- val_dec
      CONCAT('row_', LPAD(seq + i, 10, '0')),-- val_str
      FROM_UNIXTIME(UNIX_TIMESTAMP('2020-01-01') + FLOOR(RAND() * 315360000)),
      FLOOR(RAND() * 2)                       -- flag: 0 or 1
    FROM (
      SELECT ones.n + 10*tens.n + 100*hundreds.n + 1000*thousands.n AS seq
      FROM
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3
         UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7
         UNION ALL SELECT 8 UNION ALL SELECT 9) ones,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3
         UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7
         UNION ALL SELECT 8 UNION ALL SELECT 9) tens,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3
         UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7
         UNION ALL SELECT 8 UNION ALL SELECT 9) hundreds,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3
         UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7
         UNION ALL SELECT 8 UNION ALL SELECT 9) thousands
      LIMIT ${BATCH_SIZE}
    ) gen;

    SET i = batch_end;
    COMMIT;
  END WHILE;
  SET autocommit = 1;
END//
DELIMITER ;
"

info "  Calling pop_wide(${NUM_ROWS}) — this may take a while ..."
START_TIME=$(date +%s)
mysql_exec_db "CALL pop_wide(${NUM_ROWS});"
ELAPSED=$(( $(date +%s) - START_TIME ))
info "  t_wide populated: ${ELAPSED}s"

# Populate t_narrow (same row count)
info "Populating t_narrow with ${NUM_ROWS} rows ..."
mysql_exec_db "
DELIMITER //
CREATE PROCEDURE pop_narrow(IN total_rows BIGINT)
BEGIN
  DECLARE i BIGINT DEFAULT 0;
  SET autocommit = 0;
  WHILE i < total_rows DO
    INSERT INTO t_narrow (val_a, val_b)
    SELECT FLOOR(RAND() * 1000000), FLOOR(RAND() * 1000000)
    FROM (
      SELECT ones.n + 10*tens.n + 100*hundreds.n + 1000*thousands.n AS seq FROM
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) ones,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) tens,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) hundreds,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) thousands
      LIMIT ${BATCH_SIZE}
    ) gen;
    SET i = i + ${BATCH_SIZE};
    IF i >= total_rows THEN SET i = total_rows; END IF;
    COMMIT;
  END WHILE;
  SET autocommit = 1;
END//
DELIMITER ;
CALL pop_narrow(${NUM_ROWS});"

# Populate t_strings (fewer rows — strings are expensive)
STR_ROWS=$(( NUM_ROWS / 10 ))
[[ $STR_ROWS -lt 10000 ]] && STR_ROWS=10000
info "Populating t_strings with ${STR_ROWS} rows ..."
mysql_exec_db "
DELIMITER //
CREATE PROCEDURE pop_strings(IN total_rows BIGINT)
BEGIN
  DECLARE i BIGINT DEFAULT 0;
  SET autocommit = 0;
  WHILE i < total_rows DO
    INSERT INTO t_strings (s_short, s_long)
    SELECT
      CONCAT('sht_', LPAD(seq + i, 8, '0')),
      CONCAT('long_prefix_', LPAD(seq + i, 12, '0'), '_',
             REPEAT('x', FLOOR(RAND() * 100) + 20))
    FROM (
      SELECT ones.n + 10*tens.n + 100*hundreds.n + 1000*thousands.n AS seq FROM
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) ones,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) tens,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) hundreds,
        (SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) thousands
      LIMIT ${BATCH_SIZE}
    ) gen;
    SET i = i + ${BATCH_SIZE};
    IF i >= total_rows THEN SET i = total_rows; END IF;
    COMMIT;
  END WHILE;
  SET autocommit = 1;
END//
DELIMITER ;
CALL pop_strings(${STR_ROWS});"

info "  Done populating."

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Phase 2 — Load into Rapid Engine                                            ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info "=== Phase 2: Loading tables into Rapid Engine (SECONDARY_LOAD) ==="

for tbl in t_wide t_narrow t_strings; do
  info "  ALTER TABLE ${tbl} SECONDARY_LOAD ..."
  START_TIME=$(date +%s)
  mysql_exec_db "ALTER TABLE ${tbl} SECONDARY_LOAD;" || \
    info "  (SECONDARY_LOAD failed for ${tbl} — continuing anyway, Rapid may not be enabled)"
  ELAPSED=$(( $(date +%s) - START_TIME ))
  info "  ${tbl} loaded: ${ELAPSED}s"
done

# ── Verify row counts ──
info "Row counts after load:"
for tbl in t_wide t_narrow; do
  cnt=$(mysql_exec_db "SELECT COUNT(*) FROM ${tbl};")
  info "  ${tbl}: ${cnt}"
done

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Phase 3 — Warm up (fill buffer pool, code caches)                           ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info "=== Phase 3: Warm-up ==="
for ((i=0; i<3; i++)); do
  mysql_exec_db "SELECT COUNT(*) FROM t_wide;" > /dev/null
  mysql_exec_db "SELECT COUNT(*) FROM t_narrow;" > /dev/null
  mysql_exec_db "SELECT MIN(id), MAX(id), AVG(val_int), SUM(val_int) FROM t_wide;" > /dev/null
done
info "  Warm-up done."

# ─── Perf record start (after warm-up, before benchmarks) ──────────────────
if [[ "$PERF_RECORD" == "1" ]]; then
  info "Starting perf record on PID $MYSQLD_PID (events: $PERF_EVENTS, freq: $PERF_FREQ Hz, call-graph: $PERF_CALL_GRAPH) ..."
  perf_max_opt=()
  [[ -n "$PERF_MAX_SIZE" ]] && perf_max_opt=(--max-size "$PERF_MAX_SIZE")
  $PERF_CMD record -o "${RESULT_DIR}/perf.data" \
    -F "$PERF_FREQ" \
    -e "$PERF_EVENTS" \
    -g \
    -p "$MYSQLD_PID" \
    --call-graph "$PERF_CALL_GRAPH" \
    "${perf_max_opt[@]}" &
  PERF_PID=$!
  sleep 2   # let perf settle

  # Verify perf is still alive
  if ! kill -0 "$PERF_PID" 2>/dev/null; then
    wait "$PERF_PID" 2>/dev/null || true
    PERF_EXIT=$?
    PERF_PID=""
    echo "[WARN]  perf record failed to start (exit code: $PERF_EXIT)"
    echo "[WARN]  Likely causes:"
    echo "[WARN]    - perf_event_paranoid too high: check /proc/sys/kernel/perf_event_paranoid"
    echo "[WARN]    - Try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid"
    echo "[WARN]    - Or: PERF_SUDO=1 PERF_RECORD=1 ... ./bench_pure_select.sh"
    echo "[WARN]  Continuing without perf — no perf.data / flamegraph will be generated."
    rm -f "${RESULT_DIR}/perf.data"
  fi
fi

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Phase 4 — Benchmark Queries                                                 ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info ""
info "=== Phase 4: Benchmark ==="
info ""

# Determine how many runs: fewer for throughput tests, more for latency
RUNS_LAT=$(( DURATION < 10 ? 100 : 500 ))
RUNS_TP=$(( DURATION < 10 ? 10  : DURATION ))

# ──────────────────────────────────────────────────────────────────────────
# Category A: Full Table Scan (exercises vectorized scan + PushbackBatchTail)
# ──────────────────────────────────────────────────────────────────────────
info "--- A: Full Table Scan ---"
bench_query       "A1_full_scan_count"        "SELECT COUNT(*) FROM t_wide"                           "$RUNS_LAT"
bench_throughput  "A1_full_scan_count_tp"     "SELECT COUNT(*) FROM t_wide"

bench_query       "A2_full_scan_sum"          "SELECT SUM(val_int) FROM t_wide"                        "$RUNS_LAT"
bench_throughput  "A2_full_scan_sum_tp"       "SELECT SUM(val_int) FROM t_wide"

bench_query       "A3_full_scan_multi_agg"    "SELECT COUNT(*), SUM(val_int), AVG(val_int), MIN(val_int), MAX(val_int) FROM t_wide" "$RUNS_LAT"

bench_query       "A4_narrow_table_scan"      "SELECT COUNT(*), SUM(val_a), SUM(val_b) FROM t_narrow"  "$RUNS_LAT"

# ──────────────────────────────────────────────────────────────────────────
# Category B: Point Query (high selectivity — exercises IMCU pruning)
# ──────────────────────────────────────────────────────────────────────────
info "--- B: Point Query (high selectivity) ---"

# Build a random ID list once
info "  Preparing point-query ID pool ..."
POINT_IDS=$(mysql_exec_db "SELECT id FROM t_wide ORDER BY RAND() LIMIT 200;")
ID_ARRAY=($POINT_IDS)
NUM_IDS=${#ID_ARRAY[@]}
info "  Got ${NUM_IDS} random IDs"

# Run individual point queries
for ((j=0; j<20 && j<NUM_IDS; j++)); do
  rid="${ID_ARRAY[$j]}"
  bench_query "B1_point_by_pk_${j}" "SELECT * FROM t_wide WHERE id = ${rid}" "$(( RUNS_LAT / 20 ))"
done

info "--- B2: Point Query Throughput (random PK, tight loop) ---"
info "  [B2_point_pk_tp] running ${RUNS_TP}s ..."
COUNT=0; START=$(date +%s%N); END=$(( START + RUNS_TP * 1000000000 ))
while [[ $(date +%s%N) -lt $END ]]; do
  rid="${ID_ARRAY[$(( RANDOM % NUM_IDS ))]}"
  mysql_exec_db "SELECT * FROM t_wide WHERE id = ${rid}" > /dev/null 2>&1 || true
  (( ++COUNT ))
done
ELAPSED_MS=$(( ($(date +%s%N) - START) / 1000000 ))
QPS=$(( COUNT * 1000 / (ELAPSED_MS > 0 ? ELAPSED_MS : 1) ))
printf "%-45s  qps=%-8d  elapsed=%dms\n" "B2_point_pk_tp" "$QPS" "$ELAPSED_MS" | tee -a "${SUMMARY_FILE}"

# ──────────────────────────────────────────────────────────────────────────
# Category C: Range Scan (varying selectivity — exercises IMCU zone map)
# ──────────────────────────────────────────────────────────────────────────
info "--- C: Range Scan ---"

# Low selectivity (narrow range)
bench_query       "C1_range_narrow"       "SELECT COUNT(*) FROM t_wide WHERE id BETWEEN 1000 AND 2000"                         "$RUNS_LAT"
# Medium selectivity
bench_query       "C2_range_medium"       "SELECT COUNT(*) FROM t_wide WHERE id BETWEEN 1 AND ${NUM_ROWS}/2"                  "$RUNS_LAT"
# High selectivity (almost full scan)
bench_query       "C3_range_wide"          "SELECT COUNT(*), SUM(val_int) FROM t_wide WHERE id > 0"                            "$RUNS_LAT"
# Range + predicate on non-indexed column
bench_query       "C4_range_with_filter"   "SELECT COUNT(*) FROM t_wide WHERE id BETWEEN 1 AND 100000 AND flag = 1"           "$RUNS_LAT"
# Date range
bench_query       "C5_date_range"          "SELECT COUNT(*) FROM t_wide WHERE ts BETWEEN '2022-01-01' AND '2022-12-31'"       "$RUNS_LAT"

# ──────────────────────────────────────────────────────────────────────────
# Category D: GROUP BY Aggregation (exercises ProcessGroupBatchVectorized)
# ──────────────────────────────────────────────────────────────────────────
info "--- D: GROUP BY Aggregation ---"

# Low cardinality GROUP BY (10 groups) — boundary detection rare
bench_query       "D1_grp_low_card"         "SELECT grp_low, COUNT(*), SUM(val_int), AVG(val_int) FROM t_wide GROUP BY grp_low"                  "$RUNS_LAT"
bench_throughput  "D1_grp_low_card_tp"      "SELECT grp_low, COUNT(*), SUM(val_int) FROM t_wide GROUP BY grp_low"

# Medium cardinality GROUP BY (~1K groups) — boundary detection more frequent
bench_query       "D2_grp_mid_card"         "SELECT grp_mid, COUNT(*), SUM(val_int) FROM t_wide GROUP BY grp_mid"                              "$RUNS_LAT"

# High cardinality GROUP BY (near-unique) — boundary detection EVERY row → biggest test for vectorized boundary
bench_query       "D3_grp_high_card"        "SELECT grp_high, COUNT(*) FROM t_wide GROUP BY grp_high"                                       "$RUNS_LAT"

# GROUP BY with multiple aggregates
bench_query       "D4_multi_agg_grp"        "SELECT grp_low, COUNT(*), SUM(val_int), AVG(val_dec), MIN(val_int), MAX(val_int) FROM t_wide GROUP BY grp_low" "$RUNS_LAT"

# HAVING clause
bench_query       "D5_having"               "SELECT grp_low, COUNT(*) AS cnt FROM t_wide GROUP BY grp_low HAVING cnt > ${NUM_ROWS}/20"    "$RUNS_LAT"

# ──────────────────────────────────────────────────────────────────────────
# Category E: Single-Row Aggregates (no GROUP BY — fastest path)
# ──────────────────────────────────────────────────────────────────────────
info "--- E: Scalar Aggregates (no GROUP BY) ---"
bench_query       "E1_scalar_count"         "SELECT COUNT(*) FROM t_wide"                                             "$RUNS_LAT"
bench_query       "E2_scalar_sum"           "SELECT SUM(val_int) FROM t_wide"                                         "$RUNS_LAT"
bench_query       "E3_scalar_multi"         "SELECT COUNT(*), SUM(val_int), AVG(val_int), MIN(val_int), MAX(val_int) FROM t_wide" "$RUNS_LAT"
bench_throughput  "E3_scalar_multi_tp"      "SELECT COUNT(*), SUM(val_int), AVG(val_int), MIN(val_int), MAX(val_int) FROM t_wide"

# ──────────────────────────────────────────────────────────────────────────
# Category F: Mixed / Complex Queries
# ──────────────────────────────────────────────────────────────────────────
info "--- F: Complex Queries ---"

# JOIN (self-join on low cardinality)
bench_query       "F1_self_join"            "SELECT a.grp_low, COUNT(*) FROM t_wide a JOIN t_wide b ON a.grp_low = b.grp_low WHERE a.id < 100000 GROUP BY a.grp_low" "$RUNS_LAT"

# Subquery
bench_query       "F2_subquery"             "SELECT COUNT(*) FROM t_wide WHERE val_int > (SELECT AVG(val_int) FROM t_wide)" "$RUNS_LAT"

# LIMIT (short-circuit scan — tests READING_FIRST_ROW / lookahead paths)
bench_query       "F3_limit_small"          "SELECT * FROM t_wide WHERE id > 0 LIMIT 10"                               "$RUNS_LAT"
bench_query       "F4_limit_medium"         "SELECT * FROM t_wide WHERE id > 0 LIMIT 1000"                             "$RUNS_LAT"
bench_query       "F5_limit_with_offset"    "SELECT * FROM t_wide WHERE id > 0 LIMIT 100 OFFSET 50000"                "$RUNS_LAT"

# String column queries
bench_query       "F6_str_eq_lookup"        "SELECT COUNT(*) FROM t_strings WHERE s_short = 'sht_00000001'"           "$RUNS_LAT"
bench_query       "F7_str_prefix"           "SELECT COUNT(*) FROM t_strings WHERE s_short LIKE 'sht_0000%'"           "$RUNS_LAT"

# ──────────────────────────────────────────────────────────────────────────
# Category G: Storage Index / Predicate Push-down tests
# ──────────────────────────────────────────────────────────────────────────
info "--- G: Predicate / Storage Index ---"

# Equality on indexed column (should benefit from zone map / future bloom)
bench_query       "G1_eq_indexed"            "SELECT COUNT(*) FROM t_wide WHERE grp_low = 5"                           "$RUNS_LAT"
bench_query       "G2_eq_non_indexed"        "SELECT COUNT(*) FROM t_wide WHERE val_int = 500000"                       "$RUNS_LAT"

# IN clause
bench_query       "G3_in_list"               "SELECT COUNT(*) FROM t_wide WHERE grp_low IN (1, 3, 5, 7, 9)"            "$RUNS_LAT"

# BETWEEN on indexed column
bench_query       "G4_between_indexed"       "SELECT COUNT(*), SUM(val_int) FROM t_wide WHERE grp_mid BETWEEN 100 AND 200" "$RUNS_LAT"

# Composite predicates (AND)
bench_query       "G5_and_predicates"        "SELECT COUNT(*) FROM t_wide WHERE grp_low = 3 AND flag = 1"              "$RUNS_LAT"

# ──────────────────────────────────────────────────────────────────────────
# Category H: DDL-free change propagation latency micro-benchmark
# ──────────────────────────────────────────────────────────────────────────
info "--- H: Write-then-Read Latency (propagation delay) ---"

INSERT_ID=$(mysql_exec_db "SELECT MAX(id) + 1 FROM t_wide;")
info "  Insert ID base: ${INSERT_ID}"

WRITE_THEN_READ_COUNT=50
WRITE_LATENCIES=()
for ((k=0; k<WRITE_THEN_READ_COUNT; k++)); do
  START_NS=$(date +%s%N)
  mysql_exec_db "INSERT INTO t_wide (grp_low, grp_mid, grp_high, val_int, val_dec, val_str, ts, flag) VALUES (1, 100, ${INSERT_ID}, ${INSERT_ID}, ${INSERT_ID}.0, 'bench_prop_test', NOW(), 0);" > /dev/null
  # Immediately try to read it back via Rapid
  mysql_exec_db "SELECT COUNT(*) FROM t_wide WHERE id = ${INSERT_ID};" > /dev/null 2>&1 || true
  END_NS=$(date +%s%N)
  WRITE_LATENCIES+=($(( (END_NS - START_NS) / 1000000 )))
  (( ++INSERT_ID ))
done
# Stats
printf '%d\n' "${WRITE_LATENCIES[@]}" | "$AWK" '
  { a[++n]=$1; sum+=$1; if(n==1||$1<min) min=$1; if(n==1||$1>max) max=$1 }
  END {
    for (i=1; i<=n; i++) for (j=i+1; j<=n; j++) if (a[i]>a[j]) { tmp=a[i]; a[i]=a[j]; a[j]=tmp }
    printf "%-45s  runs=%-6d  avg=%-8.2f  min=%-8.2f  p50=%-8.2f  p95=%-8.2f  p99=%-8.2f  max=%-8.2f ms\n",
      "H1_write_then_read", n, sum/n, min, a[int(n*0.50)+1], a[int(n*0.95)+1], a[int(n*0.99)+1], max
  }' | tee -a "${SUMMARY_FILE}"

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  Phase 5 — Cleanup                                                           ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

info ""
info "=== Phase 5: Summary ==="
echo "============================================================"
cat "$SUMMARY_FILE"
echo "============================================================"
info ""
info "Results saved to: $RESULT_DIR"
info "  summary  : ${SUMMARY_FILE}"
info "  raw      : ${RAW_CSV}"

# ─── Stop perf ────────────────────────────────────────────────────────────────
if [[ -n "$PERF_PID" ]]; then
  info "Stopping perf record (PID $PERF_PID) ..."

  # Send SIGTERM first (perf catches it and writes the header properly).
  # SIGINT (Ctrl-C) does not always flush the final header on older kernels
  # or when perf is run under sudo, leading to "data_size field is 0".
  kill -TERM "$PERF_PID" 2>/dev/null || true

  # Wait up to 10s for graceful exit with backoff, then force-kill.
  waited=0
  while kill -0 "$PERF_PID" 2>/dev/null && [[ $waited -lt 600 ]]; do
    sleep 0.1
    (( ++waited ))
    # Every 5s print a progress dot so the user knows we're waiting
    [[ $((waited % 50)) -eq 0 ]] && printf "." >&2
  done
  [[ $waited -ge 50 ]] && echo "" >&2
  if kill -0 "$PERF_PID" 2>/dev/null; then
    echo "[WARN]  perf did not exit after ${waited}0ms — sending SIGKILL" >&2
    kill -KILL "$PERF_PID" 2>/dev/null || true
    sleep 1
  fi
  wait "$PERF_PID" 2>/dev/null || true

  # ── Validate perf.data ──────────────────────────────────────────────────
  perf_data="${RESULT_DIR}/perf.data"
  perf_fixed="${RESULT_DIR}/perf_fixed.data"

  # 1. Does the file exist and have non-zero size?
  perf_size=0
  perf_size_human=""
  if [[ -f "$perf_data" ]]; then
    perf_size=$(stat -c%s "$perf_data" 2>/dev/null || stat -f%z "$perf_data" 2>/dev/null || echo 0)
    if command -v numfmt &>/dev/null; then
      perf_size_human=$(numfmt --to=iec --suffix=B "$perf_size")
    else
      perf_size_human="${perf_size} bytes"
    fi
  fi

  if [[ "$perf_size" -eq 0 ]]; then
    echo "[ERROR] perf.data is 0 bytes — no samples were recorded." >&2
    echo "[ERROR] Likely causes:" >&2
    echo "[ERROR]   - mysqld PID was wrong (actual PID: ${MYSQLD_PID:-unknown})" >&2
    echo "[ERROR]   - perf_event_paranoid too restrictive: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)" >&2
    echo "[ERROR]   - perf does not have permission to trace the target process" >&2
    rm -f "$perf_data"
  elif [[ "$perf_size" -lt 4096 ]]; then
    echo "[WARN]  perf.data is only ${perf_size_human} — too few samples. Flamegraph will be sparse." >&2
  elif [[ "$perf_size" -gt 4294967296 ]]; then  # > 4 GiB
    echo "[WARN]  perf.data is ${perf_size_human} — very large." >&2
    echo "[WARN]  This is typical for --call-graph dwarf with many events over long runs." >&2
    echo "[WARN]  To reduce size next time:" >&2
    echo "[WARN]    - Set PERF_MAX_SIZE=4G to cap at 4 GiB" >&2
    echo "[WARN]    - Reduce PERF_FREQ (currently ${PERF_FREQ} Hz)" >&2
    echo "[WARN]    - Use fewer PERF_EVENTS (currently ${PERF_EVENTS})" >&2
    echo "[WARN]    - Switch to --call-graph fp (smaller but needs -fno-omit-frame-pointer)" >&2
  else
    info "perf.data: ${perf_size_human}"
  fi

  # 2. Check for stale file handles (lsof — only if available)
  if [[ -f "$perf_data" ]] && command -v lsof &>/dev/null; then
    open_handles=$(lsof "$perf_data" 2>/dev/null | wc -l || echo 0)
    if [[ "$open_handles" -gt 1 ]]; then
      echo "[WARN]  perf.data still has ${open_handles} open file handles — data may be incomplete" >&2
      echo "[WARN]  Waiting 2s for handles to close ..." >&2
      sleep 2
      open_handles=$(lsof "$perf_data" 2>/dev/null | wc -l || echo 0)
      [[ "$open_handles" -gt 1 ]] && echo "[WARN]  ${open_handles} handles still open — continuing anyway" >&2
    fi
  fi

  # 3. Quick magic-number sanity check
  if [[ -f "$perf_data" ]] && [[ "$perf_size" -gt 16 ]]; then
    magic=$(xxd -l 8 -p "$perf_data" 2>/dev/null || echo "")
    # perf.data v2 magic: "PERFILE2" = 50455246494c4532
    if [[ -n "$magic" ]] && [[ "$magic" != "50455246494c4532" ]]; then
      echo "[WARN]  perf.data does not start with expected 'PERFILE2' magic (got: ${magic})" >&2
      echo "[WARN]  File may be corrupted or from an incompatible perf version." >&2
    fi
  fi

  if [[ -f "$perf_data" ]] && [[ "$perf_size" -gt 4096 ]]; then
    if "$PERF_CMD" inject -i "$perf_data" -o "$perf_fixed" 2>/dev/null; then
      fixed_size=$(stat -c%s "$perf_fixed" 2>/dev/null || echo 0)
      size_diff=$((perf_size - fixed_size))
      # If fixed file is within 5% of original, accept it
      if [[ "$fixed_size" -gt 4096 ]] && [[ ${size_diff#-} -lt $((perf_size / 20)) ]]; then
        mv "$perf_fixed" "$perf_data"
        perf_size="$fixed_size"
        info "  perf.data header fixed via perf inject (${perf_size_human})"
      else
        rm -f "$perf_fixed"
        echo "[WARN]  perf inject produced suspicious output (${fixed_size} vs ${perf_size}) — keeping original" >&2
      fi
    fi
  fi

  # Generate flamegraph if tools are available
  if [[ -f "$perf_data" ]] && [[ "$perf_size" -gt 4096 ]]; then
    if command -v stackcollapse-perf.pl &>/dev/null && command -v flamegraph.pl &>/dev/null; then
      info "Generating flamegraph (input: ~${perf_size_human}) ..."
      if "$PERF_CMD" script -i "$perf_data" 2>/dev/null \
        | stackcollapse-perf.pl \
        | flamegraph.pl --title "ShannonBase Pure SELECT Benchmark" \
        > "${RESULT_DIR}/flamegraph.svg" 2>/dev/null; then
        fg_size=$(stat -c%s "${RESULT_DIR}/flamegraph.svg" 2>/dev/null || echo 0)
        info "Flamegraph: ${RESULT_DIR}/flamegraph.svg ($(numfmt --to=iec --suffix=B ${fg_size} 2>/dev/null || echo ${fg_size} bytes))"
      else
        echo "[WARN]  flamegraph generation failed" >&2
        echo "[WARN]  Try manually: $PERF_CMD report -i ${perf_data} --stdio | head" >&2
      fi
    else
      info "FlameGraph tools not found. To install:"
      info "  git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph"
      info "  export PATH=\$PATH:~/FlameGraph"
      info "  Then: $PERF_CMD script -i ${perf_data} | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg"
    fi
  fi
fi

# ─── Optional: drop test database ─────────────────────────────────────────────
if [[ "${KEEP_DATA:-0}" != "1" ]]; then
  info "Dropping test database ${TEST_DB} ..."
  mysql_exec "DROP DATABASE IF EXISTS ${TEST_DB};" || true
else
  info "KEEP_DATA=1 — test database ${TEST_DB} preserved."
fi

info ""
info "Done."
