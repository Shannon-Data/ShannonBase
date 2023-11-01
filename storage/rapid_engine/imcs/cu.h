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

#include "storage/rapid_engine/imcs/chunk.h" //chunk
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"


namespace ShannonBase{
namespace Imcs{

/**A Snapshot Metadata Unit (SMU) contains metadata and 
 * transactional information for an associated IMCU.*/
class Snapshot_meta_unit {

};

class Cu :public MemoryObject{
public:
 class Cu_header {
  public:
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
  std::atomic<long long> m_max_value, m_min_value, m_middle_value, m_median_value, m_avg_value;
  std::atomic<uint> m_num_chunks;
 };

 Cu(Field* field);
 virtual ~Cu();
 Cu_header& Get_header() { return m_headers;}

 inline void Set_next(Cu* next) { m_next = next; }
 inline void Set_prev(Cu* prev) { m_prev = prev; }
 inline Cu* Get_next() { return m_next; }
 inline Cu* Get_prev () {return m_prev; }
 bool Is_full() {return (m_headers.m_num_chunks == Cu::MAX_CHUNK_NUM_IN_CU); }
 uint Write ();
private:
  inline bool Is_supported_data_type(enum_field_types type) {
    if (type == enum_field_types::MYSQL_TYPE_DECIMAL ||
      type == enum_field_types::MYSQL_TYPE_DOUBLE  ||
      type == enum_field_types::MYSQL_TYPE_FLOAT ||
      type == enum_field_types::MYSQL_TYPE_LONGLONG  ||
      type == enum_field_types::MYSQL_TYPE_SHORT ||
      type == enum_field_types::MYSQL_TYPE_STRING)
    return true;

    return false;    
  }
private:
  std::mutex m_header_mutex;
  //header info of this Cu.
  Cu_header m_headers;
  //the star chunk pos of this chunk.
  Chunk* m_chunks {nullptr};
  //the next cu object.
  Cu* m_next {nullptr}, *m_prev{nullptr};
  //hash map to store all chunks header in this cu to accelerate find.
  //getting the chunk by chunk_id or something else.
  //std::std::unordered_map<std::string, Cu_header*> m_chunks_map;
  uint m_version_num, m_magic_num;
  static constexpr uint MAX_CHUNK_NUM_IN_CU = 2048;
};

} //ns:imcs
} //ns:shannonbase

#endif //__SHANNONBASE_CU_H__