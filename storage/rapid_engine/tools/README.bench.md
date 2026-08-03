# ShannonBase Rapid Engine — Benchmark Scripts

Two benchmark scripts covering different performance bottleneck categories:

| Script | Purpose | Flamegraph Hotspots |
|--------|---------|---------------------|
| `bench_pure_select.sh` | Pure query path (tables pre-loaded) | Vectorized scan/agg, IMCU pruning, SIMD aggregates |
| `bench_ddl_cycle.sh` | DDL lifecycle (CREATE→LOAD→UNLOAD→DROP) | Embedding/ONNX, `load_indexes_caches`, Table construction/destruction |

---

## Quick Start

### 1. Pure SELECT Benchmark (query-path flamegraph)

```bash
# Small smoke test (100K rows, 10s, single thread, no perf)
SCALE=100K DURATION=10 THREADS=1 ./tools/bench_pure_select.sh

# Small scale + flamegraph collection
SCALE=100K DURATION=10 THREADS=1 PERF_RECORD=1 MYSQLD_PID=$(pgrep mysqld) ./tools/bench_pure_select.sh

# Production-grade benchmark (10M rows, 60s, with perf)
SCALE=10M DURATION=60 PERF_RECORD=1 MYSQLD_PID=$(pgrep mysqld) ./tools/bench_pure_select.sh

# Saturate bandwidth on large-memory machines
SCALE=50M DURATION=120 THREADS=8 PERF_RECORD=1 ./tools/bench_pure_select.sh
```

**What this script tests:**
- **Category A — Full Table Scan**: `SELECT COUNT(*)/SUM/AVG/MIN/MAX`, measures vectorized scan + SIMD aggregate throughput
- **Category B — Point Query**: `WHERE id = ?` (high selectivity), measures IMCU pruning effectiveness & PK lookup latency
- **Category C — Range Scan**: `WHERE id BETWEEN` at varying widths, measures zone map pruning hit rate
- **Category D — GROUP BY Aggregation**: Low/medium/high cardinality GROUP BY, measures `ProcessGroupBatchVectorized` boundary detection overhead
- **Category E — Scalar Aggregates**: Fastest path without GROUP BY
- **Category F — Complex Queries**: JOIN, subquery, LIMIT, string operations
- **Category G — Predicate Pushdown**: Equality/IN/BETWEEN on different column types, pruning performance
- **Category H — Write-then-Read Latency**: INSERT + immediate SELECT, measures change propagation delay

### 2. DDL Lifecycle Benchmark (reproduce the DDL flamegraph)

```bash
# 50 cycles, 5 tables per cycle, 100K rows per table
CYCLES=50 TABLES_PER_CYCLE=5 ROWS_PER_TABLE=100000 ./tools/bench_ddl_cycle.sh

# With perf
PERF_RECORD=1 MYSQLD_PID=$(pgrep mysqld) CYCLES=100 ./tools/bench_ddl_cycle.sh
```

**What this script tests:**
- Full round-trip: CREATE TABLE → INSERT → `SECONDARY_LOAD` → `SECONDARY_UNLOAD` → DROP TABLE
- Per-phase timing recorded separately (`create` / `insert` / `load` / `unload` / `drop`)
- Corresponding flamegraph hotspots: embedding ONNX inference, `load_indexes_caches_impl`, Table construction/destruction

---

## Environment Variables Reference

### Connection Settings

| Variable | Default | Description |
|----------|---------|-------------|
| `MYSQL_CLIENT` | `mysql` | MySQL client binary path |
| `MYSQL_HOST` | `127.0.0.1` | Server address |
| `MYSQL_PORT` | `3306` | Port |
| `MYSQL_USER` | `root` | Username |
| `MYSQL_PASS` | (empty) | Password; interactive prompt if empty and terminal attached |
| `MYSQL_SOCKET` | (empty) | Unix socket path |

### bench_pure_select.sh Specific

| Variable | Default | Description |
|----------|---------|-------------|
| `SCALE` | `1M` | Main table row count; supports K/M/G suffixes |
| `DURATION` | `30` | Benchmark duration per query category (seconds) |
| `THREADS` | `1` | Concurrent threads (reserved) |
| `PERF_MAX_SIZE` | `2G` | Hard cap for `perf.data` size |
| `PERF_CALL_GRAPH` | `fp` | Call-graph method: `fp` (small, needs `-fno-omit-frame-pointer`) or `dwarf` (accurate, large) |

### bench_ddl_cycle.sh Specific

| Variable | Default | Description |
|----------|---------|-------------|
| `CYCLES` | `50` | Number of CREATE→LOAD→UNLOAD→DROP cycles |
| `TABLES_PER_CYCLE` | `5` | Tables created per cycle |
| `ROWS_PER_TABLE` | `100000` | Rows per table |
| `CYCLE_PAUSE` | `0.5` | Pause between cycles (seconds) |

