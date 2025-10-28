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

   The fundmental code for imcs. Rapid Table.
*/
#ifndef __SHANNONBASE_RAPID_TABLE_H__
#define __SHANNONBASE_RAPID_TABLE_H__
#include <atomic>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/trx/transaction.h"

class TABLE;
class Field;
namespace ShannonBase {
class Rapid_context;
class Rapid_load_context;
class Rapid_scan_context;
namespace Imcs {
/**
 * @class RapidTable
 * @brief Abstract base class representing a Rapid in-memory table.
 *
 * Table (RapidTable)
 * └─IMCU(stored in row format logically)
 * └── CU (Column Unit) - One CU per field
 *       ├── Cu_header: Column metadata (statistics, dictionary, encoding type)
 *       └── Chunks[] - Column data chunks (each chunk contains SHANNON_ROWS_IN_CHUNK rows)
 *             ├── Chunk_header
 *             │     ├── m_null_mask: NULL bitmap
 *             │     ├── m_del_mask: Deletion bitmap
 *             │     ├── m_smu: Version management unit
 *             │     └── m_prows: Physical row count
 *             └── ChunkMemoryManager: Actual data storage
 * This class defines the fundamental interface for all Rapid table types, including
 * normal tables (`Table`) and partitioned tables (`PartTable`). It provides APIs for
 * data manipulation (insert, update, delete), index management, metadata access, and
 * transactional rollback integration.
 *
 * Each implementation manages:
 *   - Columnar memory (Cu) layout and lifecycle.
 *   - Index structures (Rapid Indexes).
 *   - Row ID management and statistics tracking.
 *
 * Thread-safety:
 *   - Column and index containers are guarded by shared_mutex.
 *   - Access counters use atomic operations.
 */
struct Row_Result {};
class Predicate;
class RpdTable : public MemoryObject {
 public:
  /**
   * @brief Rapid table type (normal or partitioned).
   */
  enum class TYPE : uint8 { UNKONWN = 0, NORAMAL, PARTTABLE };

  /** @brief cotor. */
  RpdTable(const TABLE *&mysql_table, const TableConfig &config)
      : m_mem_root(std::move(std::make_unique<MEM_ROOT>())), m_source_table(mysql_table) {}

  /** @brief decotor. */
  virtual ~RpdTable() {}

  virtual TYPE type() const = 0;

  /** @brief set the load type. */
  void set_load_type(LoadType load_type) { m_metadata.load_type = load_type; }

  /**
   * Registers a transaction with all IMCUs in this table
   *
   * This function iterates through all IMCUs (In-Memory Compression Units)
   * belonging to this table and registers the transaction with each one.
   * This ensures that all IMCUs are aware of the transaction and can track
   * any modifications made during its lifetime.
   *
   * @param trx Pointer to the transaction to be registered
   * @return true if registration was successful for all IMCUs, false otherwise
   *
   * @note The transaction must not be null (assertion will trigger if null)
   * @note This is typically called when a transaction starts working with this table
   * @note Each IMCU will track the transaction for conflict detection and MVCC purposes
   */
  virtual int register_transaction(Transaction *trx) = 0;

  /**
   * @brief Initialize Rapid index structures based on MySQL key metadata.
   * @param[in] context  Rapid load execution context.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int create_index_memo(const Rapid_load_context *context) = 0;

  /**
   * @brief write supporting precomputed offsets and bitmaps.
   * @param[in] context Rapid context.
   * @param[in] rowdata Row data buffer.
   * @param[in] len     Buffer length.
   * @param[in] col_offsets Column offsets.
   * @param[in] n_cols  Number of columns.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @return: Inserted global row_id, returns INVALID_ROW_ID on failure.
   */
  virtual row_id_t insert_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) = 0;

  /**
   * Delete row (Core: row-level marking)
   * @param global_row_id: Global row number
   * @param context: Context
   * @return: Returns SHANNON_SUCCESS on success.
   */
  virtual int delete_row(const Rapid_load_context *context, row_id_t global_row_id) = 0;

