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
#include <array>
#include <cstring>
#include <random>
#include <sstream>

#include "sql/field.h"
#include "sql/field_common_properties.h"

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/trx/transaction.h"
#include "storage/rapid_engine/utils/crc.h"
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
      • Full binary snapshot with a CRC-32C trailer.
      • Endianness: host (little-endian on all supported x86/ARM targets).
      • Dictionary entries are stored verbatim from m_header.field_desc.dictionary's
        internal storage vector so that dict IDs embedded in m_data remain
        valid after reload.
      • Version chains are flattened to a list of VersionEntry records;
        the chain ordering (newest-first) is not significant for recovery
        because we only need old values, not time ordering.
      • CRC-32C covers every byte written before the checksum itself.
        deserialize() verifies this before modifying any in-memory state.
*/
namespace ShannonBase {
namespace Imcs {
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
    crc = Utils::crc32c_compute(data, n, crc);
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
    crc = Utils::crc32c_compute(buf, n, crc);
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
  m_header.field_desc = {.src_field = field_meta.source_fld,
                         .type = field_meta.type,
                         .pack_length = field_meta.pack_length,
                         .normalized_length = field_meta.normalized_length,
                         .charset = field_meta.charset,
                         .encoding = field_meta.encoding,
                         .compression_level = field_meta.compression_level,
                         .dictionary = field_meta.dictionary};

  auto total_capacity = capacity * m_header.field_desc.normalized_length;
  if (total_capacity < 64 * 1024) total_capacity = 64 * 1024;  // see the ref: MemoryPool::allocate_auto
  uchar *raw_ptr = static_cast<uchar *>(m_memory_pool->allocate_auto(total_capacity));
  if (!raw_ptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "CU memory allocation failed");
    return;
  }

  m_data = std::unique_ptr<uchar[], PoolDeleter>(raw_ptr, PoolDeleter(m_memory_pool, total_capacity));
  m_data_capacity.store(total_capacity, std::memory_order_relaxed);

  m_version_manager = std::make_unique<ColumnVersionManager>();

  if (needs_varlen_pool()) {
    m_varlen_pool = std::make_unique<VarlenDataPool>(/*initial_size=*/256 * 1024, m_memory_pool);
  }
}

void CU::ColumnVersionManager::untrack_version_locked(Column_Version *version) {
  if (version == nullptr || version->scn != 0) return;

  auto it = m_active_versions.find(version->txn_id);
  if (it == m_active_versions.end()) return;

  auto &refs = it->second;
  refs.erase(std::remove_if(refs.begin(), refs.end(),
                            [version](const ActiveVersionRef &ref) { return ref.version == version; }),
             refs.end());
  if (refs.empty()) m_active_versions.erase(it);
}

void CU::ColumnVersionManager::create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn,
                                              const uchar *old_value, size_t len, const uchar *old_slot,
                                              size_t slot_len, VarlenDataPool *retained_pool, bool owns_varlen_ref) {
  std::unique_lock lock(m_mutex);

  auto nv = std::make_unique<Column_Version>();
  nv->txn_id = txn_id;
  nv->scn = scn;
  nv->timestamp = std::chrono::system_clock::now();
  nv->value_length = len;
  nv->retained_pool = retained_pool;
  nv->owns_varlen_ref = owns_varlen_ref;

  if (len != UNIV_SQL_NULL && old_value != nullptr && len > 0) {
    nv->old_value = std::make_unique<uchar[]>(len);
    std::memcpy(nv->old_value.get(), old_value, len);
  }
  if (old_slot != nullptr && slot_len > 0) {
    nv->old_slot.assign(old_slot, old_slot + slot_len);
  }

  Column_Version *raw = nv.get();
  auto it = m_versions.find(local_row_id);
  if (it != m_versions.end()) {
    nv->prev = std::move(it->second);
    it->second = std::move(nv);
  } else {
    m_versions[local_row_id] = std::move(nv);
  }

  if (scn == 0) m_active_versions[txn_id].push_back({local_row_id, raw});
}

bool CU::ColumnVersionManager::pop_head(row_id_t local_row_id, std::unique_ptr<Column_Version> &out,
                                        Transaction::ID expected_txn_id) {
  std::unique_lock lock(m_mutex);
  auto it = m_versions.find(local_row_id);
  if (it == m_versions.end() || !it->second) return false;
  if (expected_txn_id != Transaction::MAX_ID && it->second->txn_id != expected_txn_id) return false;

  out = std::move(it->second);
  untrack_version_locked(out.get());
  it->second = std::move(out->prev);
  if (!it->second) m_versions.erase(it);
  return true;
}

void CU::ColumnVersionManager::restore_head(row_id_t local_row_id, std::unique_ptr<Column_Version> head) {
  if (!head) return;
  std::unique_lock lock(m_mutex);

  Column_Version *raw = head.get();
  auto it = m_versions.find(local_row_id);
  if (it != m_versions.end()) {
    head->prev = std::move(it->second);
    it->second = std::move(head);
  } else {
    m_versions[local_row_id] = std::move(head);
  }
  if (raw->scn == 0) m_active_versions[raw->txn_id].push_back({local_row_id, raw});
}

void CU::ColumnVersionManager::commit_transaction(Transaction::ID txn_id, uint64_t commit_scn) {
  std::unique_lock lock(m_mutex);
  auto it = m_active_versions.find(txn_id);
  if (it == m_active_versions.end()) return;

  for (const ActiveVersionRef &ref : it->second) {
    if (ref.version == nullptr) continue;
    // A pointer is tracked only while the node is ACTIVE.  A per-operation
    // rollback removes it before destruction, so every remaining pointer is
    // stable under this mutex.
    ref.version->scn = commit_scn;
  }
  m_active_versions.erase(it);
}

std::vector<row_id_t> CU::ColumnVersionManager::active_rows(Transaction::ID txn_id) const {
  std::shared_lock lock(m_mutex);
  std::vector<row_id_t> rows;
  auto it = m_active_versions.find(txn_id);
  if (it == m_active_versions.end()) return rows;

  rows.reserve(it->second.size());
  for (const ActiveVersionRef &ref : it->second) rows.push_back(ref.row_id);
  return rows;
}

