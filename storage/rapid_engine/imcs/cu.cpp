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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/cu.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu() {
  assert (false);
  Set_header(nullptr);
  Allocate_chunk();
}
Cu::Cu(Field* field) {
  Set_header (field);
  Allocate_chunk();
}
Cu::~Cu() {

}
uint Cu::Set_header(Field* field) {
  m_header->m_max_value = LLONG_MIN ;
  m_header->m_min_value = LLONG_MAX_DOUBLE;
  m_header->m_avg_value = 0;
  m_header->m_middle_value = 0;
  m_header->m_median_value = 0;

  m_header->m_num_rows = 0 ;
  m_header->m_num_nulls = 0;
  m_header->m_num_chunks = 0;

  m_header->m_nullable = false;

  m_header->m_compress_algo =  Compress::enum_compress_algos::NONE;
  m_header->m_local_dict = nullptr;
  if (!field) {
    m_header->m_field = nullptr;
    m_header->m_cu_type = enum_field_types::MYSQL_TYPE_NULL;
  } else {
    m_header->m_field = field;
    m_header->m_cu_type = field->type();
  }
  return 0;
}
Cu_chunk* Cu::Get_chunk(uint index) {
   if (index >= m_chunks.size())
    return nullptr;
   return m_chunks[index];
}
Cu_chunk* Cu::Allocate_chunk() {
  Cu_chunk* chunk = new (current_thd->mem_root) Cu_chunk();
  chunk->Set_owner (this);
  chunk->Allocate_one();
  m_chunks.push_back(chunk);
  return chunk;
}
uint Cu::Deallocate_chunks() {

  return 0;
}
uint Cu::Insert(uchar* data, uint length) {
  if (!data || !length) return 1;

  Cu_chunk* curr_chunk = *m_chunks.rbegin();
  if (curr_chunk->Is_full()) {
   curr_chunk = Allocate_chunk();
  }
  curr_chunk->Add (data, length);
  return 0;
}
uint Cu::Insert(Field* field){
  if (!field) return 1;

  return 0;
}
uint Cu::Delete(uchar* data, uint length) {
  if (!data || !length) return 1;

  return 0;
}
uint Cu::Update(uchar* from, uchar* to) {
  if (!from || !to) return 1;

  return 0;
}

} // ns:Imcs
} // ns:ShannonBase
