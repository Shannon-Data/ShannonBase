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
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/memory_pool.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/imcs/varlen0data.h"

class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;
namespace Imcs {
class Dictionary;
class RpdTable;
class Imcu;
class CU : public MemoryObject {
 public:
  // CU Header
  struct SHANNON_ALIGNAS CU_header {
    // Basic Information
    Imcu *owner_imcu{nullptr};         // Back reference to IMCU
    std::atomic<uint32> column_id{0};  // Column index

    Field *field_metadata{nullptr};            // Field metadata
    enum_field_types type{MYSQL_TYPE_NULL};    // Data type
    std::atomic<size_t> pack_length{0};        // Original length
    std::atomic<size_t> normalized_length{0};  // Normalized length
    const CHARSET_INFO *charset{nullptr};

    // Encoding and Compression
    Compress::Encoding_type encoding{Compress::Encoding_type::NONE};
    Compress::Compression_level compression_level{Compress::Compression_level::DEFAULT};

    // Local dictionary (for string encoding)
    std::shared_ptr<Compress::Dictionary> local_dict{nullptr};

    // Column-Level Statistics (within IMCU)
    std::atomic<double> min_value{DBL_MAX};
    std::atomic<double> max_value{DBL_MIN};
    std::atomic<double> sum{0};
    std::atomic<double> avg{0};

    std::atomic<size_t> total_count{0};
    std::atomic<size_t> null_count{0};
    std::atomic<size_t> distinct_count{0};  // Estimated value

    // Data Layout
    std::atomic<size_t> capacity{0};         // Capacity (number of rows)
    std::atomic<size_t> data_size{0};        // Actual data size
    std::atomic<size_t> compressed_size{0};  // Compressed size

    // Version Information
    std::atomic<size_t> version_count{0};      // Number of versions
    std::atomic<size_t> version_data_size{0};  // Version data size
  };

 public:
  CU(Imcu *owner, const FieldMetadata &field_meta, uint32 col_idx, size_t capacity,
     std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool);

  virtual ~CU() = default;

  /**
   * Write value (for INSERT)
   * @param local_row_id: Local row ID
   * @param data: Data pointer (NULL handled by IMCU's null_mask)
   * @param len: Data length
   * @return: Returns true if successful
   */
  bool write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len);

