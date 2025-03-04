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
#include "storage/rapid_engine/imcs/chunk.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <new>
#include <typeinfo>

#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"  //status inf
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {

/**
 * every chunks has a fixed num of rows: SHANNON_ROWS_IN_CHUN
 * K. we can calcuate
 * the row offset easily by using 'm_data / m_source_fld->pack_length' to get
 * which chunk we are in now, and 'm_data % m_source_fld->pack_length' to get
 * where we are in this chunk.
 */
static std::atomic<size_t> rapid_allocated_mem_size{0};

Chunk::Chunk(const Field *field) {
  m_header.reset(new (std::nothrow) Chunk_header());
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk header allocation failed");
    return;  // allocated faile.
  }

  m_header->m_pack_length = field->pack_length();
  if (Utils::Util::is_blob(field->type()) || Utils::Util::is_varstring(field->type()) ||
      Utils::Util::is_string(field->type()))
    m_header->m_normalized_pack_length = sizeof(uint32);
  else
    m_header->m_normalized_pack_length = field->pack_length();

  /** there's null values in, therefore, alloc the null bitmap, and del bit map will
   * lazy allocated.*/
  if (field->is_nullable()) {
    m_header->m_null_mask.reset(new (std::nothrow) ShannonBase::bit_array_t(SHANNON_ROWS_IN_CHUNK));
    if (!m_header->m_null_mask) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk header bit map allocation failed");
      return;  // allocated faile.
    }
  }

  // the SMU ptr. just like rollback ptr.
  m_header->m_smu.reset(new (std::nothrow) ShannonBase::ReadView::Snapshot_meta_unit());
  if (!m_header->m_smu) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk header SMU allocation failed");
    return;  // allocated faile.
  }

  size_t chunk_size = SHANNON_ROWS_IN_CHUNK * m_header->m_normalized_pack_length;
  ut_ad(field && chunk_size < ShannonBase::rpd_mem_sz_max);

  /**m_data_base，here, we use the same psi key with buffer pool which used in
   * innodb page allocation. Here, we use ut::xxx to manage memory allocation
   * and free as innobase doese. In SQL lay, we will use MEM_ROOT to manage the
   * memory management. In IMCS, all modules use ut:: to manage memory
   * operations, it's an effiecient memory utils. it has been initialized in
   * ha_innodb.cc: ut_new_boot(); */
  if (likely(rapid_allocated_mem_size + chunk_size <= ShannonBase::rpd_mem_sz_max)) {
    m_base = static_cast<uchar *>(ut::aligned_alloc(chunk_size, CACHE_LINE_SIZE));
    if (unlikely(!m_base)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
      return;
    }

    m_data.store(m_base);
    m_rdata.store(m_base);
    m_end.store(m_base + static_cast<ptrdiff_t>(chunk_size));
    rapid_allocated_mem_size.fetch_add(chunk_size);
  } else {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
    return;
  }

  m_chunk_key.append(field->table->s->db.str)
      .append(":")
      .append(field->table->s->table_name.str)
      .append(":")
      .append(field->field_name);
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  assert(m_header->m_source_fld);
  m_header->m_type = field->type();
}

Chunk::~Chunk() {
  if (m_base) {
    ut::aligned_free(m_base);
    m_base = m_data = nullptr;
    rapid_allocated_mem_size.fetch_sub(m_header->m_normalized_pack_length * SHANNON_ROWS_IN_CHUNK);
  }
}

