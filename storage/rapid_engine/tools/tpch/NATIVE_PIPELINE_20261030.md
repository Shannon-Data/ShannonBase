# Native batch pipeline verification — 2026-10-30

The feature branch is based on main `efcf25912fb592263d7f040fd59ab9299e90bfb9`.
Related upstream issues: [#980](https://github.com/Shannon-Data/ShannonBase/issues/980),
[#981](https://github.com/Shannon-Data/ShannonBase/issues/981),
[#985](https://github.com/Shannon-Data/ShannonBase/issues/985),
[#986](https://github.com/Shannon-Data/ShannonBase/issues/986),
[#987](https://github.com/Shannon-Data/ShannonBase/issues/987).
These references describe overlapping work; they are not claims that every acceptance
criterion in those issues is complete.

## Iterator review

- Nested loop, LIMIT and STREAM use a shared record adapter for SQL row output,
  dictionary decoding and pushback. Batch cores own their execution algorithms;
  unsupported layouts use the native MySQL fallback. Runtime capability checks follow Init.
- Sort shares its output cursor through NextPayload; materialization shares its column
  spool cursor through EnsureBlock; hash join shares its build/probe/merge kernels.
  Both row and batch APIs remain required at SQL boundaries.
- Hash join now replays pushed output tails before advancing the scalar cursor. Tests
  cover both in-memory and spilled joins. Pending build batches survive a spill transition.
- Filter retains scalar expression fallbacks. Sort, materialize and hash join consume
  physical chunks plus selection indexes without forcing filter compaction first.
- Aggregate and window output still uses the SQL row interface. This is not a claim of
  an entirely columnar end-to-end executor.
- Packed DECIMAL decoding removes per-value my_decimal conversion for exact fixed-point
  inputs with precision at most 18. It does not implement general RLE/bit-packed kernels
  or vectorize every decimal expression. Residual filters still do not provide full
  storage-level late projection.

## Correctness

Debug build: WITH_DEBUG=ON, -O2 -g, OPTIMIZE_DEBUG_BUILDS=OFF, LTO disabled.
All 18 secondary_engine unit-test targets passed: **390 cases**.
The tests include mixed Read/ReadBatch, pushback, offset boundaries, native fallback,
reinitialization, dictionary strings, NULLs, selections and spill transitions.

Full secondary_engine MTR: 315 executed; 313 passed initially. The two initial failures
were shannon_query_aux_memory (error text moved from LIMIT to the shared record adapter;
expected text updated, two independent repetitions passed) and shannon_optimizer_rules
(EXPLAIN cost numbers differed on its first run; two automatic retries and three independent
repetitions passed without changing its assertion). All executed cases therefore have a
passing verification, but the original full-run exit status was nonzero. shannon_embedding
is excluded as requested because its local model environment differs from CI.

Logs on this host: /tmp/shannon-980-unified-unittests.log,
/tmp/shannon-980-unified-mtr.log, /tmp/shannon-980-unified-aux-recheck.log,
/tmp/shannon-980-unified-rules-recheck.log.

## TPC-H comparison

Both binaries use Release (-O3 -DNDEBUG), WITH_DEBUG=OFF and LTO. The same SF1
InnoDB data (6,001,215 LINEITEM rows), indexes and statistics are reused. Each arm loads
all eight secondary tables, runs one warmup and three measured rounds; query order
alternates within each arm. Main runs before candidate: these are sequential arms,
not interleaved paired runs. InnoDB references precede the main warmup. Buffer pool is
2G (server-rounded size retained in variables.tsv), Rapid cap 4G, embedding and binlog
off, hypergraph optimizer on and secondary engine FORCED for Rapid queries.
The client consumes complete results; comparisons use sorted full-row multiset hashes.
No build or MTR ran concurrently with timed queries.

All 22 queries in all warmup/measured rounds match the InnoDB reference exactly.
Summed per-query medians are **93.477690 s main / 87.612935 s candidate**, a **6.27%**
observed reduction. Q18 is effectively unchanged; Q19 is 6.88% slower by median.
The sample ranges overlap for several queries, including Q19. Three sequential samples
are insufficient to establish small gains/regressions independently of run-order noise.
This is a workload observation, not a completed per-query non-regression gate.

Importantly, REF/STREAM/Sort batch-row counter deltas are zero in this default-plan
sweep, and several plans retain native row paths. Existing aggregate/materialize batch
counters are nonzero for some queries (raw counters are retained per sample). The new
transport is exercised by direct contract tests and MTR, but this run does not demonstrate
that all new paths are selected by default or attribute the observed speedups to them.
EXPLAIN ANALYZE is rejected by the secondary engine; retained TREE plans are estimates,
not per-node actual timings. No complete row-island-removal claim is made.

| Query | Main median s | Candidate median s | Main/candidate | Results |
|---|---:|---:|---:|---|
| Q1 | 15.224014 | 14.802622 | 1.028x | identical |
| Q2 | 6.351424 | 5.390428 | 1.178x | identical |
| Q3 | 2.196836 | 2.047343 | 1.073x | identical |
| Q4 | 0.438380 | 0.427291 | 1.026x | identical |
| Q5 | 1.314759 | 1.250128 | 1.052x | identical |
| Q6 | 4.552791 | 4.038816 | 1.127x | identical |
| Q7 | 2.509741 | 1.731737 | 1.449x | identical |
| Q8 | 3.393432 | 2.611641 | 1.299x | identical |
| Q9 | 3.919794 | 3.874975 | 1.012x | identical |
| Q10 | 1.757473 | 1.752207 | 1.003x | identical |
| Q11 | 0.450931 | 0.425454 | 1.060x | identical |
| Q12 | 1.266343 | 1.261312 | 1.004x | identical |
| Q13 | 4.491788 | 4.315033 | 1.041x | identical |
| Q14 | 1.181832 | 1.135044 | 1.041x | identical |
| Q15 | 2.162921 | 2.072815 | 1.043x | identical |
| Q16 | 0.784214 | 0.767424 | 1.022x | identical |
| Q17 | 13.686721 | 13.000957 | 1.053x | identical |
| Q18 | 6.093483 | 6.101801 | 0.999x | identical |
| Q19 | 0.250463 | 0.267692 | 0.936x | identical |
| Q20 | 4.650057 | 4.509967 | 1.031x | identical |
| Q21 | 15.603310 | 14.932276 | 1.045x | identical |
| Q22 | 1.196983 | 0.895972 | 1.336x | identical |

### Raw measured samples

| Query | Main measured samples s | Candidate measured samples s |
|---|---|---|
| Q1 | 15.444240, 14.825418, 15.224014 | 14.253208, 14.802622, 15.094837 |
| Q2 | 7.150833, 5.346862, 6.351424 | 5.390428, 5.531726, 5.334929 |
| Q3 | 2.196836, 2.030358, 2.221703 | 2.047343, 2.173284, 2.002266 |
| Q4 | 0.438380, 0.404772, 0.453924 | 0.396293, 0.456303, 0.427291 |
| Q5 | 1.314759, 1.227682, 1.447855 | 1.235897, 1.250128, 1.268501 |
| Q6 | 4.748001, 4.054667, 4.552791 | 4.038816, 4.259951, 3.855812 |
| Q7 | 2.661479, 1.663860, 2.509741 | 1.832079, 1.731737, 1.630992 |
| Q8 | 3.393432, 2.594959, 3.566859 | 2.611641, 2.572833, 2.808116 |
| Q9 | 3.919794, 3.853960, 5.491548 | 3.874975, 3.837444, 3.896264 |
| Q10 | 1.757473, 1.670190, 2.197926 | 1.789978, 1.752207, 1.720831 |
| Q11 | 0.450931, 0.432397, 0.599522 | 0.425454, 0.418374, 0.443133 |
| Q12 | 1.234895, 1.316994, 1.266343 | 1.228224, 1.261312, 1.349069 |
| Q13 | 4.269758, 4.491788, 4.972762 | 4.315033, 4.310174, 4.392863 |
| Q14 | 1.203471, 1.129318, 1.181832 | 1.093762, 1.135044, 1.143472 |
| Q15 | 2.226534, 2.162921, 2.068079 | 1.986458, 2.112792, 2.072815 |
| Q16 | 0.808436, 0.703559, 0.784214 | 0.793977, 0.767424, 0.742225 |
| Q17 | 14.141400, 13.239237, 13.686721 | 13.000957, 13.515638, 12.203146 |
| Q18 | 6.111174, 5.604698, 6.093483 | 5.542974, 6.921971, 6.101801 |
| Q19 | 0.250463, 0.282135, 0.229312 | 0.262195, 0.284420, 0.267692 |
| Q20 | 4.650057, 4.607331, 4.732300 | 4.509967, 4.820843, 4.507491 |
| Q21 | 15.499792, 15.603310, 18.138880 | 14.932276, 15.784367, 14.710563 |
| Q22 | 1.054752, 1.196983, 1.273390 | 0.948662, 0.895972, 0.837121 |

SF0.5 and a reverse-order/paired confirmation run were not performed; they remain
coverage gaps for upstream issue acceptance. Raw timings, results, counter deltas,
plans, runtime samples and binary/configuration provenance are retained under
results/native_pipeline_20261030/formal and reproduce.

Release binary SHA-256:
- main: cdfcd2d029f3178de1fbd59e30f8f82897f0cbd4af74fafb33b24ed41ae3c68a
- candidate: 731e92dbe0bd84ad7c5e738929eeeaf862511b765bd85957556e000080d58239

Follow-up: [Q19 investigation](Q19_INVESTIGATION_20261030.md) repeats Q19 with
independent full-load runs, sampling and a scalar-index ablation. The original
6.88% remains a point estimate, not an established causal regression; a smaller
regression is not ruled out.
