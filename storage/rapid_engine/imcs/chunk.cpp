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

#include "storage/innobase/include/read0types.h"
#include "storage/innobase/include/trx0sys.h"
#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"  //status inf
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {

Chunk::ChunkMemoryManager::ChunkMemoryManager(size_t size) : m_size(size) {
  if (size == 0) {
    throw std::invalid_argument("Chunk size cannot be zero");
  }

  m_base_addr = static_cast<uchar *>(ut::aligned_alloc(size, CACHE_LINE_SIZE));
  if (!m_base_addr) {
    return;
  }

  std::memset(m_base_addr, 0, size);
  rapid_allocated_mem_size.fetch_add(size, std::memory_order_relaxed);
}

Chunk::ChunkMemoryManager::~ChunkMemoryManager() {
  if (m_base_addr) {
    rapid_allocated_mem_size.fetch_sub(m_size, std::memory_order_relaxed);
    ut::aligned_free(m_base_addr);
  }
}

Chunk::ChunkMemoryManager &Chunk::ChunkMemoryManager::operator=(ChunkMemoryManager &&other) noexcept {
  if (this != &other) {
    if (m_base_addr) {
      rapid_allocated_mem_size.fetch_sub(m_size, std::memory_order_relaxed);
      ut::aligned_free(m_base_addr);
    }

    m_base_addr = other.m_base_addr;
    m_size = other.m_size;
    other.m_base_addr = nullptr;
    other.m_size = 0;
  }
  return *this;
}
/**
 * every chunks has a fixed num of rows: SHANNON_ROWS_IN_CHUN
 * K. we can calcuate
 * the row offset easily by using 'm_data / m_source_fld->pack_length' to get
 * which chunk we are in now, and 'm_data % m_source_fld->pack_length' to get
 * where we are in this chunk.
 */

Chunk::Chunk(Cu *owner, const Field *field) {
  init_chunk_key(field);
  init_header(owner, field);
  init_body(field);
  if (!validate_initialization()) {
    m_header.reset();
    m_chunk_memory.reset();
    // throw std::runtime_error("Chunk initialization failed validation");
  }
}

Chunk::Chunk(Cu *owner, const Field *field, std::string &keyname) {
  init_chunk_key(field, &keyname);
  init_header(owner, field);
  init_body(field);
  if (!validate_initialization()) {
    m_header.reset();
    m_chunk_memory.reset();
    // throw std::runtime_error("Chunk initialization failed validation");
  }
}

Chunk::~Chunk() {
  m_header.reset();
  m_chunk_memory.reset();
}

bool Chunk::validate_initialization() const {
  return m_header && m_header->m_source_fld && m_header->m_normalized_pack_length > 0 && m_header->m_smu &&
         m_chunk_memory && m_chunk_memory->valid();
}

void Chunk::init_chunk_key(const Field *field, const std::string *custom_key) {
  if (custom_key) {
    m_chunk_key = *custom_key;
  } else {
    m_chunk_key.clear();
    m_chunk_key.reserve(256);
    m_chunk_key.append(field->table->s->db.str)
        .append(":")
        .append(field->table->s->table_name.str)
        .append(":")
        .append(field->field_name);
  }
  m_chunk_footprint = m_chunk_key.append(":").append(Utils::Util::currenttime_to_string());
}

void Chunk::init_header(Cu *owner, const Field *field) {
  m_header = std::make_unique<Chunk_header>();
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk header allocation failed");
    return;  // allocated faile.
  }

  m_header->m_owner = owner;
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  if (!m_header->m_source_fld) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Failed to clone field");
    return;  // allocated faile.
  }

  m_header->m_type = field->type();
  m_header->m_pack_length = field->pack_length();
  m_header->m_normalized_pack_length = Utils::Util::normalized_length(field);
  m_header->m_key_len = field->table->file->ref_length;
  m_header->m_nullable = field->is_nullable();

  m_header->m_prows.store(0, std::memory_order_relaxed);
  m_header->m_sum.store(0, std::memory_order_relaxed);
  m_header->m_avg.store(0, std::memory_order_relaxed);
  m_header->m_min.store(std::numeric_limits<double>::max(), std::memory_order_relaxed);
  m_header->m_max.store(std::numeric_limits<double>::lowest(), std::memory_order_relaxed);
  m_header->m_middle.store(0, std::memory_order_relaxed);
  m_header->m_median.store(0, std::memory_order_relaxed);

  m_header->m_trx_min = Transaction::ID{0};
  m_header->m_trx_max = Transaction::ID{0};

  m_header->m_smu = std::make_unique<ShannonBase::ReadView::Snapshot_meta_unit>();
  if (!m_header->m_smu) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Failed to allocate Snapshot_meta_unit");
    return;
  }
  m_header->m_smu->set_owner(this);

  if (field->is_nullable()) {
    ensure_null_mask_allocated();
  }
}

