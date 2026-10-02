/**
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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023 - 2026, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_TABLE_PERSISTENCE_H__
#define __SHANNONBASE_TABLE_PERSISTENCE_H__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "my_inttypes.h"                               // uint32, uint64
#include "storage/rapid_engine/include/rapid_const.h"  // Result, ErrorCode
#include "storage/rapid_engine/recovery/durable_fs.h"  // DurableFileSystem, DurableFile
#include "storage/rapid_engine/recovery/wal.h"
/*
   CU Persistence & Recovery sub-system

   Overview
   The Rapid Engine stores all data in volatile DRAM.  After a crash or clean
   shutdown the In-Memory Column Store (IMCS) must be rebuilt.  The original
   approach was a full reload ("cold load") from InnoDB, which is slow and
   keeps the table unavailable to IMCS queries for the whole reload.

   Startup recovery is now built on source transaction outcomes ("Option 2"),
   not on physical redo.  InnoDB remains authoritative for what committed;
   Rapid only restores a checkpoint and then catches up with source
   transactions that are known to have reached a terminal outcome.  Three
   on-disk artefacts are involved:

     1. Checkpoint generations (owned by this class)
        checkpoint() freezes EVERY IMCU of the table at one quiescent point,
        writes one snapshot per IMCU (header + IMCU metadata + one serialized
        CU per column) into a new immutable generation, and only then
        publishes that generation's manifest.  kMaxRetainedGenerations
        generations are kept so recovery can fall back if the newest is
        damaged.

          Snapshot:  <data_dir>/<db>/<table>/snapshots/checkpoint-<gen>/imcu_<id>.snap
          Manifest:  <data_dir>/<db>/<table>/checkpoints/checkpoint-<gen>.manifest

     2. Capture WAL (Recovery::WAL, reached through wal(); rapid_wal.log)
        Records source row changes together with their source transaction.
        Every checkpoint generation publishes a capture cut.  At startup only
        the changes after that cut are replayed, and only for source
        transactions whose outcome is complete and terminal.  This is the
        sole input to the post-checkpoint catch-up; the transaction-outcome
        validation itself lives in Recovery::WAL.

     3. Physical row WAL (owned by this class; cu_wal.log)
        ROW_PREPARE / ROW_COMMIT redo is still appended and fsync'd before the
        in-memory CU state is mutated, so each row mutation is atomic and
        durable locally.  It also drives the LSN watermarks, and applied_lsn
        defines the checkpoint boundary.  It is NOT a source of commit
        decisions: it can contain records of source transactions that are
        still ACTIVE.  Startup therefore calls recover() with
        physical_replay=false, which restores snapshots and publishes
        watermarks but does not replay this log.  The physical-replay branch
        of recover() is retained but is not used by the startup path.

   Recovery sequence (RecoveryJob::try_snapshot_recovery, executed at start-up
   for each table in the Rapid catalog)
     1. Take a MDL_SHARED_NO_WRITE lock on the source table, so source DML and
        DDL cannot slip in between the restore and the table's registration.
     2. Require capture WAL for the table and at least one checkpoint
        manifest; otherwise go straight to the InnoDB reload (step 7).
     3. recover(physical_replay=false): select the newest generation whose
        manifest and snapshot files all validate (size, CRC, header), falling
        back to older generations; check table id and schema fingerprint;
        load every snapshot; publish the LSN watermarks.
     4. Re-bind Field* pointers and rebuild the ART indexes (neither is part
        of a snapshot).
     5. Replay the capture WAL after the generation's cut.
     6. Rebuild statistics and register the table in the loaded-table list.
        Only now can IMCS queries see it.
     7. On any failure the partial in-memory table is dropped, the previous
        WAL epoch is discarded (reset_epoch()) and the table is reloaded from
        InnoDB.

   Readiness restrictions
   This design shortens the time to a usable table; it does not remove the
   unavailability window.  Throughout recovery:
     - the table is not visible to IMCS queries until step 6 completes;
     - source DML and DDL on the table are blocked by the MDL lock of step 1;
     - a table without a complete, compatible checkpoint plus capture proof
       still pays for a full InnoDB reload, and partitioned tables never use
       snapshots;
     - while recovery_required() is set, new WAL appends and checkpoints are
       refused until the table is recovered or its epoch is reset.

   WAL record format
   Every record starts with [Magic 4 B][LSN 8 B][OpType 1 B] and ends with a
   CRC32C of all preceding bytes in the record.  The middle depends on OpType.

   Legacy single-cell layout (INSERT, UPDATE, DELETE, NULL_INSERT, NULL_UPDATE;
   OP_ABORT uses the same layout and only txn_id is meaningful):
     [ImcuId  4 B]
     [ColId   4 B]
     [RowId   8 B]
     [TxnId   8 B]
     [SCN     8 B]
     [ValLen  8 B]  UNIV_SQL_NULL for NULL, 0 for DELETE
     [ValData N B]  absent when ValLen is 0 or UNIV_SQL_NULL

   ROW_PREPARE (one multi-column row mutation):
     [OpId 8 B] (== the record's LSN) [ImcuId 4 B] [RowId 8 B] [TxnId 8 B]
     [SCN 8 B] [MutType 1 B] [CellCount 4 B]
     CellCount x { [ColId 4 B] [IsNull 1 B] [Len 8 B] [Data N B] }

   ROW_COMMIT (pairs with a ROW_PREPARE by OpId):
     [OpId 8 B] [ImcuId 4 B] [CommitLsn 8 B] [RedoCount 4 B]
     [OperationCrc 4 B]  (digest of the paired prepare's logical cells)

   WAL_MAGIC = 0x4C41574C is "LWAL" when its bytes are read little-endian.

   Thread-safety
   The WAL writer uses a dedicated mutex; multiple threads can call
   log_write() / log_update() / log_delete() concurrently and each will get
   a unique LSN.

   Lock order is capture gate -> checkpoint mutex -> IMCU mutation locks ->
   physical WAL mutex.  checkpoint() holds the table's IMCU list in shared
   mode and every IMCU's mutation_mutex EXCLUSIVELY (acquired in imcu_id
   order) while it serializes the CUs, and releases the IMCU locks before the
   snapshot files are fsync'd.
*/
namespace ShannonBase {
namespace Imcs {
class CU;
class Imcu;

// WAL constants
static constexpr uint32_t WAL_MAGIC = 0x4C41574Cu;   // "LWAL" LE
static constexpr uint32_t SNAP_MAGIC = 0x50414E53u;  // "SNAP" LE
static constexpr uint16_t WAL_FORMAT_VER = 1u;
static constexpr uint16_t SNAP_FORMAT_VER = 1u;

// Recovery manifest constants
static constexpr uint32_t MANIFEST_MAGIC = 0x4E414D52u;  // "RMAN" LE
static constexpr uint16_t MANIFEST_FORMAT_VER = 1u;
static constexpr uint64_t MAX_CU_SNAPSHOT_SIZE = (1ull << 40);  // 1 TiB sanity cap

// How many immutable checkpoint generations to retain for fallback recovery.
static constexpr size_t kMaxRetainedGenerations = 2;

// Snapshot file header size (fixed prefix before per-CU data).
// Layout: [SNAP_MAGIC 4B][version 2B][imcu_id 4B][col_count 4B][snap_lsn 8B]
//         [timestamp 8B][reserved 6B]  = 36 bytes
static constexpr size_t SNAP_FILE_HEADER_SIZE = 36;

enum class WalOpType : uint8_t {
  INSERT = 1,
  UPDATE = 2,
  DELETE = 3,
  NULL_INSERT = 4,  // INSERT with NULL value
  NULL_UPDATE = 5,  // UPDATE to NULL
  ROW_PREPARE = 6,  // multi-column row mutation (atomic group)
  ROW_COMMIT = 7,   // commit marker pairing with ROW_PREPARE
  OP_ABORT = 8,     // reserved: explicit abort marker (prepare-without-commit is the implicit abort)
};

// ROW_PREPARE mut_type field values.
static constexpr uint8_t WAL_MUT_INSERT = 1u;
static constexpr uint8_t WAL_MUT_UPDATE = 2u;
static constexpr uint8_t WAL_MUT_DELETE = 3u;

// Persistent-record input bounds.  A single cell value (and the total column
// count of a ROW_PREPARE group) must be validated BEFORE any allocation or
// decode loop so a corrupted file cannot trigger an absurd std::vector::resize
// or an unbounded per-cell loop.
static constexpr uint64_t MAX_WAL_VALUE_SIZE = (1ull << 30);  // 1 GiB per cell
static constexpr uint32_t MAX_WAL_COLUMN_COUNT = 4096;

/**
 * Result of decoding one WAL record from the stream.
 *
 * EOF_REACHED is a normal termination condition.  TRUNCATED_TAIL indicates a
 * torn write at the very end of the file and is recoverable (the tail is
 * simply ignored).  BAD_MAGIC and CRC_MISMATCH indicate corruption in the
 * middle of the log and MUST abort recovery; treating them as end-of-log
 * would silently complete an incomplete recovery.
 */
enum class WalReadStatus : uint8_t { OK = 0, EOF_REACHED, TRUNCATED_TAIL, BAD_MAGIC, CRC_MISMATCH, IO_ERROR };

/**
 * One cell of a ROW_PREPARE group: the (column, value) pair to be re-applied.
 */
struct WalCell {
  uint32_t col_id{0};
  bool is_null{false};
  std::vector<uint8_t> value;
};

/**
 * In-memory representation of a single WAL record (after parsing).
 *
 * Legacy records (INSERT / UPDATE / DELETE / NULL_*) carry a single cell in
 * col_id / val_len / val_data.  ROW_PREPARE carries a full multi-column group
 * in `cells`; ROW_COMMIT only carries the op_id it commits.
 */
struct WalRecord {
  uint64_t lsn{0};
  WalOpType op_type{WalOpType::INSERT};
  uint32_t imcu_id{0};
  uint32_t col_id{0};  // legacy single-cell column id
  uint64_t row_id{0};
  uint64_t txn_id{0};
  uint64_t scn{0};
  uint64_t op_id{0};              // ROW_PREPARE/ROW_COMMIT pairing key
  uint8_t mut_type{0};            // ROW_PREPARE only: WAL_MUT_INSERT / WAL_MUT_UPDATE / WAL_MUT_DELETE
  size_t val_len{0};              // legacy single-cell; UNIV_SQL_NULL for NULL cells
  std::vector<uint8_t> val_data;  // legacy single-cell value
  std::vector<WalCell> cells;     // ROW_PREPARE multi-cell group

