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
#include "storage/rapid_engine/imcs/cu.h"

#include <limits.h>
#include <cstring>
#include <random>
#include <sstream>

#include "sql/field.h"
#include "sql/field_common_properties.h"

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/utils/utils.h"
/*
   1. Compression (compress / decompress)
      m_data is pool-allocated for `capacity × normalized_length` bytes (the
      worst-case / uncompressed size).  Compressed data is always ≤ the
      original, so writing it back into the same buffer is safe.

      Algorithm selection:
        • LZ4  – tried first (lowest latency, good ratio for sorted/numeric data).
        • ZSTD – used instead when CU has SORTED encoding or ZSTD compression_level,
                 or when LZ4 fails to achieve the 10 % savings threshold.
        • If neither meets the threshold the CU is left uncompressed.

      Concurrent reads that arrive while the CU is compressed transparently
      decompress the whole block (write-through into m_data) before returning
      the requested value.  The operation is idempotent and protected by
      m_data_mutex.

   2. Serialization (serialize / deserialize)
      • Full binary snapshot with a CRC-32 (ISO 3309) trailer.
      • Endianness: host (little-endian on all supported x86/ARM targets).
      • Dictionary entries are stored verbatim from m_header.local_dict's
        internal storage vector so that dict IDs embedded in m_data remain
        valid after reload.
      • Version chains are flattened to a list of VersionEntry records;
        the chain ordering (newest-first) is not significant for recovery
        because we only need old values, not time ordering.
      • CRC-32 covers every byte written before the checksum itself.
        deserialize() verifies this before modifying any in-memory state.
*/
namespace ShannonBase {
namespace Imcs {
//  CRC-32 (ISO 3309, zlib polynomial 0xEDB88320)
/* static */
uint32_t CU::crc32_compute(const void *data, size_t len, uint32_t seed) {
  static constexpr uint32_t kPoly = 0xEDB88320u;
  uint32_t crc = ~seed;
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (kPoly & -(crc & 1u));
  }
  return ~crc;
}

//  CRC-accumulating ostream wrapper used during serialize()
// We cannot use a real stream transformer easily in vanilla C++, so we buffer
// a running CRC alongside each write via a thin helper.
struct CrcStream {
  std::ostream &out;
  uint32_t crc{0};
  size_t bytes_written{0};

  explicit CrcStream(std::ostream &s) : out(s) {}

  void write(const void *data, size_t n) {
    if (n == 0) return;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n));
    crc = CU::crc32_compute(data, n, crc);
    bytes_written += n;
  }

  template <typename T>
  void write_pod(const T &v) {
    write(&v, sizeof(T));
  }
};

// Dual-purpose stream reader that also accumulates a CRC.
struct CrcIStream {
  std::istream &in;
  uint32_t crc{0};

  explicit CrcIStream(std::istream &s) : in(s) {}

  bool read(void *buf, size_t n) {
    if (!in.read(reinterpret_cast<char *>(buf), static_cast<std::streamsize>(n))) return false;
    crc = CU::crc32_compute(buf, n, crc);
    return true;
  }

  template <typename T>
  bool read_pod(T &v) {
    return read(&v, sizeof(T));
  }
};

CU::CU(Imcu *owner, const FieldMetadata &field_meta, uint32 col_idx, size_t capacity,
       std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool)
    : m_memory_pool(mem_pool) {
  m_header.owner_imcu = owner;
  m_header.column_id = col_idx;
  m_header.field_metadata = field_meta.source_fld;
  m_header.type = field_meta.type;
  m_header.pack_length = field_meta.pack_length;
  m_header.normalized_length = field_meta.normalized_length;
  m_header.charset = field_meta.charset;
  m_header.local_dict = field_meta.dictionary;
  m_header.encoding = field_meta.encoding;
  m_header.compression_level = field_meta.compression_level;

  auto total_capacity = capacity * m_header.normalized_length;
  if (total_capacity < 64 * 1024) total_capacity = 64 * 1024;  // see the ref: MemoryPool::allocate_auto
  uchar *raw_ptr = static_cast<uchar *>(m_memory_pool->allocate_auto(total_capacity));
  if (!raw_ptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "CU memory allocation failed");
    return;
  }

  m_data = std::unique_ptr<uchar[], PoolDeleter>(raw_ptr, PoolDeleter(m_memory_pool, total_capacity));
  m_data_capacity.store(total_capacity, std::memory_order_relaxed);

  m_version_manager = std::make_unique<ColumnVersionManager>();
}

