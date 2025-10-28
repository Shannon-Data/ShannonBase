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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/cu.h"

#include <limits.h>
#include <iostream>
#include <random>
#include <regex>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
CU::CU(Imcu *owner, const FieldMetadata &field_meta, uint32_t col_idx, size_t capacity,
       std::shared_ptr<ShannonBase::Utils::MemoryPool> mem_pool) {
  m_header.owner_imcu = owner;
  m_header.column_id = col_idx;
  m_header.field_metadata = field_meta.source_fld;
  m_header.type = field_meta.type;
  m_header.pack_length = field_meta.pack_length;
  m_header.normalized_length = field_meta.normalized_length;
  m_header.charset = field_meta.charset;
  m_header.capacity = capacity;
  m_header.local_dict = field_meta.dictionary;

  m_data_capacity = capacity * m_header.normalized_length;
  uchar *raw_ptr = static_cast<uchar *>(mem_pool.get()->allocate_auto(m_data_capacity, SHANNON_DATA_AREAR_NAME));
  if (!raw_ptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "CU memory allocation failed");
    return;
  }

  m_data = std::unique_ptr<uchar[], PoolDeleter>(raw_ptr, PoolDeleter(mem_pool.get(), m_data_capacity));

  m_version_manager = std::make_unique<ColumnVersionManager>();
}

CU::~CU() {}

void CU::ColumnVersionManager::create_version(row_id_t local_row_id, Transaction::ID txn_id, uint64_t scn,
                                              const uchar *old_value, size_t len) {
  std::unique_lock lock(m_mutex);

  // create a new version.
  auto new_version = std::make_unique<Column_Version>();
  new_version->txn_id = txn_id;
  new_version->scn = scn;
  new_version->timestamp = std::chrono::system_clock::now();
  new_version->value_length = len;

  // copy the old value.
  if (len != UNIV_SQL_NULL && old_value) {
    new_version->old_value = std::make_unique<uchar[]>(len);
    std::memcpy(new_version->old_value.get(), old_value, len);
  }

  // add to version link.
  auto it = m_versions.find(local_row_id);
  if (it != m_versions.end()) {
    new_version->prev = it->second.release();
    it->second = std::move(new_version);
  } else {
    m_versions[local_row_id] = std::move(new_version);
  }
}

bool CU::ColumnVersionManager::get_value_at_scn(row_id_t local_row_id, uint64_t target_scn, uchar *buffer,
                                                size_t &len) const {
  return false;
}

size_t CU::ColumnVersionManager::purge(uint64_t min_active_scn) {
  size_t purged = 0;
  std::unique_lock lock(m_mutex);
  for (auto it = m_versions.begin(); it != m_versions.end();) {
    ColumnVersionManager::Column_Version *head = it->second.get();
    ColumnVersionManager::Column_Version *current = head;
    ColumnVersionManager::Column_Version *prev_valid = nullptr;

    bool found_visible = false;

    while (current != nullptr) {
      // Earlier than the minimum active SCN and not the latest visible version
      if (current->scn < min_active_scn && found_visible) {
        // purgeable.
        ColumnVersionManager::Column_Version *to_delete = current;
        current = current->prev;

        if (prev_valid) {
          prev_valid->prev = current;
        }

        delete to_delete;
        purged++;
      } else {
        // keep.
        found_visible = true;
        prev_valid = current;
        current = current->prev;
      }
    }

    // If the entire version chain has been purged
    if (head == nullptr) {
      it = m_versions.erase(it);
    } else {
      ++it;
    }
  }

  return purged;
}

/*
 * write a new value (Internal deletion/NULL management is handled by IMCU)
 */
bool CU::write(const Rapid_context *context, row_id_t local_row_id, const uchar *data, size_t len) {
  if (local_row_id >= m_header.capacity) return false;

  std::lock_guard lock(m_data_mutex);
  uchar *dest = m_data.get() + local_row_id * m_header.normalized_length;

  if (data == nullptr) {  // NULL values: The IMCU's null_mask has been set, write placeholder values here
    std::memset(dest, 0, m_header.normalized_length);
    m_header.null_count.fetch_add(1);
    m_header.data_size.fetch_add(m_header.normalized_length);
  } else {
    if (m_header.local_dict) {
      uint32_t dict_id = m_header.local_dict->store(data, len, m_header.encoding);
      std::memcpy(dest, &dict_id, sizeof(uint32_t));
      m_header.data_size.fetch_add(sizeof(uint32_t));
    } else {
      std::memcpy(dest, data, std::min(len, m_header.normalized_length.load()));
      m_header.data_size.fetch_add(std::min(len, m_header.normalized_length.load()));
    }
    update_statistics(data, len);
  }

  m_header.total_count.fetch_add(1);
  return false;
}