  // ROW_COMMIT payload (self-validating commit digest).
  uint64_t commit_lsn{0};     // == this record's lsn
  uint32_t redo_count{0};     // number of cells in the paired prepare
  uint32_t operation_crc{0};  // CRC32C over the paired prepare's logical cells
};

/**
 * Persisted per-IMCU checkpoint state inside recovery.manifest.
 */
enum class ManifestImcuState : uint8_t {
  NEVER_CHECKPOINTED = 0,
  CHECKPOINTED = 1,
};

struct ManifestImcuEntry {
  uint32_t imcu_id{0};
  ManifestImcuState state{ManifestImcuState::NEVER_CHECKPOINTED};
  uint64_t snapshot_next_lsn{0};
  uint64_t snapshot_size{0};
  uint32_t snapshot_crc{0};
  std::string snapshot_file;
};

/**
 * Per-table recovery manifest.  Authoritatively records which IMCUs exist,
 * whether each has a durable checkpoint, the schema fingerprint the snapshot
 * was written under, and the safe WAL truncation base LSN.
 */
struct RecoveryManifest {
  uint64_t table_id{0};
  uint64_t generation{0};
  uint64_t schema_fingerprint{0};
  uint64_t wal_base_lsn{0};  // min snapshot_next_lsn over CHECKPOINTED IMCUs (0 = none)
  std::vector<ManifestImcuEntry> imcus;
};
/**
 * TablePersistenceManager
 *
 * Singleton-style manager (one per table) that owns:
 *   • The WAL file for a single IMCS table partition.
 *   • Checkpoint logic (snapshot generation and loading).
 *   • The recovery entry point called at engine start.
 *
 * Usage (normal operation):
 *   auto mgr = std::make_shared<TablePersistenceManager>(data_dir, db, table);
 *   mgr->open();
 *
 *   // Before every DML:
 *   mgr->log_write(imcu_id, col_id, row_id, txn_id, scn, data, len);
 *   mgr->log_update(imcu_id, col_id, row_id, txn_id, scn, new_data, len);
 *   mgr->log_delete(imcu_id, col_id, row_id, txn_id, scn);
 *
 *   // After an IMCU becomes READ_ONLY:
 *   mgr->checkpoint(imcu);
 *
 * Usage (recovery at start-up):
 *   auto mgr = std::make_shared<TablePersistenceManager>(data_dir, db, table);
 *   auto imcus = load_imcu_list_from_catalog();   // existing IMCU objects
 *   mgr->recover(imcus);
 */
class TablePersistenceManager {
 public:
  /**
   * @param data_dir  Base data directory (e.g. MySQL datadir).
   * @param db_name   Database name.
   * @param tbl_name  Table name.
   */
  TablePersistenceManager(const std::string &data_dir, const std::string &db_name, const std::string &tbl_name);
  ~TablePersistenceManager();