bool CU::ColumnVersionManager::get_before_image_for_snapshot(row_id_t local_row_id, Transaction::ID reader_txn_id,
                                                             uint64_t reader_scn, Transaction *reader_trx,
                                                             const char *table_name, BeforeImage &out) const {
  out = BeforeImage{};

  std::shared_lock lock(m_mutex);
  auto it = m_versions.find(local_row_id);
  if (it == m_versions.end() || !it->second) return false;

  const bool use_primary_read_view = reader_trx != nullptr && table_name != nullptr && reader_trx->has_snapshot();

  for (const Column_Version *version = it->second.get(); version != nullptr; version = version->prev.get()) {
    bool creator_visible = false;

    if (version->txn_id == reader_txn_id) {
      creator_visible = true;  // read-your-writes
    } else if (version->scn == 0) {
      creator_visible = false;  // another transaction is still ACTIVE in Rapid
    } else if (use_primary_read_view && version->txn_id != 0) {
      // InnoDB ReadView is the SQL visibility source of truth.
      creator_visible = reader_trx->changes_visible(version->txn_id, table_name);
    } else {
      // Internal/recovery callers without a primary snapshot use the Rapid
      // commit sequence only as a fallback.
      creator_visible = version->scn <= reader_scn;
    }

    if (creator_visible) break;

    // This update is invisible, so reconstruct the state immediately before it
    // and continue. If an older update is also invisible, its before-image will
    // replace this one on the next iteration.
    out.found = true;
    out.is_null = version->value_length == UNIV_SQL_NULL;
    out.logical_length = out.is_null ? 0 : version->value_length;
    out.slot = version->old_slot;
    out.logical_value.clear();
    if (!out.is_null && version->old_value != nullptr && version->value_length > 0) {
      out.logical_value.assign(version->old_value.get(), version->old_value.get() + version->value_length);
    }
  }

  return out.found;
}

bool CU::ColumnVersionManager::has_version_in_range(row_id_t start_row, size_t count) const {
  if (count == 0) return false;
  std::shared_lock lock(m_mutex);
  for (size_t i = 0; i < count; ++i) {
    if (m_versions.find(static_cast<row_id_t>(start_row + i)) != m_versions.end()) return true;
  }
  return false;
}

bool CU::ColumnVersionManager::get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer, size_t &len,
                                                Transaction::ID reader_txn_id) const {
  BeforeImage image;
  if (!get_before_image_for_snapshot(local_row_id, reader_txn_id, target_scn, nullptr, nullptr, image)) return false;

  if (image.is_null) {
    len = UNIV_SQL_NULL;
    return true;
  }

  len = image.logical_length;
  if (buffer != nullptr && !image.logical_value.empty())
    std::memcpy(buffer, image.logical_value.data(), image.logical_value.size());
  return true;
}

size_t CU::ColumnVersionManager::purge(uint64_t min_active_scn) {
  size_t purged = 0;
  std::unique_lock lock(m_mutex);

  for (auto it = m_versions.begin(); it != m_versions.end();) {
    auto &head_ptr = it->second;
    std::unique_ptr<Column_Version> *current_owner = &head_ptr;
    Column_Version *current = current_owner->get();

    while (current != nullptr) {
      // ACTIVE versions must never be purged. Committed versions become
      // reclaimable only when every writer/snapshot retention fence is newer.
      if (current->scn != 0 && current->scn < min_active_scn) {
        auto obsolete = std::move(*current_owner);
        *current_owner = std::move(obsolete->prev);
        current = current_owner->get();
        ++purged;
      } else {
        current_owner = &current->prev;
        current = current_owner->get();
      }
    }
    it = (!head_ptr) ? m_versions.erase(it) : ++it;
  }
  return purged;
}

std::vector<CU::ColumnVersionManager::VersionEntry> CU::ColumnVersionManager::snapshot() const {
  std::shared_lock lock(m_mutex);
  std::vector<VersionEntry> result;
  result.reserve(m_versions.size());

  for (const auto &[rid, head] : m_versions) {
    // Walk the entire chain and flatten it (newest -> oldest).
    for (const Column_Version *cur = head.get(); cur != nullptr; cur = cur->prev.get()) {
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
  return m_data.get() + local_row_id * m_header.field_desc.normalized_length;
}

size_t CU::get_data_size() const {
  auto rows = m_header.owner_imcu ? m_header.owner_imcu->get_row_count() : 0u;
  return rows * m_header.field_desc.normalized_length;
}

VarlenDataPool::VarlenReadGuard CU::resolve_data(row_id_t local_row_id) const {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return {};

  const uchar *slot = m_data.get() + local_row_id * m_header.field_desc.normalized_length;
  if (!m_varlen_pool) return VarlenDataPool::VarlenReadGuard(slot);  // no pool → data is inline

  VarlenDataPool::VarlenReference ref{};
  std::memcpy(&ref, slot, std::min(sizeof(ref), m_header.field_desc.normalized_length));

  if (ref.is_inline()) {
    const uchar *inline_data = slot + sizeof(VarlenDataPool::VarlenReference);
    return VarlenDataPool::VarlenReadGuard(
        (m_header.field_desc.normalized_length > sizeof(VarlenDataPool::VarlenReference)) ? inline_data : nullptr);
  }

  return m_varlen_pool->get_data_ptr(ref);
}

VarlenDataPool::VarlenReadGuard CU::resolve_data(const VarlenDataPool::VarlenReference &ref) const {
  if (!m_varlen_pool || ref.is_inline()) return {};
  return m_varlen_pool->get_data_ptr(ref);
}

size_t CU::get_logical_length(row_id_t local_row_id) const {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return 0;

  std::shared_lock lock(m_data_mutex);
  const uchar *slot = m_data.get() + local_row_id * m_header.field_desc.normalized_length;
  if (m_varlen_pool) {
    VarlenDataPool::VarlenReference ref{};
    std::memcpy(&ref, slot, std::min(sizeof(ref), m_header.field_desc.normalized_length));
    return static_cast<size_t>(ref.length);
  }

  if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
      m_header.field_desc.real_type() != MYSQL_TYPE_SET && !is_blob_like()) {
    uint32 dict_id = 0;
    std::memcpy(&dict_id, slot, sizeof(dict_id));
    return m_header.field_desc.dictionary->get(dict_id).size();
  }

  return m_header.field_desc.normalized_length;
}

bool CU::get_visible_cell(row_id_t local_row_id, bool current_is_null, Transaction::ID reader_txn_id,
                          uint64_t reader_scn, Transaction *reader_trx, const char *table_name,
                          VisibleCell &out) const {
  out.reset();

  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return false;

  ColumnVersionManager::BeforeImage before;
  if (m_version_manager->get_before_image_for_snapshot(local_row_id, reader_txn_id, reader_scn, reader_trx, table_name,
                                                       before)) {
    out.is_null = before.is_null;
    out.logical_length = before.logical_length;
    out.owned_slot = std::move(before.slot);
    out.slot = out.is_null || out.owned_slot.empty() ? nullptr : out.owned_slot.data();
    return true;
  }

  out.is_null = current_is_null;
  if (out.is_null) return true;

  out.slot = get_data_address(local_row_id);
  if (out.slot == nullptr) return false;

  if (m_varlen_pool) {
    VarlenDataPool::VarlenReference ref{};
    std::memcpy(&ref, out.slot, std::min(sizeof(ref), m_header.field_desc.normalized_length));
    out.logical_length = static_cast<size_t>(ref.length);
  } else if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
             m_header.field_desc.real_type() != MYSQL_TYPE_SET && !is_blob_like()) {
    uint32 dict_id = 0;
    std::memcpy(&dict_id, out.slot, sizeof(dict_id));
    out.logical_length = m_header.field_desc.dictionary->get(dict_id).size();
  } else {
    out.logical_length = m_header.field_desc.normalized_length;
  }

  return true;
}

