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

Chunk::Chunk(enum_field_types type) {
  //allocate space for data. Here rows.
  uint size = Chunk::MAX_CHUNK_NUMS_LIMIT * Get_unit(type);
  m_data_base = new (current_thd->mem_root) uchar[size];
  if (!m_data_base) { //OOM.
    return;
  }

  memset(m_data_base, 0x0, size);
  m_data = m_data_base;

  m_headers.m_avg = 0;
  m_headers.m_max = std::numeric_limits <long long>::lowest();
  m_headers.m_min = std::numeric_limits <long long>::max();
  m_headers.m_median = 0;
  m_headers.m_middle = 0;

  assert(m_data <= m_data_end);
}
Chunk::~Chunk() {
  //deallocate data space.
  assert(m_data< m_data_end);
  if (m_data_base) {
    delete []  m_data_base;
    m_data_base = nullptr;
  }
  m_data = m_data_base;
}

uchar* Chunk::Write_data(uchar* data, uint length) {
  if (!data || !length) return nullptr;
  
  if ((m_data + length) >m_data_end) { //need a new chunk.
    return nullptr;
  }
  std::scoped_lock lk(m_data_mutex);
  //write the data to this chunk, and advance the pointer to new pos.
  uchar* curr_pos{nullptr};
  m_headers.m_rows = m_headers.m_rows + 1; 
  return curr_pos;
}

template<typename data_T>
uchar* Chunk::Write_fixed_value(data_T value) {
  uchar* curr_pos {nullptr};
  if (typeid(data_T) != typeid(int)) { //data type error, not supported now.
    return nullptr;
  }

  return curr_pos; 
}
    
template<typename data_T>
uchar* Chunk::Write_var_value(data_T value, uint length, CHARSET_INFO* charset) {
  uchar* curr_pos {nullptr};
  uchar infos;
  
  return curr_pos;
}
uchar* Chunk::Read_data(uchar* data, uint length) {
  if (!data || !length) return nullptr;

  std::scoped_lock lk(m_data_mutex);
  uchar* read_pos {nullptr};

  return read_pos;
}

} //ns:icms
} //ns:shannonbase
