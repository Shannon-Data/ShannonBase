# Rapid restart recovery

## Decision: scheme 2 — columnar checkpoint + durable capture WAL

Rapid uses a columnar checkpoint plus its own persistent incremental WAL for
restart recovery. It does not require retained source binlogs. Fast recovery is
conditional: when source history or transaction outcomes cannot be proved,
Rapid discards the tentative restore and reloads from recovered InnoDB.
An unresolved transaction is **not** assumed to have aborted or committed.

The implementation belongs in `storage/rapid_engine`. Use the server interfaces
already available in this repository; do not add recovery-specific patches to
MySQL's SQL layer, transaction coordinator or InnoDB. Tests may live in the
existing MTR and gunit directories. This boundary reduces upstream merge costs;
it does not assert that all existing ShannonBase hooks are present in vanilla
upstream MySQL.

### Persistence and transaction contract

There are two distinct logs:

* `cu_wal.log` describes physical column-store operations. Its `ROW_COMMIT`
  means row application completed; it is never proof of source transaction commit.
* `rapid_wal.log` persists detached source row images, including off-page
  values, before enqueueing changes for asynchronous application. Source outcome
  records include the captured transaction's record count and payload digest.

A capture failure durably invalidates the table's recovery proof and quarantines
Rapid reads before the row notification returns. Source DML remains under MySQL's
control; it may commit and will be recovered by primary reload. The notification
interface has no error return: setting a SQL error there violates server statement
invariants. Invalidation first appends INVALID; if that fails, it removes the
capture journal and syncs the parent directory, revoking all certificates even
when disk space is exhausted. If neither can establish durable invalidation, the
process must stop before allowing an uncaptured source change to commit.

A successful source after-commit callback forces
InnoDB redo durability before persisting the Rapid COMMIT record. It does not
wait for asynchronous propagation. Only a confirmed source rollback decision
may persist ABORT; facade cleanup or connection detach is not such proof.

Rapid registers a transaction observer through the existing server interface.
Setting a handlerton callback alone is insufficient: the server skips transaction
hook dispatch when its observer list is empty. Observer registration and removal
are paired with the propagation transaction manager's lifecycle. Rapid's final
engine commit callback requests post-engine observer dispatch via the existing
transaction hook flag, including when binlog is disabled. It never publishes a
source commit from the per-engine callback itself.

The existing after-commit callback supplies a thread ID. Rapid uses `current_thd`
only when its identity matches that ID; it does not require SQL code to populate
an additional THD pointer. If callback identity cannot be established, no durable
commit proof is written. XA PREPARE is not a final source commit. Unsupported
capture semantics invalidate fast recovery rather than guessing the outcome.

The source commit and the Rapid outcome record are **not atomic**. In particular:

```
source commit becomes durable -> crash -> Rapid COMMIT is not durable
```

The complete row images alone cannot resolve this case. Scheme 2 deliberately
reloads from the primary when any required outcome remains unknown. It also
reloads for active transactions at the crash, even when InnoDB subsequently
rolls those transactions back. A failed post-commit Rapid write cannot undo an
already committed source transaction; it must prevent an unsafe fast restore.

### Checkpoint, replay and publication

A table capture gate orders capture/enqueue, application, source outcome
publication and checkpoint creation. The initial implementation checkpoints only
when captured changes have been applied and all captured transactions have
terminal outcomes. Data files and an epoch/generation/WAL-cut certificate are
persisted before the checkpoint manifest is published. The capture epoch binds
the certificate to its log; record sequence checks, checksums and transaction
digests reject incomplete or corrupt history. Compatibility checks also apply to
source schema and the version/architecture-dependent row-image representation.

Recovery restores snapshots into unpublished structures, reconstructs indexes,
and replays captured changes after the certified cut only for committed source
transactions. It does **not** replay physical ROW_COMMIT records as source
commits. Failure discards the tentative image and takes the primary reload path.

`RecoveryJob` holds transaction-duration `MDL_SHARED_NO_WRITE` through recovery,
registration and propagation startup, fencing source writes and DDL across the
handoff. It rechecks the loaded registry after acquiring the lock. A failed
reload leaves the table unavailable; it must not publish a partial image.
Partitioned tables currently take primary reload. Old checkpoints without a
valid capture certificate cannot enable fast recovery.

