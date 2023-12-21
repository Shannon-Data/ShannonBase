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
#include <typeinfo>

#include "storage/rapid_engine/imcs/chunk.h"
namespace ShannonBase {
namespace Imcs {

struct deleter_helper {
  void operator() (uchar* ptr) {
    if (ptr) my_free(ptr);
  }
};

Chunk::Chunk(Field* field) {
   assert(field);

  //allocate space for data. Here rows.
  {
    std::scoped_lock lk(m_header_mutex);
    m_header.reset (new (current_thd->mem_root) Chunk_header);
    if (!m_header.get()) return;
    m_header->m_field = field;
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

  //m_data_base
  m_data_base = static_cast<uchar*> (my_malloc(PSI_NOT_INSTRUMENTED, Chunk::SHANNON_CHUNK_SIZE,
                                                     MYF(MY_ZEROFILL | MY_WME)));
  if (m_data_base) {
    m_header->m_avg = 0;
    m_header->m_max = std::numeric_limits <long long>::lowest();
    m_header->m_min = std::numeric_limits <long long>::max();
    m_header->m_median = 0;
    m_header->m_middle = 0;
    m_header->m_lines = 0;
    m_header->m_sum = 0;

    m_data_end = m_data_base + static_cast<ptrdiff_t>(Chunk::SHANNON_CHUNK_SIZE);
    m_data = m_data_base;
  }
}

Chunk::~Chunk() {
  if (m_data_base) {
    my_free(m_data_base);
    m_data_base = nullptr;
  }
}

uchar* Chunk::Write_data(RapidContext* context, uchar* data, uint length) {
  assert(m_data_base || data);
  std::scoped_lock lk(m_data_mutex);
  if (m_data + length > m_data_end)  return nullptr;

  memcpy(m_data, data, length);
  m_data += length;
  //updates the meta info.
  m_header->m_rows ++;
  if (m_header->m_chunk_type == MYSQL_TYPE_BLOB || m_header->m_chunk_type == MYSQL_TYPE_STRING) {
    String buff;
    String* val [[maybe_unused]]= m_header->m_field->val_str(&buff);
  } else {
    long long val = m_header->m_field->val_int();
    m_header->m_sum += val;
    m_header->m_avg = m_header->m_sum / m_header->m_rows;
    if (m_header->m_max < val)
      m_header->m_max.store(val, std::memory_order::memory_order_relaxed);
    if (m_header->m_min > val)
      m_header->m_min.store(val, std::memory_order::memory_order_relaxed);
  }
  return m_data;
}

uchar* Chunk::Read_data(RapidContext* context, uchar* from, uchar* to, uint length) {
  assert(from || to);
  if (from + length > m_data_end) return nullptr;
  return m_data;
}

uchar* Chunk::Read_data(RapidContext* context, uchar* rowid, uint length [[maybe_unused]]) {
  assert(context && rowid);  
  return nullptr;
}
uchar* Chunk::Delete_data(RapidContext* context, uchar* rowid) {
  return nullptr;
}
uchar* Chunk::Delete_all() {
  m_data = m_data_base;
  m_data_end = m_data_base;
  memset(m_data_base, 0x0, Chunk::SHANNON_CHUNK_SIZE);
  return m_data_base; 
}

uchar* Chunk::Update_date(RapidContext* context, uchar* rowid, uchar* data, uint length) {
  return nullptr;
}

uint flush(RapidContext* context, uchar* from, uchar* to) {
  bool flush_all[[maybe_unused]] {true};
  if (!from || !to)
    flush_all = false;

  assert(false);
  return 0;
}

} //ns:icms
} //ns:shannonbase
