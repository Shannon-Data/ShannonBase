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
#ifndef __SHANNONBASE_CSV_READER_H__
#define __SHANNONBASE_CSV_READER_H__
#include <string>
#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/reader/reader.h"

class key_range;
namespace ShannonBase {
class CSVReader : public Reader {
public:
  CSVReader(std::string& path) : m_path(path), m_csv_fd(nullptr){
  }
  CSVReader(CSVReader&) = delete;
  CSVReader(CSVReader&&) = delete;
  virtual ~CSVReader() = default;

  int open() override;
  int close() override;
  int read(ShannonBaseContext* context, uchar* buffer, size_t length = 0) override;
  int write(ShannonBaseContext* context, uchar*buffer, size_t lenght = 0) override;
  int records_in_range(ShannonBaseContext*, unsigned int index, key_range *, key_range *) override;
  uchar* tell() override;
  uchar* seek(uchar* pos) override;
  uchar* seek(size_t offset) override;
private:
  std::string m_path;
  FILE* m_csv_fd;
};

} //ns:shannonbase
#endif //__SHANNONBASE_CSV_READER_H__