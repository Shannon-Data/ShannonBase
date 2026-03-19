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

#include "my_inttypes.h"  // uint32, uint64
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
        b. Replay WAL records whose LSN > snapshot_lsn.
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
};

/**
 * In-memory representation of a single WAL record (after parsing).
 */
struct WalRecord {
  uint64_t lsn{0};
  WalOpType op_type{WalOpType::INSERT};
  uint32_t imcu_id{0};
  uint32_t col_id{0};
  uint64_t row_id{0};
  uint64_t txn_id{0};
  uint64_t scn{0};
  size_t val_len{0};  // UNIV_SQL_NULL for NULL cells
  std::vector<uint8_t> val_data;
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
   */
  bool log_delete(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn);

  // Checkpoint API (called by IMCU when it becomes READ_ONLY, or by a periodic checkpoint thread)
  /**
   * Write a full snapshot of every CU in the given IMCU to disk.
   *
   * Snapshot file:  <partition_dir>/imcu_<imcu_id>.snap
   *
   * The snapshot includes the current WAL LSN so that recover() knows which
   * WAL records post-date it.
   *
   * @param imcu            IMCU to checkpoint.
   * @param snapshot_lsn    Current WAL head LSN at the moment of snapshotting.
   *                        Pass 0 to use the current WAL LSN automatically.
   * @return true on success.
   */
  bool checkpoint(Imcu *imcu, uint64_t snapshot_lsn = 0);

  /**
   * Load the most-recent snapshot for a specific IMCU from disk.
   *
   * @param imcu   Target IMCU (already constructed, columns pre-allocated).
   * @return LSN stored in the snapshot (0 if no snapshot found).
   */
  uint64_t load_snapshot(Imcu *imcu);

  /**
   * Recover all IMCUs for this table.
   *
   * Algorithm:
   *   1. For each IMCU in `imcus`, call load_snapshot() to restore the last
   *      checkpoint.  Track the minimum snapshot_lsn across all IMCUs.
   *   2. Scan the WAL from the beginning; skip records with
   *      lsn ≤ snapshot_lsn for the corresponding IMCU.
   *   3. Apply remaining WAL records by calling the supplied `apply_fn`
   *      callback (caller knows how to route a WalRecord to the right IMCU/CU).
   *   4. Return the number of WAL records replayed.
   *
   * @param imcus     All IMCU objects for this table.
   * @param apply_fn  Callback: (WalRecord) → void.
   * @return Number of WAL records replayed.
   */
  size_t recover(const std::vector<Imcu *> &imcus, const std::function<void(const WalRecord &)> &apply_fn);

  /** Current WAL LSN (monotonically increasing). */
  uint64_t current_lsn() const { return m_lsn.load(std::memory_order_acquire); }

  /** Truncate WAL up to (but not including) `lsn`.  Rewrites the file. */
  bool truncate_wal(uint64_t up_to_lsn);

  /** Path to the WAL file. */
  std::filesystem::path wal_path() const { return m_wal_path; }

  /** Path to the snapshot for a given IMCU ID. */
  std::filesystem::path snap_path(uint32_t imcu_id) const;

 private:
  static constexpr size_t WAL_FOOTER_SIZE = 16;
  // Footer layout: [MAGIC_FOOT 4B][reserved 4B][max_lsn 8B]
  static constexpr uint32_t WAL_FOOT_MAGIC = 0x544F4F46u;  // "FOOT"

  bool append_record(const WalRecord &rec);
  bool read_record(std::istream &in, WalRecord &rec) const;
  void update_wal_footer(uint64_t current_max_lsn);

  /** Serialize a WAL record to a byte buffer (including CRC). */
  std::vector<uint8_t> encode_record(const WalRecord &rec) const;

  /** CRC-32 (ISO 3309). */
  static uint32_t crc32(const void *data, size_t len, uint32_t seed = 0);

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

  void close_locked();

  std::string m_db_name;
  std::string m_tbl_name;

  std::filesystem::path m_partition_dir;  // <data_dir>/<db>/<table>/
  std::filesystem::path m_wal_path;       // m_partition_dir / "cu_wal.log"

  std::ofstream m_wal_out;         // append-mode WAL writer
  int m_wal_fd{-1};                // O_WRONLY fd for fdatasync; -1 when closed
  mutable std::mutex m_wal_mutex;  // serialises all WAL appends

  std::atomic<uint64_t> m_lsn{1};  // next LSN to assign (1-based)
};
}  // namespace Imcs
}  // namespace ShannonBase

#endif  // __SHANNONBASE_CU_RECOVERY_H__