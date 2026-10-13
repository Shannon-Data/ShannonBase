<!--
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

Copyright (c) 2026, Shannon Data AI and/or its affiliates.
-->

# Rapid recovery: native checkpoints and committed MySQL binlog

The current release uses per-table recovery: validate the checkpoint and its
bound source-log identity, restore native data, replay complete committed
transactions, and publish only after all required steps succeed. Known revoked
eligibility or an inspection error leaves that table unloaded for explicit
`ALTER TABLE db.table SECONDARY_LOAD`; other eligible tables recover independently.
TP source-state comparison remains an extension interface for a future release.

`PERSISTED_SCN` is a Rapid frontier, not a MySQL GTID. Replay starts from the
manifest's bound binlog file/offset and prefix digest, which also protects the
GTID events in that prefix; there is no standalone persisted GTID-set field or
GTID-minus-SCN calculation. Disabling binlog does not disable InnoDB redo.
If an unlogged TP commit survives source crash recovery but every AP revocation
write was lost, a valid old checkpoint and replayable logs cannot detect the
missing change. That window remains outside the current safety guarantee.
Only changes actually lost by TP crash recovery may also be omitted by AP
without creating a TP/AP discrepancy.

Recovery jobs serialize through a per-table mutex and recheck loaded state
after acquiring it. They also recheck revocation after source MDL acquisition,
because a source write can invalidate an image while recovery waits for MDL.
Native replay always starts from a private restored image; failed attempts
discard it rather than append another replay to partially recovered rows.

Native and primary-reload recovery share one registration path: attach committed
binlog propagation first, then publish the loaded-table share. Registration
failure removes the consumer entry, share and partial IMCS image and fences the
table for explicit SECONDARY_LOAD. A completed job can be repeated without
reloading or replaying the already published table. Recovery counters count
successful actual restore/reload operations, not idempotent skips.
An explicit SECONDARY_LOAD on an already loaded table reports an error before
scanning. To rebuild a still-loaded failed image, explicitly SECONDARY_UNLOAD
before SECONDARY_LOAD; restart failures that remain unloaded need only load.

The directed validation includes repeated native recovery across two SIGKILL
restarts, repeated execution of one successful/failed recovery job, registration
failure after propagation attach, revocation while waiting for source MDL,
repeated SECONDARY_LOAD rejection, and independent recovery of healthy/revoked
tables with GTID enabled. SIGABRT tests verify that committed changes survive
and uncommitted changes are excluded. A file-level power-loss simulation leaves
a torn newer manifest and orphan temporary files with the server stopped, then
requires recovery from the last complete generation plus committed replay.
It does not cut host/device power or simulate OS/controller cache loss, and
must not be reported as a physical power-loss qualification.

Rapid's `CAPTURE` online path receives precommit row notifications and maintains
transaction MVCC; `COMMITTED_BINLOG` decodes committed source transactions in a
background reader. Both produce `Source::COPY_INFO` row images, distinguished by
the record's `CopyInfoMode` (`NOTIFICATION` or `COMMITTED_BINLOG`). Recovery replay
also uses the committed-binlog row-image mode independently of the online setting.
Notifications and transaction outcomes are volatile. Rapid does not append
its own capture WAL, physical row WAL, or durable commit markers.

With `rapid_reload_on_restart=ON`, the background scheduler publishes immutable
native columnar checkpoints. A checkpoint requires no active source writer, no
pending notification, and no uncommitted physical version. Its manifest records
schema fingerprint, table identity, generation, per-IMCU checksums and a committed
MySQL binlog boundary. The immutable source prefix is hashed to detect reused log
names after RESET. Manifest v2 remains readable; unsupported older formats take
the source reload path. The format-v3 SCN fields are described below.

Checkpoint creation captures the source boundary and mutation versions under the
notification gate and all IMCU mutation locks, then releases those locks. It
streams each IMCU to its snapshot file under that IMCU's mutation lock. A final
validation briefly reacquires the gate and all mutation locks; any changed
boundary or version discards the generation. File durability and storage-provider
publication follow after those locks are released. Per-IMCU serialization can
still delay writers, and continuous writes can prevent checkpoint publication;
no maximum pause or checkpoint completion time is guaranteed.