  TablePersistenceManager(const TablePersistenceManager &) = delete;
  TablePersistenceManager &operator=(const TablePersistenceManager &) = delete;

  /** Open (or create) the WAL file.  Must be called before any log_*. */
  bool open();

  /** Flush and close the WAL file.  Safe to call more than once. */
  void close();

  /**
   * Discard every durable artefact of this table and start a fresh LSN epoch.
   *
   * Called when the table has just been rebuilt from InnoDB (the slow recovery
   * lane), which renumbers every row: the WAL records and checkpoint
   * generations on disk describe the previous layout and would be replayed
   * onto unrelated rows at the next restart.
   *
   * open() alone is not enough.  It zeroes the watermarks but keeps
   * m_written_lsn at the old file's high-water mark, so the checkpoint that
   * follows a reload computes boundary = m_applied_lsn + 1 = 1, publishes
   * wal_base_lsn = 1, and truncate_wal(1) then keeps every stale record.
   */
  bool reset_epoch();

  /** Path of the reload-required marker inside the table's own directory. */
  std::filesystem::path recovery_taint_path() const { return m_partition_dir / "reload_required"; }

  /**
   * Path of the second copy of the marker, in a tree separate from the table
   * directory. It exists so that a failure scoped to the table's own directory
   * (removed, quota, permissions) cannot take out the decision as well as the
   * data the decision is about. Both copies are read back by recovery_tainted().
   */
  std::filesystem::path recovery_taint_alt_path() const {
    return m_recovery_taint_root / m_db_name / m_tbl_name / "reload_required";
  }