void Chunk::update_meta_info(OPER_TYPE type, uchar *data, uchar *old) {
  auto dict = current_imcs_instance->get_cu(m_chunk_key)->header()->m_local_dict.get();
  double data_val = data ? Utils::Util::get_field_value<double>(m_header->m_source_fld, data, dict) : 0;
  double old_val = old ? Utils::Util::get_field_value<double>(m_header->m_source_fld, old, dict) : 0;
  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   * become complex than before. Due the expensive to calc median value, so the first
   * vauel is set to middle.
   * */
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      m_header->m_prows.fetch_add(1);
      if (!data) return;  // is null, only update rows count.

      ut_a(m_header->m_prows.load() <= SHANNON_ROWS_IN_CHUNK);
      m_header->m_sum.store(m_header->m_sum + data_val);
      m_header->m_avg.store(m_header->m_sum / m_header->m_prows);

      if (m_header->m_min.load(std::memory_order_relaxed) > data_val) m_header->m_min.store(data_val);
      if (m_header->m_max.load(std::memory_order_relaxed) < data_val) m_header->m_max.store(data_val);

      m_header->m_middle.store(
          (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
      m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));

    } break;
    case ShannonBase::OPER_TYPE::OPER_DELETE: {
      // m_header->m_prows.fetch_sub(1);
      if (!data) return;  // is null, only update rows count.

      ut_a(m_header->m_prows.load() <= SHANNON_ROWS_IN_CHUNK);

      if (m_header->m_prows.load(std::memory_order_relaxed) == 0) {  // empty now.
        m_header->m_avg.store(0);
        m_header->m_sum.store(0);
        m_header->m_middle.store(0);
        m_header->m_median.store(0);
        m_header->m_min.store(SHANNON_MAX_DOUBLE);
        m_header->m_max.store(SHANNON_MIN_DOUBLE);
      } else {
        m_header->m_sum.fetch_sub(data_val);
        m_header->m_avg.store(m_header->m_sum.load(std::memory_order_relaxed) /
                              m_header->m_prows.load(std::memory_order_relaxed));

        if (are_equal(m_header->m_min.load(std::memory_order_relaxed), data_val)) {
          // re-calc the min
        }
        if (are_equal(m_header->m_max.load(std::memory_order_relaxed), data_val)) {
          // re-calc the max
        }

        m_header->m_middle.store(
            (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
        m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));
      }
    } break;
    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
      m_header->m_sum.fetch_sub(old_val);
      m_header->m_sum.fetch_add(data_val);
      m_header->m_avg.store(m_header->m_sum.load(std::memory_order_relaxed) /
                            m_header->m_prows.load(std::memory_order_relaxed));

      if (m_header->m_min.load(std::memory_order_relaxed) > data_val) m_header->m_min.store(data_val);
      if (m_header->m_max.load(std::memory_order_relaxed) < data_val) m_header->m_max.store(data_val);

      m_header->m_middle.store(
          (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
      m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));
    } break;
    default:
      break;
  }
}

void Chunk::reset_meta_info() {
  std::scoped_lock lk(m_header_mutex);
  m_header->m_avg = 0;
  m_header->m_sum = 0;
  m_header->m_prows = 0;

  m_header->m_max = std::numeric_limits<long long>::lowest();
  m_header->m_min = std::numeric_limits<long long>::max();
  m_header->m_median = std::numeric_limits<long long>::lowest();
  m_header->m_null_mask.reset(nullptr);
  m_header->m_del_mask.reset(nullptr);
}

// check the data type is leagal or not.
void Chunk::check_data_type(size_t type_size) {
  if (type_size == UNIV_SQL_NULL) return;
  std::scoped_lock lk(m_header_mutex);
  /** if the field is not text type, the data size read/write should be same as its type size. */
  switch (m_header->m_source_fld->type()) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      if (type_size != m_header->m_source_fld->pack_length()) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "column data type is illegal");
        return;
      }
    } break;
    default:
      break;
  }
}

int Chunk::is_null(const Rapid_load_context *context, row_id_t pos) {
  std::scoped_lock lk(m_header_mutex);
  if (!m_header->m_null_mask.get())
    return 0;
  else
    return Utils::Util::bit_array_get(m_header->m_null_mask.get(), pos);
}