`CheckpointStore` abstracts publish, restore, invalidate and erase. The default
`LocalCheckpointStore` uses the files durably written by TablePersistenceManager.
Future S3/OCI providers must publish their manifest last, stage complete generations
locally for normal validation, durably invalidate old generations, and prevent
invalidated state from becoming visible again. Install providers before workers
start with `CheckpointStores::install`. Full snapshots are the first implementation;
immutable objects may be shared for future incremental persistence. No cloud SDK,
credential management, remote catalogue or object-store garbage collector ships
with this interface.

Recovery excludes source writes with MDL, reconstructs an unpublished table,
validates the checkpoint, and replays complete committed source transactions from
binlog. Only then is the table registered for AP. Failed or unsupported replay
throws away the partial table and reloads from InnoDB. Log retention is not forced:
if the required log has been purged, source reload is the defined fallback.

The initial replay lane requires non-partitioned primary-key tables, exact column
metadata, FULL row images, unencrypted local logs and bounded transaction buffers.
LOB/JSON/vector/generated columns, foreign keys, MINIMAL/partial images, compressed
transaction payloads, XA, DDL and unknown events use source reload. Checkpoints
require strict source durability (`sync_binlog=1` and
`innodb_flush_log_at_trx_commit=1`). Unlogged/weakly durable source writes attempt
to revoke old checkpoint eligibility; failed revocation must not veto TP commits.
Rapid does not force additional primary log flushes. The all-evidence-lost case
is described below rather than treated as a successfully persisted revocation.

The capability follows the public HeatWave Storage Layer recovery model: persist
native data on load and as changes propagate, restore native data and catch up using
binlog, fall back to source reload on storage/history/compatibility failure. The
public documents do not specify Oracle's internal checkpoint encoding or WAL use.

MTR coverage includes committed queued changes across log rotation, crashes during
uncommitted transactions after AP execution, purged/reset binlogs, corrupt manifest
and column files, partial row images, unlogged writes, weak source durability,
schema changes and a simulated storage-provider restore failure. Recovery counters
assert the actual route; results are independently checked against expected source
state and forced Rapid queries under both optimizers. Long-transaction tests also
cover own writes, isolation, COMMIT/ROLLBACK and RR/RC statement visibility. Partial
SAVEPOINT rollback after propagated writes remains fail-closed until operation-level
undo is implemented; the tests explicitly pin that limitation.

A known revoked recovery image or an error inspecting its eligibility stops that
table's automatic restart recovery with a warning, before taking source MDL.
The table remains unloaded; ON queries use the primary and FORCED queries fail.
Run `ALTER TABLE db.table SECONDARY_LOAD` to rebuild it from the primary and start
a new recovery epoch. Pending recovery fencing remains until that explicit load.
This does not reject primary commits. Ordinary unsupported/missing checkpoint
fallback remains unchanged. If all revocation writes failed before a crash,
absence of a marker cannot reveal that failure; a primary durable witness or
full source-state verification is required to close that separate window.

## Recovery lifecycle and failure decisions

The DD `secondary_load` flag records intent to recover a loaded table; it is not
proof that an AP generation covers every TP commit. `DDWorker` discovers that
intent and `RecoveryFramework` dispatches bounded background `RecoveryJob`s.
`CheckpointScheduler` and `TablePersistenceManager` own native generations;
`NotificationTracker` supplies the volatile commit/apply boundary;
`BinlogRecovery` certifies source-log identity and replays committed row images.

For each table, the job first checks revocation/inspection failures, then takes
`MDL_SHARED_NO_WRITE` to hold the source cut through restore or reload, index
rebuild, replay, AP registration and propagation attach. An unpublished partial
table is removed on failure, so fallback cannot append TP rows to half-restored
AP rows. Source writes can wait for this existing lock during recovery; this
does not promise zero TP interference. Fast recovery is per table, not a mixture
of partial old AP data and fresh TP rows inside one table.

| Condition | Current action |
| --- | --- |
| Compatible native generation and supported committed-binlog replay succeed | Publish restored AP; attach propagation; schedule a new checkpoint |
| Missing/corrupt/incompatible image, unsupported replay or required log purged | Discard the partial table and old recovery epoch; reload from TP |
| Revoked eligibility or error inspecting it | Warning; leave AP unloaded; explicit user SECONDARY_LOAD |
| Requested FULL validation fails or no implementation is installed | Warning; leave AP unloaded; explicit user SECONDARY_LOAD |
| Unlogged TP commit and every revocation write lost, with FAST selected | Not detectable from the old valid image alone; FAST does not certify this case |

