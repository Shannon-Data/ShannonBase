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
#ifndef __SHANNONBASE_ROW0ROW_H__
#define __SHANNONBASE_ROW0ROW_H__

#include <atomic>  //std::atomic<T>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_list.h"    //for LIST
#include "sql/table.h"  //for TABLE

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/utils/concurrent.h"  //asio

class Field;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Imcu;
struct TableMetadata;
class DeferGuard {
 public:
  explicit DeferGuard(std::function<void()> fn) : m_fn(std::move(fn)), m_active(true) {}

  DeferGuard(const DeferGuard &) = delete;
  DeferGuard &operator=(const DeferGuard &) = delete;

  DeferGuard(DeferGuard &&other) noexcept : m_fn(std::move(other.m_fn)), m_active(other.m_active) {
    other.m_active = false;
  }

  ~DeferGuard() {
    if (m_active && m_fn) m_fn();
  }

  void dismiss() noexcept { m_active = false; }

 private:
  std::function<void()> m_fn;
  bool m_active;
};

/**
 * RowBuffer (Row Buffer)
 *
 * Purpose:
 * 1. Store one row of query result data
 * 2. Support zero-copy optimization (directly pointing to CU data)
 * 3. Handle type conversion and formatting
 *
 * Usage Scenarios:
 * - SELECT query result buffering
 * - UPDATE/INSERT input buffering
 * - Temporary buffering for row-level operations
 */
class RowBuffer {
 public:
  /**
   * Column Value Wrapper
   * - Supports both zero-copy and copy modes
   */
  struct SHANNON_ALIGNAS ColumnValue {
    const uchar *data;              // Data pointer (may point to CU internal data or owned buffer)
    size_t length;                  // Data length
    struct SHANNON_ALIGNAS Flags {  // Flags
      uint8_t is_null : 1;          // Whether NULL
      uint8_t is_zero_copy : 1;     // Whether zero-copy (directly points to CU)
      uint8_t is_encoded : 1;       // Whether encoded (dictionary ID)
      uint8_t needs_decode : 1;     // Whether needs decoding
      uint8_t reserved : 4;
    } flags;
    std::unique_ptr<uchar[]> owned_buffer;  // Owned buffer (used when not zero-copy)
    enum_field_types type;                  // Column metadata

    ColumnValue();
    ColumnValue(const ColumnValue &other);
    ColumnValue(ColumnValue &&other) noexcept;
    ColumnValue &operator=(const ColumnValue &other);
    ColumnValue &operator=(ColumnValue &&other) noexcept;
  };

  /**
   * Constructor
   * @param num_columns: Number of columns
   * @param field_metadata: Field metadata (optional)
   */
  RowBuffer(size_t num_columns, const std::vector<Field *> *field_metadata = nullptr)
      : m_row_id(INVALID_ROW_ID),
        m_num_columns(num_columns),
        m_is_all_zero_copy(true),
        m_field_metadata(field_metadata) {
    m_columns.resize(num_columns);
  }

  RowBuffer(const RowBuffer &other)
      : m_row_id(other.m_row_id),
        m_num_columns(other.m_num_columns),
        m_is_all_zero_copy(other.m_is_all_zero_copy),
        m_field_metadata(other.m_field_metadata) {
    m_columns = other.m_columns;  // Utilize ColumnValue's copy constructor
  }

  RowBuffer(RowBuffer &&other) noexcept
      : m_row_id(other.m_row_id),
        m_columns(std::move(other.m_columns)),
        m_num_columns(other.m_num_columns),
        m_is_all_zero_copy(other.m_is_all_zero_copy),
        m_field_metadata(other.m_field_metadata) {
    other.m_row_id = ShannonBase::INVALID_ROW_ID;
    other.m_num_columns = 0;
  }

  RowBuffer &operator=(const RowBuffer &) = default;
  RowBuffer &operator=(RowBuffer &&) noexcept = default;
  virtual ~RowBuffer() = default;