void CU::ColumnVersionManager::create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn,
                                              const uchar *old_value, size_t len) {
  std::unique_lock lock(m_mutex);

  auto nv = std::make_unique<Column_Version>();
  nv->txn_id = txn_id;
  nv->scn = scn;
  nv->timestamp = std::chrono::system_clock::now();
  nv->value_length = len;

  if (len != UNIV_SQL_NULL && old_value) {
    nv->old_value = std::make_unique<uchar[]>(len);
    std::memcpy(nv->old_value.get(), old_value, len);
  }

  auto it = m_versions.find(local_row_id);
  if (it != m_versions.end()) {
    nv->prev = it->second.release();
    it->second = std::move(nv);
  } else {
    m_versions[local_row_id] = std::move(nv);
  }
}

bool CU::ColumnVersionManager::get_value_at_scn(row_id_t /*local_row_id*/, uint64_t /*target_scn*/, uchar * /*buffer*/,
                                                size_t & /*len*/) const {
  return false;  // TODO: MVCC read path
}

size_t CU::ColumnVersionManager::purge(uint64_t min_active_scn) {
  size_t purged = 0;
  std::unique_lock lock(m_mutex);

  for (auto it = m_versions.begin(); it != m_versions.end();) {
    Column_Version *head = it->second.get();
    Column_Version *current = head;
    Column_Version *prev_valid = nullptr;
    bool found_visible = false;

    while (current != nullptr) {
      if (current->scn < min_active_scn && found_visible) {
        Column_Version *to_delete = current;
        current = current->prev;
        if (prev_valid) prev_valid->prev = current;
        delete to_delete;
        ++purged;
      } else {
        found_visible = true;
        prev_valid = current;
        current = current->prev;
      }
    }
    it = (head == nullptr) ? m_versions.erase(it) : ++it;
  }
  return purged;
}

std::vector<CU::ColumnVersionManager::VersionEntry> CU::ColumnVersionManager::snapshot() const {
  std::shared_lock lock(m_mutex);
  std::vector<VersionEntry> result;
  result.reserve(m_versions.size());

  for (const auto &[rid, head] : m_versions) {
    // Walk the entire chain and flatten it (newest → oldest).
    for (const Column_Version *cur = head.get(); cur != nullptr; cur = cur->prev) {
      VersionEntry e;
      e.row_id = rid;
      e.txn_id = cur->txn_id;
      e.scn = cur->scn;
      e.value_length = cur->value_length;
      if (cur->value_length != UNIV_SQL_NULL && cur->old_value) {
        e.value_data.assign(cur->old_value.get(), cur->old_value.get() + cur->value_length);
      }
      result.push_back(std::move(e));
    }
  }
  return result;
}

const uchar *CU::get_data_address(row_id_t local_row_id) const {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return nullptr;
  return m_data.get() + local_row_id * m_header.normalized_length;
}

size_t CU::get_data_size() const {
  auto rows = m_header.owner_imcu ? m_header.owner_imcu->get_row_count() : 0u;
  return rows * m_header.normalized_length;
}

int CU::write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len) {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return false;

  std::lock_guard lock(m_data_mutex);

  // If currently compressed, decompress before writing.
  if (m_is_compressed.load(std::memory_order_relaxed)) decompress_locked();

  uchar *dest = m_data.get() + local_row_id * m_header.normalized_length;

  if (data == nullptr) {
    std::memset(dest, 0, m_header.normalized_length);
  } else {
    if (m_header.local_dict && m_header.field_metadata->real_type() != MYSQL_TYPE_ENUM) {
      uint32 dict_id = m_header.local_dict->store(data, len, m_header.encoding);
      std::memcpy(dest, &dict_id, sizeof(uint32));
    } else {
      std::memcpy(dest, data, std::min(len, m_header.normalized_length));
    }
    update_statistics(data, len);
  }
  return ShannonBase::SHANNON_SUCCESS;
  ;
}