bool CU::has_version_in_range(row_id_t start_row, size_t count) const {
  return m_version_manager->has_version_in_range(start_row, count);
}

void CU::commit_transaction(Transaction::ID txn_id, uint64_t commit_scn) {
  m_version_manager->commit_transaction(txn_id, commit_scn);
}

bool CU::rollback_transaction(Transaction::ID txn_id, std::vector<RollbackCell> &restored_cells) {
  const std::vector<row_id_t> rows = m_version_manager->active_rows(txn_id);
  bool ok = true;

  // Roll back in reverse creation order so repeated updates to the same cell
  // unwind exactly like an undo stack.
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    bool is_null = false;
    size_t logical_length = 0;
    if (rollback_update(*it, txn_id, &is_null, &logical_length) != ShannonBase::SHANNON_SUCCESS) {
      ok = false;
      continue;
    }
    restored_cells.push_back({*it, is_null, logical_length});
  }
  return ok;
}

int CU::write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len) {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return HA_ERR_KEY_NOT_FOUND;

  std::lock_guard lock(m_data_mutex);

  // If currently compressed, decompress before writing.
  if (m_is_compressed.load(std::memory_order_relaxed)) {
    const int decomp_ret = decompress_locked();
    if (decomp_ret != ShannonBase::SHANNON_SUCCESS) return decomp_ret;
  }

  uchar *dest = m_data.get() + local_row_id * m_header.field_desc.normalized_length;
  if (data == nullptr) {
    std::memset(dest, 0, m_header.field_desc.normalized_length);
  } else if (m_varlen_pool) {
    // An empty (non-NULL) string is a zeroed slot — a valid INLINE
    // VarlenReference with length 0 — and needs no pool allocation.
    std::memset(dest, 0, m_header.field_desc.normalized_length);
    if (len > 0) {
      VarlenDataPool::VarlenReference ref;
      // Allocate first and publish second.  Zeroing the live slot before a failed
      // allocation would destroy the current value during rollback/overwrite.
      bool allocated = m_varlen_pool->allocate_in_pool(data, len, ref);
      if (!allocated) return HA_ERR_OUT_OF_MEM;
      std::memcpy(dest, &ref, std::min(sizeof(ref), m_header.field_desc.normalized_length));
      if (!ref.is_inline() && m_header.owner_imcu) {
        auto *rd = m_header.owner_imcu->get_row_directory();
        if (rd) rd->mark_overflow(local_row_id);
      }
    }
  } else {
    if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
        m_header.field_desc.real_type() != MYSQL_TYPE_SET && !is_blob_like()) {
      std::memset(dest, 0, m_header.field_desc.normalized_length);
      uint32 dict_id = m_header.field_desc.dictionary->store(data, len, m_header.field_desc.encoding);
      std::memcpy(dest, &dict_id, sizeof(uint32));
    } else {
      if (len == 0) {
        std::memset(dest, 0, m_header.field_desc.normalized_length);
      } else {
        std::memcpy(dest, data, std::min(len, m_header.field_desc.normalized_length));
      }
    }
    update_statistics(data, len);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int CU::update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len) {
  std::vector<uchar> old_value;
  size_t old_len{0};

  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return HA_ERR_KEY_NOT_FOUND;

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  {
    std::lock_guard lock(m_data_mutex);
    if (m_is_compressed.load(std::memory_order_relaxed)) {
      const int decomp_ret = decompress_locked();
      if (decomp_ret != ShannonBase::SHANNON_SUCCESS) return decomp_ret;
    }

    const size_t slot_len = m_header.field_desc.normalized_length;
    uchar *current_slot = m_data.get() + local_row_id * slot_len;
    std::vector<uchar> old_slot(slot_len, 0);
    if (slot_len > 0) std::memcpy(old_slot.data(), current_slot, slot_len);

    const bool old_is_null = m_header.owner_imcu->is_null(m_header.column_id, local_row_id);
    bool retain_old_varlen_ref = false;

    if (!old_is_null) {
      auto src_guard = resolve_data(local_row_id);
      const uchar *src = src_guard.get();

      if (m_varlen_pool) {
        VarlenDataPool::VarlenReference old_ref{};
        std::memcpy(&old_ref, old_slot.data(), std::min(sizeof(old_ref), old_slot.size()));
        old_len = static_cast<size_t>(old_ref.length);
        retain_old_varlen_ref = !old_ref.is_inline() && old_ref.block_id != 0;

        if (old_len > 0) {
          if (src == nullptr) return HA_ERR_GENERIC;
          old_value.resize(old_len);
          std::memcpy(old_value.data(), src, old_len);
        }
      } else if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
                 m_header.field_desc.real_type() != MYSQL_TYPE_SET && !is_blob_like()) {
        uint32 dict_id = 0;
        std::memcpy(&dict_id, old_slot.data(), sizeof(dict_id));
        auto decode_str = m_header.field_desc.dictionary->get(dict_id);
        old_len = decode_str.length();
        old_value.assign(reinterpret_cast<const uchar *>(decode_str.data()),
                         reinterpret_cast<const uchar *>(decode_str.data()) + old_len);
      } else {
        old_len = slot_len;
        old_value.assign(old_slot.begin(), old_slot.end());
      }
    } else {
      old_len = UNIV_SQL_NULL;
    }

    // Allocate the new varlen payload before publishing the version head.
    VarlenDataPool::VarlenReference new_ref{};
    if (len != UNIV_SQL_NULL && len > 0 && m_varlen_pool && !m_varlen_pool->allocate_in_pool(new_data, len, new_ref)) {
      return HA_ERR_OUT_OF_MEM;
    }

    // Transfer ownership of the old pool-backed VarlenReference to the version
    // node. Do NOT retire it here: an older ReadView may still select it.
    m_version_manager->create_version(
        local_row_id, txn_id, scn, old_len == UNIV_SQL_NULL || old_value.empty() ? nullptr : old_value.data(), old_len,
        old_slot.empty() ? nullptr : old_slot.data(), old_slot.size(), m_varlen_pool.get(), retain_old_varlen_ref);

    if (len == UNIV_SQL_NULL) {
      std::memset(current_slot, 0, slot_len);
    } else if (m_varlen_pool) {
      std::memset(current_slot, 0, slot_len);
      if (len > 0) std::memcpy(current_slot, &new_ref, std::min(sizeof(new_ref), slot_len));
    } else if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
               m_header.field_desc.real_type() != MYSQL_TYPE_SET) {
      std::memset(current_slot, 0, slot_len);
      uint32 dict_id = m_header.field_desc.dictionary->store(new_data, len, m_header.field_desc.encoding);
      std::memcpy(current_slot, &dict_id, sizeof(uint32));
    } else {
      std::memset(current_slot, 0, slot_len);
      if (len > 0) std::memcpy(current_slot, new_data, std::min(len, slot_len));
    }
  }

  update_statistics(new_data, len);
  return ShannonBase::SHANNON_SUCCESS;
}

