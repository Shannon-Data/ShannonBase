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

#include "storage/rapid_engine/imcs/index/index.h"
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
class Cu;
/**
 * @class RapidTable
 * @brief Abstract base class representing a Rapid in-memory table.
 *
 * Table (RapidTable)
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
class RapidTable : public MemoryObject {
 public:
  /**
   * @brief Table load type (self-load or user-initiated).
   */
  enum class LoadType {
    NOT_LOADED = 0,  ///< Table not yet loaded into memory.
    SELF_LOADED,     ///< Automatically loaded by system background process.
    USER_LOADED      ///< Explicitly loaded by user request.
  };

  /**
   * @struct TableStats
   * @brief Statistics structure tracking table activity and access frequency.
   *
   * This structure maintains access counters, row count, and timing information
   * to guide adaptive caching and garbage collection decisions.
   */
  struct TableStats {
    std::shared_mutex m_stats_lock;                         // to protect the stats modification.
    std::atomic<uint64_t> mysql_access_count{0};            // MySQL access counts.
    std::atomic<uint64_t> heatwave_access_count{0};         // Rapid access counts.
    double importance{0.0};                                 // importance score.
    std::atomic<time_t> last_accessed{std::time(nullptr)};  // the laste access time.
    LoadType load_type{LoadType::NOT_LOADED};               // load type.
    // the phyiscal # of rows in this table.
    // physical row count. If you want to get logical rows, you should consider
    // MVCC to decide that whether this phyical row is visiable or not to this
    // transaction.
    std::atomic<row_id_t> prows{0};

    /** @brief Update last accessed timestamp to now. */
    void update_access_time() { last_accessed.store(std::time(nullptr), std::memory_order_relaxed); }

    /**
     * @brief Compute elapsed seconds since last access.
     * @return Time delta in seconds.
     */
    time_t seconds_since_last_access() const {
      time_t now = std::time(nullptr);
      return now - last_accessed.load(std::memory_order_relaxed);
    }
    TableStats() = default;
  };

  /**
   * @brief Rapid table type (normal or partitioned).
   */
  enum class TYPE : uint8 { UNKONWN = 0, NORAMAL, PARTTABLE };

  RapidTable() = default;
  RapidTable(std::string schema, std::string table) : m_schema_name(schema), m_table_name(table) {}
  virtual ~RapidTable() = default;

  RapidTable(RapidTable &&) = default;
  RapidTable &operator=(RapidTable &&) = default;

  RapidTable(const RapidTable &) = delete;
  RapidTable &operator=(const RapidTable &) = delete;

  /** @brief Returns the table type (normal or partitioned). */
  virtual RapidTable::TYPE type() = 0;

  /**
   * @brief Initialize Cu field metadata and allocate per-column memory.
   * @param[in] context  Rapid load execution context.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int create_fields_memo(const Rapid_load_context *context) = 0;

  /**
   * @brief Initialize Rapid index structures based on MySQL key metadata.
   * @param[in] context  Rapid load execution context.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int create_index_memo(const Rapid_load_context *context) = 0;

  /**
   * @brief Delete a single row by its row ID.
   * @param[in] context Rapid load context.
   * @param[in] rowid   Row identifier.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int delete_row(const Rapid_load_context *context, row_id_t rowid) = 0;

  /**
   * @brief delete supporting precomputed offsets and bitmaps.
   * @param[in] context Rapid context.
   * @param[in] rowdata Row data buffer.
   * @param[in] len     Buffer length.
   * @param[in] col_offsets Column offsets.
   * @param[in] n_cols  Number of columns.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int delete_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                         size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) = 0;

  /**
   * @brief Delete multiple rows in batch.
   * @param[in] context Rapid load context.
   * @param[in] rowids  List of row IDs to delete.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) = 0;

  /**
   * @brief Insert a new row reconstructed from redo or binlog information.
   * @param[in] context  Rapid load context.
   * @param[in] rowid    Logical row ID.
   * @param[in] fields   Parsed field data from InnoDB log.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                 std::unordered_map<std::string, mysql_field_t> &fields) = 0;

  /**
   * @brief Update a single field within a row.
   * @param[in] context Rapid context.
   * @param[in] rowid   Row ID.
   * @param[in] field_key Field name.
   * @param[in] new_field_data Pointer to new field bytes.
   * @param[in] nlen Length of new field data.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                         const uchar *new_field_data, size_t nlen) = 0;

  /**
   * @brief Update a single field within a row.
   * @param[in] context Rapid context.
   * @param[in] nlen Length of row data.
   * @param[in] col_offsets Column offsets array.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @param[in] start old row data ptr.
   * @param[in] new_start new row data ptr.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int update_row(const Rapid_load_context *context, size_t nlen, ulong *col_offsets, ulong *null_byte_offsets,
                         ulong *null_bitmasks, const uchar *start, const uchar *new_start) = 0;

  /**
   * @brief Apply an update reconstructed from redo or binlog data.
   * @param[in] context Rapid context.
   * @param[in] rowid   Target row.
   * @param[in] upd_recs Updated fields.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                  std::unordered_map<std::string, mysql_field_t> &upd_recs) = 0;

  /**
   * @brief Build index entry for a specific row.
   * @param[in] context Rapid context.
   * @param[in] key     Key definition.
   * @param[in] rowid   value of key, rowid.
   * @return SHANNON_SUCCESS on success. The built key buffer stored in `context->m_extra_info.m_key_buff`.
   */
  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) = 0;

  /**
   * @brief Build index entry with explicit row data and offsets.
   * @param[in] context Rapid context.
   * @param[in] key     Key definition.
   * @param[in] rowid   value of key, rowid.
   * @param[in] rowdata Pointer to record buffer.
   * @param[in] col_offsets Column offsets array.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @return SHANNON_SUCCESS on success. The built key buffer stored in `context->m_extra_info.m_key_buff`.
   */
  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                          ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) = 0;

  /**
   * @brief Write a record to the table.
   * @param[in] context Rapid load context.
   * @param[in] data    Row data buffer.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int write(const Rapid_load_context *context, uchar *data) = 0;

  /**
   * @brief Parallelized write supporting precomputed offsets and bitmaps.
   * @param[in] context Rapid context.
   * @param[in] rowdata Row data buffer.
   * @param[in] len     Buffer length.
   * @param[in] col_offsets Column offsets.
   * @param[in] n_cols  Number of columns.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                    ulong *null_byte_offsets, ulong *null_bitmasks) = 0;

  /**
   * @brief Initialize partitioned sub-tables (for PartTable).
   * @param[in] context Rapid load context.
   * @return SHANNON_SUCCESS on success.
   */
  virtual int build_partitions(const Rapid_load_context *context) = 0;

  /** @brief Get physical row count. */
  virtual row_id_t rows(const Rapid_context *) = 0;

  /** @brief Reserve a new row ID for insertion. */
  virtual row_id_t forward_rowid(const Rapid_load_context *) = 0;

  /** @brief rollback the new row ID for insertion. */
  virtual row_id_t backward_rowid(const Rapid_load_context *) = 0;

  /** @brief Lookup Cu column by field name. */
  virtual Cu *first_field() = 0;

  /** @brief Lookup Cu column by field name. */
  virtual Cu *get_field(std::string field_name) = 0;

  /** @brief Lookup index by key name. */
  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) = 0;

  /** @brief Access all Cu field objects. */
  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() = 0;

  /** @brief Access MySQL source key metadata. */
  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() = 0;

  /** @brief Get schema name. */
  virtual std::string &schema_name() = 0;

  /** @brief Get this table name. */
  virtual std::string &name() = 0;

  /** @brief Truncate this table. */
  virtual int truncate() = 0;

  /** @brief Rollback this changes. */
  virtual int rollback_changes_by_trxid(Transaction::ID trxid) = 0;

  /** @brief Get the load type. */
  void set_load_type(LoadType load_type) { m_load_type = load_type; }

 protected:
  TYPE m_type{TYPE::UNKONWN};

  // self load or user load.
  LoadType m_load_type{LoadType::NOT_LOADED};

  // name of schema.
  std::string m_schema_name;

  // name of this table.
  std::string m_table_name;

  // the statistic of this table.
  TableStats m_stats;

  // the loaded cus. key format: field/column name.
  std::shared_mutex m_fields_mutex;
  std::unordered_map<std::string, std::unique_ptr<Cu>> m_fields;

  // key format: key_name
  // value format: vector<park part1 name , key part2 name>.
  std::unordered_map<std::string, key_meta_t> m_source_keys;

  // key format: key_name.
  std::shared_mutex m_key_buff_mutex;

  // indexes mutex for index writing.
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> m_index_mutexes;
  std::unordered_map<std::string, std::unique_ptr<Index::Index<uchar, row_id_t>>> m_indexes;
};