  // Column Value Reading
  /**
   * Get column value
   * @param col_idx: Column index
   * @return: Column value (read-only)
   */
  inline const ColumnValue *get_column(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return nullptr;
    return &m_columns[col_idx];
  }

  /**
   * Get column value (mutable)
   */
  inline ColumnValue *get_column_mutable(uint32 col_idx) {
    if (col_idx >= m_num_columns) return nullptr;
    return &m_columns[col_idx];
  }

  void set_row_id(row_id_t row_id) { m_row_id = row_id; }
  row_id_t get_row_id() const { return m_row_id; }

  // Column Value Setting (Zero-Copy)
  /**
   * Set column value (zero-copy mode)
   * - Directly points to CU data
   * - High performance, but requires ensuring CU lifecycle
   *
   * @param col_idx: Column index
   * @param data: Data pointer (points to CU internal)
   * @param length: Data length
   * @param type: Data type
   */
  void set_column_zero_copy(uint32 col_idx, const uchar *data, size_t length, enum_field_types type = MYSQL_TYPE_NULL);

  /**
   * Batch set column values (zero-copy)
   * @param data_ptrs: Array of data pointers
   * @param lengths: Array of lengths (optional)
   */
  void set_columns_zero_copy(const uchar **data_ptrs, const size_t *lengths = nullptr);

  // Column Value Setting (Copy)
  /**
   * Set column value (copy mode)
   * - Deep copy data to owned buffer
   * - Safe, but has performance overhead
   *
   * @param col_idx: Column index
   * @param data: Data pointer
   * @param length: Data length
   * @param type: Data type
   */
  void set_column_copy(uint32 col_idx, const uchar *data, size_t length, enum_field_types type = MYSQL_TYPE_NULL);

  /**
   * Set NULL column
   */
  void set_column_null(uint32 col_idx) {
    if (col_idx >= m_num_columns) return;

    ColumnValue &col = m_columns[col_idx];
    col.data = nullptr;
    col.length = UNIV_SQL_NULL;
    col.flags.is_null = 1;
    col.owned_buffer.reset();
  }

  /**
   * Check if column is NULL
   */
  inline bool is_column_null(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return true;
    return m_columns[col_idx].flags.is_null == 1;
  }

  /**
   * Get column data pointer
   */
  inline const uchar *get_column_data(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return nullptr;
    return m_columns[col_idx].data;
  }

  /**
   * Get column data length
   */
  inline size_t get_column_length(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return 0;
    return m_columns[col_idx].length;
  }

  // Type Conversion
  /**
   * Get column integer value
   */
  int64_t get_column_int(uint32 col_idx) const;

  /**
   * Get column floating-point value
   */
  double get_column_double(uint32 col_idx) const;

  /**
   * Get column string value
   * @param buffer: Output buffer
   * @param buffer_size: Buffer size
   * @return: Actual length
   */
  size_t get_column_string(uint32 col_idx, char *buffer, size_t buffer_size) const;

  // Batch Operations
  /**
   * Clear all column values
   */
  void clear();
  void reset() { clear(); }

  /**
   * Convert to fully owned mode (for cross-IMCU transfer)
   */
  void convert_to_owned();

  // Serialization and Deserialization
  /**
   * Serialize to binary format
   * @param out: Output stream
   * @return: Returns true if successful
   */
  bool serialize(std::ostream &out) const;

  /**
   * Deserialize from binary format
   * @param in: Input stream
   * @return: Returns true if successful
   */
  bool deserialize(std::istream &in);

  /**
   * Read data from MySQL Field array
   * @param fields: MySQL Field array
   * @param num_fields: Number of fields
   * @param[in] rowdata Row data buffer.
   * @param[in] col_offsets Column offsets.
   * @param[in] null_byte_offsets Null byte offsets.
   * @param[in] null_bitmasks Null bitmasks.
   */
  int copy_from_mysql_fields(const Rapid_load_context *context, uchar *rowdata,
                             const std::vector<FieldMetadata> &fields, ulong *col_offsets, ulong *null_byte_offsets,
                             ulong *null_bitmasks);

