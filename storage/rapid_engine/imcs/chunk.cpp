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

#include <stddef.h>
#include <memory>
#include <typeinfo>

#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"  //status inf
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
/**
 * every chunks has a fixed num of rows: SHANNON_ROWS_IN_CHUNK. we can calcuate
 * the row offset easily by using 'm_data / m_source_fld->pack_length' to get
 * which chunk we are in now, and 'm_data % m_source_fld->pack_length' to get
 * where we are in this chunk.
 */
static unsigned long rapid_allocated_mem_size{0};
Chunk::Chunk(const Field *field) {
  m_header = std::make_unique<Chunk_header>();
  ut_a(m_header);
  m_header->m_pack_length = field->pack_length();

  auto normalized_pack_length = field->pack_length();
  switch (field->type()) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      /**if this is a string type, it will be use local dictionary encoding, therefore,
       * using stringid as field value. */
      normalized_pack_length = sizeof(uint32);
      break;
    default:
      break;
  }
  m_header->m_normailzed_pack_length = normalized_pack_length;
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  m_header->m_type = field->type();

  /** there's null values in, therefore, alloc the null bitmap, and del bit map will
   * lazy allocated.*/
  if (field->is_nullable()) {
    m_header->m_null_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
  }

  // the SMU ptr. just like rollback ptr.
  m_header->m_smu = std::make_unique<Snapshot_meta_unit>();

  auto chunk_size = SHANNON_ROWS_IN_CHUNK * m_header->m_normailzed_pack_length;
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
    m_end = m_base + static_cast<ptrdiff_t>(chunk_size);
    rapid_allocated_mem_size += chunk_size;
  } else {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
    return;
  }
}

Chunk::~Chunk() {
  if (m_base) {
    ut::aligned_free(m_base);
    m_base = m_data = nullptr;
    rapid_allocated_mem_size -= (m_header->m_normailzed_pack_length * SHANNON_ROWS_IN_CHUNK);
  }
}

void Chunk::update_meta_info(OPER_TYPE type, uchar *data) {
  ut_a(data);
  double data_val{0};
  switch (m_header->m_source_fld->type()) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      data_val = *reinterpret_cast<double *>(data);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      data_val = *reinterpret_cast<double *>(data);
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      data_val = *reinterpret_cast<int *>(data);
    } break;
    default:
      break;
  }
  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   *  become complex than before.*/
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      m_header->m_prows.fetch_add(1);
      ut_a(m_header->m_prows.load() <= SHANNON_ROWS_IN_CHUNK);
      m_header->m_sum.store(m_header->m_sum + data_val);
      m_header->m_avg = m_header->m_sum / m_header->m_prows;
    } break;
    case ShannonBase::OPER_TYPE::OPER_DELETE: {
    } break;
    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
    } break;
    default:
      break;
  }
  // UPDATE sum, avg, middle, etc.
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
  /** if the field is not text type, the data size read/write should be
   * same as its type size. */
  switch (m_header->m_source_fld->type()) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      ut_a(type_size == m_header->m_source_fld->pack_length());
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

void Chunk::build_version(row_id_t rowid, Transaction::ID id, const uchar *data, size_t len) {
  smu_item_t si(m_header->m_normailzed_pack_length);
  if (len == UNIV_SQL_NULL) {
    si.data = nullptr;
  } else
    std::memcpy(si.data.get(), data, len);
  si.trxid = id;
  if (m_header->m_smu->m_version_info.find(rowid) != m_header->m_smu->m_version_info.end()) {
    m_header->m_smu->m_version_info[rowid].emplace_back(std::move(si));
  } else {
    smu_item_vec iv;
    iv.emplace_back(std::move(si));
    m_header->m_smu->m_version_info.emplace(rowid, std::move(iv));
  }

  return;
}

uchar *Chunk::read(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  check_data_type(len);

  if (unlikely(m_rdata.load() + len > m_end.load())) {
    m_rdata.store(m_base.load());
    return nullptr;
  }

  ut_a(len == m_header->m_normailzed_pack_length);
  auto ret = reinterpret_cast<uchar *>(std::memcpy(data, m_rdata, len));
  m_rdata.fetch_add(len);

  return ret;
}