  /**
   * Durably record that the persisted image must not be fast-restored: a reload
   * from InnoDB is required.
   *
   * Deliberately independent of the capture journal. Recovery consults it before
   * it will restore anything, so a failure of the journal's own machinery (the
   * channel that would normally revoke the certificate) cannot take out both the
   * judgement and the object being judged.
   *
   * @return true when the marker is durable.
   */
  bool mark_recovery_taint();

  /** True while the on-disk marker requires a reload from the primary. */
  bool recovery_tainted() const;

  /** Discharge the marker: the table has just been rebuilt from InnoDB. */
  void clear_recovery_taint();

  /**
   * Durably revoke every proof that would let a restart fast-restore this
   * table's live image.
   *
   * Called when change capture can no longer certify the image. The ladder is:
   * revoke the capture journal (INVALID record, else remove the journal and
   * fsync its directory); failing that, discard the whole epoch with
   * reset_epoch(); failing that, require recovery so no further append or
   * checkpoint can certify the image. The source transaction is never blocked
   * or failed: the primary keeps the committed rows, so a reload reconstructs
   * the table.
   *
   * @return true when a restart can no longer fast-restore the live image.
   */
  bool revoke_fast_recovery();

  /** Flush dirty WAL bytes to the OS buffer (fsync on the file). */
  bool sync();