  // MySQL Compatibility Interface
  /**
   * Copy row data to MySQL Field array
   * @param to: MySQL TABLE
   * @param meta: MySQL TABLE metadata
   */
  int copy_to_mysql_fields(const TABLE *to, const TableMetadata *meta) const;

  // MySQL Compatibility Interface, async version with parallel column processing
  /**
   * Copy row data to MySQL Field array
   * @param to: MySQL TABLE
   * @param meta: MySQL TABLE metadata
   * @param executor: executor.
   * @param max_batch_size: batch size.
   */
  boost::asio::awaitable<int> copy_to_mysql_fields_async(const TABLE *to, const TableMetadata *meta,
                                                         boost::asio::any_io_executor &executor,
                                                         size_t max_batch_size = 32) const;

  // Statistics and Debugging
  /**
   * Get number of columns
   */
  inline size_t get_num_columns() const { return m_num_columns; }

  /**
   * Whether all columns are zero-copy
   */
  inline bool is_all_zero_copy() const { return m_is_all_zero_copy; }

  /**
   * Get total data size (bytes)
   */
  inline size_t get_total_size() const {
    size_t total = 0;
    for (const auto &col : m_columns) {
      if (!col.flags.is_null) total += col.length;
    }
    return total;
  }

  /**
   * Get owned memory size (excluding zero-copy parts)
   */
  inline size_t get_owned_memory_size() const {
    size_t total = 0;
    for (const auto &col : m_columns) {
      if (col.owned_buffer) total += col.length;
    }
    return total;
  }

  /**
   * Print row content (for debugging)
   */
  void dump(std::ostream &out) const;

  /**
   * Validate row data integrity
   */
  bool validate() const;

  inline bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                            const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];
    // gets null byte.
    uchar null_byte = rowdata[byte_offset];
    // check null bit.
    return (null_byte & bitmask) != 0;
  }

 private:
  // Internal helper to extract field data from different sources
  struct FieldDataInfo {
    uchar *data_ptr;
    uint32 data_len;
    bool is_null;
  };

  FieldDataInfo extract_field_data(const Rapid_load_context *context, Field *fld, size_t col_idx, uchar *rowdata,
                                   ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks);
  // Row ID
  row_id_t m_row_id{0};

  // Column value array
  std::vector<ColumnValue> m_columns;

  // Number of columns
  size_t m_num_columns{0};

  // Whether all columns are zero-copy
  bool m_is_all_zero_copy{false};

  // Field metadata (optional, for type conversion)
  const std::vector<Field *> *m_field_metadata;
};

// Auxiliary Type Definitions
/**
 * RowCallback (Row Callback Function Type)
 * Used for scan operation callbacks
 */
using RowCallback = std::function<void(row_id_t row_id, const std::vector<const uchar *> &row_data)>;

/**
 * RowBufferPool (Row Buffer Pool)
 * Used to reduce RowBuffer allocation overhead
 */
class RowBufferPool {
 private:
  std::vector<std::unique_ptr<RowBuffer>> m_buffers;
  std::mutex m_mutex;
  size_t m_num_columns;
  const std::vector<Field *> *m_field_metadata;

 public:
  RowBufferPool(size_t num_columns, const std::vector<Field *> *field_metadata = nullptr)
      : m_num_columns(num_columns), m_field_metadata(field_metadata) {}
  /**
   * Acquire a row buffer
   */
  std::unique_ptr<RowBuffer> acquire() {
    std::lock_guard lock(m_mutex);
    if (!m_buffers.empty()) {
      auto buffer = std::move(m_buffers.back());
      m_buffers.pop_back();
      buffer->reset();
      return buffer;
    }

    return std::make_unique<RowBuffer>(m_num_columns, m_field_metadata);
  }