/**
 * @class Table
 * @brief Concrete implementation of RapidTable for non-partitioned tables.
 *
 * The `Table` class manages an in-memory Cu-columnar table, handling field
 * storage, index building, and transactional rollback integration. It provides
 * full CRUD APIs for Redo/Undo recovery and online synchronization with
 * the MySQL engine.
 *
 * Features:
 *   - Field-level Cu memory allocation and columnar access.
 *   - Rapid index management integrated with MySQL KEY metadata.
 *   - Thread-safe concurrent reads via shared_mutex.
 *   - Row ID management and basic statistics tracking.
 */
class Table : public RapidTable {
 public:
  Table() = default;
  Table(std::string schema, std::string table) : RapidTable(schema, table) {}
  Table(std::string schema, std::string table, std::string partkey) : RapidTable(schema, table), m_part_key(partkey) {}
  virtual ~Table() {
    m_fields.clear();
    m_source_keys.clear();
    m_indexes.clear();
  }

  virtual TYPE type() final { return RapidTable::TYPE::NORAMAL; }
  virtual int create_fields_memo(const Rapid_load_context *context) final;
  virtual int create_index_memo(const Rapid_load_context *context) final;

  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) final;
  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                          ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) final;
  virtual int write(const Rapid_load_context *context, uchar *data) final;
  virtual int write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                    ulong *null_byte_offsets, ulong *null_bitmasks) final;
  virtual int delete_row(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets,
                         size_t n_cols, ulong *null_byte_offsets, ulong *null_bitmasks) final;
  virtual int delete_row(const Rapid_load_context *context, row_id_t rowid) final;
  virtual int delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) final;

  virtual int write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                 std::unordered_map<std::string, mysql_field_t> &fields) final;
  virtual int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                         const uchar *new_field_data, size_t nlen) final;
  virtual int update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                  std::unordered_map<std::string, mysql_field_t> &upd_recs) final;
  virtual int update_row(const Rapid_load_context *context, size_t nlen, ulong *col_offsets, ulong *null_byte_offsets,
                         ulong *null_bitmasks, const uchar *start, const uchar *new_start) final;
  virtual int truncate() final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_partitions(const Rapid_load_context *) final { return ShannonBase::SHANNON_SUCCESS; }

  // rollback a modified record.
  virtual int rollback_changes_by_trxid(Transaction::ID trxid) final;

  // gets the # of physical rows.
  virtual row_id_t rows(const Rapid_context *) final { return m_stats.prows.load(); }

  // to reserer a row place for this operation.
  virtual row_id_t forward_rowid(const Rapid_load_context *) final { return m_stats.prows.fetch_add(1); }

  // to reserer back the row id for rollback operation.
  virtual row_id_t backward_rowid(const Rapid_load_context *) final { return m_stats.prows.fetch_sub(1); }

  virtual Cu *first_field() final { return m_fields.begin()->second.get(); }

  virtual Cu *get_field(std::string field_name) final;

  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) final {
    if (m_indexes.find(key_name) == m_indexes.end())
      return nullptr;
    else
      return m_indexes[key_name].get();
  }

  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() final { return m_fields; }

  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() final { return m_source_keys; }

  virtual std::string &schema_name() final { return m_schema_name; }

  virtual std::string &name() final { return m_table_name; }

 private:
  bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                     const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }

  /**
    Build the metadata for a hidden clustered index.

    When a table has no user-defined primary key, MySQL automatically
    generates a hidden clustered index using a 6-byte row_id as the
    unique row reference.

    This function creates an in-memory index object (Index<uchar, row_id_t>)
    corresponding to that hidden key and registers its name, key length,
    and lock.

    @param[in]  context   Rapid load context (unused but kept for interface consistency)

    @retval SHANNON_SUCCESS  Hidden index successfully registered
  */
  int build_hidden_index_memo(const Rapid_load_context *context);

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
    @param[in]  key       The KEY descriptor for the index (nullptr if hidden PK)
    @param[in]  rowid     The row identifier to be associated with the key

    @retval SHANNON_SUCCESS  Index entry successfully created
  */
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid);

  /**
    Insert a record reference into the in-memory index structure

    This variant is designed for parallel or asynchronous loading,
    where row data resides in an external buffer rather than
    TABLE::record[0].

    It encodes the primary key directly from rowdata using the
    per-column offsets and null maps, then inserts the resulting
    key→rowid mapping into the index. Access to each index is
    serialized via its dedicated mutex to ensure thread safety.

    @param[in]  context           Rapid load context
    @param[in]  key               The KEY descriptor for the index
    @param[in]  rowid             The row identifier to associate
    @param[in]  rowdata           External row data buffer
    @param[in]  col_offsets       Per-column byte offsets
    @param[in]  null_byte_offsets Per-column NULL byte offsets
    @param[in]  null_bitmasks     Per-column NULL bit masks

    @retval SHANNON_SUCCESS  Index entry successfully inserted
  */
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                       ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks);

  /**
    Build the row reference key (rowid or primary key) for the current record.

    This function reproduces the logic of ha_innodb::position(), which
    generates the row reference used by MySQL to uniquely identify a record.
    However, we re-implement it here to decouple from the InnoDB engine layer.

    The resulting key buffer (m_extra_info.m_key_buff) is stored inside the
    Rapid_load_context for later use (e.g., for synchronization or external
    index update).

    If the table has no explicit primary key, we use the hidden InnoDB row ID
    (6-byte ref) as the record identifier. Otherwise, we build the encoded
    primary key image from the current record (record[0]) using the
    same field-by-field encoding rules as InnoDB’s key_copy(), but with
    additional handling for sortable encoding of floating-point and decimal
    types.

    @param[in]  context    The current rapid load context
    @param[in]  key        The KEY descriptor for the primary key (nullptr if none)

    @retval SHANNON_SUCCESS  Key buffer successfully built
  */
  int build_key_info(const Rapid_load_context *context, const KEY *key);

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
  int build_key_info(const Rapid_load_context *context, const KEY *key, uchar *rowdata, ulong *col_offsets,
                     ulong *null_byte_offsets, ulong *null_bitmasks);

 private:
  std::string m_part_key;
};