  /**
   * Update value (for UPDATE)
   * - Create column-level version
   * - Write new value
   * @param local_row_id: Local row ID
   * @param new_data: New data
   * @param len: Length
   * @param context: Context (contains transaction information)
   * @return: Returns SHANNON_SUCCESS if successful
   */
  int update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len);

  /**
   * Batch write (optimized version, for initial loading)
   * @param start_row: Starting row
   * @param data_array: Data array
   * @param count: Number of rows
   */
  bool write_batch(const Rapid_context *context, row_id_t start_row, const std::vector<uchar *> &data_array,
                   size_t count);

  /**
   * Read value (returns current value, does not consider versions)
   * @param local_row_id: Local row ID
   * @param buffer: Output buffer
   * @return: Data length, returns UNIV_SQL_NULL for NULL
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const;

  /**
   * Read value (specified SCN version)
   * @param local_row_id: Local row ID
   * @param target_scn: Target SCN
   * @param buffer: Output buffer
   * @return: Data length
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uint64_t target_scn, uchar *buffer) const;

  /**
   * Get value pointer (zero-copy, for scanning)
   * @param local_row_id: Local row ID
   * @return: Data pointer (directly points to internal buffer)
   */
  const uchar *read(const Rapid_context *context, row_id_t local_row_id);

  /**
   * Batch read (vectorized)
   * @param row_ids: List of row IDs to read
   * @param output: Output buffer (pre-allocated)
   * @return: Number of rows read
   */
  size_t read_batch(const Rapid_context *context, const std::vector<row_id_t> &row_ids, uchar *output) const;

  /**
   * Scan column (continuous read)
   * @param start_row: Starting row
   * @param count: Number of rows
   * @param output: Output buffer
   */
  size_t scan_range(const Rapid_context *context, row_id_t start_row, size_t count, uchar *output) const;

  /**
   * Create version (called during UPDATE)
   * @param local_row_id: Local row ID
   * @param context: Context
   */
  void create_version(const Rapid_context *context, row_id_t local_row_id);

  /**
   * Get version count
   */
  inline size_t get_version_count(const Rapid_context *context) const { return m_version_manager->get_version_count(); }

  /**
   * Clean up old versions
   * @param min_active_scn: Minimum active SCN
   * @return: Number of bytes reclaimed
   */
  size_t purge_versions(const Rapid_context *context, uint64_t min_active_scn);

  /**
   * Dictionary encoding (for strings)
   * @param data: Original data
   * @param len: Length
   * @param encoded: Encoded value (dictionary ID)
   * @return: Returns true if successful
   */
  bool encode_value(const Rapid_context *context, const uchar *data, size_t len, uint32 &encoded);

  /**
   * Dictionary decoding
   * @param encoded: Dictionary ID
   * @param buffer: Output buffer
   * @param len: Output length
   * @return: Returns true if successful
   */
  bool decode_value(const Rapid_context *context, uint32 encoded, uchar *buffer, size_t &len) const;

  /**
   * Compress column data (background compression)
   */
  bool compress();

  /**
   * Decompress column data
   */
  bool decompress();

  /**
   * Update statistics
   */
  void update_statistics(const uchar *data, size_t len);

  /**
   * Get column statistics
   */
  ColumnStatistics get_statistics() const;

  inline size_t get_data_size() const { return m_header.data_size; }
  inline size_t get_capacity() const { return m_header.capacity; }
  inline enum_field_types get_type() const { return m_header.type; }
  inline size_t get_normalized_length() const { return m_header.normalized_length; }
  inline Field *get_source_field() const { return m_header.field_metadata; }

  bool serialize(std::ostream &out) const;
  bool deserialize(std::istream &in);

  /**
   * Get data address
   */
  inline const uchar *get_data_address(row_id_t local_row_id) const {
    if (local_row_id >= m_header.capacity) return nullptr;
    return (m_data.get() + local_row_id * m_header.normalized_length);
  }

 private:
  /**
   * Check if dictionary encoding is needed
   */
  inline bool needs_dictionary() const { return true; }

  /**
   * Calculate numeric statistics
   */
  double get_numeric_value(const Rapid_context *context, const uchar *data, size_t len) const;

 private:
  const char *m_magic = "SHANNON_CU";
  CU_header m_header;

  // Data Storage
  // Main data area (continuous memory, normalized length)
  struct PoolDeleter {
    std::weak_ptr<ShannonBase::Utils::MemoryPool> pool;
    size_t size;

    PoolDeleter(std::shared_ptr<ShannonBase::Utils::MemoryPool> p, size_t s) : pool(p), size(s) {}
    PoolDeleter() : pool(), size(0) {}
    PoolDeleter(const PoolDeleter &other) = default;
    PoolDeleter &operator=(const PoolDeleter &other) = default;

    void operator()(uchar *ptr) const {
      auto sp = pool.lock();
      if (sp && ptr) {
        try {
          sp->deallocate(ptr, size);
        } catch (const std::exception &e) {
          // Log error but don't throw in destructor
          DBUG_PRINT("memory_pool", ("PoolDeleter failed: %s", e.what()));
        }
      }
    }
  };

  std::unique_ptr<uchar[], PoolDeleter> m_data;
  std::atomic<size_t> m_data_capacity;

  // Variable-length data area (optional, for long strings)
  std::unique_ptr<VarlenDataPool> m_varlen_pool;

  // Column-Level Version Management
  /**
   * Column Version Manager
   * - Only creates versions for modified rows
   * - Only saves old values for this column
   */
  class ColumnVersionManager {
   public:
    struct SHANNON_ALIGNAS Column_Version {
      Transaction::ID txn_id{Transaction::MAX_ID};
      uint64_t scn{0};
      std::chrono::system_clock::time_point timestamp;

      // Column value (does not include metadata, metadata is in IMCU's Transaction Journal)
      std::unique_ptr<uchar[]> old_value{nullptr};
      size_t value_length{0};
      Column_Version *prev{nullptr};  // Version chain
    };

   private:
    // key: local_row_id, value: version chain head
    std::unordered_map<row_id_t, std::unique_ptr<Column_Version>> m_versions;
    mutable std::shared_mutex m_mutex;

   public:
    /**
     * Create version
     */
    void create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn, const uchar *old_value,
                        size_t len);

    /**
     * Get value at specified SCN
     */
    bool get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer, size_t &len) const;

    /**
     * Clean up old versions
     */
    size_t purge(uint64_t min_active_scn);

    /**
     * Get version count
     */
    size_t get_version_count() const {
      std::shared_lock lock(m_mutex);
      return m_versions.size();
    }
  };

  std::unique_ptr<ColumnVersionManager> m_version_manager{nullptr};

  // Locking and Concurrency Control
  // CU-level lock (protects data writes)
  // Read operations are typically lock-free (consistency ensured by IMCU's visibility judgment)
  std::mutex m_data_mutex;

  // CU local Memory Management Pool.
  std::shared_ptr<ShannonBase::Utils::MemoryPool> m_memory_pool;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CU_H__