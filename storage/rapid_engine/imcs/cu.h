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
#ifndef __SHANNONBASE_CU_H__
#define __SHANNONBASE_CU_H__

#include <vector>

#include "field_types.h" //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_sys.h"      //for page size
#include "sql/sql_class.h"
#include "sql/field.h"   //Field
#include "sql/current_thd.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_object.h"

namespace ShannonBase{
namespace Imcs{

class Cu;
//A cu divide into lots of chunk, each chunk has 65K rows.
struct Cu_chunk {
 //index. index <= m_num_chunks
 uint m_index;
 //belongs to which cu.
 Cu* m_owner;
 //start pos, current pos, null pos.
 uchar* m_data, *m_current_pos, *m_null_pos;
 uint m_chunk_size;
 Cu_chunk () {
  m_chunk_size = (uint)my_getpagesize();
  m_index = 0;
  m_owner = nullptr;
  m_null_pos = nullptr;
  m_data = nullptr;
  m_current_pos = m_data;
 }
 inline void Set_owner(Cu* owner) {
   m_owner = owner;
 }
 uchar* Allocate_one () {
  //TODO: use own memory pool.
  MEM_ROOT* mem_root = current_thd->mem_root;
  m_data = (uchar*) mem_root->Alloc(m_chunk_size);
  if (!m_data) return nullptr;
  memset(m_data, 0x0, m_chunk_size);
  m_current_pos = m_data;
  return m_data;
 }
 uint Deallocate() {
   return 0;
 }
 inline bool Is_full() { return m_current_pos == (m_data + m_chunk_size); }
 inline bool Is_free() { return m_current_pos == m_data; }
 uchar* Add(uchar* data, uint length) {
    memcpy(m_current_pos, data, length);
    m_current_pos += length;
    return m_current_pos;
 }
};

class Cu_header {
public:
  Cu_header() {}
  virtual ~Cu_header() {}
 //which field belongs to.
 Field* m_field;
 //field type of this cu.
 enum_field_types m_cu_type;
 //whether the is not null or not.
 bool m_nullable;

 //compression alg.
 Compress::enum_compress_algos m_compress_algo;
 Compress::Dictionary* m_local_dict;

 //statistics info.
 long double m_max_value, m_min_value, m_middle_value, m_median_value, m_avg_value;
 uint m_num_rows, m_num_nulls;
 uint m_num_chunks;
};

class Cu :public MemoryObject{
public:
 Cu();
 Cu(Field* field);
 virtual ~Cu();
 uint Insert(Field* field);
 uint Insert(uchar* data, uint length);
 uint Delete(uchar* data, uint length);
 uint Update(uchar* from, uchar* to);
 
private:
  uint Set_header(Field* field);
  Cu_chunk* Get_chunk(uint index);
  Cu_chunk* Allocate_chunk();
  uint Deallocate_chunks();
private:
  Cu_header* m_header;
  std::vector<Cu_chunk*> m_chunks;
};

} //ns:imcs
} //ns:shannonbase

#endif //__SHANNONBASE_CU_H__