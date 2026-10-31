# Q19 regression investigation — 2026-10-30

The public scalar-index refactor is **not established as the cause** of the original
6.88% slowdown. A regression is **not disproven**, either. The repeated experiment
still has a positive candidate/main point estimate, but uncertainty includes zero.
No performance workaround is justified by these measurements, and the per-query
non-regression gate remains open.

## Findings

1. Q19 does not select the new batch NLJ/REF/aggregate pipeline. All six full-load
   executions retain the same operator structure and predicate order: PART scan,
   scalar filtering, native nested-loop/index lookup, scalar decimal aggregate.
   All newly added batch-row counter deltas are zero. Every work check reports
   exactly 14,321 Handler_read_rnd_next operations.
2. The complete result hash is identical in all 360 full-load measured queries.
   No measured full-load query has a major page fault. Reload-time Rapid sampling
   changes the estimated PART filter cardinality (2346/2411/2476), but normalizing
   cost/row annotations produces one identical plan hash across all arms.
3. Restoring only the old scalar index_read body does not produce a stable runtime
   recovery. Candidate and ablation mean latency differ by only 0.05%; their
   median difference and bootstrap interval are reported below, not suppressed.
4. Hardware-count cohorts retire essentially the same instructions: candidate/main
   differs by -0.021%, and candidate/ablation by +0.063%. An increase in executed
   instructions/read work is not observed. This does not exclude code-layout,
   dependency/stall or cache effects with the same instruction count.
5. The sampled index_read inclusive share is approximately 0.86% main, 0.95%
   candidate and 0.68% ablation. These are sampled stack shares, not per-node actual
   elapsed times. Most sampled work remains memory copies, MySQL Item/string IN
   and collation evaluation, dictionary/string restoration, visibility checks and
   column/row transfer. The extraction adds no visible large new hotspot.

## Controlled wall-time experiment

Frozen binaries from the original measurement are used, not the later working-tree
class renames. Each arm independently restarts against the same SF1 datadir, loads
all eight tables in the original order, pins every existing server thread to guest
CPU 2, warms Q19 twenty times and measures sixty fresh-client sessions. New server
threads inherit the affinity. The sequence is candidate → main → ablation →
candidate → ablation → main. There are **two independent server executions per arm**,
not 120 independent restarts. No builds or MTR run concurrently with timed queries.

The original 2G InnoDB/4G Rapid caps, session settings, schema and indexes are reused.
InnoDB is the full-result reference. Persistent statistics are reused; Rapid load
sampling is not frozen, as the plan-estimate differences above demonstrate.
Initial two-table diagnostic runs are retained separately and excluded from all
figures and confidence intervals in this section.

| Block | Binary | Samples | Median ms | Mean ms | Range ms |
|---|---|---:|---:|---:|---:|
| 3 | candidate | 60 | 235.727 | 228.743 | 163.177–297.974 |
| 4 | main | 60 | 228.111 | 226.659 | 155.636–323.575 |
| 5 | ablation | 60 | 238.669 | 234.850 | 160.091–304.991 |
| 6 | candidate | 60 | 243.565 | 237.466 | 166.676–285.720 |
| 7 | ablation | 60 | 232.252 | 231.127 | 169.391–300.857 |
| 8 | main | 60 | 236.453 | 228.140 | 164.042–313.309 |

| Binary | Samples | Pooled median ms | Mean ms |
|---|---:|---:|---:|
| main | 120 | 231.338 | 227.400 |
| candidate | 120 | 242.533 | 233.104 |
| ablation | 120 | 234.360 | 232.989 |

Candidate/main point estimates are **+4.84% median / +2.51% mean**. Candidate/
ablation estimates are **+3.49% median / +0.05% mean**. Thus neither "the original
6.88% is a proven algorithmic regression" nor "there is definitely no regression"
is supported.

An exploratory hierarchical bootstrap resamples server-execution blocks and then
samples within each selected block (10,000 repetitions, seed 20261030):

| Comparison (candidate slower = positive) | Median-ratio 95% interval | Mean-ratio 95% interval |
|---|---:|---:|
| candidate/main | -2.45% to +10.16% | -2.10% to +6.85% |
| candidate/ablation | -3.70% to +6.20% | -4.42% to +4.29% |

Only two independent executions per arm make these intervals exploratory. Repeated
queries within one execution must not be treated as 120 independent environment
samples. The intervals include zero and do not establish the sign of a small change.

## Separate hardware-count cohorts

Root perf stat enables counters only around 100 complete fresh-client Q19 sessions,
then disables them. All outputs match the reference. These counters include the
same connection/session SET handling as well as Q19; they are not isolated per-node
SQL execution counters. The cohorts run **before the wall-time harness applies CPU
2 affinity**, with the server's default CPU set, matching the original sweep's
unrestricted placement. Consequently their time/IPC figures must not be directly
substituted for the pinned wall-time results. The raw records show approximately
1.8–1.9 server CPU migrations per completed session. Every listed counter has 100%
running coverage.

