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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/chunk.h"

#include <stddef.h>
#include <memory>
#include <typeinfo>

#include "sql/field.h"                            //Field
#include "storage/innobase/include/read0types.h"  //readview
#include "storage/innobase/include/trx0trx.h"
#include "storage/innobase/include/univ.i"    //new_withkey
#include "storage/innobase/include/ut0new.h"  //new_withkey

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
static unsigned long rapid_allocated_mem_size{0};
extern unsigned long rapid_memory_size;
Chunk::Chunk(Field *field) {
  ut_ad(field);
  ut_ad(ShannonBase::SHANNON_CHUNK_SIZE < rapid_memory_size);
  m_inited = handler::NONE;

  m_header = std::make_unique<Chunk_header> ();
  if (!m_header.get()) {
    assert(false);
    return ;
  }

  /**m_data_base，here, we use the same psi key with buffer pool which used in
   * innodb page allocation. Here, we use ut::xxx to manage memory allocation
   * and free as innobase doese. In SQL lay, we will use MEM_ROOT to manage the
   * memory management. In IMCS, all modules use ut:: to manage memory operations,
   * it's an effiecient memory utils. it has been initialized in
   * ha_innodb.cc: ut_new_boot(); */
  if (likely(rapid_allocated_mem_size + ShannonBase::SHANNON_CHUNK_SIZE <=
      rapid_memory_size)) {
    m_data_base = static_cast<uchar *>(ut::aligned_alloc(ShannonBase::SHANNON_CHUNK_SIZE,
        ALIGN_WORD(ShannonBase::SHANNON_CHUNK_SIZE, SHANNON_ROW_TOTAL_LEN)));

    if (unlikely(!m_data_base)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
      return;
    }
    m_data = m_data_base;
    m_data_cursor = m_data_base;
    m_data_end =
        m_data_base + static_cast<ptrdiff_t>(ShannonBase::SHANNON_CHUNK_SIZE);
    rapid_allocated_mem_size += ShannonBase::SHANNON_CHUNK_SIZE;

    init_header_info(field);
  } else {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Rapid allocated memory exceeds over the maximum");
    return;
  }
}

Chunk::~Chunk() {
  std::scoped_lock lk(m_header_mutex);

  if (m_data_base) {
    ut::aligned_free(m_data_base);
    m_data_base = nullptr;
    m_data_cursor = nullptr;
    m_data_end = nullptr;
  }
}

uint Chunk::rnd_init(bool scan) {
  DBUG_TRACE;
  ut_ad(m_inited == handler::NONE);
  m_data_cursor = m_data_base;
  m_inited = handler::RND;
  return 0;
}

uint Chunk::rnd_end() {
  DBUG_TRACE;
  ut_ad(m_inited == handler::RND);
  m_data_cursor = m_data_base;
  m_inited = handler::NONE;
  return 0;
}

bool Chunk::init_header_info(const Field* field){
  std::scoped_lock lk(m_header_mutex);

  m_header->m_avg = 0;
  m_header->m_sum = 0;
  m_header->m_rows = 0;

  m_header->m_max = std::numeric_limits<long long>::lowest();
  m_header->m_min = std::numeric_limits<long long>::max();
  m_header->m_median = std::numeric_limits<long long>::lowest();
  m_header->m_middle = std::numeric_limits<long long>::lowest();

  m_header->m_field_no = field->field_index();
  m_header->m_chunk_type = field->type();
  m_header->m_null = field->is_nullable();

  switch (m_header->m_chunk_type) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_GEOMETRY:
      m_header->m_varlen = true;
      break;
    default:
      m_header->m_varlen = false;
      break;
  }
  return true;
}

bool Chunk::update_header_info(double old_v, double new_v, OPER_TYPE type) {
  std::scoped_lock lk(m_header_mutex);
  //string type do nothing. will build up the histogram in next.
  if (m_header->m_chunk_type == MYSQL_TYPE_BLOB ||
          m_header->m_chunk_type == MYSQL_TYPE_STRING ||
          m_header->m_chunk_type == MYSQL_TYPE_VARCHAR)
    return true;

  switch (type) {
    case OPER_TYPE::OPER_INSERT: {
      m_header->m_rows.fetch_add(1, std::memory_order_seq_cst);
      // writes success, then updates the meta info.
      m_header->m_sum = m_header->m_sum + new_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;

      if (is_less_than(m_header->m_max, new_v))
        m_header->m_max.store(new_v, std::memory_order::memory_order_relaxed);
      if (is_greater_than(m_header->m_min, new_v))
        m_header->m_min.store(new_v, std::memory_order::memory_order_relaxed);
    } break;
    case OPER_TYPE::OPER_UPDATE: break;{
      m_header->m_sum = m_header->m_sum - old_v + new_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;

      double value = is_greater_than_or_eq(old_v, new_v) ? old_v : new_v;
      if (is_less_than_or_eq(m_header->m_max, value))
        m_header->m_max.store(value, std::memory_order::memory_order_relaxed);
      if (is_greater_than_or_eq(m_header->m_min, value))
        m_header->m_min.store(value, std::memory_order::memory_order_relaxed);
    } break;
    case OPER_TYPE::OPER_DELETE: {
      m_header->m_rows.fetch_sub(1, std::memory_order_seq_cst);
      m_header->m_sum = m_header->m_sum - old_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;
        //here, we keeps the boundary of the data. will update in future.
    } break;
    default:
      ut_a(false);
    break;
  }

  return true;
}