  // WAL write API (called by IMCU/CU during normal operation)
  /**
   * Append an INSERT record.
   * val_data/val_len: the cell value to persist (UNIV_SQL_NULL for NULL).
   */
  bool log_write(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                 const uint8_t *val_data, size_t val_len);

  /**
   * Append an UPDATE record (new value).
   */
  bool log_update(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                  const uint8_t *new_val, size_t val_len);

  /**
   * Append a DELETE record.
   * @return the delete record LSN, or 0 if the append failed.
   */
  uint64_t log_delete(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn);

  /**
   * Append a ROW_PREPARE record covering every cell of a single row mutation.
   *
   * The record is written but NOT fsync'd here; the caller must call sync()
   * before mutating in-memory CU state so redo is durable before dirty memory.
   *
   * @param out_operation_crc  Optional output: CRC32C over the prepare's
   *                           logical cells, to be passed back to
   *                           log_row_commit() so the COMMIT record carries a
   *                           self-validating digest of the whole operation.
   * @return the op_id (== prepare record LSN) used to pair with
   *         log_row_commit(), or 0 if the append failed.
   */
  uint64_t log_row_prepare(uint32_t imcu_id, uint64_t row_id, uint64_t txn_id, uint64_t scn, uint8_t mut_type,
                           const std::vector<WalCell> &cells, uint32_t *out_operation_crc = nullptr);

  /**
   * Append + fsync the ROW_COMMIT marker for a previously prepared operation.
   *
   * The COMMIT record carries a digest of the paired prepare (cell count +
   * operation CRC) so recovery can validate the whole operation rather than
   * trusting op_id matching alone.
   *
   * @param op_id          op_id returned by log_row_prepare().
   * @param imcu_id        owning IMCU (must match the prepare record).
   * @param redo_count     number of cells in the paired prepare.
   * @param operation_crc  operation CRC returned by log_row_prepare().
   * @return the commit record LSN (durable), or 0 on failure.
   */
  uint64_t log_row_commit(uint64_t op_id, uint32_t imcu_id, uint32_t redo_count, uint32_t operation_crc);

  /**
    Durably mark a rolled-back transaction in the physical row WAL.

    Scope. Only the physical-replay branch of recover() (physical_replay=true)
    reads this marker, to drop rows written by a transaction that was later
    rolled back.  The startup path in use calls recover() with
    physical_replay=false and does not replay the physical WAL, so this marker
    plays no part in deciding which rows come back after a restart; that
    decision comes from source transaction outcomes in the capture WAL.  The
    record is still written so the physical log stays self-consistent for the
    replay branch.

    Known boundary of the physical-replay branch (not of the overall recovery
    scheme). It is compensation, not two-phase commit: the rows are already
    committed in the WAL when the host transaction decides, and the abort
    record cancels them afterwards. rollback_transaction() writes and fsyncs
    the abort BEFORE undoing anything in memory, which puts the window on the
    safe side -- abort durable, undo not yet applied, replays as "never
    happened". What it cannot cover is a crash after InnoDB has rolled the
    transaction back but before this fsync returns: the WAL then holds only
    the COMMIT records and a physical replay would resurrect the rolled-back
    rows.

    Closing it would need the operations to be PREPARE-only until the host
    transaction commits, so nothing is ever committed in the WAL that InnoDB
    might still undo.

    @return true when the record is durable.
  */
  bool log_abort(uint64_t txn_id);

  Recovery::WAL *wal() { return m_capture_enabled.load(std::memory_order_acquire) ? m_wal.get() : nullptr; }

  /**
   * Start capture for a table that is about to be (re)loaded from the primary.
   *
   * This is a full epoch reset, not just a new journal: checkpoint generations,
   * the physical WAL, the recovery-required flag and the reload-required marker of
   * the previous image are all discharged, because the load that follows renumbers
   * every row. The flag that makes wal() non-null is only raised once the journal
   * is usable.
   */
  bool enable_capture();