int CU::update(const Rapid_context *context, row_id_t local_row_id, const uchar *new_data, size_t len) {
  // 1. read old value;
  uchar old_value[MAX_FIELD_WIDTH] = {0};
  size_t old_len = read(context, local_row_id, old_value);

  // 2. Create column-level version (only store old values of this column)
  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64_t scn = context->m_extra_info.m_scn;

  m_version_manager->create_version(local_row_id, txn_id, scn, old_value, old_len);

  // 3. write new value.
  {
    std::lock_guard lock(m_data_mutex);

    auto dest = static_cast<void *>(const_cast<uchar *>(get_data_address(local_row_id)));

    if (len == UNIV_SQL_NULL) {
      // NULL values are handled by the null_mask of IMCU
      std::memset(dest, 0x0, m_header.normalized_length);
    } else {
      // dealing with encoding.
      if (m_header.local_dict) {
        uint32_t dict_id = m_header.local_dict.get()->store(new_data, len, m_header.encoding);
        std::memcpy(dest, &dict_id, sizeof(uint32_t));
      } else {
        std::memcpy(dest, new_data, std::min(len, m_header.normalized_length.load(std::memory_order_relaxed)));
      }
    }
  }

  // 4. update statistics.
  update_statistics(new_data, len);

  return ShannonBase::SHANNON_SUCCESS;
}

bool CU::write_batch(const Rapid_context *context, row_id_t start_row, const std::vector<uchar *> &data_array,
                     size_t count) {
  return false;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uchar *buffer) const {
  if (local_row_id >= m_header.capacity) return 0;

  // check IMCU's NULL mask.
  if (m_header.owner_imcu->is_null(m_header.column_id, local_row_id)) {
    return UNIV_SQL_NULL;
  }

  const uchar *src = m_data.get() + local_row_id * m_header.normalized_length;

  // dealing with dictionary encode.
  if (m_header.local_dict) {
    uint32_t dict_id = *reinterpret_cast<const uint32_t *>(src);
    auto decode_str = m_header.local_dict->get(dict_id);
    std::memcpy(buffer, decode_str.c_str(), decode_str.length());
    return decode_str.length();
  }

  std::memcpy(buffer, src, m_header.normalized_length);
  return m_header.normalized_length;
}

size_t CU::read(const Rapid_context *context, row_id_t local_row_id, uint64_t target_scn, uchar *buffer) const {
  return 0;
}

const uchar *read(const Rapid_context *context, row_id_t local_row_id) { return nullptr; }

size_t CU::read_batch(const Rapid_context *context, const std::vector<row_id_t> &row_ids, uchar *output) const {
  return 0;
}

size_t CU::scan_range(const Rapid_context *context, row_id_t start_row, size_t count, uchar *output) const { return 0; }

void CU::create_version(const Rapid_context *context, row_id_t local_row_id) {}

size_t CU::purge_versions(const Rapid_context *context, uint64_t min_active_scn) {
  return m_version_manager->purge(min_active_scn);
}

bool CU::encode_value(const Rapid_context *context, const uchar *data, size_t len, uint32_t &encoded) { return false; }

bool CU::decode_value(const Rapid_context *context, uint32_t encoded, uchar *buffer, size_t &len) const {
  return false;
}

bool CU::compress() { return false; }

bool CU::decompress() { return false; }

void CU::update_statistics(const uchar *data, size_t len) {
  if (!is_integer_type(m_header.type)) return;

  double value = Utils::Util::get_field_numeric<double>(m_header.field_metadata, data, nullptr);
  m_header.sum.fetch_add(value);

  m_header.min_value.store(std::min(m_header.min_value.load(std::memory_order_relaxed), value));
  m_header.max_value.store(std::max(m_header.max_value.load(std::memory_order_relaxed), value));

  m_header.avg = m_header.sum.load(std::memory_order_relaxed) / m_header.total_count.load(std::memory_order_relaxed);
}

bool CU::serialize(std::ostream &out) const { return false; }

bool CU::deserialize(std::istream &in) { return false; }

double CU::get_numeric_value(const Rapid_context *context, const uchar *data, size_t len) const { return 0.0f; }

}  // namespace Imcs
}  // namespace ShannonBase