  /**
   * Batch delete (optimized version)
   * @param row_ids: List of row IDs to delete
   * @param context: Context
   * @return: Number of successfully deleted rows
   */
  virtual size_t delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) = 0;

  /**
   * Update row (Core: only operate on modified columns)
   * @param global_row_id: Global row number
   * @param updates: Column index -> new value mapping
   * @param context: Context
   * @return: Returns SHANNON_SUCCESS on success
   */
  virtual int update_row(const Rapid_load_context *context, row_id_t global_row_id,
                         const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) = 0;

  /**
   * @brief returen the global record row id in the table.
   * @param[in] context Rapid context.
   * @param[in] rowdata Row data buffer.
   * @param[in] len     Buffer length.
   * @param[in] col_offsets Column offsets.
   * @param[in] n_cols  Number of columns.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @return: Inserted global row_id, returns INVALID_ROW_ID on failure.
   */
  virtual row_id_t locate_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) = 0;

  /**
   * Full table scan (vectorized)
   * @param context: Scan context
   * @param predicates: List of predicates
   * @param projection: List of projection columns
   * @param callback: Callback function
   * @return: Returns SHANNON_SUCCESS on success
   */
  virtual int scan_table(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                         const std::vector<uint32_t> &projection, RowCallback callback) = 0;

  /**
   * Scan table with offset and limit support (for incremental scanning)
   * @param context: Scan context
   * @param start_offset: Global row offset to start from
   * @param limit: Maximum number of rows to return
   * @param predicates: Filter conditions
   * @param projection: Columns to read
   * @param callback: Callback for each matching row
   * @return: Number of rows actually returned
   */
  virtual size_t scan_table(Rapid_scan_context *context, row_id_t start_offset, size_t limit,
                            const std::vector<std::unique_ptr<Predicate>> &predicates,
                            const std::vector<uint32_t> &projection, RowCallback callback) = 0;

  /**
   * Point query (based on primary key)
   * @param key_value: Primary key value
   * @param context: Context
   * @param result: Output result
   */
  virtual bool read(Rapid_scan_context *context, const uchar *key_value, Row_Result &result) = 0;

  /**
   * Range scan
   * @param start_key: Start key
   * @param end_key: End key
   * @param context: Context
   * @param callback: Callback
   */
  virtual bool range_scan(Rapid_scan_context *context, const uchar *start_key, const uchar *end_key,
                          RowCallback callback) = 0;

  /**
   * Get row count (considering visibility)
   */
  virtual uint64_t get_row_count(const Rapid_scan_context *context) const = 0;

  /**
   * Get column statistics
   */
  virtual ColumnStatistics get_column_stats(uint32_t col_idx) const = 0;

  /**
   * Update statistics
   */
  virtual void update_statistics(bool force = false) = 0;

  /**
   * Garbage collection
   */
  virtual size_t garbage_collect(uint64_t min_active_scn) = 0;

  /**
   * Compress IMCUs
   */
  virtual size_t compact(double delete_ratio_threshold = 0.5) = 0;

  /**
   * Reorganize table
   */
  virtual bool reorganize() = 0;

  /**
   * Return imcu_idth imcu pointer.
   */
  virtual Imcu *locate_imcu(size_t imcu_id) = 0;

  /** @brief Lookup index by key name. */
  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) = 0;

  Table_Metadata &meta() { return m_metadata; }

 protected:
  /**
   * Locate IMCU based on global row_id
   */
  uint32_t generate_table_id() {
    static std::atomic<uint32_t> counter{1};
    return counter.fetch_add(1);
  }

  std::unique_ptr<MEM_ROOT> m_mem_root;

  const TABLE *m_source_table{nullptr};

  Table_Metadata m_metadata;

  // IMCU list (supports dynamic expansion)
  std::mutex m_imcu_mtex;
  std::vector<std::shared_ptr<Imcu>> m_imcus;

  // IMCU index (fast positioning)
  struct SHANNON_ALIGNAS Imcu_Index {
    row_id_t start_row{0};
    row_id_t end_row{0};
    std::shared_ptr<Imcu> imcu{nullptr};

    // IMCU-level statistics (for query optimization)
    std::vector<double> min_values;  // each column
    std::vector<double> max_values;  // each column
    bool has_deletes{false};
    double delete_ratio{0.0};

    Imcu_Index()
        : min_values(SHANNON_MAX_COLUMNS, SHANNON_MAX_DOUBLE),
          max_values(SHANNON_MAX_COLUMNS, SHANNON_MIN_DOUBLE),
          has_deletes(false),
          delete_ratio(0.0) {}
    Imcu_Index(size_t col_num)
        : min_values(col_num, SHANNON_MAX_DOUBLE),
          max_values(col_num, SHANNON_MIN_DOUBLE),
          has_deletes(false),
          delete_ratio(0.0) {}
  };
  std::vector<Imcu_Index> m_imcu_index;

  // Current IMCU
  std::atomic<Imcu *> m_current_imcu{nullptr};

  // Table-level lock (coarse-grained, protects IMCU list)
  std::shared_mutex m_table_mutex;

  // Transaction manager [TODO:]
  // std::unique_ptr<Transaction_Coordinator> m_txn_coordinator;

  // Version manager (global view) [TODO:]
  // std::unique_ptr<Global_Version_Manager> m_version_manager;

  // Memory pool
  std::shared_ptr<Utils::MemoryPool> m_memory_pool;

  // Background worker threads [TODO:]
  // std::unique_ptr<Background_Worker_Pool> m_bg_workers;

  // indexes mutex for index writing.
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> m_index_mutexes;
  std::unordered_map<std::string, std::unique_ptr<Index::Index<uchar, row_id_t>>> m_indexes;
};