int CU::rollback_update(row_id_t local_row_id, Transaction::ID expected_txn_id, bool *restored_is_null,
                        size_t *restored_logical_length) {
  std::unique_ptr<ColumnVersionManager::Column_Version> head;
  if (!m_version_manager->pop_head(local_row_id, head, expected_txn_id) || !head) return HA_ERR_GENERIC;

  std::lock_guard lock(m_data_mutex);
  if (m_is_compressed.load(std::memory_order_relaxed)) {
    const int decomp_ret = decompress_locked();
    if (decomp_ret != ShannonBase::SHANNON_SUCCESS) {
      m_version_manager->restore_head(local_row_id, std::move(head));
      return decomp_ret;
    }
  }

  const size_t slot_len = m_header.field_desc.normalized_length;
  uchar *current_slot = m_data.get() + local_row_id * slot_len;

  // The value being discarded is the current physical owner of its varlen
  // allocation. Retire it before transferring the old retained reference back.
  if (m_varlen_pool) {
    VarlenDataPool::VarlenReference current_ref{};
    std::memcpy(&current_ref, current_slot, std::min(sizeof(current_ref), slot_len));
    if (!current_ref.is_inline() && current_ref.block_id != 0) m_varlen_pool->retire(current_ref);
  }

  std::memset(current_slot, 0, slot_len);
  if (head->value_length != UNIV_SQL_NULL && !head->old_slot.empty()) {
    std::memcpy(current_slot, head->old_slot.data(), std::min(slot_len, head->old_slot.size()));
  }

  if (head->owns_varlen_ref) {
    // The retained reference is now the live CU slot again.
    head->relinquish_varlen_ownership();
  }

  if (restored_is_null) *restored_is_null = head->value_length == UNIV_SQL_NULL;
  if (restored_logical_length) *restored_logical_length = head->value_length == UNIV_SQL_NULL ? 0 : head->value_length;

  return ShannonBase::SHANNON_SUCCESS;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) {
  auto cap = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0u;
  if (local_row_id >= cap) return 0;

  if (context && context->m_extra_info.m_scn != 0) {
    size_t version_len = 0;
    if (m_version_manager->get_value_at_scn(local_row_id, context->m_extra_info.m_scn, buffer, version_len,
                                            context->m_extra_info.m_trxid)) {
      return (version_len == UNIV_SQL_NULL) ? UNIV_SQL_NULL : version_len;
    }
  }

  if (m_header.owner_imcu->is_null(m_header.column_id, local_row_id)) return UNIV_SQL_NULL;

  // Stripe-aware read: acquire lock first, then check stripe conditions
  // to avoid data race on m_stripes / m_is_compressed / m_stripes_valid.
  {
    std::shared_lock lock(m_data_mutex);
    if (m_is_compressed.load(std::memory_order_relaxed) && m_stripes_valid.load(std::memory_order_acquire)) {
      const size_t stripe_idx = local_row_id / STRIPE_ROWS;
      if (stripe_idx < m_stripes.size() && m_stripes[stripe_idx].active) {
        // Decompress only this stripe into a stack buffer.
        const size_t rows_in_stripe = std::min(STRIPE_ROWS, cap - stripe_idx * STRIPE_ROWS);
        const size_t stripe_sz = rows_in_stripe * m_header.field_desc.normalized_length;
        auto stripe_buf = std::make_unique<uchar[]>(stripe_sz);
        if (decompress_stripe_locked(stripe_idx, stripe_buf.get())) {
          const size_t row_offset = (local_row_id % STRIPE_ROWS) * m_header.field_desc.normalized_length;
          const uchar *src = stripe_buf.get() + row_offset;

          // Varlen pool columns: resolve from decompressed stripe slot.
          if (m_varlen_pool) {
            VarlenDataPool::VarlenReference ref{};
            std::memcpy(&ref, src, std::min(sizeof(ref), m_header.field_desc.normalized_length));
            if (ref.is_inline()) {
              const uchar *inline_data = src + sizeof(VarlenDataPool::VarlenReference);
              size_t copy_len = std::min(static_cast<size_t>(ref.length), m_header.field_desc.normalized_length);
              if (buffer && inline_data) std::memcpy(buffer, inline_data, copy_len);
              return copy_len;
            }
            return m_varlen_pool->read(ref, buffer, m_header.field_desc.normalized_length);
          }

          if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
              m_header.field_desc.real_type() != MYSQL_TYPE_SET) {
            uint32 dict_id = 0;
            std::memcpy(&dict_id, src, sizeof(dict_id));
            auto decode_str = m_header.field_desc.dictionary->get(dict_id);
            std::memcpy(buffer, decode_str.c_str(), decode_str.length());
            return decode_str.length();
          }
          std::memcpy(buffer, src, m_header.field_desc.normalized_length);
          return m_header.field_desc.normalized_length;
        }
        // Stripe decompress failed — fall through to full decompress.
      }
    }
  }

  // Fallback: full decompress (legacy path or when stripe data is unavailable).
  std::lock_guard lock(m_data_mutex);
  if (m_is_compressed.load(std::memory_order_relaxed)) decompress_locked();

  const uchar *src = m_data.get() + local_row_id * m_header.field_desc.normalized_length;

  // Varlen pool columns: resolve the VarlenReference stored in the slot.
  if (m_varlen_pool) {
    VarlenDataPool::VarlenReference ref{};
    std::memcpy(&ref, src, std::min(sizeof(ref), m_header.field_desc.normalized_length));
    size_t actual_len = 0;
    if (ref.is_inline()) {
      const uchar *inline_data = src + sizeof(VarlenDataPool::VarlenReference);
      actual_len = ref.length;
      if (buffer && inline_data)
        std::memcpy(buffer, inline_data, std::min(actual_len, m_header.field_desc.normalized_length));
    } else {
      actual_len = m_varlen_pool->read(ref, buffer, m_header.field_desc.normalized_length);
    }
    return actual_len;
  }

  if (m_header.field_desc.dictionary && m_header.field_desc.src_field->real_type() != MYSQL_TYPE_ENUM &&
      m_header.field_desc.src_field->real_type() != MYSQL_TYPE_SET) {
    uint32 dict_id = 0;
    std::memcpy(&dict_id, src, sizeof(dict_id));
    auto decode_str = m_header.field_desc.dictionary->get(dict_id);
    std::memcpy(buffer, decode_str.c_str(), decode_str.length());
    return decode_str.length();
  }

  std::memcpy(buffer, src, m_header.field_desc.normalized_length);
  return m_header.field_desc.normalized_length;
}