int CU::update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len) {
  uchar old_value[MAX_FIELD_WIDTH] = {0};
  size_t old_len = read(context, local_row_id, old_value);

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;
  m_version_manager->create_version(local_row_id, txn_id, scn, old_value, old_len);

  {
    std::lock_guard lock(m_data_mutex);
    if (m_is_compressed.load(std::memory_order_relaxed)) decompress_locked();

    auto dest = static_cast<void *>(const_cast<uchar *>(get_data_address(local_row_id)));
    if (len == UNIV_SQL_NULL) {
      std::memset(dest, 0, m_header.normalized_length);
    } else if (m_header.local_dict && m_header.field_metadata->real_type() != MYSQL_TYPE_ENUM) {
      uint32 dict_id = m_header.local_dict->store(new_data, len, m_header.encoding);
      std::memcpy(dest, &dict_id, sizeof(uint32));
    } else {
      std::memcpy(dest, new_data, std::min(len, m_header.normalized_length));
    }
  }

  update_statistics(new_data, len);
  return ShannonBase::SHANNON_SUCCESS;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return 0;

  if (m_header.owner_imcu->is_null(m_header.column_id, local_row_id)) return UNIV_SQL_NULL;

  // If compressed, decompress the whole block first (write-through).
  if (m_is_compressed.load(std::memory_order_acquire)) {
    std::lock_guard lock(m_data_mutex);
    if (m_is_compressed.load(std::memory_order_relaxed)) const_cast<CU *>(this)->decompress_locked();
  }

  const uchar *src = m_data.get() + local_row_id * m_header.normalized_length;

  if (m_header.local_dict && m_header.field_metadata->real_type() != MYSQL_TYPE_ENUM) {
    uint32 dict_id = *reinterpret_cast<const uint32 *>(src);
    auto decode_str = m_header.local_dict->get(dict_id);
    std::memcpy(buffer, decode_str.c_str(), decode_str.length());
    return decode_str.length();
  }

  std::memcpy(buffer, src, m_header.normalized_length);
  return m_header.normalized_length;
}

size_t CU::purge_versions(const Rapid_context *context, uint64_t min_active_scn) {
  return m_version_manager->purge(min_active_scn);
}

Compress::CompressAlgorithm *CU::select_compressor(CU_CompressAlgo *algo_out) const {
  // Use ZSTD for SORTED encoding (better ratio for sorted runs) or when the
  // caller explicitly requested ZSTD-class compression.
  bool prefer_zstd = (m_header.encoding == Compress::ENCODING_TYPE::SORTED) ||
                     (m_header.compression_level == Compress::COMPRESS_LEVEL::ZSTD);

  if (prefer_zstd) {
    if (algo_out) *algo_out = CU_CompressAlgo::ZSTD;
    return Compress::get_compressor(Compress::COMPRESS_ALGO::ZSTD);
  }
  if (algo_out) *algo_out = CU_CompressAlgo::LZ4;
  return Compress::get_compressor(Compress::COMPRESS_ALGO::LZ4);
}

