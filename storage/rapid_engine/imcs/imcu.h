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

   The fundmental code for imcs.
*/
/**
 * The specification of IMCU, pls ref:
 * https://github.com/Shannon-Data/ShannonBase/issues/8
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU1              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU2              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ...
 *  * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCUN              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 *
 */
#ifndef __SHANNONBASE_IMCU_H__
#define __SHANNONBASE_IMCU_H__

#include <atomic>  //std::atomic<T>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_list.h"    //for LIST
#include "sql/table.h"  //for TABLE

#include "storage/innobase/include/univ.i"  //UNIV_SQL_NULL

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/row0row.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/trx/transaction.h"  // Transaction_Journal
#include "storage/rapid_engine/utils/memory_pool.h"

class Field;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Imcu;
/**
 * Storage Index, Every IMCU header automatically creates and manages In-Memory Storage Indexes (IM storage indexes) for
 * its CUs. An IM storage index stores the minimum and maximum for all columns within the IMCU.
 * - Column statistics at IMCU level
 * - Used for query optimization: skip irrelevant IMCUs
 * - Similar to Oracle's Storage Index
 */
class Imcu : public MemoryObject {
 public:
  // IMCU Header
  struct SHANNON_ALIGNAS imcu_header_t {
    // Basic Information
    uint32 imcu_id{0};                    // IMCU identifier
    row_id_t start_row{0};                // Global start row ID
    row_id_t end_row{0};                  // Global end row ID (exclusive)
    size_t capacity{0};                   // Capacity (number of rows)
    std::atomic<size_t> current_rows{0};  // Current number of rows

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_modified;

    // Row-level Metadata (Shared). [TODO] In future we will use Hybrid Approach
    // Delete bitmap (shared across all columns)
    std::unique_ptr<bit_array_t> del_mask{nullptr};

    // NULL bitmaps (per column)
    // null_masks[col_idx] = NULL bitmap of the column
    std::vector<std::unique_ptr<bit_array_t>> null_masks;

    // Lightweight transaction journal (metadata only)
    std::unique_ptr<TransactionJournal> txn_journal{nullptr};

    // Storage index (per-column statistics)
    std::unique_ptr<StorageIndex> storage_index{nullptr};

    // Optional row directory (for quick lookup of variable-length rows)
    std::unique_ptr<RowDirectory> row_directory{nullptr};

    // IMCU-level Statistics
    std::atomic<uint64> insert_count{0};
    std::atomic<uint64> update_count{0};
    std::atomic<uint64> delete_count{0};

    std::atomic<double> avg_row_size{0};
    std::atomic<size_t> compressed_size{0};
    std::atomic<size_t> uncompressed_size{0};

    // Transaction Boundaries
    Transaction::ID min_trx_id{Transaction::MAX_ID};
    Transaction::ID max_trx_id{0};
    uint64 min_scn{UINT64_MAX};
    uint64 max_scn{0};

    // GC Information
    std::chrono::system_clock::time_point last_gc_time{std::chrono::system_clock::now()};
    size_t version_count{0};
    double delete_ratio{0.0};

    std::chrono::system_clock::time_point last_compact_time{std::chrono::system_clock::now()};

    // Status Flags
    enum Status {
      ACTIVE,      // Writable
      READ_ONLY,   // Read-only (full)
      COMPACTING,  // Being compacted
      TOMBSTONE    // Marked for deletion
    };
    std::atomic<Status> status{ACTIVE};

    imcu_header_t() = default;

    // Copy constructor (deep copy)
    imcu_header_t(const imcu_header_t &other)
        : imcu_id(other.imcu_id),
          start_row(other.start_row),
          end_row(other.end_row),
          capacity(other.capacity),
          current_rows(other.current_rows.load()),
          created_at(other.created_at),
          last_modified(other.last_modified),
          insert_count(other.insert_count.load()),
          update_count(other.update_count.load()),
          delete_count(other.delete_count.load()),
          avg_row_size(other.avg_row_size.load()),
          compressed_size(other.compressed_size.load()),
          uncompressed_size(other.uncompressed_size.load()),
          min_trx_id(other.min_trx_id),
          max_trx_id(other.max_trx_id),
          min_scn(other.min_scn),
          max_scn(other.max_scn),
          last_gc_time(other.last_gc_time),
          version_count(other.version_count),
          delete_ratio(other.delete_ratio),
          status(other.status.load()) {
      if (other.del_mask) del_mask = std::make_unique<bit_array_t>(*other.del_mask);
      null_masks.reserve(other.null_masks.size());
      for (const auto &mask : other.null_masks) {
        null_masks.emplace_back(mask ? std::make_unique<bit_array_t>(*mask) : nullptr);
      }
      if (other.capacity) txn_journal = std::make_unique<TransactionJournal>(other.capacity);
      if (other.storage_index) storage_index = std::make_unique<StorageIndex>(*other.storage_index);
      if (other.row_directory) row_directory = other.row_directory->clone();
    }