  /**
   * Release row buffer
   */
  void release(std::unique_ptr<RowBuffer> buffer) {
    if (!buffer) return;

    std::lock_guard lock(m_mutex);
    // Limit pool size
    if (m_buffers.size() < 64) {
      buffer->clear();
      m_buffers.push_back(std::move(buffer));
    }
  }

  /**
   * Clear pool
   */
  void clear() {
    std::lock_guard lock(m_mutex);
    m_buffers.clear();
  }
};

/**
 * RowDirectory (Row Directory)
 *
 * Purpose:
 * 1. Quickly locate data positions of variable-length columns
 * 2. Support offset lookup after row-level compression
 * 3. Optimize column access for wide tables
 *
 * Usage Scenarios:
 * - Contains multiple variable-length columns (VARCHAR, TEXT, BLOB)
 * - Row-level compression is enabled
 * - Requires fast random access
 */
class RowDirectory {
 public:
  /**
   * Row Entry
   * - Records metadata for each row in the IMCU
   */
  struct SHANNON_ALIGNAS RowEntry {
    // Row start offset (relative to CU base address)
    uint32 offset;
    // Actual row length (after compression or variable-length encoding)
    uint32 length;
    // Row flags
    struct Flags {
      uint8_t is_compressed : 1;  // Whether compressed
      uint8_t is_deleted : 1;     // Whether deleted (redundant, for quick checking)
      uint8_t has_null : 1;       // Whether contains NULL
      uint8_t is_overflow : 1;    // Whether has overflow page
      uint8_t reserved : 4;       // Reserved bits
    } flags;

    // Checksum (optional, for data integrity checking)
    uint32 checksum;
    RowEntry() : offset(0), length(0), checksum(0) { std::memset(&flags, 0, sizeof(flags)); }
  };

  /**
   * Column Offset Table (Optional)
   * - Used to quickly locate individual columns within a row
   * - Suitable for wide tables or scenarios with many variable-length columns
   */
  struct SHANNON_ALIGNAS ColumnOffsetTable {
    // Relative offset of each column within the row
    std::vector<uint16> column_offsets;
    // Actual length of each column
    std::vector<uint16> column_lengths;
    ColumnOffsetTable(size_t num_columns) {
      column_offsets.reserve(num_columns);
      column_lengths.reserve(num_columns);
    }
  };

 private:
  // Row entry array (fixed size, consistent with IMCU capacity)
  std::unique_ptr<RowEntry[]> m_entries;
  size_t m_capacity;

  // Column offset tables (optional, built on demand)
  // key: row_id, value: column offset table
  std::unordered_map<row_id_t, std::unique_ptr<ColumnOffsetTable>> m_column_offset_tables;

  // Whether column offset tables are enabled
  bool m_enable_column_offsets;

  // Number of columns (for initializing column offset tables)
  size_t m_num_columns;

  std::atomic<size_t> m_total_data_size{0};       // Total data size
  std::atomic<size_t> m_compressed_data_size{0};  // Compressed data size
  std::atomic<size_t> m_overflow_count{0};        // Overflow row count

  mutable std::shared_mutex m_mutex;

 public:
  /**
   * Constructor
   * @param capacity: IMCU capacity (number of rows)
   * @param num_columns: Number of columns
   * @param enable_column_offsets: Whether to enable column offset tables
   */
  RowDirectory(size_t capacity, size_t num_columns, bool enable_column_offsets = false)
      : m_capacity(capacity), m_enable_column_offsets(enable_column_offsets), m_num_columns(num_columns) {
    // Allocate row entry array
    m_entries = std::make_unique<RowEntry[]>(capacity);

    // Initialize all entries
    for (size_t i = 0; i < capacity; i++) {
      m_entries[i] = RowEntry();
    }
  }

