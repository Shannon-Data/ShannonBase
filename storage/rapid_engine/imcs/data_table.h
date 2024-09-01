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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_DATA_TABLE_H__
#define __SHANNONBASE_DATA_TABLE_H__

#include <atomic>
#include <vector>

#include "storage/rapid_engine/include/rapid_object.h"

class TABLE;
namespace ShannonBase {
namespace Imcs {
class Imcs;
class Cu;
class DataTable : public MemoryObject {
 public:
  DataTable(TABLE *source_table);
  virtual ~DataTable();

  // open a cursor on db_table to read/write.
  int open();

  // close a cursor.
  int close();

  // intitialize this data table object.
  int init();

  // to the next rows.
  int next(uchar *buf);

  // end of scan.
  int end();

  // get the data pos.
  row_id_t find(uchar *buf);

 private:
  void scan_init();

  std::atomic<bool> m_initialized{false};

  // the data source, an IMCS.
  TABLE *m_data_source{nullptr};

  // all Cu ptr of all feilds.
  std::vector<Cu *> m_field_cus;

  // start from where.
  std::atomic<row_id_t> m_rowid{0};
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_DATA_TABLE_H__