bool Chunk::ensure_null_mask_allocated() {
  std::unique_lock<std::shared_mutex> lock(m_header_mutex);

  if (!m_header->m_null_mask) {
    m_header->m_null_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
    if (!m_header->m_null_mask) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Failed to allocate null mask");
      return true;
    }
  }
  return false;
}

bool Chunk::ensure_del_mask_allocated() {
  std::unique_lock<std::shared_mutex> lock(m_header_mutex);

  if (!m_header->m_del_mask) {
    m_header->m_del_mask = std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
    if (!m_header->m_del_mask) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Failed to allocate deletion mask");
      return true;
    }
  }
  return false;
}

void Chunk::init_body(const Field *field) {
  size_t chunk_size = SHANNON_ROWS_IN_CHUNK * m_header->m_normalized_pack_length;
  ut_ad(field && chunk_size < ShannonBase::rpd_mem_sz_max);

  /**m_data_base，here, we use the same psi key with buffer pool which used in
   * innodb page allocation. Here, we use ut::xxx to manage memory allocation
   * and free as innobase doese. In SQL lay, we will use MEM_ROOT to manage the
   * memory management. In IMCS, all modules use ut:: to manage memory
   * operations, it's an effiecient memory utils. it has been initialized in
   * ha_innodb.cc: ut_new_boot(); */
  if (likely(rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
    return;
  }

  m_chunk_memory = std::make_unique<ChunkMemoryManager>(chunk_size);
  if (unlikely(!m_chunk_memory->get())) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
    return;
  }

  m_capacity = chunk_size;
  m_data.store(m_chunk_memory->get());
  m_end.store(m_chunk_memory->get() + static_cast<ptrdiff_t>(m_capacity));
  m_rdata.store(m_chunk_memory->get());
}

void Chunk::update_meta_info(const Rapid_load_context *context, OPER_TYPE type, uchar *data, uchar *old) {
  auto dict = m_header->m_owner->header()->m_local_dict.get();
  double data_val = data ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, dict) : 0;
  double old_val = old ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, old, dict) : 0;
  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   * become complex than before. Due the expensive to calc median value, so the first
   * vauel is set to middle.
   * */
  auto trxid = context->m_trx ? context->m_trx->get_id() : 0;
  m_header->m_trx_min = std::min(m_header->m_trx_min, trxid);
  m_header->m_trx_max = std::max(m_header->m_trx_max, trxid);

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
  std::unique_lock<std::shared_mutex> lk(m_header_mutex);
  m_header->m_avg.store(0);
  m_header->m_sum.store(0);
  m_header->m_prows.store(0);

  m_header->m_max = std::numeric_limits<long long>::lowest();
  m_header->m_min = std::numeric_limits<long long>::max();
  m_header->m_median = std::numeric_limits<long long>::lowest();
  m_header->m_null_mask.reset(nullptr);
  m_header->m_del_mask.reset(nullptr);
}

// check the data type is leagal or not.
void Chunk::check_data_type(size_t type_size) {
  if (type_size == UNIV_SQL_NULL) return;

  std::shared_lock<std::shared_mutex> lk(m_header_mutex);
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
  std::shared_lock<std::shared_mutex> lk(m_header_mutex);
  if (!m_header->m_null_mask.get())
    return static_cast<int>(false);
  else
    return Utils::Util::bit_array_get(m_header->m_null_mask.get(), pos);
}

int Chunk::is_deleted(const Rapid_load_context *context, row_id_t pos) {
  std::shared_lock<std::shared_mutex> lk(m_header_mutex);
  if (!m_header->m_del_mask.get())
    return SHANNON_SUCCESS;
  else
    return Utils::Util::bit_array_get(m_header->m_del_mask.get(), pos);
}

int Chunk::build_version(row_id_t rowid, Transaction::ID trxid, const uchar *data, size_t len, OPER_TYPE oper) {
  assert(trxid);
  ShannonBase::ReadView::smu_item_t si(len);

  si.oper_type = oper;

  // if is null. in version link, we set the data to nullptr. otherwise, we set the data to the real data.
  if (data == nullptr) {
    si.data.reset(nullptr);
    si.sz = UNIV_SQL_NULL;
  } else {
    si.data.reset(new uchar[len]);
    std::memcpy(si.data.get(), data, len);
  }

  auto now = std::chrono::high_resolution_clock::now();
  si.trxid = trxid;
  si.tm_stamp = si.tm_committed = now;
  m_header->m_smu->versions(rowid).add(si);

  return SHANNON_SUCCESS;
}