int Chunk::is_deleted(const Rapid_load_context *context, row_id_t pos) {
  std::scoped_lock lk(m_header_mutex);
  if (!m_header->m_del_mask.get())
    return 0;
  else
    return Utils::Util::bit_array_get(m_header->m_del_mask.get(), pos);
}

bool Chunk::build_version(row_id_t rowid, Transaction::ID trxid, const uchar *data, size_t len, OPER_TYPE oper) {
  assert(trxid);
  ShannonBase::ReadView::smu_item_t si(m_header->m_normalized_pack_length);

  si.oper_type = oper;

  // if is null. in version link, we set the data to nullptr. otherwise, we set the data to the real data.
  if (data == nullptr) {
    si.data.reset(nullptr);
  } else {
    si.data.reset(new uchar[len]);
    std::memcpy(si.data.get(), data, len);
  }

  si.trxid = trxid;
  si.tm_stamp = std::chrono::high_resolution_clock::now();

  if (m_header->m_smu->m_version_info.find(rowid) != m_header->m_smu->m_version_info.end()) {
    // has old versions, then add
    m_header->m_smu->m_version_info[rowid].add(std::move(si));
  } else {  // this is the first version, then build up the SMU, and add a new one.
    ShannonBase::ReadView::smu_item_vec_t iv;
    iv.add(std::move(si));
    m_header->m_smu->m_version_info.emplace(rowid, std::move(iv));
  }

  if (trxid < m_header->m_trx_min) {
    m_header->m_trx_min = trxid;
  }

  if (trxid > m_header->m_trx_max) {
    m_header->m_trx_max = trxid;
  }
  return false;
}

uchar *Chunk::read(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  check_data_type(len);

  if (unlikely(m_rdata.load() + len > m_end.load())) {
    m_rdata.store(m_base.load());
    return nullptr;
  }

  ut_a(len == m_header->m_normalized_pack_length);
  auto ret = reinterpret_cast<uchar *>(std::memcpy(data, m_rdata, len));
  m_rdata.fetch_add(len);

  return ret;
}

uchar *Chunk::write(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  check_data_type(len);

  auto normal_len = (len == UNIV_SQL_NULL) ? m_header->m_normalized_pack_length : len;
  auto diff = m_data.load(std::memory_order_relaxed) - m_base.load(std::memory_order_relaxed);
  ut_a(diff % m_header->m_normalized_pack_length == 0);
  if (unlikely((m_data.load(std::memory_order_relaxed) + normal_len) >
               m_end.load(std::memory_order_relaxed))) {  // this chunk is full.
    ut_a(diff / m_header->m_normalized_pack_length == SHANNON_ROWS_IN_CHUNK);
    return nullptr;
  }

  row_id_t rowid = diff / m_header->m_normalized_pack_length;
  uchar *ret{m_data.load(std::memory_order_relaxed)};
  if (len == UNIV_SQL_NULL) {      // to write a null value.
    if (!m_header->m_null_mask) {  // allocate a null bitmap.
      m_header->m_null_mask.reset(new (std::nothrow) ShannonBase::bit_array_t(SHANNON_ROWS_IN_CHUNK));
      if (!m_header->m_null_mask) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk header bit map allocation failed");
        return nullptr;
      }
    }

    std::scoped_lock lk(m_header_mutex);
    /**Here, is trying to write a null value, first of all, we update the null bit
     * mask, then writting a placehold to chunk, we dont care about what read data
     * was written down.*/
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), m_header->m_prows);
    len = m_header->m_normalized_pack_length;
  } else {
    std::scoped_lock data_guard(m_data_mutex);
    ret = static_cast<uchar *>(std::memcpy(m_data.load(), data, len));
  }
  m_data.fetch_add(len);

  if (context->m_extra_info.m_trxid) {  // means not from secondary_load operation.
    build_version(rowid, context->m_extra_info.m_trxid, data, len, OPER_TYPE::OPER_INSERT);
  }
  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, data, data);

