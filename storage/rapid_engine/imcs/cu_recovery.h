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
#ifndef __SHANNONBASE_CU_RECOVERY_H__
#define __SHANNONBASE_CU_RECOVERY_H__

#include <atomic>
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
/*
   CU Persistence & Recovery sub-system

   Overview
   The Rapid Engine stores all data in volatile DRAM.  After a crash or clean
   shutdown the In-Memory Column Store (IMCS) must be rebuilt.  The original
   approach was a full DDL-replay ("cold load") from InnoDB, which is both
   slow and introduces a consistency window during which IMCS queries are
   unavailable.

   This module adds a two-tier persistence layer that eliminates that window:

     Tier 1 – Checkpoint snapshots
       Each IMCU writes its CUs to a per-IMCU snapshot file when it becomes
       READ_ONLY (full) or when a periodic checkpoint fires.  A snapshot is an
       append of one serialized CU (via CU::serialize()) per column.

         File name:  <data_dir>/<db>/<table>/imcu_<imcu_id>.snap

     Tier 2 – Write-Ahead Log (WAL)
       Every INSERT / UPDATE / DELETE that modifies a CU appends a compact
       redo record to a shared WAL file before the in-memory write is applied.
       On recovery the WAL is replayed on top of the last checkpoint snapshot.

         File name:  <data_dir>/<db>/<table>/cu_wal.log

   Recovery sequence (executed at engine start-up)
     1. For each table in the Rapid catalog:
        a. Discover all *.snap files → load the most-recent snapshot per IMCU.
        b. Replay WAL records whose LSN >= snapshot_next_lsn.
        c. Mark the IMCU as READ_ONLY once replay is complete.
     2. Drop any WAL records that are superseded by the loaded snapshot
        (i.e. WAL truncation / log recycling).

   WAL record format
     [Magic   4 B]  WAL_MAGIC  = 0x4C41574C ("LWAK" LE)
     [LSN     8 B]  monotonically increasing
     [OpType  1 B]  WalOpType enum
     [ImcuId  4 B]
     [ColId   4 B]
     [RowId   8 B]
     [TxnId   8 B]
     [SCN     8 B]
     [ValLen  8 B]  UNIV_SQL_NULL for NULL, 0 for DELETE
     [ValData N B]  absent when ValLen ∈ {0, UNIV_SQL_NULL}
     [CRC32   4 B]  of all preceding bytes in this record

   Thread-safety
   The WAL writer uses a dedicated mutex; multiple threads can call
   log_write() / log_update() / log_delete() concurrently and each will get
   a unique LSN.

   The checkpoint writer holds the IMCU header mutex (read mode) while
   serializing CUs, mirroring the same lock ordering used by the scan path.
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
 * CURecoveryManager
 *
 * Singleton-style manager (one per table) that owns:
 *   • The WAL file for a single IMCS table partition.
 *   • Checkpoint logic (snapshot generation and loading).
 *   • The recovery entry point called at engine start.
 *
 * Usage (normal operation):
 *   auto mgr = std::make_shared<CURecoveryManager>(data_dir, db, table);
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
 *   auto mgr = std::make_shared<CURecoveryManager>(data_dir, db, table);
 *   auto imcus = load_imcu_list_from_catalog();   // existing IMCU objects
 *   mgr->recover(imcus);
 */
class CURecoveryManager {
 public:
  /**
   * @param data_dir  Base data directory (e.g. MySQL datadir).
   * @param db_name   Database name.
   * @param tbl_name  Table name.
   */
  CURecoveryManager(const std::string &data_dir, const std::string &db_name, const std::string &tbl_name);
  ~CURecoveryManager();

  CURecoveryManager(const CURecoveryManager &) = delete;
  CURecoveryManager &operator=(const CURecoveryManager &) = delete;

  /** Open (or create) the WAL file.  Must be called before any log_*. */
  bool open();

  /** Flush and close the WAL file.  Safe to call more than once. */
  void close();

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

  // Checkpoint API (called by IMCU when it becomes READ_ONLY, or by a periodic checkpoint thread)
  /**
   * Write a full snapshot of every CU in the given IMCU to disk.
   *
   * Snapshot file:  <partition_dir>/imcu_<imcu_id>.snap
   *
   * The snapshot includes the current WAL LSN so that recover() knows which
   * WAL records post-date it.
   *
   * @param imcu                IMCU to checkpoint.
   * @param snapshot_next_lsn   The next LSN to be assigned at the moment of
   *                            snapshotting.  The snapshot therefore contains
   *                            every modification with lsn < snapshot_next_lsn.
   *                            Pass 0 to capture the current WAL LSN under the
   *                            freeze lock automatically.
   * @return true on success.
   */
  bool checkpoint(Imcu *imcu, uint64_t snapshot_next_lsn = 0);

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
   * Recover all IMCUs for this table.
   *
   * Algorithm:
   *   1. For each IMCU in `imcus`, call load_snapshot() to restore the last
   *      checkpoint.  Track the minimum snapshot_next_lsn across all IMCUs.
   *   2. Scan the WAL from the beginning; skip records with
   *      lsn < snapshot_next_lsn for the corresponding IMCU.
   *   3. Apply remaining WAL records by calling the supplied `apply_fn`
   *      callback (caller knows how to route a WalRecord to the right IMCU/CU).
   *   4. Return the number of WAL records replayed.
   *
   * @param imcus     All IMCU objects for this table.
   * @param apply_fn  Callback: (WalRecord) → ErrorCode.  A non-OK return
   *                  aborts recovery immediately so replay failures are not
   *                  silently reported as a successful recovery.
   * @return Result whose value is the number of WAL records replayed; the
   *         error field is set to ErrorCode::CORRUPTION (or IO_ERROR) if the
   *         WAL is damaged, or to the apply_fn error if replay failed.
   */
  Result<size_t> recover(const std::vector<Imcu *> &imcus, const std::function<ErrorCode(const WalRecord &)> &apply_fn);

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
  void remove_generation(uint64_t generation);

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

  std::string m_db_name;
  std::string m_tbl_name;

  std::filesystem::path m_partition_dir;  // <data_dir>/<db>/<table>/
  std::filesystem::path m_wal_path;       // m_partition_dir / "cu_wal.log"

  Recovery::DurableFile m_wal_file;  // fd-backed append writer (explicit durability boundary)
  mutable std::mutex m_wal_mutex;    // serialises LSN assignment + WAL append + sync
  uint64_t m_last_appended_lsn{0};   // protected by m_wal_mutex

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

#endif  // __SHANNONBASE_CU_RECOVERY_H__