  // Delete copy constructor and assignment operator
  RowDirectory(const RowDirectory &) = delete;
  RowDirectory &operator=(const RowDirectory &) = delete;

  // Allow move operations
  RowDirectory(RowDirectory &&other) noexcept;
  RowDirectory &operator=(RowDirectory &&other) noexcept;

  ~RowDirectory() = default;
  /**
   * Set row entry
   * @param row_id: Row ID
   * @param offset: Start offset
   * @param length: Data length
   * @param is_compressed: Whether compressed
   */
  void set_row_entry(row_id_t row_id, uint32 offset, uint32 length, bool is_compressed = false);

  /**
   * Get row entry
   * @param row_id: Row ID
   * @return: Row entry (read-only)
   */
  const RowEntry *get_row_entry(row_id_t row_id) const;

  /**
   * Mark row as deleted
   */
  void mark_deleted(row_id_t row_id);

  /**
   * Mark row as containing NULL
   */
  void mark_has_null(row_id_t row_id);

  /**
   * Mark row as having overflow page
   */
  void mark_overflow(row_id_t row_id);

  /**
   * Build column offset table (for wide tables with many variable-length columns)
   * @param row_id: Row ID
   * @param column_offsets: Column offset array
   * @param column_lengths: Column length array
   */
  void build_column_offset_table(row_id_t row_id, const std::vector<uint16> &column_offsets,
                                 const std::vector<uint16> &column_lengths);

  /**
   * Get column offset table
   * @param row_id: Row ID
   * @return: Column offset table pointer, returns nullptr if not exists
   */
  const ColumnOffsetTable *get_column_offset_table(row_id_t row_id) const;

  /**
   * Get column offset within row (fast path)
   * @param row_id: Row ID
   * @param col_idx: Column index
   * @return: Column offset, returns UINT16_MAX on failure
   */
  uint16 get_column_offset(row_id_t row_id, uint32 col_idx) const;

  /**
   * Get actual column length
   * @param row_id: Row ID
   * @param col_idx: Column index
   * @return: Column length, returns 0 on failure
   */
  uint16 get_column_length(row_id_t row_id, uint32 col_idx) const;

  /**
   * Batch get row offsets (for vectorized scanning)
   * @param start_row: Start row
   * @param count: Number of rows
   * @param offsets: Output offset array (pre-allocated)
   * @param lengths: Output length array (pre-allocated)
   */
  void get_batch_offsets(row_id_t start_row, size_t count, uint32 *offsets, uint32 *lengths) const;

  /**
   * Update compression statistics
   * @param row_id: Row ID
   * @param original_size: Original size
   * @param compressed_size: Compressed size
   */
  void update_compression_stats(row_id_t row_id, size_t original_size, size_t compressed_size);

  /**
   * Get compression ratio
   * @return: Compression ratio [0.0, 1.0]
   */
  double get_compression_ratio() const;

  /**
   * Get directory size (bytes)
   */
  size_t get_directory_size() const;

  /**
   * Get total data size
   */
  size_t get_total_data_size() const { return m_total_data_size.load(); }

  /**
   * Get compressed data size
   */
  size_t get_compressed_data_size() const { return m_compressed_data_size.load(); }

  /**
   * Get overflow row count
   */
  size_t get_overflow_count() const { return m_overflow_count.load(); }

  /**
   * Validate directory integrity
   * @return: true indicates integrity is normal
   */
  bool validate() const;

  /**
   * Print directory summary
   */
  void dump_summary(std::ostream &out) const;

  std::unique_ptr<RowDirectory> clone() const;

 private:
  /**
   * Calculate checksum (simple CRC32)
   */
  uint32 compute_checksum(uint32 offset, uint32 length) const {
    // Simplified implementation, should use standard CRC32 in practice
    return offset ^ length ^ 0xDEADBEEF;
  }
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ROW0ROW_H__