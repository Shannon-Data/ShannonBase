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
#include <typeinfo>
#include <memory>

#include "sql/field.h"  //Field
#include "storage/innobase/include/read0types.h"  //readview
#include "storage/innobase/include/trx0trx.h"
#include "storage/innobase/include/univ.i"        //new_withkey
#include "storage/innobase/include/ut0new.h"      //new_withkey

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase {
namespace Imcs {
static unsigned long rapid_allocated_mem_size{0};
extern unsigned long rapid_memory_size;
Chunk::Chunk(Field* field) {
  ut_ad(field);
  ut_ad(ShannonBase::SHANNON_CHUNK_SIZE < rapid_memory_size);
  m_inited = handler::NONE;
  /**m_data_base，here, we use the same psi key with buffer pool which used in innodb page allocation.
   * Here, we use ut::xxx to manage memory allocation and free as innobase doese. In SQL lay, we will
   * use MEM_ROOT to manage the memory management. In IMCS, all modules use ut:: to manage memory
   * operations, it's an effiecient memory utils. it has been initialized in ha_innodb.cc: ut_new_boot();
  */
  if (rapid_allocated_mem_size + ShannonBase::SHANNON_CHUNK_SIZE <= rapid_memory_size) {
     m_data_base = static_cast<uchar *>(ut::malloc_large_page_withkey(
        ut::make_psi_memory_key(mem_key_buf_buf_pool), ShannonBase::SHANNON_CHUNK_SIZE,
        ut::fallback_to_normal_page_t{}));
     if (!m_data_base) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
        return;
      }
     m_data = m_data_base;
     m_data_cursor = m_data_base;
     m_data_end = m_data_base + static_cast<ptrdiff_t>(ShannonBase::SHANNON_CHUNK_SIZE);
     rapid_allocated_mem_size += ShannonBase::SHANNON_CHUNK_SIZE;
  } else {
     my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
     return;
  }

  m_header = ut::new_withkey<Chunk_header>(UT_NEW_THIS_FILE_PSI_KEY);
  //m_header = ut::make_unique<Chunk_header>(ut::make_psi_memory_key(mem_key_buf_buf_pool));
  if (!m_header) return;

  m_header->m_avg = 0;
  m_header->m_sum = 0;
  m_header->m_rows = 0;

  m_header->m_max = std::numeric_limits <long long>::lowest();
  m_header->m_min = std::numeric_limits <long long>::max();
  m_header->m_median =  std::numeric_limits <long long>::lowest();
  m_header->m_middle =  std::numeric_limits <long long>::lowest();

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
}
Chunk::~Chunk() {
  std::scoped_lock lk(m_header_mutex);
  if (m_header) {
    ut::delete_(m_header);
    m_header = nullptr;
  }

  if (!m_data_base && ut::free_large_page(m_data_base, ut::fallback_to_normal_page_t{})) {
    m_data_base = nullptr;
    m_data_cursor = m_data_base;
    m_data_end = m_data_base;
  }
}
uint Chunk::rnd_init(bool scan){
  DBUG_TRACE;
  ut_ad(m_inited == handler::NONE);
  m_data_cursor = m_data_base;
  m_inited = handler::RND;
  return 0;
}
uint Chunk::rnd_end(){
  DBUG_TRACE;
  ut_ad(m_inited == handler::RND);
  m_data_cursor = m_data_base;
  m_inited = handler::NONE;
  return 0;
}
uchar* Chunk::write_data_direct(ShannonBase::RapidContext* context, uchar* data, uint length) {
  DBUG_TRACE;
  ut_ad(m_data_base || data);
  std::scoped_lock lk(m_data_mutex);
  if (m_data + length > m_data_end)  return nullptr;

  double val {0};
  memcpy(m_data, data, length);
  m_data += length;
  m_header->m_rows.fetch_add(1, std::memory_order_seq_cst);
  //writes success, then updates the meta info.
  val = *(double*) (m_data - length + SHANNON_DATA_BYTE_OFFSET);
  if (m_header->m_chunk_type == MYSQL_TYPE_BLOB || m_header->m_chunk_type == MYSQL_TYPE_STRING ||
      m_header->m_chunk_type == MYSQL_TYPE_VARCHAR) {
  } else {
    m_header->m_sum = m_header->m_sum + val;
    m_header->m_avg = m_header->m_sum / m_header->m_rows;
    if (is_less_than(m_header->m_max, val))
      m_header->m_max.store(val, std::memory_order::memory_order_relaxed);
    if (is_greater_than(m_header->m_min, val))
      m_header->m_min.store(val, std::memory_order::memory_order_relaxed);
  }
  return m_data;
}
uchar* Chunk::read_data_direct(ShannonBase::RapidContext* context, uchar* buffer) {
  DBUG_TRACE;
  ut_ad(context && buffer);
  //has to the end.
  ptrdiff_t diff = m_data_cursor - m_data;
  if (diff >= 0) return nullptr;

  uint8 info = *((uint8*)(m_data_cursor + SHANNON_INFO_BYTE_OFFSET));   //info byte
  if (info & DATA_DELETE_FLAG_MASK) {//deleted.
    m_data_cursor += SHANNON_ROW_TOTAL_LEN;
    memcpy(buffer, m_data_cursor, SHANNON_ROW_TOTAL_LEN);
    m_data_cursor += SHANNON_ROW_TOTAL_LEN; //go to the next.
    return m_data_cursor;
  }

  uint64 trxid = *((uint64*)(m_data_cursor + SHANNON_TRX_ID_BYTE_OFFSET)); //trxid bytes
  //visibility check at firt.
  table_name_t name{const_cast<char*>(context->m_current_db.c_str())};
  ReadView* read_view = trx_get_read_view(context->m_trx);
  ut_ad(read_view);
  if (!read_view->changes_visible(trxid, name) || (info & DATA_DELETE_FLAG_MASK)) {//invisible and deleted
    //TODO: travel the change link to get the visibile version data.
    m_data_cursor += SHANNON_ROW_TOTAL_LEN; //to the next value.
    diff = m_data_cursor - m_data;
    if (diff > 0) return nullptr; //no data here.
    return m_data_cursor;
  }
 #ifdef SHANNON_ONLY_DATA_FETCH
  uint32 sum_ptr_off;
  uint64 pk, data;
  //reads info field
  info [[maybe_unused]] = *(m_data_cursor ++);
  m_data_cursor += SHANNON_TRX_ID_BYTE_LEN;
  //reads PK field
  memcpy(&pk, m_data_cursor, SHANNON_ROWID_BYTE_LEN);
  m_data_cursor += SHANNON_ROWID_BYTE_LEN;
  //reads sum_ptr field
  memcpy(&sum_ptr_off, m_data_cursor, SHANNON_SUMPTR_BYTE_LEN);
  m_data_cursor +=SHANNON_SUMPTR_BYTE_LEN;
  if (!sum_ptr_off) {
  //To be impled.
  }
  //reads real data field. if it string type stores strid otherwise, real data.
  memcpy(&data, m_data_cursor, SHANNON_DATA_BYTE_LEN);
  m_data_cursor +=SHANNON_DATA_BYTE_LEN;
  //cpy the data into buffer.
  memcpy(buffer, &data, SHANNON_DATA_BYTE_LEN);
 #else
  memcpy(buffer, m_data_cursor, SHANNON_ROW_TOTAL_LEN);
  m_data_cursor += SHANNON_ROW_TOTAL_LEN; //go to the next.
 #endif
  return m_data_cursor;
}
ha_rows  Chunk::records_in_range(ShannonBase::RapidContext* context, key_range* min_key,
                                 key_range* max_key) {
  ha_rows count{0};
  uchar* cur_pos = m_data_base;
  double data_val {0}, minkey{0}, maxkey{0};
  while (cur_pos < m_data.load(std::memory_order::memory_order_seq_cst)) {
    auto info = *(uint8*) cur_pos;
    if (info & DATA_DELETE_FLAG_MASK || info & DATA_NULL_FLAG_MASK) { //deleted.
      cur_pos += SHANNON_ROW_TOTAL_LEN;
      continue;
    }

    switch (m_header->m_chunk_type) {
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:{
        data_val  = *(int*) (cur_pos + SHANNON_DATA_BYTE_OFFSET);
        minkey = min_key ? *(int*) min_key->key : std::numeric_limits <int>::lowest();
        maxkey = max_key ? *(int*) max_key->key : std::numeric_limits <int>::max();
      }break;
      case MYSQL_TYPE_LONGLONG: {
        data_val  = *(longlong*) (cur_pos + SHANNON_DATA_BYTE_OFFSET);
        minkey = min_key? *(longlong*) min_key->key : std::numeric_limits <longlong>::lowest();
        maxkey = max_key? *(longlong*) max_key->key : std::numeric_limits <longlong>::max();
      } break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        data_val  = *(double*) (cur_pos + SHANNON_DATA_BYTE_OFFSET);
        minkey = min_key ? *(double*) min_key->key : std::numeric_limits <double>::lowest();
        maxkey = max_key ? *(double*) max_key->key : std::numeric_limits <double>::max();;
      }break;
      default:
        break;
    }
    if ((is_greater_than(data_val, minkey) || are_equal(data_val, minkey)) &&
       (is_less_than(data_val, maxkey) || are_equal(data_val, maxkey)))
      count++;
    cur_pos += SHANNON_ROW_TOTAL_LEN;
  }
  return count;
}
uchar* Chunk::read_data_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* buffer) {
  assert(context && rowid && buffer);
  return nullptr;
}
uchar* Chunk::delete_data_direct(ShannonBase::RapidContext* context, uchar* rowid) {
  return nullptr;
}
uchar* Chunk::delete_all_direct() {
  if (m_data_base) {
    my_free(m_data_base);
    m_data_base = nullptr;
  }
  m_data = m_data_base;
  m_data_end = m_data_base;
  return m_data_base; 
}

uchar* Chunk::update_date_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* data, uint length) {
  return nullptr;
}

uint flush_direct(ShannonBase::RapidContext* context, uchar* from, uchar* to) {
  bool flush_all[[maybe_unused]] {true};
  if (!from || !to)
    flush_all = false;

  assert(false);
  return 0;
}

} //ns:icms
} //ns:shannonbase
