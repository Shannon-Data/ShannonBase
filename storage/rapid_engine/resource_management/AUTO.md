# Automatic resource isolation

Tracking: [#935](https://github.com/Shannon-Data/ShannonBase/issues/935), building on [#932](https://github.com/Shannon-Data/ShannonBase/issues/932).

Enable AUTO with one server option:

```ini
[mysqld]
rapid-resource-isolation=AUTO
```

For a running server, unload its Rapid tables first, then run:

```sql
SET GLOBAL rapid_resource_isolation = AUTO;
SHOW GLOBAL STATUS LIKE 'rapid_resource_status';
```

Applications keep their existing SQL. Users do not create resource groups or bind threads. Setup runs after native MySQL resource groups are initialized and repeats after restart. AUTO applies to both CAPTURE and COMMITTED_BINLOG.

AUTO inspects the server's effective Linux CPU affinity and physical topology. SMT siblings remain together. At least five physical cores are required: two for ordinary source work, one for AP, one for SYNC, and one for MAINT. Larger systems reserve approximately one third of their cores for Rapid, with separate SYNC and MAINT cores and the remainder for AP. Ordinary source work receives the other cores. Admission limits are derived from that allocation; explicitly configured `rapid_resource_*_limit` values remain upper bounds. Smaller systems use one active operation per class in SOFT mode.

The engine creates five internal, nonpersistent native groups named `__rapid_auto_tp`, `__rapid_auto_ap`, `__rapid_auto_sync`, `__rapid_auto_maint`, and `__rapid_auto_maint_user`. They are protected against concurrent group DDL while AUTO owns them. Source threads keep their native default group and receive the TP affinity automatically; notifications cover new sessions and default-group restoration. AP, propagation, and maintenance use the existing work admission and temporary native binding paths.

`rapid_resource_status` reports the configured mode, effective `STRICT_CPU` or `SOFT` mode, effective admission limits, CPU assignments when native isolation is active, and an English fallback reason. Insufficient physical cores, unavailable native services, permission failures, reserved-name conflicts, existing DBA assignments, or explicit per-thread restrictions retain software admission. AUTO preserves those assignments rather than replacing them. Persistent coverage failures discovered during work admission also switch AUTO to SOFT. Source commits do not acquire a resource-admission lease.

AUTO restores only bindings whose thread identity and affinity still match its own recorded changes. DBA group assignments and externally changed affinities supersede AUTO ownership. Disabling AUTO or shutting down restores owned source bindings and removes unbound engine-owned groups. A group explicitly adopted by a DBA remains in the native manager. After resolving a fallback cause, restart or switch OFF and back to AUTO while all Rapid tables are unloaded to provision a new plan.

This version isolates CPU execution and admission. It does not provide hard RSS, memory-bandwidth, disk-I/O, or NUMA isolation, and cannot guarantee unchanged TP throughput on a shared machine.

## Validation and measured performance

Debug and RelWithDebInfo (`-O2 -DNDEBUG -g1`, LTO) builds completed. All 175 unit tests passed, including six AUTO planning/admission tests. The full secondary-engine MTR run covered 250 checks: 246 initially passed; four existing fixture failures were corrected (dynamic KILL identifiers and unrelated schema-embedding cancellation during restart). All 19 focused resource/recovery checks then passed, including every AUTO case. Help/sysvar checks passed. A separate optimized-server test with real four-core affinity verified SOFT fallback, both propagation modes, preserved affinity, and clean shutdown.

The optimized benchmark used three alternating repeats per configuration, a separate five-second warmup and a fifteen-second measurement. Linux reports twelve vCPUs; the server used CPUs 0–9 and clients 10–11. Both configurations gave ordinary source work CPUs 0–6. OFF shared those CPUs with background work; AUTO assigned AP 7, SYNC 8, and MAINT 9, with effective budgets 1/2/1. The physical placement of these vCPUs by the hypervisor was not controlled.

| Workload | TP threads | Mode | OFF median TPS | AUTO median TPS | Difference |
| --- | ---: | --- | ---: | ---: | ---: |
| TP + propagation | 1 | CAPTURE | 484.73 | 495.38 | +2.20% |
| TP + propagation | 1 | COMMITTED_BINLOG | 488.43 | 474.21 | -2.91% |
| TP + propagation | 4 | CAPTURE | 1250.48 | 1283.19 | +2.62% |
| TP + propagation | 4 | COMMITTED_BINLOG | 1276.20 | 1156.75 | -9.36% |
| TP + AP | 4 | CAPTURE | 1111.08 | 1070.78 | -3.63% |
| TP + AP | 4 | COMMITTED_BINLOG | 1110.04 | 1066.56 | -3.92% |

For the mixed workload, median AP query rates were CAPTURE 126.86 → 113.74 QPS and COMMITTED_BINLOG 103.31 → 71.42 QPS. All 36 runs matched primary/Rapid COUNT/SUM results, retained STRICT_CPU for AUTO, and reported zero affinity-restoration failures.

These measurements include background parsing/apply and, for the mixed workload, AP execution. They do not isolate foreground commit-hook cost or identify the cause of each TPS difference. CPU-only isolation still leaves shared memory/cache, disk I/O, and other server resources; remaining isolation work is tracked in #932. Short measurements on one virtual machine do not establish a zero-overhead guarantee.
