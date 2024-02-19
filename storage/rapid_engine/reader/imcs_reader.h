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
#ifndef __SHANNONBASE_IM_READER_H__
#define __SHANNONBASE_IM_READER_H__
#include <string>
#include <memory>
#include <atomic>

#include "include/my_inttypes.h"
#include "include/my_base.h" //HA_ERR_END_OF_FILE

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/reader/reader.h"

class TABLE;
class Field;
class key_range;
namespace ShannonBase {
namespace Imcs{
  class Imcs;
  class Cu;
}

class CuView  {
public:
  CuView() = default;
  CuView(TABLE* table, Field* field);
  virtual ~CuView() = default;
  int open();
  int close();
  int read(ShannonBaseContext* context, uchar* buffer, size_t length = 0);
  int read_index(ShannonBaseContext* context, uchar* key, size_t key_len, uchar* value,
                 ha_rkey_function find_flag);
  int records_in_range(ShannonBaseContext*, unsigned int , key_range *, key_range *);
  uchar* write(ShannonBaseContext* context, uchar*buffer, size_t length = 0);
  uchar* seek(size_t offset);
  inline Imcs::Cu* get_source() {return m_source_cu;}
private:
  std::string m_key_name;
  TABLE* m_source_table{nullptr};
  Field* m_source_field {nullptr};

  //full table scan info.
  //reader info, includes: current_chunkid, pos. read id
  std::atomic<uint> m_rnd_chunk_rid {0};
  //writer info, to which chunk be written. write id
  std::atomic<uint> m_rnd_chunk_wid {0};
  //where the data be written.
  std::atomic<uchar*> m_rnd_wpos {nullptr};
  //just like cursor. full table scan cursor.
  std::atomic<uchar*> m_rnd_rpos {nullptr};
  //source Cu of this cu view.
  Imcs::Cu* m_source_cu{nullptr};
};

class ImcsReader : public Reader {
public:
  ImcsReader(TABLE* table);
  ImcsReader() = delete;
  virtual ~ImcsReader() {}
  using CuViews_t = std::map<std::string, std::unique_ptr<CuView>>;

  int open() override;
  int close() override;
  //seqential read. full table scan
  int read(ShannonBaseContext*, uchar*, size_t = 0) override;
  //sequential write, full table scan.
  int write(ShannonBaseContext*, uchar*, size_t= 0) override;
  //get the nums of rows in range.
  int records_in_range(ShannonBaseContext*, unsigned int, key_range *, key_range *) override ;
  //read a row via index with a key.
  int index_read(ShannonBaseContext*, uchar*, uchar*, uint, ha_rkey_function) override;
  //read the rows without a key value, just like travel over index tree.
  int index_general(ShannonBaseContext*, uchar*, size_t = 0) override;
  int index_next(ShannonBaseContext*, uchar*, size_t = 0) override;

  uchar* tell(uint = 0) override;
  uchar* seek(size_t offset) override;
  bool is_open() const { return m_start_of_scan; }
private:
  int cond_comp(ShannonBaseContext*, uchar*, uchar*, uint, ha_rkey_function);
private:
  //local buffer.
  uchar m_buff[SHANNON_ROW_TOTAL_LEN] = {0};
  //viewer of cus.
  std::map<std::string, std::unique_ptr<CuView>> m_cu_views;
  //source table.
  TABLE* m_source_table{nullptr};
  //source name info.
  std::string m_db_name, m_table_name;
  //row nums has read.
  ha_rows m_rows_read {0};
  //whether start to read or not.
  bool m_start_of_scan {false};
  //stores the filed string
  String m_field_str;
};

} //ns:shannonbase
#endif //__SHANNONBASE_IM_READER_H__