  // Checkpoint API (called by the periodic checkpoint scheduler, or after an
  // IMCU becomes READ_ONLY)
  /**
   * Checkpoint the whole table into one new, immutable generation.
   *
   * This is a table-level operation even though it takes a single IMCU:
   * `trigger` only identifies the owning table.  Every IMCU of that table is
   * frozen (mutation_mutex, exclusive) and saved, so all snapshots in the
   * generation share one boundary.
   *
   * Steps:
   *   1. Freeze all IMCUs and refuse if any has uncommitted changes.
   *   2. boundary = applied_lsn + 1, computed under the freeze.
   *   3. Serialize each IMCU (header + metadata + one serialized CU per
   *      column) into snapshots/checkpoint-<gen>.tmp/imcu_<id>.snap.
   *   4. Release the IMCU locks, fsync every snapshot file and the directory,
   *      then rename the directory to checkpoint-<gen>.
   *   5. Publish the capture cut, then persist the manifest
   *      (wal_base_lsn = boundary), then GC old generations.
   *
   * Nothing is published, and the next sweep retries, when: the manager is in
   * recovery-required state, the table has no IMCUs, the capture WAL is not
   * quiescent, any IMCU has uncommitted changes, the requested boundary is
   * not the safe one, or any I/O step fails.
   *
   * @param trigger             Any IMCU of the table to checkpoint.
   * @param snapshot_next_lsn   Optional assertion of the boundary.  The
   *                            snapshots contain every committed modification
   *                            with lsn < the boundary.  Pass 0 (normal) to
   *                            use the boundary computed under the freeze; a
   *                            non-zero value must equal it or the checkpoint
   *                            fails.
   * @return true when the generation and its manifest are durable.
   */
  bool checkpoint(Imcu *trigger, uint64_t snapshot_next_lsn = 0);

  /**
   * If the capture journal has held an unresolved source transaction for longer
   * than @a threshold_secs, durably revoke fast recovery so a restart reloads
   * the table from the primary instead of the journal blocking checkpoints (and
   * growing) forever. @a threshold_secs == 0 disables the check.
   * @return true when a revoke was performed.
   */
  bool revoke_if_unresolved_stale(uint64_t threshold_secs);

  /**
   * Load the snapshot for a specific IMCU from a specific checkpoint
   * generation.
   *
   * @param imcu        Target IMCU (already constructed, columns pre-allocated).
   * @param generation  Checkpoint generation to load from.
   * @return Result whose value is the snapshot's next-LSN boundary on OK; the
   *         error field is NOT_FOUND when no snapshot exists in that
   *         generation, CORRUPTION / IO_ERROR / CONFLICT when damaged.
   */
  Result<uint64_t> load_snapshot(Imcu *imcu, uint64_t generation);

  /**
   * Restore the checkpoint for all IMCUs of this table.
   *
   * Common to both modes:
   *   1. Select the newest generation whose manifest and every referenced
   *      snapshot validate (existence, size, CRC, header); fall back through
   *      older generations, never mixing two.
   *   2. Check table id and schema fingerprint against the live table
   *      (mismatch -> CONFLICT).
   *   3. load_snapshot() every IMCU; a snapshot that the manifest declares
   *      CHECKPOINTED but that is missing is CORRUPTION.
   *   4. Publish the LSN watermarks.
   *
   * physical_replay == false (what startup uses):
   *   Stop after step 4.  The physical WAL is not replayed because it may
   *   hold records of source transactions that are still ACTIVE; the catch-up
   *   after the checkpoint comes from the capture WAL, driven by the caller.
   *   Returns NOT_FOUND when no valid manifest exists.  The result value is 0.
   *
   * physical_replay == true (legacy branch, not used by startup):
   *   Additionally scan the physical WAL, drop transactions that have an
   *   OP_ABORT, pair ROW_PREPARE / ROW_COMMIT records (an unpaired prepare is
   *   discarded) and re-apply those at or after each IMCU's checkpoint LSN
   *   through `apply_fn`.
   *
   * @param imcus              All IMCU objects for this table.
   * @param apply_fn           Replay callback (WalRecord) -> ErrorCode; only
   *                           used when physical_replay is true.  A non-OK
   *                           return aborts recovery immediately.
   * @param physical_replay    See above.
   * @param restored_generation  Optional out: the generation that was loaded.
   * @return Result whose value is the number of WAL records replayed (0 when
   *         physical_replay is false); the error field is CORRUPTION /
   *         IO_ERROR / CONFLICT when the checkpoint or WAL is damaged or does
   *         not match the table, or the apply_fn error if replay failed.
   */
  Result<size_t> recover(const std::vector<Imcu *> &imcus, const std::function<ErrorCode(const WalRecord &)> &apply_fn,
                         bool physical_replay = true, uint64_t *restored_generation = nullptr);

