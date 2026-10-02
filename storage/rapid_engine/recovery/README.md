# Rapid restart recovery

## Mechanism: — columnar checkpoint + durable capture WAL

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
invariants, because the row is already written and the statement is not failing.
The source transaction is never blocked or failed; refusing it would be scheme 3
(see below).

Revocation is `TablePersistenceManager::revoke_fast_recovery`, and it succeeds if
either of two independent channels does:

1. Revoke the capture journal itself: append INVALID; if that fails, remove the
   journal and sync the parent directory, revoking all certificates even when
   disk space is exhausted.
2. Write a `reload_required` marker in the table directory, outside the journal.

Channel 2 exists because channel 1 is both the object being judged and the place
the judgement is recorded: when the journal's own machinery is broken, a decision
recorded only inside it may not be readable. Recovery consults the marker before
it restores anything, so it outranks a certificate that survived. Only if both
channels fail is the decision left in memory (`require_recovery()`), which still
refuses new appends and checkpoints for the life of the process and is reported
as an ERROR and in `rapid_recovery_wal_truncation_failures`. That last case --
both channels unwritable, then a crash before any reload -- is the one residual
window, and it cannot be closed from inside one local filesystem.

The marker itself is written twice: in the table's own directory and in a
separate tree (`<datadir>/rapid_taint/...`). One filesystem cannot give a second
failure domain, but it can stop a failure scoped to one directory from taking out
the decision along with the data.

Why revocation is eager here and lazy in HeatWave: HeatWave decides at recovery
time, because its doubt has an external source -- are the binary logs that would
carry the delta still retained, is the DB System version compatible, is object
storage reachable. Rapid deliberately keeps no source binlogs, so it has no such
oracle: the only evidence that an image cannot be trusted is what it writes
itself, at the moment capture fails. That is the price of that choice, and it is
why channel 2 is required rather than nice to have.

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

The published reload behaviour is worth stating precisely, because it is the
same shape as scheme 2 and it shows where our exposure differs:

* The HeatWave cluster (`RAPID` engine) holds data in volatile memory, so a DB
  System or cluster reboot requires a reload.
* Before 9.2.0 every restart reloaded from InnoDB (or Object Storage for Lakehouse
  tables). From 9.2.0 the HeatWave Storage Layer -- object storage, on OCI or the
  AWS equivalent -- persists the loaded data plus extra metadata, so recovery can
  rehydrate from it instead of scanning InnoDB; the documented fallback when that
  fails is reloading from InnoDB or Object Storage ("rare in 9.4.2 due to improved
  compatibility checks").
* So HeatWave keeps a durable copy of its own, uses it for conditional fast
  recovery, and falls back to the source when it cannot be used -- exactly the
  posture above. What it does *not* have is our single point: its durable copy
  lives in a separate failure domain (remote object storage), not on the same
  local filesystem as the certificate that judges it. Channel 2 above is the
  local stand-in for that separation, and is why the marker is a different object
  from the journal.
* Still undisclosed: the consistency protocol behind "propagated changes". The
  reload material asserts that "data changes made while the cluster was offline
  are also incorporated during recovery", but not how completeness is proven, so
  no guarantee can be copied from it.
* Oracle also enumerates per-table reasons to reload instead of recovering. Most
  map onto checks Rapid already makes: no manifest (absent from the periodic
  metadata checkpoint), the reload marker (stale table), `compatibility()`
  binding the journal to `MYSQL_VERSION_ID`/pointer width/endianness plus the
  manifest's schema fingerprint (incompatible stored data), and partitioned
  tables always being rebuilt from InnoDB (interrupted partition load/unload).
  Dictionary-encoded columns are a property of their storage format and have no
  counterpart here. The one with no equivalent is the source-binlog condition:
  HeatWave can discover at recovery time that the delta source is gone, Rapid
  cannot -- see the eager-revocation note above.
* Oracle's point-in-time recovery is source-side (backups plus backed-up binary
  logs replayed into a new DB System, to as close as five minutes before the last
  update); it says nothing about cluster-side propagation consistency.