uchar *Chunk::read(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  check_data_type(len);

  ut_a(len % m_header->m_normalized_pack_length == 0);

  if (unlikely(m_rdata.load(std::memory_order_acquire) + static_cast<ptrdiff_t>(len) >
               m_end.load(std::memory_order_acquire))) {  // out of range.
    m_rdata.store(base());
    return nullptr;
  }

  auto ret = reinterpret_cast<uchar *>(std::memcpy(data, m_rdata.load(std::memory_order_acquire), len));
  m_rdata.fetch_add(len);

  return ret;
}

uchar *Chunk::write(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));

  check_data_type(len);

  uchar *ret{m_data.load(std::memory_order_acquire)};
  auto normal_len = (len == UNIV_SQL_NULL) ? m_header->m_normalized_pack_length : len;
  ut_a(normal_len == m_header->m_normalized_pack_length);
  auto rowid = current_pos();

  if (len == UNIV_SQL_NULL) {  // to write a null value.
    if (!m_header->m_null_mask && ensure_null_mask_allocated()) return nullptr;
    /**Here, is trying to write a null value, first of all, we update the null bit
     * mask, then writting a placehold to chunk, we dont care about what read data
     * was written down.*/
    std::unique_lock<std::shared_mutex> lk(m_header_mutex);
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), m_header->m_prows);
  } else {
    ut_a(len % m_header->m_normalized_pack_length == 0);
    if (unlikely((m_data + static_cast<ptrdiff_t>(normal_len)) > m_end.load(std::memory_order_acquire)))
      return nullptr;  // this chunk is full.

    ret = static_cast<uchar *>(std::memcpy(m_data.load(std::memory_order_acquire), data, normal_len));
  }
  m_data.fetch_add(normal_len);

  if (context->m_extra_info.m_trxid) {  // means not from secondary_load operation.
    build_version(rowid, context->m_extra_info.m_trxid, data, normal_len, OPER_TYPE::OPER_INSERT);
  }

  update_meta_info(context, ShannonBase::OPER_TYPE::OPER_INSERT, data, data);

#ifndef NDEBUG
  uint64 data_rows = static_cast<uint64>(static_cast<ptrdiff_t>(m_data.load(std::memory_order_acquire) - base()) /
                                         m_header->m_normalized_pack_length);
  ut_a(data_rows <= SHANNON_ROWS_IN_CHUNK);
#endif
  return ret;
}

uchar *Chunk::write_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a((!data && len == UNIV_SQL_NULL) || (data && len != UNIV_SQL_NULL));
  ut_a(len == m_header->m_normalized_pack_length);
  check_data_type(len);

  auto normal_len = (len == UNIV_SQL_NULL) ? m_header->m_normalized_pack_length : len;
  auto to_addr = base() + static_cast<ptrdiff_t>(rowid * normal_len);
  if (to_addr > end()) return nullptr;  // out of range.

  uchar *ret{m_data.load(std::memory_order_acquire)};
  if (len == UNIV_SQL_NULL) {                                                    // to write a null value.
    if (!m_header->m_null_mask && ensure_null_mask_allocated()) return nullptr;  // allocate a null bitmap failed.

    /**Here, is trying to write a null value, first of all, we update the null bit
     * mask, then writting a placehold to chunk, we dont care about what read data
     * was written down.*/
    std::unique_lock<std::shared_mutex> lk(m_header_mutex);
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), rowid);
  } else {
    ret = static_cast<uchar *>(std::memcpy(to_addr, data, normal_len));
  }
  m_data.fetch_add(normal_len);

  if (context->m_extra_info.m_trxid) {  // means not from secondary_load operation.
    build_version(rowid, context->m_extra_info.m_trxid, data, normal_len, OPER_TYPE::OPER_INSERT);
  }

  update_meta_info(context, ShannonBase::OPER_TYPE::OPER_INSERT, data, data);

#ifndef NDEBUG
  uint64 data_rows =
      static_cast<uint64>(static_cast<ptrdiff_t>(m_data.load() - base()) / m_header->m_normalized_pack_length);
  ut_a(data_rows <= SHANNON_ROWS_IN_CHUNK);
#endif
  return ret;
}