| Cohort | Instructions/query, millions | User cycles/query, millions | Server task-clock/query ms | IPC |
|---|---:|---:|---:|---:|
| main | 856.026 | 791.496 | 228.609 | 1.082 |
| candidate | 855.847 | 818.242 | 226.421 | 1.046 |
| ablation | 855.307 | 759.184 | 212.596 | 1.127 |
| repeat-ablation | 855.208 | 778.544 | 220.485 | 1.098 |

Candidate has 0.96% lower task-clock than main in these cohorts, reversing the
original three-sample wall-time ranking. The same ablation binary changes from
212.596 to 220.485 ms/query (+3.71%), with instructions changing only -0.012%.
Its cycles change +2.55%. This directly demonstrates runtime-state variation without
a source/binary change; it does not identify which host-level state caused it.

Cycles/task-clock ratios also differ (main 3.462, candidate 3.614 user cycles/ns).
They are an effective measured ratio, **not a direct CPU-frequency measurement**:
cycles exclude kernel execution while task-clock includes it, and this host is a
VMware guest. Guest cpufreq data is unavailable. Host frequency/power policy,
scheduling, shared-cache/memory latency and code/data placement are possible
contributors, not individually proven causes. The cache/branch events do not show
an added candidate workload consistent with attributing the original 17 ms solely
to the new scalar index helper.

## What is and is not localized

The earlier root-cause suggestion was too specific. Evidence now localizes the
problem to **an unresolved performance estimate in a noisy, insufficiently paired
measurement**, not a confirmed vectorized-NLJ defect. Default Q19 does not exercise
that new pipeline, and reverting the suspected scalar-index body does not establish
a causal recovery. Current measurements cannot uniquely identify a physical cause
for the original 6.88% point estimate or rule out a smaller genuine regression.

The actionable remaining gate is more independent, counterbalanced runs on an
exclusive host/physical core with controlled host power policy and cache/load
protocol, including the original mixed-query sweep. If a sign-consistent difference
persists, investigate frontend/data-stall events and binary/code/data layout with
matched builds. No query-number-specific guard or speculative executor rollback is
introduced on inconclusive evidence.

Q19's continuing row-bound cost is a different, directly observed issue: native
consumer boundaries keep predicate/string restoration and scalar decimal expression
work in place. Merely implementing BatchReadable does not cause the planner to select
it. That coverage problem belongs to the existing #980/#985/#986/#987 work; this
investigation does not mark those issues complete.

## Ablation and provenance

The ablation restores the original scalar RapidCursor::index_read body from main
while retaining candidate position_index/index_seek for batch interfaces. It is a
separate Release/LTO build of the candidate source with that single-function change.
Recompilation can itself change layout, so the experiment tests the source restoration
plus its generated code, not an in-place single-instruction patch.

- main: efcf25912fb592263d7f040fd59ab9299e90bfb9;
  binary SHA-256 cdfcd2d029f3178de1fbd59e30f8f82897f0cbd4af74fafb33b24ed41ae3c68a
- candidate: frozen 7ad9d6ea2 implementation;
  binary SHA-256 731e92dbe0bd84ad7c5e738929eeeaf862511b765bd85957556e000080d58239
- ablation:
  binary SHA-256 e65fd02f7c5904df4c5bd4a5bca1044175368f8a66015b5dd167c0fc0542314d

Compiler and Release flags match (-O3 -DNDEBUG, WITH_DEBUG=OFF, ENABLE_LTO=ON,
WITH_LTO=OFF). Later VectorizedLookup renaming/declaration separation and HashJoin
constexpr cleanup are intentionally excluded from these frozen binaries.

The temporary baseline build directory was reused for the ablation. Frozen copies
of all three measured executables are retained in the artifact binaries/ directory;
use their hashes rather than assuming the old temporary build path is still main.

Artifacts on this host: `results/q19_investigation_20261030/` contains all 360 full-load
raw timing outputs, plans/estimates, work checks, server settings, result hashes,
raw perf data/reports, CSV counters, ablation patch, scripts and bootstrap output.
The initial partial-load diagnostics are retained as excluded diagnostics. Scripts
are an audit trail of this run and require fresh directories/path adjustments for
re-execution; they are not a packaged benchmark service. No password is stored.

## Code organization verification

- VectorizedLookup declarations are in record_iterator.h; definitions in .cpp.
- The empty VectorizedRefIterator marker is removed; lookup output uses the shared
  VectorizedRecordIterator adapter.
- HashJoin cancellation masks and spill bit offsets/widths use named constexpr values.
- Latest code: all 18 secondary-engine unit targets / 390 cases pass, and the lookup,
  batch pipeline and vectorized NLJ MTR smoke run passes (4 including shutdown).