    // assgin operator, move constructors disabled
    imcu_header_t &operator=(imcu_header_t other) = delete;
    imcu_header_t(imcu_header_t &&other) = delete;
    imcu_header_t &operator=(imcu_header_t &&) = delete;
  };

 public:
  Imcu(RpdTable *owner, TableMetadata &table_meta, row_id_t start_row, size_t capacity,
       std::shared_ptr<Utils::MemoryPool> mem_pool);
  Imcu() = default;
  virtual ~Imcu() = default;

  Imcu &operator=(Imcu other) = delete;
  Imcu(Imcu &&other) = delete;
  Imcu &operator=(Imcu &&) = delete;

  inline RpdTable *owner() { return m_owner_table; }

  inline imcu_header_t::Status get_status() const { return m_header.status.load(std::memory_order_acquire); }

  inline void set_status(imcu_header_t::Status status) { m_header.status.store(status, std::memory_order_release); }

  inline double deleted_ratio() const { return m_header.delete_ratio; }
  /**
   * Insert a row (IMCU-level entry point)
   * @param row_data: array of column data
   * @param context: execution context
   * @return local row_id within the IMCU, or INVALID_ROW_ID on failure
   */
  row_id_t insert_row(const Rapid_load_context *context, const RowBuffer &row_data);

  /**
   * Delete a row (core: mark deletion, column-independent)
   * @param local_row_id: local row ID within the IMCU
   * @param context: execution context
   * @return SHANNON_SUCCESS on sucess.
   */
  int delete_row(const Rapid_load_context *context, row_id_t local_row_id);

