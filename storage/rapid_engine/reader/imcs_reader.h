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

#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/reader/reader.h"

class TABLE;
class Field;
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
  int read(ShannonBaseContext* context, uchar* buffer, size_t length =0);
  inline Imcs::Cu* get_source() {return m_source_cu;}
private:
  std::string m_key_name;
  TABLE* m_source_table{nullptr};
  Field* m_source_field {nullptr};
  std::atomic<uint> m_current_chunk_id {0};
  std::atomic<uchar*> m_current_pos {nullptr};
  Imcs::Cu* m_source_cu{nullptr};
};
class ImcsReader : public Reader {
public:
  ImcsReader(TABLE* table);
  ImcsReader() = delete;
  virtual ~ImcsReader() {}
  int open() override;
  int close() override;
  int read(ShannonBaseContext* context, uchar* buffer, size_t length = 0) override;
  int write(ShannonBaseContext* context, uchar*buffer, size_t lenght = 0) override;
  uchar* tell() override;
  uchar* seek(uchar* pos) override;
  uchar* seek(size_t offset) override;
private:
   //viewer of cus.
  std::map< std::string, std::unique_ptr<CuView>> m_cu_views;
  //source table.
  TABLE* m_source_table;
  //source name info.
  std::string m_db_name, m_table_name;
  ha_rows m_rows_read;
  bool m_start_of_scan;
};

} //ns:shannonbase
#endif //__SHANNONBASE_IM_READER_H__