WAL compaction retains the suffix needed by the oldest retained checkpoint.
Reload establishes a fresh capture epoch and schedules a new checkpoint.
Explicit unload/drop metadata remains authoritative: snapshot files alone must
not resurrect an unloaded table.

This protocol targets local server crash/restart recovery. It does not establish
correctness for independently restored source backups, arbitrary source epoch
changes, distributed failover or point-in-time recovery. Those require additional
source identity and lifecycle protocols; do not infer support from a valid CRC.

### Cost and verification

Synchronous capture and source outcome persistence add source write/commit
latency. The current implementation uses synchronous writes and a per-table
gate; it does not implement capture group commit. Checkpoint creation and WAL
compaction under that gate can block writers. Group commit, segmented retention
and shorter checkpoint fencing are subsequent performance work, subject to the
same durability ordering. No production latency or throughput claim follows
from functional recovery tests.

Status counters distinguish `rapid_recovery_storage_restores` from
`rapid_recovery_primary_reloads`. Regression coverage must assert the chosen
route as well as compare results with InnoDB:

* R01: commit INSERT/UPDATE/DELETE while propagation is stalled, crash, and
  recover the queued changes from the durable capture WAL.
* R02: apply an active source transaction, crash before source commit, and
  reload without exposing rolled-back changes.
* Deliberately omit a source commit marker: reload despite complete row images.
* Fail a capture write: quarantine Rapid, durably revoke fast recovery, and
  reload the committed source state after a crash.
* Check torn/corrupt records, aborted transactions, checkpoint epoch mismatch,
  retained-checkpoint compaction, concurrent capture and repeated restarts.

The implementation and test sources being present are not a release sign-off.
Record actual build, test and performance results separately.

## Deferred scheme 3 — participation in source transaction recovery

Scheme 3 would require rapid recovery even when the source committed but its
Rapid outcome marker was not persisted. This is a stronger guarantee than
scheme 2, and **is not implemented or promised here**.

It requires an authoritative transaction coordination/recovery protocol, not an
extra independent COMMIT record. Possible designs include making Rapid a proper
recoverable transaction participant, or obtaining durable transaction decisions
from an existing coordinator. Either way, the design must specify:

1. Durable transaction identity, prepare/decision ordering and crash recovery of
   every intermediate state, including XA, group commit and coordinator failure.
2. How Rapid obtains a final source outcome after restart, including when the
   source has already discarded its local transaction state.
3. Retention of prepared changes and decisions until all dependent checkpoints
   and participants no longer require them.
4. Atomicity across affected tables, retry/idempotence, source durability modes,
   failure handling and the impact on primary availability and commit latency.

In that responsibility, an independent Rapid journal would overlap with the
transaction-consistent durability responsibilities of binlog. Reusing an existing
coordinator is preferable to quietly introducing another independent decision
log, but its interface and guarantees must be demonstrated first.

**Maintenance constraint:** a design that needs changes in `sql/`, server commit
ordering, coordinator recovery, binlog internals or InnoDB is outside the approved
scheme 2 scope. Such patches make tracking upstream harder and must not be
introduced as incidental fixes. First investigate whether existing supported
engine/plugin interfaces can provide the complete protocol from inside
`rapid_engine`. If they cannot, keep scheme 2 and its safe reload fallback;
document the missing interface and obtain a separate architecture decision before
considering any exception. Merely registering a callback is not equivalent to
participating in durable transaction coordination.

## HeatWave reference

Oracle documents storage-layer recovery with source reload when required logs
are unavailable or stored data is incompatible:
[HeatWave Cluster Data Recovery](https://docs.oracle.com/en-us/iaas/mysql-database/doc/heatwave-cluster-failure-and-recovery.html).
This motivates conditional fast recovery, but does not disclose a transaction
protocol that Rapid can assume or copy. Rapid's capture-WAL protocol and the
scheme 2/3 distinction above are its own design decisions.