  /**
   * Batch delete (vectorized)
   * @param local_row_ids: list of local row IDs
   * @param context: execution context
   * @return number of deleted rows
   */
  size_t delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &local_row_ids);

  /**
   * Update a row (only modifies affected columns)
   * @param local_row_id: local row ID
   * @param updates: column index -> new value map
   * @param context: execution context
   * @return SHANNON_SUCCESS on success
   */
  int update_row(const Rapid_load_context *context, row_id_t local_row_id,
                 const std::unordered_map<uint32, RowBuffer::ColumnValue> &updates);

  /**
   * Scan the IMCU (vectorized)
   * @param context: scan context
   * @param predicates: filter conditions
   * @param projection: list of projected columns
   * @param callback: row callback function
   * @return number of scanned rows
   */
  size_t scan(Rapid_scan_context *context, const std::vector<std::shared_ptr<Predicate>> &predicates,
              const std::vector<uint32> &projection, RowCallback callback);

  /**
  Imcu::scan_range - Scan a specified range within the IMCU
  @param context - Scan context (transaction ID, SCN, etc.)
  @param start_offset - Starting row offset (internal IMCU offset)
  @param limit - Maximum number of rows to return
  @param predicates - Filter conditions
  @param projection - Columns to read
  @param callback - Callback function (invoked for each matching row)
  @return Actual number of rows scanned and returned
  */
  size_t scan_range(Rapid_scan_context *context, size_t start_offset, size_t limit,
                    const std::vector<std::shared_ptr<Predicate>> &predicates, const std::vector<uint32> &projection,
                    RowCallback callback);
  /**
   * Check row visibility (core: single-row check)
   * @param context: read scan context
   * @param local_row_id: local row ID
   * @param reader_txn_id: reader transaction ID
   * @param reader_scn: reader SCN
   * @return true if visible
   */
  bool is_row_visible(Rapid_scan_context *context, row_id_t local_row_id, Transaction::ID reader_txn_id,
                      uint64 reader_scn) const;

  /**
   * Batch visibility check (vectorized)
   * @param start_row: start row ID
   * @param count: number of rows
   * @param context: scan context
   * @param visibility_mask: output list of visible local row IDs
   */
  void check_visibility_batch(Rapid_scan_context *context, row_id_t start_row, size_t count,
                              bit_array_t &visibility_mask) const;

  /**
   * Read row data (specified columns)
   * @param local_row_id: local row ID
   * @param col_indices: indices of columns to read
   * @param context: scan context
   * @param output: output buffer
   */
  bool read_row(Rapid_scan_context *context, row_id_t local_row_id, const std::vector<uint32> &col_indices,
                RowBuffer &output);

  // Storage Index Operations
  /**
   * Determine if this IMCU can be skipped (based on Storage Index)
   * @param predicates: filter conditions
   * @return true if the IMCU can be skipped
   */
  bool can_skip_imcu(const std::vector<std::shared_ptr<Predicate>> &predicates) const;

  /**
   * Update the Storage Index
   */
  void update_storage_index();

  // Maintenance
  /**
   * Garbage Collection
   * @param min_active_scn: minimum active SCN
   * @return number of bytes reclaimed
   */
  size_t garbage_collect(uint64 min_active_scn);

  /**
   * Compact this IMCU (remove deleted rows)
   * @return new compacted IMCU instance
   */
  Imcu *compact();

  bool prune(const Predicate *pred) const;
  /**
   * Check if compaction is required
   */
  bool needs_compaction() const {
    auto last_compact = m_header.last_compact_time;
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - last_compact).count();
    return ((elapsed > 30) &&
            ((m_header.delete_ratio >= SHANNON_HIGH_DELETE_RATIO) ||  // Deletion ratio exceeds 30%
             (m_header.delete_count.load() > SHANNON_LARGE_DELETE_COUNT &&
              m_header.delete_ratio >= SHANNON_MEDIUM_DELETE_RATIO)));  // Or over 10,000 rows deleted with ratio > 20%
  }

  // Status Queries
  inline bool is_full() const {
    return m_header.current_rows.load(std::memory_order_acquire) >= m_header.capacity ||
           m_header.status.load(std::memory_order_acquire) == imcu_header_t::READ_ONLY;
  }

  inline bool is_empty() const { return m_header.current_rows.load(std::memory_order_acquire) == 0; }

  inline bool is_null(uint32 col_idx, row_id_t local_row_id) {
    if (col_idx > m_header.null_masks.size()) return false;
    return Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), local_row_id);
  }

  inline double get_usage_ratio() const {
    size_t current = m_header.current_rows.load(std::memory_order_acquire);
    if (m_header.capacity == 0) return 0.0;
    return static_cast<double>(current) / m_header.capacity;
  }

  size_t estimate_size() const {
    size_t total_size = 0;
    // Header size.
    total_size += sizeof(imcu_header_t);
    // delete bit mask.Storage_Index
    if (m_header.del_mask) total_size += m_header.capacity / 8;
    // NULL bit mask.
    for (const auto &null_mask : m_header.null_masks) {
      if (null_mask) total_size += m_header.capacity / 8;
    }
    // TrxnJ
    if (m_header.txn_journal) {
      total_size += m_header.txn_journal->get_total_size();
    }
    // all column data size.
    for (const auto &[col_idx, cu] : m_column_units) {
      total_size += cu->get_data_size();
    }
    return total_size;
  }

  inline row_id_t get_start_row() const { return m_header.start_row; }

  inline void new_start_row(row_id_t rowid) { m_header.start_row = rowid; }

  inline row_id_t get_end_row() const { return m_header.end_row; }

  inline size_t get_capacity() const { return m_header.capacity; }

  inline size_t get_row_count() const { return m_header.current_rows.load(std::memory_order_acquire); }

  inline double get_delete_ratio() const { return m_header.delete_ratio; }

  inline TransactionJournal *get_transaction_journal() const { return m_header.txn_journal.get(); }

  inline StorageIndex *get_storage_index() const { return m_header.storage_index.get(); }

  inline std::vector<std::unique_ptr<bit_array_t>> &get_null_masks() { return m_header.null_masks; }
  // CU Management
  inline CU *get_cu(uint32 col_idx) {
    auto it = m_column_units.find(col_idx);
    return (it != m_column_units.end()) ? it->second.get() : nullptr;
  }

  inline const CU *get_cu(uint32 col_idx) const {
    auto it = m_column_units.find(col_idx);
    return (it != m_column_units.end()) ? it->second.get() : nullptr;
  }

  // Serialization
  /**
   * Serialize to disk
   */
  bool serialize(std::ostream &out) const;

  /**
   * Deserialize from disk
   */
  bool deserialize(std::istream &in);

  /**
   * Update IMCU-level statistics
   */
  void update_statistics() {}

 private:
  /**
   * Initialize the header
   */
  void init_header(const TableMetadata &table_meta) {}

  /**
   * Initialize all column units
   */
  void init_column_units(const TableMetadata &table_meta) {}

  /**
   * Allocate a local row ID
   */
  inline row_id_t allocate_row_id() {
    size_t current = m_header.current_rows.fetch_add(1, std::memory_order_relaxed);

    if (current >= m_header.capacity) {
      m_header.current_rows.fetch_sub(1, std::memory_order_relaxed);
      m_header.status.store(imcu_header_t::READ_ONLY);
      return INVALID_ROW_ID;
    }

    return static_cast<row_id_t>(current);
  }

  /**
   * Record transaction log entry
   */
  void log_transaction(row_id_t local_row_id, OPER_TYPE operation, Transaction::ID txn_id,
                       const std::unordered_map<uint32, RowBuffer::ColumnValue> *updates = nullptr) {}

  /**
   * Increment the internal version counter
   */
  inline void increment_version() { m_version.fetch_add(1, std::memory_order_release); }

  /**
   * @brief Evaluate a single predicate against a row
   *
   * @param pred Predicate (Simple or Compound)
   * @param local_row_id Row ID within IMCU
   * @param row_cache Cache of column values for this row
   * @return true if row matches predicate
   */
  inline bool evaluate_predicate(const Predicate *pred, row_id_t local_row_id,
                                 std::unordered_map<uint32, const uchar *> &row_cache) const;
  /**
   * @brief Evaluate a simple predicate against a row
   */
  inline bool evaluate_simple_predicate(const Simple_Predicate *pred, row_id_t local_row_id,
                                        std::unordered_map<uint32, const uchar *> &row_cache) const;
  /**
   * @brief Evaluate a compound predicate against a row
   */
  inline bool evaluate_compound_predicate(const Compound_Predicate *pred, row_id_t local_row_id,
                                          std::unordered_map<uint32, const uchar *> &row_cache) const;
  /**
   * @brief Get column value for a row (with caching)
   *
   * @param col_id Column index
   * @param local_row_id Row ID
   * @param row_cache Cache to store/retrieve values
   * @return Pointer to column value (nullptr if NULL)
   */
  inline const uchar *get_column_value(uint32 col_id, row_id_t local_row_id,
                                       std::unordered_map<uint32, const uchar *> &row_cache) const;

 private:
  // Memory Management
  std::shared_ptr<Utils::MemoryPool> m_memory_pool;

  // IMCU-level read/write lock (protects header)
  mutable std::shared_mutex m_header_mutex;
  imcu_header_t m_header;

  // key: column_id, value: CU
  std::unordered_map<uint32, std::unique_ptr<CU>> m_column_units;

  // Ordered CU index (for efficient batch operations)
  std::vector<CU *> m_cu_array;

  // CU creation mutex (prevents duplicate creation)
  std::mutex m_cu_creation_mutex;

  // Optimistic concurrency version counter
  std::atomic<uint64> m_version{0};

  // Back Reference
  RpdTable *m_owner_table;
};