Revocation first fences memory, then attempts provider invalidation and local
markers under the table directory and the separate `rapid_taint` tree. An
unreadable marker path fails closed. Check markers before provider restore so
staging an old image cannot erase local revocation evidence. This redundancy
helps individual storage failures; it is not a durable TP transaction witness.

Explicit `ALTER TABLE db.table SECONDARY_LOAD` starts a new epoch and scans TP.
The SQL load path holds `MDL_EXCLUSIVE` across scan, AP registration and capture
attach, preventing a write gap between the snapshot and propagation. It includes
previously unlogged committed source changes. ON queries use TP while AP is
unloaded; FORCED queries fail. An online load with reduced locking would require
a separate snapshot/capture/catch-up/atomic-publication protocol.

The committed-binlog consumer uses `rapid_query_sync_timeout_ms` as a shared
no-progress deadline across metadata retries, resource admission, queue pressure
and apply barriers. Only cursor progress resets it. Expiry does not publish a
partial transaction or advance the cursor; it quarantines the current loaded
catalog until unload/reload. This cooperative deadline cannot interrupt a
blocking syscall. Its default 30 seconds is a policy, not a TP/AP latency SLA.

## Native storage recovery and persisted progress

Fast recovery loads the native Rapid columnar generation and rebuilds indexes,
then replays complete committed MySQL binlog transactions before publication. It
does not scan and convert all source rows. Missing/incompatible images or missing
required binlog history retain the primary reload fallback; known revoked or
uninspectable eligibility retains the warning/manual SECONDARY_LOAD policy above.

Manifest format 3 records SNAPSHOT_SCN (the consistent load baseline) and
PERSISTED_SCN (the Rapid SCN frontier covered by the complete stored generation),
alongside source identity, schema fingerprint, binlog position and checksums.
SCN is not a notification sequence, InnoDB transaction ID or binlog byte offset.
The frontier is captured while notifications are quiescent and IMCU versions are
stable. Runtime PERSISTED_SCN advances only after file/manifest durability and
successful provider publication. Provider errors/exceptions discard the candidate
and retain the previous progress. Source apply alone cannot advance this field.
A new load clears persistence progress until its first successful checkpoint.
Recovery restores both values from the validated selected manifest and observes
the persisted frontier before allocating replay SCNs. Format 2 remains readable,
with unknown SCN fields reported as zero; no progress is inferred for that format.

### ShannonBase native storage format

The local native hierarchy is `RpdTable -> IMCU -> CU`. IMCUs are row chunks;
each CU holds one represented column's slot buffer plus its encoding state.
This follows HeatWave's documented partition/chunk/vector organization without
assuming its private object layout. The chunk row count comes from Rapid's
configuration, not an assumed 64 MB HeatWave file size. Partitioned tables still
use source reload; there is no distributed node-placement map in this format.

```text
<snapshot_dir>/<schema>/<table>/
  checkpoints/checkpoint-<generation>.manifest
  snapshots/checkpoint-<generation>/imcu_<id>.snap
```

These names are ShannonBase's own layout. The default root is
`<datadir>/rapid_snapshots`; a remote provider must transfer the same complete
logical generation and publish its remote manifest only after all referenced
objects are durable. No HeatWave S3 object names are inferred from this layout.

| Record | Contents, in write order |
| --- | --- |
| Manifest v3 | u32 magic, u16 version; u64 table ID, generation, schema fingerprint, SNAPSHOT_SCN, PERSISTED_SCN; binlog filename, u64 offset, prefix digest; u32 IMCU count; entries; CRC32C trailer |
| Manifest entry | u32 IMCU ID, u8 state, u64 notification sequence, u64 snapshot byte size, u32 CRC32C, relative snapshot filename |
| Snapshot header (36 bytes) | u32 magic, u16 version, u32 IMCU ID, u32 column count, u64 notification sequence, u64 timestamp, 6 reserved bytes |
| IMCU metadata | u64 row count/start/end/capacity, u8 status; deletion bitmap; column NULL bitmaps. Each bitmap carries u64 row and byte counts followed by its bytes |
| Column section | u64 CU byte length followed by a CU v2 record; zero length is permitted only for a NOT_SECONDARY column with no CU |
| CU v2 | 20-byte identity/encoding/codec header; 32-byte widths/size/row counts; 24-byte min/max/sum; u64 payload size and native slot payload; optional dictionaries, version records and varlen values; CRC32C trailer |