uchar *Chunk::update(const Rapid_load_context *context, row_id_t rowid, uchar *new_data, size_t len) {
  ut_a((!new_data && len == UNIV_SQL_NULL) || (new_data && len != UNIV_SQL_NULL));
  check_data_type(len);

  ut_a(len % m_header->m_normalized_pack_length == 0);

  auto normal_len = (len == UNIV_SQL_NULL) ? m_header->m_normalized_pack_length : len;
  auto where_ptr = tell(rowid);
  if (context->m_extra_info.m_trxid) {
    build_version(rowid, context->m_extra_info.m_trxid, where_ptr, normal_len, OPER_TYPE::OPER_UPDATE);
  }

  if (len == UNIV_SQL_NULL) {
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), rowid);
    // std::memcpy(where_ptr, static_cast<void *>(const_cast<uchar *>(SHANNON_BLANK_PLACEHOLDER)), len);
  } else {
    std::memcpy(where_ptr, new_data, normal_len);
  }

  update_meta_info(context, ShannonBase::OPER_TYPE::OPER_UPDATE, new_data, where_ptr);

  return where_ptr;
}

uchar *Chunk::update_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *new_data, size_t len) {
  ut_a((!new_data && len == UNIV_SQL_NULL) || (new_data && len != UNIV_SQL_NULL));
  check_data_type(len);

  auto normal_len = (len == UNIV_SQL_NULL) ? m_header->m_normalized_pack_length : len;
  auto where_ptr = tell(rowid);

  if (context->m_extra_info.m_trxid) {
    build_version(rowid, context->m_extra_info.m_trxid, where_ptr, normal_len, OPER_TYPE::OPER_UPDATE);
  }

  if (len == UNIV_SQL_NULL) {
    Utils::Util::bit_array_set(m_header->m_null_mask.get(), rowid);
    // std::memcpy(where_ptr, static_cast<void *>(const_cast<uchar *>(SHANNON_BLANK_PLACEHOLDER)), len);
  } else {
    std::memcpy(where_ptr, new_data, normal_len);
  }

  update_meta_info(context, ShannonBase::OPER_TYPE::OPER_UPDATE, new_data, where_ptr);

  return where_ptr;
}

uchar *Chunk::remove(const Rapid_load_context *context, row_id_t rowid) {
  if (rowid >= m_header->m_prows.load() && rowid <= SHANNON_ROWS_IN_CHUNK) return nullptr;  // out of rowid range.

  if (!m_header->m_del_mask.get() && ensure_del_mask_allocated()) return nullptr;  // allocated del mask failed.

  {
    std::unique_lock<std::shared_mutex> lk(m_header_mutex);
    Utils::Util::bit_array_set(m_header->m_del_mask.get(), rowid);
  }

  std::shared_lock<std::shared_mutex> lk(m_header_mutex);
  bool is_null = (m_header->m_null_mask.get()) ? Utils::Util::bit_array_get(m_header->m_null_mask.get(), rowid) : false;

  auto del_from = tell(rowid);
  // get the old data and insert smu ptr link.
  auto data_len = m_header->m_normalized_pack_length;
  if (context->m_extra_info.m_trxid) {
    build_version(rowid, context->m_extra_info.m_trxid, is_null ? nullptr : del_from,
                  is_null ? UNIV_SQL_NULL : data_len, OPER_TYPE::OPER_DELETE);
  }

  update_meta_info(context, ShannonBase::OPER_TYPE::OPER_DELETE, del_from, del_from);
  return del_from;
}

void Chunk::truncate() {
  m_header->m_smu.reset();
  m_header.reset();
  m_chunk_memory.reset();

  // todo: remove all index record from index tree.
  reset_meta_info();
}

int Chunk::purge() {
  auto ret{SHANNON_SUCCESS};

  if (ShannonBase::is_greater_than_or_eq((m_header->m_smu->version_info().size() * 1.0 / m_header->m_prows.load()),
                                         ShannonBase::SHANNON_GC_RATIO_THRESHOLD)) {
    // need to do fully GC.
    ret = this->GC();
  } else {
    ::ReadView oldest_view;
    trx_sys->mvcc->clone_oldest_view(&oldest_view);
    auto table_name = m_header->m_owner->header()->m_owner->m_table_name;
    ret = m_header->m_smu->purge(table_name.c_str(), &oldest_view);
    m_header->m_last_gc_tm = std::chrono::steady_clock::now();
  }
  return ret;
}

int Chunk::GC() { return SHANNON_SUCCESS; }

row_id_t Chunk::rows(Rapid_load_context *context) {
  // in furture, we get the rows with visibility check. Now, just return the prows.
  if (context == nullptr)
    return m_header->m_prows;
  else
    assert(false);  // not ready now.
}

}  // namespace Imcs
}  // namespace ShannonBase