bool Chunk::reset_header_info() {
  std::scoped_lock lk(m_header_mutex);
  m_header->m_avg = 0;
  m_header->m_sum = 0;
  m_header->m_rows = 0;

  m_header->m_max = std::numeric_limits<long long>::lowest();
  m_header->m_min = std::numeric_limits<long long>::max();
  m_header->m_median = std::numeric_limits<long long>::lowest();
  m_header->m_middle = std::numeric_limits<long long>::lowest();

  m_header->m_field_no = 0;
  m_header->m_chunk_type = MYSQL_TYPE_NULL;
  m_header->m_null = false;
  m_header->m_varlen = false;
  return true;
}

bool Chunk::deleted (const uchar* data) {
  ut_a(data);

  uint8 info =
      *((uint8 *)(m_data_cursor + SHANNON_INFO_BYTE_OFFSET));  // info byte
  return (info & DATA_DELETE_FLAG_MASK) ? true : false;
}

bool Chunk::is_null(const uchar* data) {
  ut_a(data);

  uint8 info =
      *((uint8 *)(m_data_cursor + SHANNON_INFO_BYTE_OFFSET));  // info byte
  return (info & DATA_NULL_FLAG_MASK) ? true : false;
}

uchar *Chunk::write_data_direct(ShannonBase::RapidContext *context, const uchar* pos,
                                const uchar *data, uint length) {
  DBUG_TRACE;
  ut_ad(m_data_base || data || m_data);
  ut_ad(length == SHANNON_ROW_TOTAL_LEN);

  std::scoped_lock lk(m_data_mutex);
  if (unlikely(pos > m_data_end)) return nullptr; //out of range.
  
  return nullptr;
}

uchar *Chunk::write_data_direct(ShannonBase::RapidContext *context, const uchar *data,
                                uint length) {
  DBUG_TRACE;
  ut_ad(m_data_base || data || m_data);
  ut_ad(length == SHANNON_ROW_TOTAL_LEN);

  std::scoped_lock lk(m_data_mutex);
  if (unlikely(m_data + length > m_data_end)) return nullptr;

  memcpy(m_data, data, length);
  m_data += length;

  auto val = *(double *)(m_data - length + SHANNON_DATA_BYTE_OFFSET);
  update_header_info(val, val, OPER_TYPE::OPER_INSERT);
  return (m_data - length);
}

uchar *Chunk::read_data_direct(ShannonBase::RapidContext *context, uchar *buffer) {
  DBUG_TRACE;
  ut_ad(context && buffer);
  // has to the end.
  ptrdiff_t diff = m_data_cursor - m_data;
  if (diff >= 0) return nullptr;

  while (diff < 0) { //find the first visiable an no-deleted data in chunk.
    uint64 trxid =
        *((uint64 *)(m_data_cursor + SHANNON_TRX_ID_BYTE_OFFSET));  // trxid bytes
    // visibility check at firt.
    table_name_t name{const_cast<char *>(context->m_current_db.c_str())};
    ReadView *read_view = trx_get_read_view(context->m_trx);
    ut_ad(read_view);

    if (!read_view->changes_visible(trxid, name) || deleted(m_data_cursor)) {
      // TODO: travel the change link to get the visibile version data.
      m_data_cursor += SHANNON_ROW_TOTAL_LEN;  // to the next value.
      diff = m_data_cursor - m_data;
      if (diff > 0) return nullptr;  // no data here.
    } else {
      memcpy(buffer, m_data_cursor, SHANNON_ROW_TOTAL_LEN);
      m_data_cursor += SHANNON_ROW_TOTAL_LEN;  // go to the next.
      return m_data_cursor;
    }
  }

  return nullptr;
}