/**
 * @class PartTable
 * @brief Implementation of RapidTable supporting table partitioning.
 *
 * The `PartTable` extends RapidTable by managing multiple child partitions,
 * each represented by an internal `Table` instance. It coordinates partition
 * metadata, routing, and global statistics aggregation.
 *
 * Features:
 *   - Multi-partition management via `std::vector<Table>`.
 *   - Partition metadata (key mapping, name, schema).
 *   - Unified transactional rollback propagation.
 */
class PartTable : public RapidTable {
 public:
  PartTable() = default;
  PartTable(std::string schema, std::string table, std::string part_key)
      : RapidTable(schema, table), m_part_key(part_key) {}
  virtual ~PartTable() {
    m_fields.clear();
    m_source_keys.clear();
    m_indexes.clear();
  }

  virtual TYPE type() final { return RapidTable::TYPE::PARTTABLE; }

  virtual int create_fields_memo(const Rapid_load_context *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int create_index_memo(const Rapid_load_context *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write(const Rapid_load_context *, uchar *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write(const Rapid_load_context *, uchar *, size_t, ulong *, size_t, ulong *, ulong *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int delete_row(const Rapid_load_context *, row_id_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int delete_row(const Rapid_load_context *, uchar *, size_t, ulong *, size_t, ulong *, ulong *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int delete_rows(const Rapid_load_context *, const std::vector<row_id_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t, uchar *, ulong *, ulong *, ulong *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write_row_from_log(const Rapid_load_context *, row_id_t,
                                 std::unordered_map<std::string, mysql_field_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int update_row(const Rapid_load_context *, row_id_t, std::string &, const uchar *, size_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int update_row_from_log(const Rapid_load_context *, row_id_t,
                                  std::unordered_map<std::string, mysql_field_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int update_row(const Rapid_load_context *context, size_t nlen, ulong *col_offsets, ulong *null_byte_offsets,
                         ulong *null_bitmasks, const uchar *start, const uchar *new_start) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int truncate() final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_partitions(const Rapid_load_context *context) final;

  // rollback a modified record.
  virtual int rollback_changes_by_trxid(Transaction::ID) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  // gets the # of physical rows.
  virtual row_id_t rows(const Rapid_context *) final { return m_stats.prows.load(); }

  // to reserer a row place for this operation.
  virtual row_id_t forward_rowid(const Rapid_load_context *) final { return m_stats.prows.fetch_add(1); }

  // to reserer back the new row for rollback operation.
  virtual row_id_t backward_rowid(const Rapid_load_context *) final { return m_stats.prows.fetch_sub(1); }

  virtual Cu *first_field() final { return m_fields.begin()->second.get(); }

  virtual Cu *get_field(std::string) final {
    assert(false);
    return nullptr;
  }

  virtual Index::Index<uchar, row_id_t> *get_index(std::string) final {
    assert(false);
    return nullptr;
  }

  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() final { return m_fields; }

  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() final { return m_source_keys; }

  virtual std::string &schema_name() final { return m_schema_name; }
  virtual std::string &name() final { return m_table_name; }
  virtual row_id_t reserver_rowid() final { return m_stats.prows.fetch_add(1); }

  inline RapidTable *get_partition(std::string part_key) {
    if (m_partitions.find(part_key) == m_partitions.end()) return nullptr;
    return m_partitions[part_key].get();
  }

 private:
  bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                     const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }

 private:
  // all the partition sub-tables.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_partitions;

  // part_name+"#"+ part_id
  std::string m_part_key;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_TABLE_H__