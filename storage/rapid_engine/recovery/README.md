# Rapid recovery: native checkpoints and committed MySQL binlog

Rapid's online path receives precommit row notifications and maintains transaction
MVCC. Notifications and transaction outcomes are volatile. Rapid does not append
its own capture WAL, physical row WAL, or durable commit markers.

With `rapid_reload_on_restart=ON`, the background scheduler publishes immutable
native columnar checkpoints. A checkpoint requires no active source writer, no
pending notification, and no uncommitted physical version. Its manifest records
schema fingerprint, table identity, generation, per-IMCU checksums and a committed
MySQL binlog boundary. The immutable source prefix is hashed to detect reused log
names after RESET. Legacy manifests are rejected and reloaded from InnoDB.

Native snapshot bytes are fixed under the notification gate and IMCU mutation
locks. Those locks are released before writing/flushing snapshot files and before
calling the storage provider. Serialization still has a bounded impact on writers;
removing independent WAL does not make notification or checkpoint overhead zero.

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
`innodb_flush_log_at_trx_commit=1`). Unlogged/weakly durable source writes fence old
checkpoints before commit; Rapid does not force additional primary log flushes.

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