  /** Current WAL LSN (monotonically increasing, next LSN to assign). */
  uint64_t current_lsn() const { return m_written_lsn.load(std::memory_order_acquire); }

  /** Watermarks: written >= durable >= applied must always hold. */
  uint64_t written_lsn() const { return m_written_lsn.load(std::memory_order_acquire); }
  uint64_t durable_lsn() const { return m_durable_lsn.load(std::memory_order_acquire); }
  uint64_t applied_lsn() const { return m_applied_lsn.load(std::memory_order_acquire); }

  /** Advance applied_lsn after an operation is fully published to memory. */
  void mark_applied(uint64_t lsn) {
    uint64_t cur = m_applied_lsn.load(std::memory_order_relaxed);
    while (cur < lsn &&
           !m_applied_lsn.compare_exchange_weak(cur, lsn, std::memory_order_release, std::memory_order_relaxed)) {
    }
  }

  /**
    Block until every WAL record up to @a lsn is durable.

    Group commit: concurrent callers share one flush. The first caller to find
    no flush in flight becomes the leader and flushes everything appended so
    far -- which covers every other waiter in the same window -- and the rest
    wait for it, so a burst of N row writes costs one flush rather than N.

    Returns false when the WAL is not open, when the flush failed (the manager
    is then in recovery-required state), or when it already was.

    Callers must not hold m_wal_mutex. Unlike sync(), this does not force a
    flush when @a lsn is already durable.
  */
  bool wait_durable(uint64_t lsn);

  /** WAL flushes performed so far. Group commit's whole point is to keep this
    below the number of durability waits; exposed for tests and diagnostics. */
  uint64_t flush_count() const { return m_flush_count.load(std::memory_order_acquire); }

  /**
   * True once a COMMIT fsync outcome became unknown (fsync failed after the
   * commit record was written).  In that state the engine cannot tell whether
   * the operation committed, so new WAL appends are refused until restart.
   */
  bool recovery_required() const { return m_recovery_required.load(std::memory_order_acquire); }

  /** Force the table into recovery-required state after an in-memory rollback
   *  itself fails and the live image can no longer be trusted. */
  void require_recovery() { m_recovery_required.store(true, std::memory_order_release); }

  /** Truncate WAL up to (but not including) `lsn`.  Rewrites the file. */
  bool truncate_wal(uint64_t up_to_lsn);

  /** Path to the WAL file. */
  std::filesystem::path wal_path() const { return m_wal_path; }

  /** Path to the snapshot for a given IMCU ID within a checkpoint generation. */
  std::filesystem::path snap_path(uint64_t generation, uint32_t imcu_id) const;

  /** Path to the manifest file of a checkpoint generation. */
  std::filesystem::path manifest_path(uint64_t generation) const;

  /** Highest checkpoint generation currently on disk (0 when none). */
  uint64_t latest_generation() const;

  /** All manifest generation numbers on disk, ascending. */
  std::vector<uint64_t> list_manifest_generations() const;

  /**
   * Load and validate the manifest of a specific generation.
   * @return NOT_FOUND when no such manifest exists, CORRUPTION when the file is
   *         damaged, otherwise OK with the parsed manifest.
   */
  Result<RecoveryManifest> load_manifest(uint64_t generation) const;

  /** Durably persist a checkpoint-generation manifest (atomic tmp→rename→dirfsync). */
  bool persist_manifest(const RecoveryManifest &manifest);

  /** Remove one checkpoint generation (snapshot dir + manifest). Best-effort GC. */
  bool remove_generation(uint64_t generation);

 private:
  bool append_record(WalRecord &rec);
  WalReadStatus read_record(std::istream &in, WalRecord &rec) const;

  /** Serialize a WAL record to a byte buffer (including CRC). */
  std::vector<uint8_t> encode_record(const WalRecord &rec) const;