ha_rows Chunk::records_in_range(ShannonBase::RapidContext *context,
                                double &min_key, double &max_key) {
  /**
   * in future, we will use sampling to get the nums in range, not to scan all
   * data. it's a templ approach used here.*/
  ha_rows count{0};
  uchar *cur_pos = m_data_base;
  double data_val{0};

  while (cur_pos < m_data.load(std::memory_order::memory_order_seq_cst)) {
    if (deleted(cur_pos)) {  // deleted.
      cur_pos += SHANNON_ROW_TOTAL_LEN;
      continue;
    }

    uint64 trxid =
        *((uint64 *)(cur_pos + SHANNON_TRX_ID_BYTE_OFFSET));  // trxid bytes
    // visibility check at firt.
    table_name_t name{const_cast<char *>(context->m_current_db.c_str())};
    ReadView *read_view = trx_get_read_view(context->m_trx);
    ut_ad(read_view);
    if (!read_view->changes_visible(trxid, name) || deleted(cur_pos)) {  // invisible and deleted
      // TODO: travel the change link to get the visibile version data.
      cur_pos += SHANNON_ROW_TOTAL_LEN;  // to the next value.
      continue;
    }

    data_val = *(double *)(cur_pos + SHANNON_DATA_BYTE_OFFSET);
    if ((is_valid(min_key) && !is_valid(max_key)) &&
        is_greater_than_or_eq(data_val, min_key)) {
      count++;
    } else if ((!is_valid(min_key) && is_valid(max_key)) && is_less_than_or_eq(data_val, max_key)) {
      count++;
    } else if ((is_valid(min_key) && is_valid(max_key)) &&  are_equal(data_val, min_key))
      count++;

    cur_pos += SHANNON_ROW_TOTAL_LEN;
  }

  return count;
}

uchar *Chunk::where(uint offset) {
  return (offset > SHANNON_ROWS_IN_CHUNK)
             ? nullptr
             : (m_data_base + offset * SHANNON_ROW_TOTAL_LEN);
}

uchar *Chunk::seek(uint offset) {
  auto current_pos = m_data_base + (offset * SHANNON_ROW_TOTAL_LEN);
  m_data_cursor = (current_pos > m_data.load(std::memory_order_acq_rel))
                      ? m_data.load(std::memory_order_acq_rel)
                      : current_pos;
  return m_data_cursor;
}

uchar *Chunk::read_data_direct(ShannonBase::RapidContext *context, const uchar *rowid,
                               uchar *buffer) {
  assert(context && rowid && buffer);
  ut_a(false);
  return nullptr;
}

uchar *Chunk::delete_data_direct(ShannonBase::RapidContext *context,
                                 const uchar *rowid) {

  if (rowid > m_data) return nullptr; //out of range.

  std::scoped_lock lk(m_data_mutex);
  uint8 info = *(uint8*) (rowid + SHANNON_INFO_BYTE_OFFSET);
  info |= DATA_DELETE_FLAG_MASK;
  *(uint8*) rowid  = info;
  return const_cast<uchar*>(rowid);
}

uchar *Chunk::delete_all_direct() {
  std::scoped_lock lk(m_data_mutex);
  if (m_data_base) {
    my_free(m_data_base);
    m_data_base = nullptr;
  }
  m_data = m_data_base;
  m_data_end = m_data_base;

  reset_header_info();
  return m_data_base;
}

uchar *Chunk::update_data_direct(ShannonBase::RapidContext *context, const uchar *rowid,
                                 const uchar *data, uint length) {

  ut_a(length == SHANNON_ROW_TOTAL_LEN);
  if (rowid > m_data_end) return nullptr; //out of range.

  std::scoped_lock lk(m_data_mutex);  
  double old = *(double*)(rowid + SHANNON_DATA_BYTE_OFFSET);

  memcpy(const_cast<uchar*>(rowid), data, length);
  auto val = *(double *)(data + SHANNON_DATA_BYTE_OFFSET);
  update_header_info(old, val, OPER_TYPE::OPER_UPDATE);
  return const_cast<uchar*>(rowid);
}

uint Chunk::flush_direct(ShannonBase::RapidContext *context, const uchar *from, const uchar *to) {
  bool flush_all [[maybe_unused]]{true};
  if (!from || !to) flush_all = false;

  assert(false);
  return 0;
}

uchar* Chunk::GC() {
  /*TODO: it's simple way: allocate a new chunk, then copy all of un-delete marked data to
    this new chunk, meanwhile to rebuid the index: drop the old one, and create a new
    one. At first, lock this chunk. after re-build the new chunk, unlock this chunk.
  */
 return nullptr;
}

}  // namespace Imcs
}  // namespace ShannonBase