int CU::compress() {
  // Fast path: already compressed or disabled.
  if (m_is_compressed.load(std::memory_order_acquire)) return ShannonBase::SHANNON_SUCCESS;
  if (m_header.compression_level == Compress::COMPRESS_LEVEL::NONE) return ShannonBase::SHANNON_SUCCESS;

  std::lock_guard lock(m_data_mutex);
  // Re-check under lock (another thread may have just compressed).
  if (m_is_compressed.load(std::memory_order_relaxed)) return ShannonBase::SHANNON_SUCCESS;

  const size_t data_sz = get_data_size();
  if (data_sz < 1024) return ShannonBase::SHANNON_SUCCESS;  // too small to benefit

  // Phase 1: try the preferred algorithm
  CU_CompressAlgo algo1;
  auto *compressor1 = select_compressor(&algo1);

  std::string_view input(reinterpret_cast<const char *>(m_data.get()), data_sz);
  std::string compressed = compressor1->compress(input);

  constexpr double kThreshold = 0.90;  // must shrink by ≥ 10 %
  if (!compressed.empty() && compressed.size() < static_cast<size_t>(data_sz * kThreshold)) {
    // Good ratio — commit.
    std::memcpy(m_data.get(), compressed.data(), compressed.size());
    m_original_data_size.store(data_sz, std::memory_order_relaxed);
    m_compressed_data_size.store(compressed.size(), std::memory_order_relaxed);
    m_compress_algo_used.store(static_cast<uint8_t>(algo1), std::memory_order_relaxed);
    m_is_compressed.store(true, std::memory_order_release);
    return ShannonBase::SHANNON_SUCCESS;
  }

  // Phase 2: fall back to the other algorithm
  CU_CompressAlgo algo2 = (algo1 == CU_CompressAlgo::LZ4) ? CU_CompressAlgo::ZSTD : CU_CompressAlgo::LZ4;
  auto *compressor2 = Compress::get_compressor((algo2 == CU_CompressAlgo::ZSTD) ? Compress::COMPRESS_ALGO::ZSTD
                                                                                : Compress::COMPRESS_ALGO::LZ4);

  std::string compressed2 = compressor2->compress(input);
  if (!compressed2.empty() && compressed2.size() < static_cast<size_t>(data_sz * kThreshold)) {
    std::memcpy(m_data.get(), compressed2.data(), compressed2.size());
    m_original_data_size.store(data_sz, std::memory_order_relaxed);
    m_compressed_data_size.store(compressed2.size(), std::memory_order_relaxed);
    m_compress_algo_used.store(static_cast<uint8_t>(algo2), std::memory_order_relaxed);
    m_is_compressed.store(true, std::memory_order_release);
    return true;
  }

  // Neither algorithm met the threshold — leave data uncompressed.
  return HA_ERR_GENERIC;
}

int CU::decompress() {
  if (!m_is_compressed.load(std::memory_order_acquire)) return ShannonBase::SHANNON_SUCCESS;
  std::lock_guard lock(m_data_mutex);
  return decompress_locked();
}

int CU::decompress_locked() {
  if (!m_is_compressed.load(std::memory_order_relaxed)) return ShannonBase::SHANNON_SUCCESS;

  const size_t compressed_sz = m_compressed_data_size.load(std::memory_order_relaxed);
  const size_t original_sz = m_original_data_size.load(std::memory_order_relaxed);
  const auto algo = static_cast<CU_CompressAlgo>(m_compress_algo_used.load(std::memory_order_relaxed));
  Compress::CompressAlgorithm *decompressor =
      (algo == CU_CompressAlgo::ZSTD)   ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZSTD)
      : (algo == CU_CompressAlgo::ZLIB) ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZLIB)
                                        : Compress::get_compressor(Compress::COMPRESS_ALGO::LZ4);

  std::string_view payload(reinterpret_cast<const char *>(m_data.get()), compressed_sz);
  std::string plain = decompressor->decompress(payload);
  if (plain.size() != original_sz) {
    DBUG_PRINT("cu_decompress", ("size mismatch: expected %zu, got %zu", original_sz, plain.size()));
    return HA_ERR_GENERIC;
  }

  std::memcpy(m_data.get(), plain.data(), original_sz);

  m_is_compressed.store(false, std::memory_order_release);
  m_compressed_data_size.store(0, std::memory_order_relaxed);
  // m_original_data_size is kept; it equals get_data_size() when uncompressed.
  return ShannonBase::SHANNON_SUCCESS;
}

void CU::update_statistics(const uchar *data, size_t /*len*/) {
  if (!is_numeric_type(m_header.type) && !is_temporal_type(m_header.type)) return;

  double value = Utils::Util::get_field_numeric<double>(m_header.field_metadata, data, nullptr);
  m_header.sum.fetch_add(value);
  m_header.min_value.store(std::min(m_header.min_value.load(std::memory_order_relaxed), value));
  m_header.max_value.store(std::max(m_header.max_value.load(std::memory_order_relaxed), value));
}