#ifndef NDEBUG
  uint64 data_rows = static_cast<uint64>(static_cast<ptrdiff_t>(m_data.load() - m_base.load()) / len);
  ut_a(data_rows <= SHANNON_ROWS_IN_CHUNK);
#endif
  return ret;
}

uchar *Chunk::update(const Rapid_load_context *context, row_id_t where, uchar *new_data, size_t len) {
  ut_a((!new_data && len == UNIV_SQL_NULL) || (new_data && len != UNIV_SQL_NULL));
  check_data_type(len);

  std::scoped_lock data_guard(m_data_mutex);
  auto where_ptr = m_base + where * m_header->m_normalized_pack_length;

  if (context->m_extra_info.m_trxid) {
    build_version(where, context->m_extra_info.m_trxid, where_ptr, len, OPER_TYPE::OPER_UPDATE);
  }

  if (len == UNIV_SQL_NULL) {
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), where);
    len = m_header->m_normalized_pack_length;
    // std::memcpy(where_ptr, static_cast<void *>(const_cast<uchar *>(SHANNON_BLANK_PLACEHOLDER)), len);
  } else {
    len = m_header->m_normalized_pack_length;
    std::memcpy(where_ptr, new_data, len);
  }

  update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, new_data, where_ptr);

  return where_ptr;
}

uchar *Chunk::remove(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a(data && len == m_header->m_source_fld->pack_length());

  if (!m_header->m_del_mask.get()) {
    m_header->m_del_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
  }

  // no data in.
  if (m_data <= m_base) return m_base;
  std::atomic<uchar *> start_pos{m_base.load()};
  size_t row_index{0};

  std::scoped_lock data_guard(m_data_mutex);
  while (start_pos < m_data.load()) {
    assert(!std::memcmp(start_pos, data, len));  // the data we want to del.

    Utils::Util::bit_array_set(m_header->m_del_mask.get(), row_index);

    auto is_null = Utils::Util::bit_array_get(m_header->m_null_mask.get(), row_index);
    if (context->m_extra_info.m_trxid) {
      build_version(row_index, context->m_extra_info.m_trxid, data, is_null ? UNIV_SQL_NULL : len,
                    OPER_TYPE::OPER_DELETE);
    }
    update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, start_pos, start_pos);

    start_pos += m_header->m_source_fld->pack_length();
    row_index++;
  }

  return nullptr;
}

uchar *Chunk::remove(const Rapid_load_context *context, row_id_t rowid) {
  uchar *del_from{nullptr};

  if (rowid >= m_header->m_prows.load()) return del_from;  // out of rowid range.

  if (!m_header->m_del_mask.get()) {
    // TODO: to impl a more smart algorithm to alloc null and del bitmap.
    m_header->m_del_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
  }

  Utils::Util::bit_array_set(m_header->m_del_mask.get(), rowid);

  std::scoped_lock data_guard(m_data_mutex);
  del_from = m_base + rowid * m_header->m_normalized_pack_length;
  ut_a(del_from <= m_data);

  // get the old data and insert smu ptr link.
  auto data_len = m_header->m_normalized_pack_length;
  if (context->m_extra_info.m_trxid) {
    build_version(rowid, context->m_extra_info.m_trxid, del_from, data_len, OPER_TYPE::OPER_DELETE);
  }
  update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, del_from, del_from);
  return del_from;
}

void Chunk::truncate() {
  std::scoped_lock lk(m_data_mutex);
  if (m_base) {
    ut::aligned_free(m_base);
    m_base = m_data = nullptr;
    rapid_allocated_mem_size -= (SHANNON_ROWS_IN_CHUNK * m_header->m_normalized_pack_length);
  }

  reset_meta_info();
}

row_id_t Chunk::rows(Rapid_load_context *context) {
  // in furture, we get the rows with visibility check. Now, just return the prows.
  return m_header->m_prows;
}

}  // namespace Imcs
}  // namespace ShannonBase