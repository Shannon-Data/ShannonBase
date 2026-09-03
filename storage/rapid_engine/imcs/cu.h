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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Column Unit.
*/
#ifndef __SHANNONBASE_CU_H__
#define __SHANNONBASE_CU_H__

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "field_types.h"  // MYSQL_TYPE_XXX
#include "my_inttypes.h"  // uintxxx

#include "sql/log.h"  // LogErr

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_types.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/imcs/varlen0data.h"

class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;
class Transaction;
namespace Imcs {
class Dictionary;
class RpdTable;
class Imcu;

// Magic stored as little-endian uint32 == "SHCU" (0x55 48 43 53).
static constexpr uint32_t CU_SERIAL_MAGIC = 0x55484353u;
static constexpr uint16_t CU_FORMAT_VERSION = 1u;

// Bit-flags in the 1-byte Flags field of the binary header.
static constexpr uint8_t CU_FLAG_COMPRESSED = 0x01u;
static constexpr uint8_t CU_FLAG_HAS_DICT = 0x02u;
static constexpr uint8_t CU_FLAG_HAS_VERSIONS = 0x04u;
static constexpr uint8_t CU_FLAG_HAS_VARLEN = 0x08u;

/**
 * Algorithm tag persisted in the snapshot (decoupled from the runtime enum so
 * that the on-disk format stays stable even if Compress::COMPRESS_ALGO changes).
 */
enum class CU_CompressAlgo : uint8_t { NONE = 0, LZ4 = 1, ZSTD = 2, ZLIB = 3 };

class CU : public MemoryObject {
 public:
  // Copyable per-column descriptor: the subset of FieldMetadata that a CU
  // needs at runtime and for its serialized header.  Kept separate from
  // CU_header so the CU owns one coherent copy instead of duplicating each
  // scalar field.
  struct CUFieldDesc {
    Field *src_field{nullptr};  // mysql field which this CU is based on.
    enum_field_types type{MYSQL_TYPE_NULL};
    size_t pack_length{0};
    size_t normalized_length{0};
    const CHARSET_INFO *charset{nullptr};

    Compress::ENCODING_TYPE encoding{Compress::ENCODING_TYPE::NONE};
    Compress::COMPRESS_LEVEL compression_level{Compress::COMPRESS_LEVEL::DEFAULT};
    std::shared_ptr<Compress::Dictionary> dictionary{nullptr};

    inline enum_field_types real_type() const { return src_field->real_type(); }
  };

  struct SHANNON_ALIGNAS CU_header {
    Imcu *owner_imcu{nullptr};
    // NOTE: column_id is set from FieldMetadata::field_id at construction
    // time for consistency across the serialization format and the runtime
    // field-index map used by IMCU's m_cu_array / m_column_units.
    uint32 column_id{0};

    // Per-column metadata (copy of the relevant FieldMetadata subset).
    CUFieldDesc field_desc;

    // Zone-map statistics (per-CU min/max/sum).
    std::atomic<double> min_value{DBL_MAX};
    std::atomic<double> max_value{std::numeric_limits<double>::lowest()};
    std::atomic<double> sum{0};
  };

 public:
  CU(Imcu *owner, const FieldMetadata &field_meta, uint32 col_idx, size_t capacity,
     std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool);

  virtual ~CU() = default;

  /**
   * Snapshot-resolved cell in the same normalized internal representation used
   * by the CU slot / ColumnChunk pipeline.
   *
   * `slot` either borrows the current CU slot or points into `owned_slot` when
   * an older before-image had to be reconstructed.
   */
  struct VisibleCell {
    bool is_null{true};
    size_t logical_length{0};
    const uchar *slot{nullptr};
    std::vector<uchar> owned_slot;

    void reset() {
      is_null = true;
      logical_length = 0;
      slot = nullptr;
      owned_slot.clear();
    }
  };

  struct RollbackCell {
    row_id_t row_id{0};
    bool is_null{false};
    size_t logical_length{0};
  };

  int write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len);