size_t CU::purge_versions(const Rapid_context *context, uint64_t min_active_scn) {
  return m_version_manager->purge(min_active_scn);
}

Compress::CompressAlgorithm *CU::select_compressor(CU_CompressAlgo *algo_out) const {
  // Use ZSTD for SORTED encoding (better ratio for sorted runs) or when the
  // caller explicitly requested ZSTD-class compression.
  bool prefer_zstd = (m_header.field_desc.encoding == Compress::ENCODING_TYPE::SORTED) ||
                     (m_header.field_desc.compression_level == Compress::COMPRESS_LEVEL::ZSTD);

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
  if (m_header.field_desc.compression_level == Compress::COMPRESS_LEVEL::NONE) return ShannonBase::SHANNON_SUCCESS;

  std::lock_guard lock(m_data_mutex);
  // Re-check under lock (another thread may have just compressed).
  if (m_is_compressed.load(std::memory_order_relaxed)) return ShannonBase::SHANNON_SUCCESS;

  const size_t data_sz = get_data_size();
  if (data_sz < 1024) return ShannonBase::SHANNON_SUCCESS;  // too small to benefit

  const size_t capacity = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0;
  if (capacity == 0) return ShannonBase::SHANNON_SUCCESS;

  // Stripe-based compression for latency-optimized point reads
  m_num_stripes = (capacity + STRIPE_ROWS - 1) / STRIPE_ROWS;
  m_stripes.clear();
  m_stripes.resize(m_num_stripes);

  CU_CompressAlgo best_algo;
  auto *compressor = select_compressor(&best_algo);
  constexpr double kThreshold = 0.90;
  size_t total_compressed = 0;
  size_t total_original = 0;
  bool any_compressed = false;

  for (size_t si = 0; si < m_num_stripes; ++si) {
    const size_t row_start = si * STRIPE_ROWS;
    const size_t rows_in_stripe = std::min(STRIPE_ROWS, capacity - row_start);
    const size_t stripe_original_sz = rows_in_stripe * m_header.field_desc.normalized_length;
    total_original += stripe_original_sz;

    std::string_view input(
        reinterpret_cast<const char *>(m_data.get() + row_start * m_header.field_desc.normalized_length),
        stripe_original_sz);
    std::string compressed = compressor->compress(input);

    if (!compressed.empty() && compressed.size() < static_cast<size_t>(stripe_original_sz * kThreshold)) {
      // Allocate stripe compressed buffer from the memory pool.
      uchar *stripe_buf = static_cast<uchar *>(m_memory_pool->allocate_auto(compressed.size()));
      if (stripe_buf) {
        std::memcpy(stripe_buf, compressed.data(), compressed.size());
        m_stripes[si].compressed_data =
            std::unique_ptr<uchar[], PoolDeleter>(stripe_buf, PoolDeleter(m_memory_pool, compressed.size()));
        m_stripes[si].compressed_size = compressed.size();
        m_stripes[si].algo = best_algo;
        m_stripes[si].active = true;
        total_compressed += compressed.size();
        any_compressed = true;
      }
    }
  }

  if (!any_compressed) {
    m_stripes.clear();
    return HA_ERR_GENERIC;  // No stripe benefited from compression
  }

  m_original_data_size.store(total_original, std::memory_order_relaxed);
  m_compressed_data_size.store(total_compressed, std::memory_order_relaxed);
  m_compress_algo_used.store(static_cast<uint8_t>(best_algo), std::memory_order_relaxed);
  m_stripes_valid.store(true, std::memory_order_release);
  m_is_compressed.store(true, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

int CU::decompress() {
  if (!m_is_compressed.load(std::memory_order_acquire)) return ShannonBase::SHANNON_SUCCESS;
  std::lock_guard lock(m_data_mutex);
  return decompress_locked();
}

int CU::decompress_locked() {
  if (!m_is_compressed.load(std::memory_order_relaxed)) return ShannonBase::SHANNON_SUCCESS;

  // If stripe data is present, decompress from stripes.
  if (m_stripes_valid.load(std::memory_order_relaxed) && !m_stripes.empty()) {
    const size_t capacity = m_header.owner_imcu ? m_header.owner_imcu->get_capacity() : 0;
    for (size_t si = 0; si < m_stripes.size(); ++si) {
      if (!m_stripes[si].active) continue;
      const size_t row_start = si * STRIPE_ROWS;
      const size_t rows_in_stripe = std::min(STRIPE_ROWS, capacity - row_start);
      const size_t stripe_sz = rows_in_stripe * m_header.field_desc.normalized_length;
      auto stripe_buf = std::make_unique<uchar[]>(stripe_sz);
      if (!decompress_stripe_locked(si, stripe_buf.get())) {
        return HA_ERR_GENERIC;
      }
      std::memcpy(m_data.get() + row_start * m_header.field_desc.normalized_length, stripe_buf.get(), stripe_sz);
    }
    invalidate_stripes_locked();
    m_is_compressed.store(false, std::memory_order_release);
    return ShannonBase::SHANNON_SUCCESS;
  }

  // Legacy path: single-block compressed data.
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
  return ShannonBase::SHANNON_SUCCESS;
}

bool CU::decompress_stripe_locked(size_t stripe_idx, uchar *out_buffer) const {
  if (stripe_idx >= m_stripes.size() || !m_stripes[stripe_idx].active) return false;

  const auto &stripe = m_stripes[stripe_idx];
  Compress::CompressAlgorithm *decompressor =
      (stripe.algo == CU_CompressAlgo::ZSTD)   ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZSTD)
      : (stripe.algo == CU_CompressAlgo::ZLIB) ? Compress::get_compressor(Compress::COMPRESS_ALGO::ZLIB)
                                               : Compress::get_compressor(Compress::COMPRESS_ALGO::LZ4);

  std::string_view payload(reinterpret_cast<const char *>(stripe.compressed_data.get()), stripe.compressed_size);
  std::string plain = decompressor->decompress(payload);
  if (plain.empty()) return false;

  std::memcpy(out_buffer, plain.data(), plain.size());
  return true;
}

void CU::invalidate_stripes_locked() {
  m_stripes.clear();
  m_num_stripes = 0;
  m_stripes_valid.store(false, std::memory_order_release);
}

void CU::update_statistics(const uchar *data, size_t /*len*/) {
  if (!is_numeric_type(m_header.field_desc.type) && !is_temporal_type(m_header.field_desc.type) &&
      !(m_header.field_desc.src_field &&
        (m_header.field_desc.real_type() == MYSQL_TYPE_ENUM || m_header.field_desc.real_type() == MYSQL_TYPE_SET)))
    return;

  // NULL column value — nothing to decode; skip min/max/sum update.
  if (!data) return;

  double value = Utils::Util::get_field_numeric<double>(m_header.field_desc.src_field, data, nullptr);
  m_header.sum.fetch_add(value);
  m_header.min_value.store(std::min(m_header.min_value.load(std::memory_order_relaxed), value));
  m_header.max_value.store(std::max(m_header.max_value.load(std::memory_order_relaxed), value));
}

int CU::serialize(std::ostream &out, size_t snapshot_row_count) const {
  // If the CU is currently compressed, we can serialize the compressed payload directly (saves I/O). If not, we
  // compress on-the-fly into a temp buffer so the snapshot is as compact as possible.
  std::lock_guard lock(m_data_mutex);
  if (snapshot_row_count == 0) snapshot_row_count = m_header.owner_imcu ? m_header.owner_imcu->get_row_count() : 0;

  // Pool-backed (BLOB / TEXT / JSON / VECTOR) payloads cannot be reconstructed
  // from a compressed slot array: decompress first so the varlen section below
  // reads the raw VarlenReference slots.
  if (m_varlen_pool && m_is_compressed.load(std::memory_order_relaxed)) {
    const int decomp_ret = const_cast<CU *>(this)->decompress_locked();
    if (decomp_ret != ShannonBase::SHANNON_SUCCESS) return decomp_ret;
  }

  const size_t original_sz = snapshot_row_count * m_header.field_desc.normalized_length;
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
    if (m_header.field_desc.compression_level != Compress::COMPRESS_LEVEL::NONE && original_sz >= 1024) {
      CU_CompressAlgo algo;
      auto *comp = select_compressor(&algo);
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
  bool has_dict = (m_header.field_desc.dictionary && m_header.field_desc.dictionary->size() > 1);
  auto version_snap = m_version_manager->snapshot();
  bool has_versions = !version_snap.empty();
  bool has_varlen = (m_varlen_pool != nullptr);

  uint8_t flags = 0;
  if (payload_compressed) flags |= CU_FLAG_COMPRESSED;
  if (has_dict) flags |= CU_FLAG_HAS_DICT;
  if (has_versions) flags |= CU_FLAG_HAS_VERSIONS;
  if (has_varlen) flags |= CU_FLAG_HAS_VARLEN;

  // Write through CrcStream so we compute the checksum as we go
  CrcStream cs(out);

  // Fixed header (20 bytes)
  cs.write_pod(CU_SERIAL_MAGIC);                                     // 4
  cs.write_pod(CU_FORMAT_VERSION);                                   // 2
  cs.write_pod(flags);                                               // 1
  cs.write_pod(m_header.column_id);                                  // 4
  cs.write_pod(static_cast<uint8_t>(m_header.field_desc.type));      // 1
  cs.write_pod(static_cast<uint8_t>(m_header.field_desc.encoding));  // 1
  cs.write_pod(static_cast<uint8_t>(payload_algo));                  // 1
  const uint8_t reserved[6] = {};
  cs.write(reserved, 6);  // 6
  // total: 4+2+1+4+1+1+1+6 = 20 ✓

  // Lengths (32 bytes)
  cs.write_pod(static_cast<uint64_t>(m_header.field_desc.pack_length));        // 8
  cs.write_pod(static_cast<uint64_t>(m_header.field_desc.normalized_length));  // 8
  cs.write_pod(static_cast<uint64_t>(original_sz));                            // 8
  cs.write_pod(static_cast<uint64_t>(snapshot_row_count));                     // 8
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
    uint32_t entry_count = static_cast<uint32_t>(m_header.field_desc.dictionary->content_size());
    cs.write_pod(entry_count);

    // Entry 0 is reserved; start from 1.
    for (uint32_t id = 1; id < entry_count; ++id) {
      // Retrieve the raw stored string (flag byte + payload).
      // We call get() which decompresses; to persist the compressed form we
      // need the internal storage.  For now store the decoded value and re-
      // encode on load — this is the safest cross-version approach.
      std::string value = m_header.field_desc.dictionary->get(id);
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

  // Varlen payload section: persist the logical value of every pool-backed
  // cell so a reconstructed CU does not depend on the old allocator layout.
  // block_id / offset / sizeof(VarlenReference) ABI are all rebuilt on load.
  if (has_varlen) {
    struct VarlenEntry {
      uint64_t row_id;
      std::vector<uint8_t> value;
    };
    std::vector<VarlenEntry> varlen_entries;

    const bit_array_t *null_mask = nullptr;
    if (m_header.owner_imcu) {
      const auto &null_masks = m_header.owner_imcu->get_null_masks();
      if (m_header.column_id < null_masks.size()) null_mask = null_masks[m_header.column_id].get();
    }

    for (uint64_t row = 0; row < snapshot_row_count; ++row) {
      if (null_mask && Utils::Util::bit_array_get_fast(null_mask->data, static_cast<size_t>(row)))
        continue;  // NULL cell: slot is zeroed and needs no pool payload.

      const uchar *slot = m_data.get() + row * m_header.field_desc.normalized_length;
      VarlenDataPool::VarlenReference ref{};
      std::memcpy(&ref, slot, std::min(sizeof(ref), m_header.field_desc.normalized_length));
      if (ref.is_inline() || ref.length == 0) continue;  // inline payload already in m_data.

      VarlenEntry entry;
      entry.row_id = row;
      entry.value.resize(ref.length);
      size_t out_len = 0;
      if (!m_varlen_pool->copy_data(ref, entry.value.data(), ref.length, out_len) || out_len != ref.length)
        return HA_ERR_GENERIC;
      varlen_entries.push_back(std::move(entry));
    }

    cs.write_pod(static_cast<uint64_t>(varlen_entries.size()));
    for (const auto &e : varlen_entries) {
      cs.write_pod(e.row_id);
      cs.write_pod(static_cast<uint64_t>(e.value.size()));
      if (!e.value.empty()) cs.write(e.value.data(), e.value.size());
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

  constexpr size_t kMaxDictEntries = 1u << 20;      // 1M
  constexpr size_t kMaxVersionEntries = 1u << 20;   // 1M
  constexpr size_t kMaxVarlenEntries = 1u << 20;    // 1M
  constexpr size_t kMaxVarlenValueLen = 64u << 20;  // 64 MiB

  // Fixed header
  uint32_t magic = 0;
  uint16_t version = 0;
  uint8_t flags = 0;
  uint32_t col_id = 0;
  uint8_t ftype = 0;
  uint8_t enc = 0;
  uint8_t algo = 0;
  uint8_t reserved[6] = {};

  if (!cis.read_pod(magic) || magic != CU_SERIAL_MAGIC) return HA_ERR_GENERIC;
  if (!cis.read_pod(version) || version != CU_FORMAT_VERSION) return HA_ERR_GENERIC;
  if (!cis.read_pod(flags)) return HA_ERR_GENERIC;
  if (!cis.read_pod(col_id)) return HA_ERR_GENERIC;
  if (!cis.read_pod(ftype)) return HA_ERR_GENERIC;
  if (!cis.read_pod(enc)) return HA_ERR_GENERIC;
  if (!cis.read_pod(algo)) return HA_ERR_GENERIC;
  if (!cis.read(reserved, 6)) return HA_ERR_GENERIC;

  // Cross-check header identity before trusting any sizes that follow.
  if (col_id != m_header.column_id) return HA_ERR_GENERIC;
  if (ftype != static_cast<uint8_t>(m_header.field_desc.type)) return HA_ERR_GENERIC;

  // Lengths
  uint64_t pack_len = 0, norm_len = 0, orig_sz = 0, row_cnt = 0;
  if (!cis.read_pod(pack_len)) return HA_ERR_GENERIC;
  if (!cis.read_pod(norm_len)) return HA_ERR_GENERIC;
  if (!cis.read_pod(orig_sz)) return HA_ERR_GENERIC;
  if (!cis.read_pod(row_cnt)) return HA_ERR_GENERIC;

  // Zone map
  double zm_min = 0, zm_max = 0, zm_sum = 0;
  if (!cis.read_pod(zm_min)) return HA_ERR_GENERIC;
  if (!cis.read_pod(zm_max)) return HA_ERR_GENERIC;
  if (!cis.read_pod(zm_sum)) return HA_ERR_GENERIC;

  // Data payload
  uint64_t payload_sz = 0;
  if (!cis.read_pod(payload_sz)) return HA_ERR_GENERIC;

  // ---- Defensive boundary: validate every untrusted size before allocating. ----
  const size_t data_capacity = m_data_capacity.load(std::memory_order_acquire);
  if (orig_sz > data_capacity) return HA_ERR_GENERIC;
  if (payload_sz > orig_sz) return HA_ERR_GENERIC;
  if (pack_len != m_header.field_desc.pack_length) return HA_ERR_GENERIC;
  if (norm_len != m_header.field_desc.normalized_length) return HA_ERR_GENERIC;
  if (norm_len == 0) {
    if (orig_sz != 0 || row_cnt != 0) return HA_ERR_GENERIC;
  } else {
    if (orig_sz % norm_len != 0 || row_cnt != orig_sz / norm_len) return HA_ERR_GENERIC;
  }
  if (enc > static_cast<uint8_t>(Compress::ENCODING_TYPE::VARLEN)) return HA_ERR_GENERIC;
  if (algo > static_cast<uint8_t>(CU_CompressAlgo::ZLIB)) return HA_ERR_GENERIC;

  std::vector<uchar> payload(payload_sz);
  if (payload_sz > 0 && !cis.read(payload.data(), payload_sz)) return HA_ERR_GENERIC;

  // Dictionary (optional)
  std::vector<std::string> dict_entries;
  if (flags & CU_FLAG_HAS_DICT) {
    uint32_t entry_count = 0;
    if (!cis.read_pod(entry_count)) return HA_ERR_GENERIC;
    if (entry_count > kMaxDictEntries) return HA_ERR_GENERIC;
    dict_entries.resize(entry_count);
    for (uint32_t i = 1; i < entry_count; ++i) {
      uint64_t vlen = 0;
      if (!cis.read_pod(vlen)) return HA_ERR_GENERIC;
      if (vlen > kMaxVarlenValueLen) return HA_ERR_GENERIC;
      if (vlen > 0) {
        dict_entries[i].resize(vlen);
        if (!cis.read(&dict_entries[i][0], vlen)) return HA_ERR_GENERIC;
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
    if (vcnt > kMaxVersionEntries) return HA_ERR_GENERIC;
    version_recs.resize(vcnt);
    for (auto &vr : version_recs) {
      if (!cis.read_pod(vr.row_id)) return HA_ERR_GENERIC;
      if (!cis.read_pod(vr.txn_id)) return HA_ERR_GENERIC;
      if (!cis.read_pod(vr.scn)) return HA_ERR_GENERIC;
      if (!cis.read_pod(vr.val_len)) return HA_ERR_GENERIC;
      if (vr.val_len != UNIV_SQL_NULL && vr.val_len > kMaxVarlenValueLen) return HA_ERR_GENERIC;
      if (vr.val_len != UNIV_SQL_NULL && vr.val_len > 0) {
        vr.val_data.resize(vr.val_len);
        if (!cis.read(vr.val_data.data(), vr.val_len)) return HA_ERR_GENERIC;
      }
    }
  }

  struct VarlenRec {
    uint64_t row_id = 0;
    std::vector<uchar> val_data;
  };
  std::vector<VarlenRec> varlen_recs;
  if (flags & CU_FLAG_HAS_VARLEN) {
    uint64_t vcnt = 0;
    if (!cis.read_pod(vcnt)) return HA_ERR_GENERIC;
    if (vcnt > kMaxVarlenEntries) return HA_ERR_GENERIC;
    varlen_recs.resize(vcnt);
    for (auto &vr : varlen_recs) {
      if (!cis.read_pod(vr.row_id)) return HA_ERR_GENERIC;
      uint64_t vlen = 0;
      if (!cis.read_pod(vlen)) return HA_ERR_GENERIC;
      if (vlen > VarlenDataPool::MAX_VARLEN_VALUE_SIZE) return HA_ERR_GENERIC;
      if (vlen > 0) {
        vr.val_data.resize(vlen);
        if (!cis.read(vr.val_data.data(), vlen)) return HA_ERR_GENERIC;
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
  m_header.field_desc.pack_length = static_cast<size_t>(pack_len);
  m_header.field_desc.normalized_length = static_cast<size_t>(norm_len);
  m_header.min_value.store(zm_min, std::memory_order_relaxed);
  m_header.max_value.store(zm_max, std::memory_order_relaxed);
  m_header.sum.store(zm_sum, std::memory_order_relaxed);

  if ((flags & CU_FLAG_HAS_DICT) && m_header.field_desc.dictionary) {
    for (size_t i = 1; i < dict_entries.size(); ++i) {
      const auto &val = dict_entries[i];
      m_header.field_desc.dictionary->restore_entry(i, reinterpret_cast<const uchar *>(val.data()), val.size());
    }
  }

  // Restore version journal. serialize() flattens each row newest -> oldest;
  // create_version() pushes at the head, so replay in reverse to preserve the
  // original chain order.
  for (auto it = version_recs.rbegin(); it != version_recs.rend(); ++it) {
    const auto &vr = *it;
    const size_t logical_len = static_cast<size_t>(vr.val_len);
    const uchar *val_ptr = vr.val_len != UNIV_SQL_NULL && !vr.val_data.empty() ? vr.val_data.data() : nullptr;

    std::vector<uchar> old_slot(m_header.field_desc.normalized_length, 0);
    bool owns_varlen_ref = false;

    if (vr.val_len != UNIV_SQL_NULL) {
      if (m_varlen_pool) {
        if (!vr.val_data.empty()) {
          VarlenDataPool::VarlenReference ref{};
          if (!m_varlen_pool->allocate_in_pool(vr.val_data.data(), vr.val_data.size(), ref)) return HA_ERR_GENERIC;
          std::memcpy(old_slot.data(), &ref, std::min(sizeof(ref), old_slot.size()));
          owns_varlen_ref = !ref.is_inline() && ref.block_id != 0;
        }
      } else if (m_header.field_desc.dictionary && m_header.field_desc.real_type() != MYSQL_TYPE_ENUM &&
                 m_header.field_desc.real_type() != MYSQL_TYPE_SET && !is_blob_like()) {
        // Dictionary::store() should never receive a null pointer for a
        // non-NULL logical value, even when that value is the empty string.
        static const uchar kEmptyValue = 0;
        const uchar *dict_value = val_ptr != nullptr ? val_ptr : &kEmptyValue;
        uint32 dict_id = m_header.field_desc.dictionary->store(dict_value, logical_len, m_header.field_desc.encoding);
        std::memcpy(old_slot.data(), &dict_id, sizeof(dict_id));
      } else if (val_ptr != nullptr && logical_len > 0) {
        std::memcpy(old_slot.data(), val_ptr, std::min(logical_len, old_slot.size()));
      }
    }

    m_version_manager->create_version(static_cast<row_id_t>(vr.row_id), static_cast<Transaction::ID>(vr.txn_id), vr.scn,
                                      val_ptr, logical_len, old_slot.data(), old_slot.size(), m_varlen_pool.get(),
                                      owns_varlen_ref);
  }

  if ((flags & CU_FLAG_HAS_VARLEN) && m_varlen_pool) {
    for (const auto &vr : varlen_recs) {
      if (vr.row_id >= row_cnt) return HA_ERR_GENERIC;

      uchar *slot = m_data.get() + static_cast<size_t>(vr.row_id) * m_header.field_desc.normalized_length;
      std::memset(slot, 0, m_header.field_desc.normalized_length);

      // An empty (non-NULL) string round-trips as a zeroed slot — a valid
      // INLINE VarlenReference with length 0.
      if (vr.val_data.empty()) continue;

      VarlenDataPool::VarlenReference new_ref;
      if (!m_varlen_pool->allocate_in_pool(vr.val_data.data(), vr.val_data.size(), new_ref)) return HA_ERR_GENERIC;

      std::memcpy(slot, &new_ref, std::min(sizeof(new_ref), m_header.field_desc.normalized_length));

      if (!new_ref.is_inline() && m_header.owner_imcu) {
        auto *rd = m_header.owner_imcu->get_row_directory();
        if (rd) rd->mark_overflow(static_cast<row_id_t>(vr.row_id));
      }
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace Imcs
}  // namespace ShannonBase