Variable manifest strings use a u32 byte length followed by bytes, without a
terminating NUL. Scalars use host byte order, little-endian on the supported
x86/ARM builds; this is not a portable cross-endian or pointer-ABI image.
[cu.h](../imcs/cu.h) documents the column fields. Dictionary strings preserve
their IDs on restore; pool-backed values are serialized logically and rebuilt
in a new allocator. Native checkpoints contain committed current slots, NULL
and deletion masks, and no pre-crash MVCC version journal. Generic CU snapshots
can additionally carry version records; that is a separate use of the codec.

Restore checks manifest topology, identity, file size/CRC and column schema,
encoding, codec, row count and complete section consumption. A bounded column
stream reads directly from the staged native snapshot, avoiding an extra
whole-CU input copy and preventing one column from reading the next column's
bytes. The decoder still stages/validates its payload and may decompress or
rebuild dictionaries/pool references. Native recovery avoids TP-to-AP conversion;
it is not a claim of zero parsing, zero copies or a complete process RAM clone.

CU I/O/allocation exceptions become error returns. Corrupt or incomplete
generations are not published; ordinary native restore failures retain the
existing source-reload fallback, while revoked eligibility uses manual load.
The selected table generation is restored as a unit, not assembled from pieces
of different generations. Default FAST does not add full source-row validation.

The default provider is local storage. OCI/S3 SDK integration, cluster provisioning
and distributed failover are not supplied by these changes. A custom provider's
publish success must mean a complete recoverable generation has been durably
committed; an unreliable acknowledgement cannot be made safe by an SCN counter.
These AP progress fields also cannot certify an unlogged primary commit after all
revocation writes failed: that separate primary-witness gap remains open.