  int update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len);

  /**
   * Undo the most recent update to a cell by restoring the pre-update value
   * recorded in the version journal.  No new version entry is created.
   * @return SHANNON_SUCCESS, or HA_ERR_GENERIC if there is nothing to undo.
   */
  int rollback_update(row_id_t local_row_id, Transaction::ID expected_txn_id = Transaction::MAX_ID,
                      bool *restored_is_null = nullptr, size_t *restored_logical_length = nullptr);

  // Finalize / undo every active before-image produced by txn_id in this CU.
  void commit_transaction(Transaction::ID txn_id, uint64_t commit_scn);
  bool rollback_transaction(Transaction::ID txn_id, std::vector<RollbackCell> &restored_cells);

  /**
   * Resolve a cell for a SQL snapshot.  InnoDB ReadView decides creator
   * visibility; reader_scn is only the no-ReadView fallback.
   */
  bool get_visible_cell(row_id_t local_row_id, bool current_is_null, Transaction::ID reader_txn_id, uint64_t reader_scn,
                        Transaction *reader_trx, const char *table_name, VisibleCell &out) const;

  bool has_version_in_range(row_id_t start_row, size_t count) const;

  /**
   * Read the current value of a cell.
   * If the CU is currently compressed the whole block is transparently
   * decompressed before reading (write-through: m_data is updated in-place).
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer);

  inline size_t get_version_count(const Rapid_context *) const { return m_version_manager->get_version_count(); }
  size_t purge_versions(const Rapid_context *context, uint64_t min_active_scn);

  /**
   * Compress the data payload in-place.
   *
   * Algorithm selection (in priority order):
   *   1. If compression_level == NONE  → no-op, return false.
   *   2. Try LZ4  (low latency, good ratio for columnar numeric data).
   *   3. If LZ4 saves < 10 %,  try ZSTD level-3.
   *   4. If neither saves ≥ 10 %, leave the CU uncompressed and return false.
   *
   * Memory model
   * m_data was allocated with capacity = rows × normalized_length (≥ 64 KiB).
   * The compressed payload is always ≤ the original, so writing it back into
   * the same pool-allocated buffer is always safe.  The original row-count and
   * uncompressed byte-count are saved in m_original_data_size so that
   * decompress() can restore the buffer exactly.
   *
   * Thread-safety: acquires m_data_mutex exclusively.
   *
   * @return true  – data is now compressed (ratio ≥ threshold).
   *         false – CU is unchanged (already compressed, too small, no gain).
   */
  int compress();

  /**
   * Decompress the data payload in-place.
   *
   * Allocates a temporary heap buffer, decompresses from the front of m_data
   * into it, then copies the uncompressed bytes back.  After this call m_data
   * is again directly addressable at the usual row/stride offsets.
   *
   * Thread-safety: acquires m_data_mutex exclusively.
   *
   * @return true on success; false if the CU was not compressed or on error.
   */
  int decompress();

  /** Returns true while m_data holds a compressed payload. */
  bool is_compressed() const { return m_is_compressed.load(std::memory_order_acquire); }

  /**
   * Serialize the complete CU state to a binary output stream.
   *
   * Binary layout (all multi-byte fields little-endian):
   * ┌──────────────────────────────────────────────────────────────┐
   * │ Fixed header  (20 bytes)                                     │
   * │   magic          4 B  CU_SERIAL_MAGIC = "SHCU"               │
   * │   version        2 B  CU_FORMAT_VERSION                      │
   * │   flags          1 B  CU_FLAG_COMPRESSED | HAS_DICT |        │
   * │                       HAS_VERSIONS                           │
   * │   column_id      4 B                                         │
   * │   field_type     1 B  enum_field_types                       │
   * │   encoding       1 B  Compress::ENCODING_TYPE                │
   * │   compress_algo  1 B  CU_CompressAlgo (data payload)         │
   * │   reserved       6 B  (zero-padded)                          │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Lengths  (32 bytes)                                          │
   * │   pack_length          8 B                                   │
   * │   normalized_length    8 B                                   │
   * │   original_data_size   8 B  uncompressed byte count          │
   * │   row_count            8 B  rows included in this snapshot   │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Zone map  (24 bytes)                                         │
   * │   min_value  8 B  double                                     │
   * │   max_value  8 B  double                                     │
   * │   sum        8 B  double                                     │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Data payload                                                 │
   * │   payload_size  8 B  (bytes that follow; ≤ original_size)    │
   * │   payload       N B                                          │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Dictionary  (only when CU_FLAG_HAS_DICT is set)              │
   * │   entry_count  4 B                                           │
   * │   for each entry:                                            │
   * │     entry_size  8 B                                          │
   * │     entry_data  N B  (flag byte + payload as stored)         │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Version journal  (only when CU_FLAG_HAS_VERSIONS is set)     │
   * │   version_entry_count  8 B                                   │
   * │   for each entry:                                            │
   * │     row_id     8 B                                           │
   * │     txn_id     8 B                                           │
   * │     scn        8 B                                           │
   * │     val_len    8 B  (UNIV_SQL_NULL sentinel for NULL cells)  │
   * │     val_data   N B  (absent when val_len == UNIV_SQL_NULL)   │
   * ├──────────────────────────────────────────────────────────────┤
   * │ Trailer                                                      │
   * │   checksum  4 B  CRC-32C           of all preceding bytes    │
   * └──────────────────────────────────────────────────────────────┘
   *
   * @param out                 Binary output stream.
   * @param snapshot_row_count  Rows to snapshot; 0 = IMCU's current count.
   * @return SHANNON_SUCCESS (0) on success, otherwise an error code.
   */
  int serialize(std::ostream &out, size_t snapshot_row_count = 0) const;

  /**
   * Restore a CU from a binary snapshot produced by serialize().
   *
   * The CU must already be constructed (owner Imcu set, memory allocated).
   * CRC-32C is verified before any in-memory state is modified, so a corrupt
   * file leaves the CU unchanged.
   *
   * @param in  Binary input stream.
   * @return SHANNON_SUCCESS (0) on success, otherwise an error code.
   */
  int deserialize(std::istream &in);

  inline void patch_field_metadata(Field *f, const CHARSET_INFO *cs) {
    m_header.field_desc.src_field = f;
    m_header.field_desc.charset = cs;
  }

  void update_statistics(const uchar *data, size_t len);
  ColumnStatistics get_statistics() const;

  // Table-level (tier 2) statistics object for this CU's column, or nullptr if
  // this CU is not attached to a table yet. Resolved through owner_imcu, so it
  // must not be called before set_owner().
  ColumnStatistics *table_column_statistics() const;

  inline Field *field() const { return m_header.field_desc.src_field; }
  inline enum_field_types type() const { return m_header.field_desc.type; }
  inline enum_field_types real_type() const { return m_header.field_desc.real_type(); }
  inline Compress::Dictionary *dictionary() const { return m_header.field_desc.dictionary.get(); }
  inline size_t get_normalized_length() const { return m_header.field_desc.normalized_length; }
  inline Field *get_source_field() const { return m_header.field_desc.src_field; }

  inline double get_min_value() const { return m_header.min_value.load(); }
  inline double get_max_value() const { return m_header.max_value.load(); }

  const uchar *get_data_address(row_id_t local_row_id) const;
  size_t get_data_size() const;

  /**
   * Resolve the actual data pointer for a row, transparently handling
   * VarlenDataPool indirection for BLOB / TEXT columns.
   *
   * - Columns without a VarlenDataPool: same as get_data_address().
   * - Columns with a VarlenDataPool: the m_data slot holds a
   *   VarlenReference; resolve_data() follows it to the real payload
   *   (inline, pool-allocated, or overflow page).
   */
  VarlenDataPool::VarlenReadGuard resolve_data(row_id_t local_row_id) const;

  /**
   * Resolve a pool-backed reference carried in a ColumnChunk.  This is required
   * for historical MVCC slots: resolving by row_id would incorrectly jump to
   * the current physical slot.
   */
  VarlenDataPool::VarlenReadGuard resolve_data(const VarlenDataPool::VarlenReference &ref) const;

  size_t get_logical_length(row_id_t local_row_id) const;

  /** True when this CU uses a VarlenDataPool for large-value storage. */
  inline bool has_varlen_pool() const { return m_varlen_pool != nullptr; }

  /** Return the VarlenDataPool, or nullptr if this CU does not use one. */
  inline VarlenDataPool *get_varlen_pool() const { return m_varlen_pool.get(); }

 private:
  inline bool needs_dictionary() const {
    return (m_header.field_desc.type == MYSQL_TYPE_VARCHAR || m_header.field_desc.type == MYSQL_TYPE_STRING ||
            m_header.field_desc.type == MYSQL_TYPE_VAR_STRING);
  }

  /** True for BLOB / TEXT types that benefit from VarlenDataPool overflow. */
  inline bool needs_varlen_pool() const {
    switch (m_header.field_desc.type) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_VECTOR:  // VECTOR(N) can be large
        return true;
      case MYSQL_TYPE_BIT:
        // BIT(N) with N > 64 has pack_length larger than what fits in an
        // inline slot, but only create the varlen pool when the slot is
        // at least large enough to hold a complete VarlenReference.
        return (m_header.field_desc.normalized_length >= VarlenDataPool::VARLEN_REF_SIZE);
      default:
        return false;
    }
  }

  /** Types that must NEVER go through dictionary encoding (blob-like, unique values). */
  inline bool is_blob_like() const {
    switch (m_header.field_desc.type) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_VECTOR:
        return true;
      default:
        return false;
    }
  }

  /** Decompress without locking.  Caller MUST hold m_data_mutex write-lock. */
  int decompress_locked();

  /**
   * Decompress a single stripe without locking.
   * Caller MUST hold m_data_mutex shared-lock.
   * @param stripe_idx which stripe to decompress
   * @param out_buffer caller-allocated buffer of stripe_size bytes
   * @return true on success
   */
  bool decompress_stripe_locked(size_t stripe_idx, uchar *out_buffer) const;

  /** Invalidate all stripe compressed data (called after writes). */
  void invalidate_stripes_locked();

  /** Number of rows per compression stripe. */
  static constexpr size_t STRIPE_ROWS = 4096;

  /**
   * Pick the best compressor for this CU and optionally return the algo tag.
   * Default: LZ4 (best latency; ZSTD as fallback for high-compression mode).
   */
  Compress::CompressAlgorithm *select_compressor(CU_CompressAlgo *algo_out = nullptr) const;

  template <typename T>
  static void write_pod(std::ostream &out, const T &v) {
    out.write(reinterpret_cast<const char *>(&v), sizeof(T));
  }
  template <typename T>
  static bool read_pod(std::istream &in, T &v) {
    return static_cast<bool>(in.read(reinterpret_cast<char *>(&v), sizeof(T)));
  }

  const char *m_magic = "SHANNON_CU";
  CU_header m_header;

  // Pool-owned main data buffer.
  struct PoolDeleter {
    std::weak_ptr<ShannonBase::Utils::MemoryPool> pool;
    size_t size;

    PoolDeleter(std::shared_ptr<ShannonBase::Utils::MemoryPool> p, size_t s) : pool(p), size(s) {}
    PoolDeleter() : pool(), size(0) {}
    PoolDeleter(const PoolDeleter &) = default;
    PoolDeleter &operator=(const PoolDeleter &) = default;

    void operator()(uchar *ptr) const {
      auto sp = pool.lock();
      if (sp && ptr) {
        sp->deallocate(ptr, size);
      } else if (ptr && !sp) {
        LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
               "CU::PoolDeleter: memory pool already released; "
               "skipping deallocation of %zu bytes",
               size);
      }
    }
  };

  std::unique_ptr<uchar[], PoolDeleter> m_data;
  std::atomic<size_t> m_data_capacity{0};  // allocated bytes in m_data

  // --- Stripe-based compression (latency-optimized) ---
  // Each stripe covers STRIPE_ROWS rows and is compressed independently,
  // so point queries only decompress ~32 KB instead of the entire CU.
  struct Stripe {
    std::unique_ptr<uchar[], PoolDeleter> compressed_data;
    size_t compressed_size{0};
    CU_CompressAlgo algo{CU_CompressAlgo::NONE};
    bool active{false};  // true when this stripe holds valid compressed data
  };
  std::vector<Stripe> m_stripes;
  size_t m_num_stripes{0};
  // When true, stripe data is valid and can be used for on-demand decompression.
  std::atomic<bool> m_stripes_valid{false};

  std::atomic<bool> m_is_compressed{false};
  std::atomic<size_t> m_original_data_size{0};    // uncompressed byte count
  std::atomic<size_t> m_compressed_data_size{0};  // compressed byte count in m_data
  std::atomic<uint8_t> m_compress_algo_used{      // algo that produced payload
                                            static_cast<uint8_t>(CU_CompressAlgo::NONE)};

  std::unique_ptr<VarlenDataPool> m_varlen_pool;

  // MVCC Version Management
  class ColumnVersionManager {
   public:
    struct SHANNON_ALIGNAS Column_Version {
      Transaction::ID txn_id{Transaction::MAX_ID};
      uint64_t scn{0};  // 0 => ACTIVE; finalized to primary commit sequence
      std::chrono::system_clock::time_point timestamp;

      // Logical before-image. Kept for stable serialization/recovery.
      std::unique_ptr<uchar[]> old_value{nullptr};
      size_t value_length{0};

      // Normalized physical before-image used directly by the vectorized batch
      // path. For dictionary columns this contains the old dict-id; for varlen
      // columns it contains the old VarlenReference.
      std::vector<uchar> old_slot;

      // A pool-backed old VarlenReference must stay alive as long as this
      // version can be selected by any ReadView.
      VarlenDataPool *retained_pool{nullptr};
      bool owns_varlen_ref{false};

      std::unique_ptr<Column_Version> prev{nullptr};

      void retire_retained_ref() {
        if (!owns_varlen_ref || retained_pool == nullptr || old_slot.empty()) return;
        VarlenDataPool::VarlenReference ref{};
        std::memcpy(&ref, old_slot.data(), std::min(sizeof(ref), old_slot.size()));
        if (!ref.is_inline() && ref.block_id != 0) retained_pool->retire(ref);
        owns_varlen_ref = false;
        retained_pool = nullptr;
      }

      // Transfer ownership of the retained old value back to the current slot
      // during rollback.
      void relinquish_varlen_ownership() {
        owns_varlen_ref = false;
        retained_pool = nullptr;
      }

      ~Column_Version() { retire_retained_ref(); }
    };

    struct BeforeImage {
      bool found{false};
      bool is_null{false};
      size_t logical_length{0};
      std::vector<uchar> slot;
      std::vector<uchar> logical_value;
    };

   private:
    struct ActiveVersionRef {
      row_id_t row_id{0};
      Column_Version *version{nullptr};
    };

    std::unordered_map<row_id_t, std::unique_ptr<Column_Version>> m_versions;
    std::unordered_map<Transaction::ID, std::vector<ActiveVersionRef>> m_active_versions;
    mutable std::shared_mutex m_mutex;

    // Lock-free mirror of m_versions.size(), republished by every mutator while
    // it still holds m_mutex exclusively.  Readers use it to skip the shared
    // lock and hash probe on columns that have no update history at all.
    std::atomic<size_t> m_versioned_rows{0};

    void untrack_version_locked(Column_Version *version);

    // Must be called with m_mutex held exclusively.
    inline void publish_versioned_rows() { m_versioned_rows.store(m_versions.size(), std::memory_order_release); }

    class VersionedRowsLock {
      ColumnVersionManager *mgr_;
      std::unique_lock<std::shared_mutex> lock_;

     public:
      explicit VersionedRowsLock(ColumnVersionManager *mgr) : mgr_(mgr), lock_(mgr->m_mutex) {}
      ~VersionedRowsLock() { mgr_->publish_versioned_rows(); }
    };

   public:
    void create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn, const uchar *old_value, size_t len,
                        const uchar *old_slot, size_t slot_len, VarlenDataPool *retained_pool = nullptr,
                        bool owns_varlen_ref = false);

    /**
     * Remove and return the newest version entry for a row (used by rollback).
     * If expected_txn_id != MAX_ID, refuse to pop another writer's head.
     */
    bool pop_head(row_id_t local_row_id, std::unique_ptr<Column_Version> &out,
                  Transaction::ID expected_txn_id = Transaction::MAX_ID);

    void restore_head(row_id_t local_row_id, std::unique_ptr<Column_Version> head);

    void commit_transaction(Transaction::ID txn_id, uint64_t commit_scn);
    std::vector<row_id_t> active_rows(Transaction::ID txn_id) const;

    /**
     * Walk newest -> oldest updates. Every update invisible to the reader is
     * undone by selecting its before-image; traversal stops at the first update
     * whose creator is visible in the InnoDB ReadView.
     */
    bool get_before_image_for_snapshot(row_id_t local_row_id, Transaction::ID reader_txn_id, uint64_t reader_scn,
                                       Transaction *reader_trx, const char *table_name, BeforeImage &out) const;

    bool has_version_in_range(row_id_t start_row, size_t count) const;

    /**
     * True when at least one row in this column carries an update history.
     * Lock-free; false means no snapshot reconstruction is possible and the
     * current cell is by definition the visible one.
     */
    inline bool has_any_version() const { return m_versioned_rows.load(std::memory_order_acquire) != 0; }

    // Legacy fallback API retained for non-SQL callers without ReadView.
    bool get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer, size_t &len,
                          Transaction::ID reader_txn_id = Transaction::MAX_ID) const;

    size_t purge(uint64_t min_active_scn);

    size_t get_version_count() const {
      std::shared_lock lock(m_mutex);
      return m_versions.size();
    }

    // Flat logical snapshot used by serialize(); physical old_slot is rebuilt
    // on load so allocator-local VarlenReference bytes never hit disk.
    struct VersionEntry {
      row_id_t row_id;
      Transaction::ID txn_id;
      uint64_t scn;
      size_t value_length;            // UNIV_SQL_NULL ⟹ NULL cell
      std::vector<uchar> value_data;  // empty when NULL
    };
    std::vector<VersionEntry> snapshot() const;
  };

  std::unique_ptr<ColumnVersionManager> m_version_manager{nullptr};

  mutable std::shared_mutex m_data_mutex;

  std::shared_ptr<ShannonBase::Utils::MemoryPool> m_memory_pool;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_CU_H__