  template <typename T>
  static void write_pod(std::ostream &out, const T &v) {
    out.write(reinterpret_cast<const char *>(&v), sizeof(T));
  }
  template <typename T>
  static bool read_pod(std::istream &in, T &v) {
    return static_cast<bool>(in.read(reinterpret_cast<char *>(&v), sizeof(T)));
  }

  /** Write the 36-byte snapshot file header. */
  bool write_snap_header(std::ostream &out, uint32_t imcu_id, uint32_t col_count, uint64_t snap_lsn) const;

  /** Read and validate the snapshot file header. */
  bool read_snap_header(std::istream &in, uint32_t &imcu_id, uint32_t &col_count, uint64_t &snap_lsn) const;

  /** Serialize IMCU-level metadata (current_rows, del/null masks, ...). */
  bool write_imcu_metadata(std::ostream &out, const Imcu *imcu) const;

  /** Restore IMCU-level metadata from a snapshot. */
  bool read_imcu_metadata(std::istream &in, Imcu *imcu) const;

  /** Serialize one IMCU's full snapshot into `out` (header + metadata + CUs). */
  bool serialize_imcu(Imcu *imcu, uint64_t snapshot_next_lsn, std::string &out) const;

  /** Drop checkpoint generations beyond kMaxRetainedGenerations. */
  void gc_old_generations();

  void close_locked();

  /** reset_epoch() body; @a reset_capture_wal says whether the capture journal is part of the reset. */
  bool reset_epoch_impl(bool reset_capture_wal);

#ifndef NDEBUG
  /** Debug builds track every flush, independently of thread-local DBUG flags. */
  bool note_durable_bytes();
  bool invalidate_durable_marker();
  bool apply_simulated_power_cut();
  std::filesystem::path durable_marker_path() const { return m_partition_dir / "cu_wal.durable"; }
#endif

  std::string m_db_name;
  std::string m_tbl_name;

  std::filesystem::path m_partition_dir;        // <data_dir>/<db>/<table>/
  std::filesystem::path m_recovery_taint_root;  // sibling of <data_dir>, holds the marker's second copy
  std::filesystem::path m_wal_path;             // m_partition_dir / "cu_wal.log"

  std::unique_ptr<Recovery::WAL> m_wal;
  std::atomic<bool> m_capture_enabled{false};

  Recovery::DurableFile m_wal_file;  // fd-backed append writer (explicit durability boundary)

  // WAL length after the open() base plus every append_record() since. Only the
  // power-cut simulation reads it; all accesses hold m_wal_mutex.
  uint64_t m_appended_bytes{0};
  mutable std::mutex m_wal_mutex;  // serialises LSN assignment + WAL append + flush
  // Highest appended LSN. Atomic because the group-commit leader reads it to
  // size its flush without holding m_wal_mutex, which the flush itself needs.
  std::atomic<uint64_t> m_last_appended_lsn{0};

  // Group-commit state. A durability waiter either flushes everything appended
  // so far (leader) or waits for the flush already in flight (follower).
  mutable std::mutex m_flush_mutex;
  mutable std::condition_variable m_flush_cv;
  bool m_flushing{false};  // guarded by m_flush_mutex
  std::atomic<uint64_t> m_flush_count{0};

  // Serialises checkpoint publication/GC with WAL truncation policy decisions.
  mutable std::mutex m_checkpoint_mutex;

  // Recovery watermarks.  Invariant: applied <= durable < written.
  //   written_lsn: next LSN to assign (append high-water).
  //   durable_lsn: highest LSN known to have been fdatasync'd.
  //   applied_lsn: highest operation commit published to in-memory state.
  std::atomic<uint64_t> m_written_lsn{1};  // 1-based
  std::atomic<uint64_t> m_durable_lsn{0};
  std::atomic<uint64_t> m_applied_lsn{0};

  // Set when a COMMIT fsync failed after its record was appended: the outcome
  // is unknown, so the engine must recover before accepting further writes.
  std::atomic<bool> m_recovery_required{false};
};
}  // namespace Imcs
}  // namespace ShannonBase

#endif  // __SHANNONBASE_TABLE_PERSISTENCE_H__