int CU::serialize(std::ostream &out, size_t snapshot_row_count) const {
  // If the CU is currently compressed, we can serialize the compressed payload
  // directly (saves I/O).  If not, we compress on-the-fly into a temp buffer
  // so the snapshot is as compact as possible.

  std::lock_guard lock(m_data_mutex);
  if (snapshot_row_count == 0) snapshot_row_count = m_header.owner_imcu ? m_header.owner_imcu->get_row_count() : 0;

  const size_t original_sz = snapshot_row_count * m_header.normalized_length;
  // Prepare data payload (we may compress on-the-fly if not already so).
  bool payload_compressed = m_is_compressed.load(std::memory_order_relaxed);
  size_t payload_size = 0;
  std::string temp_compressed;  // non-empty when we just compressed
  CU_CompressAlgo payload_algo = CU_CompressAlgo::NONE;

  if (payload_compressed) {
    // Already compressed — use the bytes already in m_data.
    payload_size = m_compressed_data_size.load(std::memory_order_relaxed);
    payload_algo = static_cast<CU_CompressAlgo>(m_compress_algo_used.load(std::memory_order_relaxed));
  } else {
    // Try to compress on-the-fly for a more compact snapshot.
    if (m_header.compression_level != Compress::COMPRESS_LEVEL::NONE && original_sz >= 1024) {
      CU_CompressAlgo algo;
      auto *comp = const_cast<CU *>(this)->select_compressor(&algo);
      std::string_view input(reinterpret_cast<const char *>(m_data.get()), original_sz);
      temp_compressed = comp->compress(input);

      if (!temp_compressed.empty() && temp_compressed.size() < original_sz * 9 / 10) {
        payload_compressed = true;
        payload_size = temp_compressed.size();
        payload_algo = algo;
      }
    }
    if (!payload_compressed) payload_size = original_sz;
  }

  // Determine presence of optional sections.
  bool has_dict = (m_header.local_dict && m_header.local_dict->size() > 1);
  auto version_snap = m_version_manager->snapshot();
  bool has_versions = !version_snap.empty();

  uint8_t flags = 0;
  if (payload_compressed) flags |= CU_FLAG_COMPRESSED;
  if (has_dict) flags |= CU_FLAG_HAS_DICT;
  if (has_versions) flags |= CU_FLAG_HAS_VERSIONS;

  // Write through CrcStream so we compute the checksum as we go
  CrcStream cs(out);

  // Fixed header (20 bytes)
  cs.write_pod(CU_SERIAL_MAGIC);                          // 4
  cs.write_pod(CU_FORMAT_VERSION);                        // 2
  cs.write_pod(flags);                                    // 1
  cs.write_pod(m_header.column_id);                       // 4
  cs.write_pod(static_cast<uint8_t>(m_header.type));      // 1
  cs.write_pod(static_cast<uint8_t>(m_header.encoding));  // 1
  cs.write_pod(static_cast<uint8_t>(payload_algo));       // 1
  const uint8_t reserved[6] = {};
  cs.write(reserved, 6);  // 6
  // total: 4+2+1+4+1+1+1+6 = 20 ✓

  // Lengths (32 bytes)
  cs.write_pod(static_cast<uint64_t>(m_header.pack_length));        // 8
  cs.write_pod(static_cast<uint64_t>(m_header.normalized_length));  // 8
  cs.write_pod(static_cast<uint64_t>(original_sz));                 // 8
  cs.write_pod(static_cast<uint64_t>(snapshot_row_count));          // 8
  // total: 32 ✓

  // Zone map (24 bytes)
  double zm_min = m_header.min_value.load(std::memory_order_acquire);
  double zm_max = m_header.max_value.load(std::memory_order_acquire);
  double zm_sum = m_header.sum.load(std::memory_order_acquire);
  cs.write_pod(zm_min);
  cs.write_pod(zm_max);
  cs.write_pod(zm_sum);
  // total: 24 ✓

  // Data payload
  cs.write_pod(static_cast<uint64_t>(payload_size));  // 8

  if (payload_compressed && !temp_compressed.empty()) {
    cs.write(temp_compressed.data(), payload_size);
  } else if (payload_compressed) {
    // m_data currently holds the compressed bytes.
    cs.write(m_data.get(), payload_size);
  } else {
    // Uncompressed — write raw data.
    cs.write(m_data.get(), payload_size);
  }

  // Dictionary
  if (has_dict) {
    uint32_t entry_count = static_cast<uint32_t>(m_header.local_dict->content_size());
    cs.write_pod(entry_count);

    // Entry 0 is reserved; start from 1.
    for (uint32_t id = 1; id < entry_count; ++id) {
      // Retrieve the raw stored string (flag byte + payload).
      // We call get() which decompresses; to persist the compressed form we
      // need the internal storage.  For now store the decoded value and re-
      // encode on load — this is the safest cross-version approach.
      std::string value = m_header.local_dict->get(id);
      uint64_t vlen = static_cast<uint64_t>(value.size());
      cs.write_pod(vlen);
      if (vlen > 0) cs.write(value.data(), vlen);
    }
  }

  // Version journal
  if (has_versions) {
    uint64_t vcnt = static_cast<uint64_t>(version_snap.size());
    cs.write_pod(vcnt);

    for (const auto &ve : version_snap) {
      cs.write_pod(static_cast<uint64_t>(ve.row_id));
      cs.write_pod(static_cast<uint64_t>(ve.txn_id));
      cs.write_pod(ve.scn);
      cs.write_pod(static_cast<uint64_t>(ve.value_length));
      if (ve.value_length != UNIV_SQL_NULL && !ve.value_data.empty())
        cs.write(ve.value_data.data(), ve.value_data.size());
    }
  }

  // Checksum trailer
  uint32_t checksum = cs.crc;
  out.write(reinterpret_cast<const char *>(&checksum), sizeof(checksum));
  return out.good() ? ShannonBase::SHANNON_SUCCESS : HA_ERR_GENERIC;
}

