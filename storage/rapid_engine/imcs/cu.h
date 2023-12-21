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
#include <memory>

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
  std::unique_ptr<Compress::Dictionary> m_local_dict;

  //statistics info.
  std::atomic<long long> m_max, m_min, m_middle, m_median;
  std::atomic<long long> m_rows, m_sum, m_avg;
 };

 explicit Cu(Field* field);
 virtual ~Cu();

 //writes the data into this chunk. length unspecify means calc by chunk. 
 uchar* Write_data(RapidContext* context, uchar* data, uint length = 0);
 //reads the data by from address .
 uchar* Read_data(RapidContext* context, uchar* from, uchar* to, uint length = 0);
 //reads the data by rowid.
 uchar* Read_data(RapidContext* context, uchar* rowid, uint length = 0);
 //deletes the data by rowid
 uchar* Delete_data(RapidContext* context, uchar* rowid);
 //deletes all
 uchar* Delete_all();
 //updates the data with rowid with the new data.
 uchar* Update_data(RapidContext* context, uchar* rowid, uchar* data, uint length = 0);
 //flush the data to disk. by now, we cannot impl this part.
 uint flush(RapidContext* context, uchar* from = nullptr, uchar* to = nullptr);
private:
  uint m_magic{SHANNON_MAGIC_CU};
  //proctect header.
  std::mutex m_header_mutex;
  //header info of this Cu.
  std::unique_ptr<Cu_header> m_header;
  //chunks in this cu.
  std::vector<std::unique_ptr<Chunk> > m_chunks;
  //the next and prev cu object.
  std::unique_ptr<Cu> m_next {nullptr};
  std::unique_ptr<Cu> m_prev{nullptr};
};

} //ns:imcs
} //ns:shannonbase

#endif //__SHANNONBASE_CU_H__