class Table : public RpdTable {
 public:
  Table(const TABLE *&mysql_table, const TableConfig &config);

  virtual ~Table();

  virtual TYPE type() const override { return TYPE::NORAMAL; }

  virtual int create_index_memo(const Rapid_load_context *context) override;

  virtual row_id_t insert_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) override;

  virtual int delete_row(const Rapid_load_context *context, row_id_t global_row_id) override;

  virtual size_t delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) override;

  virtual int update_row(const Rapid_load_context *context, row_id_t global_row_id,
                         const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) override;

  virtual row_id_t locate_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) override;

  virtual int scan_table(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                         const std::vector<uint32_t> &projection, RowCallback callback) override;

  virtual size_t scan_table(Rapid_scan_context *context, row_id_t start_offset, size_t limit,
                            const std::vector<std::unique_ptr<Predicate>> &predicates,
                            const std::vector<uint32_t> &projection, RowCallback callback) override;

  virtual bool read(Rapid_scan_context *context, const uchar *key_value, Row_Result &result) override;

  virtual bool range_scan(Rapid_scan_context *context, const uchar *start_key, const uchar *end_key,
                          RowCallback callback) override;

  virtual uint64_t get_row_count(const Rapid_scan_context *context) const override;

  virtual ColumnStatistics get_column_stats(uint32_t col_idx) const override;

  virtual void update_statistics(bool force = false) override;

  virtual size_t garbage_collect(uint64_t min_active_scn) override;

  virtual size_t compact(double delete_ratio_threshold = 0.5) override;

  virtual bool reorganize() override;

  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) final {
    if (m_indexes.find(key_name) == m_indexes.end())
      return nullptr;
    else
      return m_indexes[key_name].get();
  }

  virtual Imcu *locate_imcu(size_t imcu_id) override {
    // size_t imcu_idx = global_row_id / m_metadata.rows_per_imcu;
    if (imcu_id >= m_imcus.size()) return nullptr;
    return m_imcus[imcu_id].get();
  }

  virtual row_id_t rows(const Rapid_context *) final { return m_metadata.total_rows; }

  virtual Imcu *locate_imcu_by_rowid(row_id_t global_row_id) {
    auto imcu_id = global_row_id / m_metadata.rows_per_imcu;
    // size_t imcu_idx = global_row_id / m_metadata.rows_per_imcu;
    if (imcu_id >= m_imcus.size()) return nullptr;
    return m_imcus[imcu_id].get();
  }

  virtual int register_transaction(Transaction *trx) override;

 private:
  row_id_t insert_row_impl(const Rapid_load_context *context, const RowBuffer &row_data);

  bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                     const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }

  inline void create_initial_imcu() {
    auto imcu = std::make_shared<Imcu>(this, m_metadata,
                                       0,  // global start_row
                                       m_metadata.rows_per_imcu, m_memory_pool);

    m_imcus.push_back(imcu);
    m_current_imcu.store(imcu.get());

    m_imcu_index.clear();
    m_imcu_index.reserve(m_imcus.size());

    for (auto &imcu : m_imcus) {
      Imcu_Index idx(imcu->owner()->meta().num_columns);
      idx.start_row = imcu->get_start_row();
      idx.end_row = imcu->get_end_row();
      idx.imcu = imcu;
      m_imcu_index.push_back(idx);
    }
  }

  Imcu *get_or_create_write_imcu() {
    Imcu *current = m_current_imcu.load();

    if (current && !current->is_full()) {
      return current;
    }

    /**
     * EACH MCU CONTAINS `SHANNON_ROWS_IN_CHUNK` (DEFAULT) ROWS.
     */
    std::unique_lock lock(m_table_mutex);
    row_id_t start_row = m_imcus.empty() ? 0 : m_imcus.size() * m_metadata.rows_per_imcu;

    auto new_imcu = std::make_shared<Imcu>(this, m_metadata, start_row /*start_row_#*/,
                                           m_metadata.rows_per_imcu /*capacity*/, m_memory_pool);
    m_imcus.push_back(new_imcu);
    m_current_imcu.store(new_imcu.get());

    update_imcu_index(new_imcu.get());

    return new_imcu.get();
  }

  void build_imcu_index() {  // to update the all imcu indexe statistics
    m_imcu_index.clear();
    m_imcu_index.reserve(m_imcus.size());

    for (auto &imcu : m_imcus) {
      Imcu_Index idx(imcu->owner()->meta().num_columns);
      idx.start_row = imcu->get_start_row();
      idx.end_row = imcu->get_end_row();
      idx.imcu = imcu;

      // TODO: collect statistics infor.
      // min_values[x] =xxx;
      // max_values[x] =yyy;
      // bool has_deletes = true;
      // double delete_ratio = 0.5;
      m_imcu_index.push_back(idx);
    }
  }

  void update_imcu_index(Imcu *imcu) {
    Imcu_Index idx;
    idx.start_row = imcu->get_start_row();
    idx.end_row = imcu->get_end_row();
    idx.imcu = m_imcus.back();

    m_imcu_index.push_back(idx);
  }

  /**
    Build metadata for all user-defined secondary indexes.

    Iterates through all KEY structures in TABLE::key_info and constructs
    corresponding in-memory Index objects. Each index is mapped by its
    key name, with associated key length and column names stored in
    m_source_keys for later lookup or index scan operations.

    @param[in]  context   Rapid load context providing TABLE definition

    @retval SHANNON_SUCCESS  All user-defined indexes successfully registered
  */
  int build_user_defined_index_memo(const Rapid_load_context *context);

  /**
    Insert a record reference into the in-memory index structure.

    This function encodes the record’s primary key (or hidden row ID)
    into a key buffer and inserts a mapping from that key to the
    given row_id into the corresponding Index instance.

    The logic is equivalent to ha_innodb::position(), but implemented
    independently of InnoDB to avoid engine coupling. Numeric columns
    (FLOAT, DOUBLE, DECIMAL) are encoded using ShannonBase’s sortable
    encoding rules to preserve lexical order.

    @param[in]  context   Rapid load context
    @param[in]  rowid     The row identifier to be associated with the key

    @retval SHANNON_SUCCESS  Index entry successfully created
  */
  int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, const RowBuffer *row_data);

  /**
   * The impl of `build_index`.
   */
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, const RowBuffer *row_data);

  /**
    Build the encoded row reference key (primary key) from a given row buffer.

    This is a parallel/async variant of build_key_info(), designed for
    high-performance data loading or parallel scanning where the current
    row buffer (rowdata) is not stored in TABLE::record[] but in an
    external buffer.

    The encoding logic is identical to InnoDB’s key_copy(), with extensions
    for sortable encoding of FLOAT/DOUBLE/DECIMAL types.

    This function assumes the table has an explicit primary key; otherwise,
    a sequential scan should be used. For concurrency safety, access to
    the shared key buffer is protected by m_key_buff_mutex.

    @param[in]  context           The current rapid load context
    @param[in]  key               The KEY descriptor for the primary key
    @param[in]  rowdata           Pointer to the raw row data buffer
    @param[in]  col_offsets       Per-column byte offsets within rowdata
    @param[in]  null_byte_offsets Per-column NULL byte offsets
    @param[in]  null_bitmasks     Per-column NULL bit masks

    @retval SHANNON_SUCCESS  Key buffer successfully built
  */
  int build_key_info(const Rapid_load_context *context, const KEY *key, uchar *rowdata, const RowBuffer *row_data);
};