/**
 * @brief Vectorized evaluation for batch of rows
 *
 * Instead of evaluating predicates row-by-row, we can evaluate
 * an entire batch at once for better CPU cache utilization.
 *
 * This is useful for very selective predicates where most rows
 * are filtered out early.
 */
/*
// In scan_range(), replace row-by-row evaluation with:
// Build list of visible row IDs
std::vector<row_id_t> visible_rows;
for (size_t i = 0; i < batch_size; i++) {
  if (Utils::Util::bit_array_get(&visibility_mask, i)) {
    visible_rows.push_back(start + i);
  }
}

// Vectorized evaluation
bit_array_t match_mask(visible_rows.size());
VectorizedPredicateEvaluator::evaluate_batch(
    predicates[0].get(), this, visible_rows, match_mask);

// Collect matching rows
std::vector<row_id_t> matching_rows;
for (size_t i = 0; i < visible_rows.size(); i++) {
  if (Utils::Util::bit_array_get(&match_mask, i)) {
    matching_rows.push_back(visible_rows[i]);
  }
}
*/
class VectorizedPredicateEvaluator {
 public:
  /**
   * @brief Evaluate predicate on a batch of rows
   *
   * @param pred Predicate to evaluate
   * @param imcu IMCU containing the data
   * @param row_ids Vector of row IDs to evaluate
   * @param result Bitmap of results (1 = match, 0 = no match)
   */
  static void evaluate_batch(const Predicate *pred, const Imcu *imcu, const std::vector<row_id_t> &row_ids,
                             bit_array_t &result);

 private:
  static void evaluate_simple_batch(const Simple_Predicate *pred, const Imcu *imcu,
                                    const std::vector<row_id_t> &row_ids, bit_array_t &result);
  static void evaluate_compound_batch(const Compound_Predicate *pred, const Imcu *imcu,
                                      const std::vector<row_id_t> &row_ids, bit_array_t &result);
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCU_H__