int CU::deserialize(std::istream &in) {
  // Step 1: Read entire record into a memory buffer so we can verify CRC
  // We read the stream progressively via CrcIStream, then verify at the end
  // before touching any live state.

  CrcIStream cis(in);

  // Fixed header
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t flags = 0;
  uint32_t col_id = 0;
  uint8_t ftype = 0;
  uint8_t enc = 0;
  uint8_t algo = 0;
  uint8_t reserved[6] = {};

  if (!cis.read_pod(magic) || magic != CU_SERIAL_MAGIC) return false;
  if (!cis.read_pod(version) || version != CU_FORMAT_VERSION) return false;
  if (!cis.read_pod(flags)) return false;
  if (!cis.read_pod(col_id)) return false;
  if (!cis.read_pod(ftype)) return false;
  if (!cis.read_pod(enc)) return false;
  if (!cis.read_pod(algo)) return false;
  if (!cis.read(reserved, 6)) return false;

  // Lengths
  uint64_t pack_len = 0, norm_len = 0, orig_sz = 0, row_cnt = 0;
  if (!cis.read_pod(pack_len)) return false;
  if (!cis.read_pod(norm_len)) return false;
  if (!cis.read_pod(orig_sz)) return false;
  if (!cis.read_pod(row_cnt)) return false;

  // Zone map
  double zm_min = 0, zm_max = 0, zm_sum = 0;
  if (!cis.read_pod(zm_min)) return false;
  if (!cis.read_pod(zm_max)) return false;
  if (!cis.read_pod(zm_sum)) return false;

  // Data payload
  uint64_t payload_sz = 0;
  if (!cis.read_pod(payload_sz)) return false;

  std::vector<uchar> payload(payload_sz);
  if (payload_sz > 0 && !cis.read(payload.data(), payload_sz)) return false;

  // Dictionary (optional)
  std::vector<std::string> dict_entries;
  if (flags & CU_FLAG_HAS_DICT) {
    uint32_t entry_count = 0;
    if (!cis.read_pod(entry_count)) return false;
    dict_entries.resize(entry_count);
    for (uint32_t i = 1; i < entry_count; ++i) {
      uint64_t vlen = 0;
      if (!cis.read_pod(vlen)) return false;
      if (vlen > 0) {
        dict_entries[i].resize(vlen);
        if (!cis.read(&dict_entries[i][0], vlen)) return false;
      }
    }
  }

  // Version journal (optional)
  struct VersionRec {
    uint64_t row_id, txn_id, scn, val_len;
    std::vector<uchar> val_data;
  };
  std::vector<VersionRec> version_recs;
  if (flags & CU_FLAG_HAS_VERSIONS) {
    uint64_t vcnt = 0;
    if (!cis.read_pod(vcnt)) return HA_ERR_GENERIC;
    version_recs.resize(vcnt);
    for (auto &vr : version_recs) {
      if (!cis.read_pod(vr.row_id)) return false;
      if (!cis.read_pod(vr.txn_id)) return false;
      if (!cis.read_pod(vr.scn)) return false;
      if (!cis.read_pod(vr.val_len)) return false;
      if (vr.val_len != UNIV_SQL_NULL && vr.val_len > 0) {
        vr.val_data.resize(vr.val_len);
        if (!cis.read(vr.val_data.data(), vr.val_len)) return HA_ERR_GENERIC;
      }
    }
  }

  // Checksum verification
  uint32_t stored_crc = 0;
  if (!in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc))) return HA_ERR_GENERIC;

  if (cis.crc != stored_crc) {
    DBUG_PRINT("cu_deserialize", ("CRC mismatch: computed 0x%08x stored 0x%08x", cis.crc, stored_crc));
    return HA_ERR_GENERIC;
  }

  // All checks passed — commit to live state
  std::lock_guard lock(m_data_mutex);

  // Ensure our buffer is large enough.
  if (orig_sz > m_data_capacity.load(std::memory_order_relaxed)) {
    DBUG_PRINT("cu_deserialize", ("snapshot size %llu exceeds buffer capacity %llu", (unsigned long long)orig_sz,
                                  (unsigned long long)m_data_capacity.load()));
    return HA_ERR_GENERIC;
  }

  // Decompress payload if needed before writing into m_data.
  bool snap_compressed = (flags & CU_FLAG_COMPRESSED) != 0;

  if (snap_compressed) {
    auto snap_algo = static_cast<CU_CompressAlgo>(algo);
    Compress::CompressAlgorithm *decomp =
        (snap_algo == CU_CompressAlgo::ZSTD)   ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZSTD)
        : (snap_algo == CU_CompressAlgo::ZLIB) ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZLIB)
                                               : Compress::get_compressor(Compress::COMPRESS_ALGO::LZ4);

    std::string_view cv(reinterpret_cast<const char *>(payload.data()), payload_sz);
    std::string plain = decomp->decompress(cv);
    if (plain.size() != static_cast<size_t>(orig_sz)) return HA_ERR_GENERIC;
    std::memcpy(m_data.get(), plain.data(), orig_sz);
  } else {
    std::memcpy(m_data.get(), payload.data(), payload_sz);
  }

  // Mark uncompressed after loading (runtime can re-compress if desired).
  m_is_compressed.store(false, std::memory_order_release);
  m_original_data_size.store(orig_sz, std::memory_order_relaxed);
  m_compressed_data_size.store(0, std::memory_order_relaxed);
  m_compress_algo_used.store(static_cast<uint8_t>(CU_CompressAlgo::NONE), std::memory_order_relaxed);

  // Restore zone-map statistics.
  m_header.pack_length = static_cast<size_t>(pack_len);
  m_header.normalized_length = static_cast<size_t>(norm_len);
  m_header.min_value.store(zm_min, std::memory_order_relaxed);
  m_header.max_value.store(zm_max, std::memory_order_relaxed);
  m_header.sum.store(zm_sum, std::memory_order_relaxed);

  // Restore dictionary entries.
  if ((flags & CU_FLAG_HAS_DICT) && m_header.local_dict) {
    for (size_t i = 1; i < dict_entries.size(); ++i) {
      const auto &val = dict_entries[i];
      if (!val.empty()) {
        m_header.local_dict->store(reinterpret_cast<const uchar *>(val.data()), val.size(), m_header.encoding);
      }
    }
  }

  // Restore version journal.
  for (const auto &vr : version_recs) {
    const uchar *val_ptr = vr.val_len != UNIV_SQL_NULL && !vr.val_data.empty() ? vr.val_data.data() : nullptr;
    m_version_manager->create_version(static_cast<row_id_t>(vr.row_id), static_cast<Transaction::ID>(vr.txn_id), vr.scn,
                                      val_ptr, static_cast<size_t>(vr.val_len));
  }
  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace Imcs
}  // namespace ShannonBase