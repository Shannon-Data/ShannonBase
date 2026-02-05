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
namespace Imcs {
class Dictionary;
class RpdTable;
class Imcu;
class CU : public MemoryObject {
 public:
  // CU Header
  struct SHANNON_ALIGNAS CU_header {
    // Basic Information
    Imcu *owner_imcu{nullptr};  // Back reference to IMCU
    uint32 column_id{0};        // Column index

    Field *field_metadata{nullptr};          // Field metadata
    enum_field_types type{MYSQL_TYPE_NULL};  // Data type
    size_t pack_length{0};                   // Original length
    size_t normalized_length{0};             // Normalized length
    const CHARSET_INFO *charset{nullptr};

    // Encoding and Compression
    Compress::ENCODING_TYPE encoding{Compress::ENCODING_TYPE::NONE};
    Compress::COMPRESS_LEVEL compression_level{Compress::COMPRESS_LEVEL::DEFAULT};
    std::shared_ptr<Compress::Dictionary> local_dict{nullptr};

    // Column-Level Statistics (for Zone Maps / Storage Index optimization)
    // These are essential for query optimization (predicate pushdown, IMCU pruning)
    std::atomic<double> min_value{DBL_MAX};
    std::atomic<double> max_value{DBL_MIN};
    std::atomic<double> sum{0};
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
   * Read value (returns current value, does not consider versions)
   * @param local_row_id: Local row ID
   * @param buffer: Output buffer
   * @return: Data length, returns UNIV_SQL_NULL for NULL
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const;

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

  bool compress();

  bool decompress();

  /**
   * Update statistics (min/max/sum for Zone Maps)
   */
  void update_statistics(const uchar *data, size_t len);

  ColumnStatistics get_statistics() const;

  inline Field *field() const { return m_header.field_metadata; }
  inline Compress::Dictionary *dictionary() const { return m_header.local_dict.get(); }
  inline enum_field_types get_type() const { return m_header.type; }
  inline size_t get_normalized_length() const { return m_header.normalized_length; }
  inline Field *get_source_field() const { return m_header.field_metadata; }

  bool serialize(std::ostream &out) const;
  bool deserialize(std::istream &in);

  const uchar *get_data_address(row_id_t local_row_id) const;

  size_t get_data_size() const;

 private:
  inline bool needs_dictionary() const {
    return (m_header.type == MYSQL_TYPE_VARCHAR || m_header.type == MYSQL_TYPE_STRING ||
            m_header.type == MYSQL_TYPE_VAR_STRING);
  }

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