Reference behavior: [HeatWave Cluster Data Recovery](https://docs.oracle.com/en-us/iaas/mysql-database/doc/heatwave-cluster-failure-recovery.html)
and [rpd_tables](https://dev.mysql.com/doc/heatwave/en/mys-hw-rpd-tables-table.html).
The explicit manual policy for known eligibility failures follows the user's TP
priority constraint; it is not a claim that every HeatWave fallback is manual.

## HeatWave references and ShannonBase table metadata checkpoints

The public HeatWave architecture is a reference for behavior, not a specification
of ShannonBase's persistence protocol:

- The official [Scale Out Data Management video](https://www.youtube.com/watch?v=_gjBiCqdTjo)
  describes copying the in-memory representation to Object Storage. Native-format
  recovery avoids rebuilding columnar data from InnoDB rows; it still requires
  reads and reconstruction of runtime structures. ShannonBase similarly restores
  IMCU/CU snapshots, dictionaries and variable-length values, reconnects Field
  pointers, rebuilds ART indexes, and replays committed changes.
- [HeatWave Cluster Data Recovery](https://docs.oracle.com/en-us/iaas/mysql-database/doc/heatwave-cluster-failure-recovery.html)
  documents persistence of loaded/propagated data and periodic metadata
  checkpoints. After a DB System restart, stale tables and tables absent from
  these checkpoints require source reload; missing required binlogs also prevent
  storage-layer recovery. This restriction does not establish whether individual
  state transitions are synchronously or asynchronously persisted.
- [rpd_tables](https://dev.mysql.com/doc/heatwave/en/mys-hw-rpd-tables-table.html)
  exposes LOAD_STATUS and, from 9.7.0, STALE_REASON. RECOVERY_SOURCE reports the
  source of the last successful recovery; it is not a recovery eligibility proof.
  SNAPSHOT_SCN and PERSISTED_SCN describe the snapshot and persisted progress.
- [Change Propagation](https://dev.mysql.com/doc/heatwave/en/mys-hw-change-propagation.html)
  documents STALE after propagation failures. Idle checks arrange automatic
  reload for out-of-code failures; other causes require manual unload/load.
- The official [9.4.2 reload article](https://blogs.oracle.com/mysql/mysql-heatwave-942-improvements-in-reloading-data-into-the-heatwave-cluster-after-rebooting)
  describes additional persisted metadata and storage-layer recovery after
  restart. The reviewed public sources do not specify persistence of each
  LOAD_STATUS transition, its atomicity with TP commits, or a state file format.
  Names such as metadata.json and manifest.bin must not be attributed to
  HeatWave. ShannonBase RFCs are not evidence of HeatWave internals.

ShannonBase's own implementation writes a per-table `table_state.checkpoint`
under the local snapshot directory on the existing periodic checkpoint interval
(default 300 seconds). The versioned, CRC-protected record contains table
identity, generation, snapshot/persisted SCNs, AVAILABLE/STALE and stale reason.
It records stable terminal states, not LOADING/recovery progress. The record is
written through the local durable-file helper (temporary file, data sync,
rename, parent-directory sync), not through frequent mysql.tables updates or
an Object Storage metadata API. These names and semantics are ShannonBase's
own design. Generation/SCNs describe observed progress; native manifests and
bound binlog/GTID boundaries remain authoritative for restoring data.

Before provider restore, persisted STALE or unreadable/corrupt metadata rejects
FAST and leaves AP unpublished with a manual SECONDARY_LOAD warning. AVAILABLE
cannot clear a live revocation. Explicit load starts a new recovery epoch and
removes the old metadata; periodic checkpoints can then record the new state.
Missing metadata remains compatible with checkpoints created before this format.
Existing provider invalidation and revocation markers remain in use.

`shannon_recovery_table_state` verifies that periodic STALE alone rejects crash
recovery after provider invalidation and both marker writes fail.
`shannon_recovery_table_state_corrupt` verifies fail-closed metadata parsing and
successful explicit reload. `shannon_recovery_many_tables` checks discovery and
recovery of twelve tables across two schemas, exceeding the eight-worker limit,
and validates every table's data after native crash recovery. Protocol unit tests
cover metadata watermarks, corruption/truncation, live revocation and epoch reset.

For this release, **TP/AP consistency is an explicit product precondition**.
Together with checkpoint validation, committed-log replay and refusal of known
eligibility failures, this is the agreed closure condition for the release's
recovery contract. The full TP/AP comparison remains an extension interface for
a later release. It is not executed, and assuming consistency is not evidence
that an unlogged surviving TP commit was automatically detected. Periodic
metadata adds protection once STALE is durable; if all revocation writes fail
and a crash precedes the next metadata write, no new durable evidence exists.
Tests of persisted STALE must not be described as closing that combined window.

## Optional full source validation interface

This version defaults to FAST under the TP/AP consistency precondition above.
It validates native storage generations and
replays available committed binlog history, without scanning all TP rows to
compare them with AP. FAST skips the source validator even if one is registered;
it does not prove coverage of an unlogged source commit when every revocation
write failed. Manual SECONDARY_LOAD rebuilds a fresh consistent source snapshot.

[source_validation.h](source_validation.h) reserves a FULL verification policy
and SourceRecoveryValidator interface. Install the policy with
SourceRecoveryValidation::install before starting recovery workers; each job
retains a policy snapshot. A caller may also pass a policy directly to the
RecoveryJob constructor for a specific table. The hook runs after native restore/index rebuild and
binlog replay, before AP registration and propagation attach, under the existing
source MDL. It receives the THD, primary TABLE, recovered table, selected manifest
and a cooperative deadline based on rapid_query_sync_timeout_ms. Implementations
must be safe for concurrent recovery of different tables and must not retain
context pointers, mutate TP, publish AP or force additional source flushes.

A future full verification implementation must compare every represented field
including NULL and binary values, primary-key identity, and missing/extra rows
at that stable source cut. Counts, sampling, checksums without a complete source
read, or binlog/SCN validation alone cannot certify that unlogged commits were
covered. Its TP I/O, recovery duration and source-write lock cost must be measured
before enabling it. Source rows are read for validation; AP still loads its
native generation rather than converting TP rows into a replacement AP table.

FULL permits publication only after VERIFIED. Missing implementations, SKIPPED,
exceptions, incomplete checks and expired deadlines become UNAVAILABLE;
MISMATCH/UNAVAILABLE leave AP unpublished, issue a warning requesting explicit
SECONDARY_LOAD and prohibit automatic primary reload for that failed validation.
The timeout is cooperative and cannot interrupt a blocking provider call.
There is no built-in full-table verifier or SQL mode switch in this version:
this is an extension interface, not a completed full safety implementation.

## HeatWave references and future work

| Official reference | Confirmed behavior and Rapid application |
| --- | --- |
| [Data load to HeatWave with MySQL Autopilot](https://blogs.oracle.com/mysql/mysql-heatwave-best-practices-series-data-load-to-heatwave-with-mysql-autopilot), Data Format/Figure 2 | Partitions contain fixed-size chunks and encoded/compressed vector buffers; one column can have multiple buffers. Rapid keeps native IMCU/CU blocks and encoding metadata |
| [Inside HeatWave Lakehouse on OCI](https://blogs.oracle.com/cloud-infrastructure/first-principles-mysql-heatwave-lakehouse), Computation and communication overlap | The optimized internal format is used in memory and persistence. Rapid restores native slots rather than rebuilding them by scanning TP |
| [AWS scale-out data management](https://blogs.oracle.com/mysql/mysql-heatwave-new-machine-learning-multicloud-capabilities) | S3 stores the hybrid-columnar representation for parallel native reload. Rapid reserves transport in CheckpointStore; no AWS/OCI SDK adapter ships |
| [Cluster Data Recovery](https://docs.oracle.com/en-us/iaas/mysql-database/doc/heatwave-cluster-failure-recovery.html) and [rpd_tables](https://dev.mysql.com/doc/heatwave/en/mys-hw-rpd-tables-table.html) | Native storage is preferred; metadata checkpoints and binlog history matter; persisted progress differs from the load snapshot. Rapid binds both SCNs and binlog position to a complete generation |
| [Dictionary encoding](https://dev.mysql.com/doc/heatwave/en/mys-hw-dictionary-encoding.html) and [8.0.31 zone maps](https://dev.mysql.com/doc/relnotes/heatwave/en/news-8-0-31.html) | SORTED means dictionary encoding; min/max statistics support chunk pruning. Rapid persists dictionary IDs/values and statistics, then rebuilds runtime statistics/indexes |

These documents do not publish a complete binary file specification, SCN field
offsets, object naming, per-chunk metadata headers or a guaranteed 64 MB chunk
size. "RAPID Serialized Hybrid-Columnar Blocks" and a byte-for-byte RAM clone
are not treated as verified official format definitions. Encoding/compression
are not encryption; the local native snapshots have no encryption envelope.
Remote encryption/key management and distributed topology remain future work.
Vendor recovery examples are not ShannonBase benchmarks or SLA guarantees.

MySQL's [WL#14936](https://github.com/mysql/mysql-server/commit/32fdb823e9eec7eef31e37f4d480214d9366b0b6)
persists DD load intent; [WL#15339](https://github.com/mysql/mysql-server/commit/742c8d86304fd5e6ba7453aa3aa3c3dbadf1e831)
describes committed GTID/binlog recovery identity; [WL#15517](https://github.com/mysql/mysql-server/commit/3ac3baf33e8d2488d74b487a5d1fa752c5c94bcd)
describes persisted AP change packets. None publishes the private protocol
atomically tying every unlogged source commit to AP recovery eligibility.

A future durable witness must cover table incarnation and all source outcomes,
survive purge/redo recycling, and become durable with the original source commit,
not first appear in an after-commit file write. InnoDB's
[undo-to-gtid_executed handoff](https://github.com/mysql/mysql-server/commit/b676cc1ae2ee23e34e96e1c0ecf3c2759fb2c664)
is a reference for durable handoff, not an existing per-table AP certificate.
Adding metadata writes or waits requires TP throughput/latency measurements;
AP certification failure must not change successful TP commit semantics.

Directed tests cover native crash recovery, publication failure/watermarks,
revocation/provider/marker failures, concurrent manual loading and consumer
stalls. The optional validation policy also has unit tests for FAST skipping,
FULL success/mismatch/missing implementation, exceptions and deadlines, plus
`shannon_recovery_validation_unavailable` for keeping failed AP unpublished and
manual loading without changing TP data. These checks do not replace full-suite,
long-duration pressure, GC/checkpoint measurements or SLA qualification.