// partitioned rapid table.
class PartTable : public RpdTable {
 public:
  PartTable(const TABLE *&mysql_table, const TableConfig &config) : RpdTable(mysql_table, config) {}
  virtual ~PartTable() {}

  virtual TYPE type() const override { return TYPE::PARTTABLE; }

  virtual int register_transaction(Transaction *trx) override;

  virtual int create_index_memo(const Rapid_load_context *context) override { return 0; }

  virtual row_id_t insert_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) override {
    return INVALID_ROW_ID;
  }

  virtual int delete_row(const Rapid_load_context *context, row_id_t global_row_id) override { return 0; }

  virtual size_t delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &row_ids) override {
    return 0;
  }

  virtual int update_row(const Rapid_load_context *context, row_id_t global_row_id,
                         const std::unordered_map<uint32_t, RowBuffer::ColumnValue> &updates) override {
    return 0;
  }

  virtual row_id_t locate_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                              size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) override {
    return INVALID_ROW_ID;
  }

  virtual int scan_table(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                         const std::vector<uint32_t> &projection, RowCallback callback) override {
    return 0;
  }

  virtual size_t scan_table(Rapid_scan_context *context, row_id_t start_offset, size_t limit,
                            const std::vector<std::unique_ptr<Predicate>> &predicates,
                            const std::vector<uint32_t> &projection, RowCallback callback) override {
    return 0;
  }

  virtual bool read(Rapid_scan_context *context, const uchar *key_value, Row_Result &result) override { return 0; }

  virtual bool range_scan(Rapid_scan_context *context, const uchar *start_key, const uchar *end_key,
                          RowCallback callback) override {
    return 0;
  }

  virtual uint64_t get_row_count(const Rapid_scan_context *context) const override { return 0; }

  virtual ColumnStatistics get_column_stats(uint32_t col_idx) const override {
    ColumnStatistics col_stat(col_idx, "col_name", MYSQL_TYPE_NULL);
    return col_stat;
  }

  virtual void update_statistics(bool force = false) override {}

  virtual size_t garbage_collect(uint64_t min_active_scn) override { return 0; }

  virtual size_t compact(double delete_ratio_threshold = 0.5) override { return 0; }

  virtual bool reorganize() override { return false; }

  virtual Imcu *locate_imcu(size_t imcu_id) override { return nullptr; }

  virtual Index::Index<uchar, row_id_t> *get_index(std::string) final { return nullptr; }

  virtual int build_partitions(const Rapid_load_context *context);

  inline RpdTable *get_partition(std::string part_key) {
    if (m_partitions.find(part_key) == m_partitions.end()) return nullptr;
    return m_partitions[part_key].get();
  }

 private:
  // all the partition sub-tables.
  std::unordered_map<std::string, std::unique_ptr<RpdTable>> m_partitions;

  // part_name+"#"+ part_id
  std::string m_part_key;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_TABLE_H__