uchar *Chunk::write(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  check_data_type(len);

  if (unlikely((m_data.load() + ((len == UNIV_SQL_NULL) ? m_header->m_normailzed_pack_length : len)) >
               m_end.load())) {  // full
    auto diff = m_data.load() - m_base.load();
    ut_a(diff % m_header->m_normailzed_pack_length == 0);
    ut_a(diff / m_header->m_normailzed_pack_length == SHANNON_ROWS_IN_CHUNK);

    m_data.store(m_base.load());
    return nullptr;
  }

  if (len == UNIV_SQL_NULL) {
    if (!m_header->m_null_mask)
      m_header->m_null_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
    std::scoped_lock lk(m_header_mutex);
    /**Here, is trying to write a null value, first of all, we update the null bit
     * mask, then writting a placehold to chunk, we dont care about what read data
     * was written down.*/
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), m_header->m_prows);

    len = m_header->m_normailzed_pack_length;
    data = const_cast<uchar *>(SHANNON_NULL_PLACEHOLDER);
  }

  std::scoped_lock data_guard(m_data_mutex);
  auto ret = static_cast<uchar *>(std::memcpy(m_data.load(), data, len));
  m_data.fetch_add(len);

  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, data);

  uint64 data_rows = static_cast<uint64>(static_cast<ptrdiff_t>(m_data.load() - m_base.load()) / len);
  ut_a(data_rows == m_header->m_prows.load());
  return ret;
}

uchar *Chunk::update(const Rapid_load_context *context, row_id_t where, uchar *new_data, size_t len) {
  ut_a((!new_data && len == UNIV_SQL_NULL) || (new_data && len != UNIV_SQL_NULL));
  check_data_type(len);

  ut_a(where < m_header->m_prows);

  std::scoped_lock data_guard(m_data_mutex);
  auto where_ptr = m_base + where * m_header->m_normailzed_pack_length;

  build_version(where, context->m_extra_info.m_trxid, where_ptr, len);
  if (len == UNIV_SQL_NULL) {
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), where);
    len = m_header->m_normailzed_pack_length;
    std::memcpy(where_ptr, static_cast<void *>(const_cast<uchar *>(SHANNON_BLANK_PLACEHOLDER)), len);
  } else {
    len = m_header->m_normailzed_pack_length;
    std::memcpy(where_ptr, new_data, len);
  }

  update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, new_data);

  return where_ptr;
}

uchar *Chunk::del(const Rapid_load_context *context, uchar *data, size_t len) {
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
    if (!std::memcmp(start_pos, data, len)) {  // same
      Utils::Util::bit_array_set(m_header->m_del_mask.get(), row_index);
      // to set the mem to blank holder.
      auto is_null = Utils::Util::bit_array_get(m_header->m_null_mask.get(), row_index);
      build_version(row_index, context->m_extra_info.m_trxid, data, is_null ? UNIV_SQL_NULL : len);

      std::memcpy(start_pos, reinterpret_cast<void *>(const_cast<uchar *>(SHANNON_BLANK_PLACEHOLDER)), len);
      update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, start_pos);
    }

    start_pos += m_header->m_source_fld->pack_length();
    row_index++;
  }

  return nullptr;
}

uchar *Chunk::del(const Rapid_load_context *context, row_id_t rowid) {
  uchar *del_from{nullptr};

  if (rowid >= m_header->m_prows.load()) return del_from;  // out of rowid range.

  if (!m_header->m_del_mask.get()) {
    // TODO: to impl a more smart algorithm to alloc null and del bitmap.
    m_header->m_del_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
  }

  Utils::Util::bit_array_set(m_header->m_del_mask.get(), rowid);

  std::scoped_lock data_guard(m_data_mutex);
  del_from = m_base + rowid * m_header->m_normailzed_pack_length;
  ut_a(del_from <= m_data);

  // get the old data and insert smu ptr link. should check whether data is null.
  auto is_null = Utils::Util::bit_array_get(m_header->m_null_mask.get(), rowid);
  auto data_len = is_null ? UNIV_SQL_NULL : m_header->m_normailzed_pack_length;
  build_version(rowid, context->m_extra_info.m_trxid, del_from, data_len);

  del_from = static_cast<uchar *>(std::memcpy(del_from, SHANNON_BLANK_PLACEHOLDER, m_header->m_normailzed_pack_length));

  if (del_from) update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, del_from);
  return del_from;
}

void Chunk::truncate() {
  std::scoped_lock lk(m_data_mutex);
  if (m_base) {
    ut::aligned_free(m_base);
    m_base = m_data = nullptr;
    rapid_allocated_mem_size -= (SHANNON_ROWS_IN_CHUNK * m_header->m_normailzed_pack_length);
  }

  reset_meta_info();
}

uchar *Chunk::seek(row_id_t rowid) {
  ut_a(!((m_data - m_base) % m_header->m_normailzed_pack_length));
  auto real_row = (m_data - m_base) / m_header->m_normailzed_pack_length;

  if (rowid >= real_row)
    return m_data;
  else
    return m_base + rowid * m_header->m_normailzed_pack_length;
}

row_id_t Chunk::prows() { return m_header->m_prows; }

row_id_t Chunk::rows(Rapid_load_context *context) {
  // in furture, we get the rows with visibility check. Now, just return the prows.
  return m_header->m_prows;
  ;
}

}  // namespace Imcs
}  // namespace ShannonBase