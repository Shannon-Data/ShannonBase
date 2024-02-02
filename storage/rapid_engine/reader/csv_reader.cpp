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
#include "storage/rapid_engine/reader/csv_reader.h"

#include <string>

#include "include/ut0dbg.h"
#include "include/my_base.h" //key_range

namespace ShannonBase {

int CSVReader::open() {
  return 0;
}
int CSVReader::close() {
  return 0;
}
int CSVReader::read(ShannonBaseContext* context, uchar* buffer, size_t length) {
  return 0;
}
int CSVReader::records_in_range(ShannonBaseContext*, unsigned int index, key_range *, key_range *) {
  return 0;
}
int CSVReader::index_read(ShannonBaseContext* context,  uchar*buffer, size_t length) {
  return 0;
}
int CSVReader::write(ShannonBaseContext* context, uchar*buffer, size_t length) {
  return 0;
}
uchar* CSVReader::tell() {
  return nullptr;
}
uchar* CSVReader::seek(uchar* pos) {
  return nullptr;
}
uchar* CSVReader::seek(size_t offset) {
  return nullptr;
}

} //ns:shannonbase