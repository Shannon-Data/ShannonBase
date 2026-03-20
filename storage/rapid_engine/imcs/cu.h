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

#include "field_types.h"  // MYSQL_TYPE_XXX
#include "my_inttypes.h"  // uintxxx

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

/**
 * Algorithm tag persisted in the snapshot (decoupled from the runtime enum so
 * that the on-disk format stays stable even if Compress::COMPRESS_ALGO changes).
 */
enum class CU_CompressAlgo : uint8_t { NONE = 0, LZ4 = 1, ZSTD = 2, ZLIB = 3 };

class CU : public MemoryObject {
 public:
  struct SHANNON_ALIGNAS CU_header {
    Imcu *owner_imcu{nullptr};
    uint32 column_id{0};

    Field *field_metadata{nullptr};
    enum_field_types type{MYSQL_TYPE_NULL};
    size_t pack_length{0};
    size_t normalized_length{0};
    const CHARSET_INFO *charset{nullptr};

    Compress::ENCODING_TYPE encoding{Compress::ENCODING_TYPE::NONE};
    Compress::COMPRESS_LEVEL compression_level{Compress::COMPRESS_LEVEL::DEFAULT};
    std::shared_ptr<Compress::Dictionary> local_dict{nullptr};

    // Zone-map statistics (per-CU min/max/sum).
    std::atomic<double> min_value{DBL_MAX};
    std::atomic<double> max_value{DBL_MIN};
    std::atomic<double> sum{0};
  };

 public:
  CU(Imcu *owner, const FieldMetadata &field_meta, uint32 col_idx, size_t capacity,
     std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool);

  virtual ~CU() = default;

  int write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len);

  int update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len);

  /**
   * Read the current value of a cell.
   * If the CU is currently compressed the whole block is transparently
   * decompressed before reading (write-through: m_data is updated in-place).
   */
  size_t read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const;

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
   * │   checksum  4 B  CRC-32 (ISO 3309) of all preceding bytes    │
   * └──────────────────────────────────────────────────────────────┘
   *
   * @param out                 Binary output stream.
   * @param snapshot_row_count  Rows to snapshot; 0 = IMCU's current count.
   * @return true when the stream is still good after writing.
   */
  int serialize(std::ostream &out, size_t snapshot_row_count = 0) const;

  /**
   * Restore a CU from a binary snapshot produced by serialize().
   *
   * The CU must already be constructed (owner Imcu set, memory allocated).
   * CRC-32 is verified before any in-memory state is modified, so a corrupt
   * file leaves the CU unchanged.
   *
   * @param in  Binary input stream.
   * @return true on success (magic, version and CRC all matched).
   */
  int deserialize(std::istream &in);

  inline void patch_field_metadata(Field *f, const CHARSET_INFO *cs) {
    m_header.field_metadata = f;
    m_header.charset = cs;
  }

  void update_statistics(const uchar *data, size_t len);
  ColumnStatistics get_statistics() const;

  inline Field *field() const { return m_header.field_metadata; }
  inline Compress::Dictionary *dictionary() const { return m_header.local_dict.get(); }
  inline enum_field_types get_type() const { return m_header.type; }
  inline size_t get_normalized_length() const { return m_header.normalized_length; }
  inline Field *get_source_field() const { return m_header.field_metadata; }

  const uchar *get_data_address(row_id_t local_row_id) const;
  size_t get_data_size() const;

  /** CRC-32 (ISO 3309 / zlib polynomial 0xEDB88320). */
  static uint32_t crc32_compute(const void *data, size_t len, uint32_t seed = 0);

 private:
  inline bool needs_dictionary() const {
    return (m_header.type == MYSQL_TYPE_VARCHAR || m_header.type == MYSQL_TYPE_STRING ||
            m_header.type == MYSQL_TYPE_VAR_STRING);
  }

  /** Decompress without locking.  Caller MUST hold m_data_mutex write-lock. */
  int decompress_locked();

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
      }
    }
  };

  std::unique_ptr<uchar[], PoolDeleter> m_data;
  std::atomic<size_t> m_data_capacity{0};  // allocated bytes in m_data

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
      uint64_t scn{0};
      std::chrono::system_clock::time_point timestamp;

      std::unique_ptr<uchar[]> old_value{nullptr};
      size_t value_length{0};
      Column_Version *prev{nullptr};
    };

   private:
    std::unordered_map<row_id_t, std::unique_ptr<Column_Version>> m_versions;
    mutable std::shared_mutex m_mutex;

   public:
    void create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn, const uchar *old_value,
                        size_t len);

    bool get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer, size_t &len) const;

    size_t purge(uint64_t min_active_scn);

    size_t get_version_count() const {
      std::shared_lock lock(m_mutex);
      return m_versions.size();
    }

    // Flat snapshot of all version entries (used by serialize()).
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

  mutable std::mutex m_data_mutex;

  std::shared_ptr<ShannonBase::Utils::MemoryPool> m_memory_pool;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_CU_H__