### Perf Collection (shared by both scripts)

| Variable | Default | Description |
|----------|---------|-------------|
| `PERF_RECORD` | `0` | Set to `1` to enable perf collection |
| `PERF_EVENTS` | `cycles:u,instructions:u,branches:u,...` | Perf event list |
| `PERF_FREQ` | `997` | Sampling frequency (Hz) |
| `PERF_SUDO` | `0` | Set to `1` to run perf via sudo |
| `MYSQLD_PID` | (auto-detect) | mysqld process PID |

### Other

| Variable | Default | Description |
|----------|---------|-------------|
| `KEEP_DATA` | `0` | Set to `1` to preserve the test database |
| `RESULT_DIR` | `results/bench_<timestamp>` | Output directory |

---

## Output Artifacts

> **Note**: `perf.data` and `flamegraph.svg` are only generated when `PERF_RECORD=1`.
> Without this flag, only `summary.txt` and `raw.csv` are produced.

```
results/bench_20260803_143022/
├── summary.txt          # Latency / throughput summary
├── raw.csv              # Per-query raw timings (label,latency_ms)
├── perf.data            # perf record raw data (when PERF_RECORD=1)
└── flamegraph.svg       # Flamegraph (requires FlameGraph tools installed)
```

---

## Typical Workflows

### Scenario: I want to identify query-path performance bottlenecks

```bash
# Step 1: Run pure SELECT benchmark with flamegraph collection
SCALE=10M DURATION=60 PERF_RECORD=1 MYSQLD_PID=$(pgrep mysqld) ./tools/bench_pure_select.sh

# Step 2: Open the generated flamegraph.svg and examine these hotspots:
#   - Vectorized scan (VectorizedTableScanIterator) percentage
#   - Aggregation (ProcessGroupBatchVectorized / ProcessVectorizedAggregates) percentage
#   - IMCU pruning (can_skip_imcu / StorageIndex) percentage
#   - Change propagation (parse_log_func_main) percentage

# Step 3: Check summary.txt latency data — which query category is slowest?
#   - D3_grp_high_card is slow → boundary detection (RestoreGroupKeyField) is the bottleneck → vectorize boundaries
#   - B1_point_by_pk slow but has zone map → equality predicate pruning insufficient → add Bloom Filter
#   - A1/A2 full scan slow → memory bandwidth bottleneck → huge pages / NUMA optimization
```

### Scenario: I want to identify DDL / load-unload performance bottlenecks

```bash
# Use a larger CYCLES value to collect enough samples for the flamegraph
PERF_RECORD=1 MYSQLD_PID=$(pgrep mysqld) CYCLES=100 ./tools/bench_ddl_cycle.sh

# In the flamegraph, focus on:
#   - libonnxruntime → whether embedding inference is still on the DDL critical path
#   - load_indexes_caches_impl percentage
#   - Table/RpdTable ~Table / MemoryPool ~MemoryPool / ART ~ART
```

### Scenario: Before/after optimization comparison

```bash
# Before optimization
SCALE=10M DURATION=60 ./tools/bench_pure_select.sh
mv results/bench_* results/before/

# ... modify code, rebuild, restart ...

# After optimization
SCALE=10M DURATION=60 ./tools/bench_pure_select.sh
mv results/bench_* results/after/

# Compare
diff <(grep "avg=" results/before/*/summary.txt) <(grep "avg=" results/after/*/summary.txt)
```

---

## Notes

1. **Ensure Rapid Engine is enabled**: `bench_pure_select.sh` will skip `SECONDARY_LOAD` failures (without aborting), but queries will fall back to InnoDB and miss the vectorized path. Verify before running:
   ```sql
   SHOW VARIABLES LIKE 'rapid%';
   ```

2. **Keep data size within available memory**: Setting `SCALE` too large causing swap will make latency data meaningless. Keep table size within ~80% of buffer pool.

3. **Warm up before testing**: The script already performs 3 warm-up rounds. For large production buffer pools, consider running additional manual warm-up rounds.

4. **perf requires permissions**: `echo 1 > /proc/sys/kernel/perf_event_paranoid` or use `sudo`.

5. **ASan/Debug builds skew absolute values**: Relative percentages in the flamegraph remain valid, but absolute latency will be inflated. For release builds, use `-DCMAKE_BUILD_TYPE=RelWithDebInfo`.

6. **The script auto-enables Rapid Engine**: Each MySQL connection runs `SET USE_SECONDARY_ENGINE=FORCED;` automatically. CREATE TABLE already includes `SECONDARY_ENGINE=RAPID`. No manual configuration needed.
