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
#ifndef __SHANNONBASE_RAPID_TABLE_META_H__
#define __SHANNONBASE_RAPID_TABLE_META_H__
#include <atomic>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_const.h"

class Field;
namespace ShannonBase {
namespace Imcs {
/**
 * @brief Table load type (self-load or user-initiated).
 */
enum class LoadType {
  NOT_LOADED = 0,  ///< Table not yet loaded into memory.
  SELF_LOADED,     ///< Automatically loaded by system background process.
  USER_LOADED      ///< Explicitly loaded by user request.
};

struct SHANNON_ALIGNAS TableConfig {
  std::string tenant_name{SHANNON_DATA_AREAR_NAME};
  uint64_t max_table_mem_size{SHANNON_DEFAULT_MEMRORY_SIZE};
  uint64_t rows_per_imcu{SHANNON_ROWS_IN_CHUNK};
  uint64_t max_imcu_size_mb{SHANNON_MAX_IMCU_MEMRORY_SIZE};
};

struct SHANNON_ALIGNAS FieldMetadata {
  Field *source_fld{nullptr};
  uint32_t field_id{0};
  std::string field_name;
  enum_field_types type{MYSQL_TYPE_NULL};
  size_t pack_length{0};
  size_t normalized_length{0};
  bool nullable{false};
  bool is_key{false};
  bool is_secondary_field{false};

  Compress::Compression_level compression_level{Compress::Compression_level::DEFAULT};
  Compress::Encoding_type encoding;
  const CHARSET_INFO *charset{nullptr};
  std::shared_ptr<Compress::Dictionary> dictionary;

  double global_min{0.0}, global_max{0.0f};
  size_t distinct_count{0};
  double null_ratio{0.0};
};

struct SHANNON_ALIGNAS KeyPart {
  uint8_t null_bit{0};
  uint key_field_ind;
  uint16 key_part_flag{0}; /* 0 or HA_REVERSE_SORT */
  uint16 length{0};
};

struct SHANNON_ALIGNAS Key {
  std::string key_name;
  uint key_length{0};

  std::vector<KeyPart> key_parts;
};

struct SHANNON_ALIGNAS TableMetadata {
  // Basic information
  std::string db_name;
  std::string table_name;
  table_id_t table_id{0};

  bool db_low_byte_first{false}; /* Portable row format */
  // Column information
  uint32_t num_columns{0};
  std::vector<FieldMetadata> fields;
  std::vector<Key> keys;  // key name <-->key part info.
  std::vector<ulong> col_offsets;
  std::vector<ulong> null_byte_offsets;
  std::vector<ulong> null_bitmasks;

  // IMCU configuration
  size_t rows_per_imcu{SHANNON_ROWS_IN_CHUNK};             // Default 8192000
  size_t max_imcu_size_mb{SHANNON_MAX_IMCU_MEMRORY_SIZE};  // Default 256 MB

  // Global statistics
  std::atomic<uint64_t> total_imcus{0};
  std::atomic<uint64_t> total_rows{0};
  std::atomic<uint64_t> deleted_rows{0};
  std::atomic<uint64_t> version_count{0};

  std::atomic<uint64_t> mysql_access_count{0};            // MySQL access counts.
  std::atomic<uint64_t> heatwave_access_count{0};         // Rapid access counts.
  double importance{0.0};                                 // importance score.
  std::atomic<time_t> last_accessed{std::time(nullptr)};  // the laste access time.
  LoadType load_type{LoadType::NOT_LOADED};               // load type.
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_TABLE_META_H__