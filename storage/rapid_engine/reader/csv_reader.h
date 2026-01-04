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

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/reader/reader.h"

class key_range;
namespace ShannonBase {
namespace Reader {
// CSV parsing helper class
class CSVParser {
 public:
  static std::vector<std::string> parseLine(const std::string &line, char delimiter = ',') {
    std::vector<std::string> fields;
    std::string field;
    bool inQuotes = false;

    for (size_t i = 0; i < line.length(); ++i) {
      char c = line[i];

      if (c == '"') {
        inQuotes = !inQuotes;
      } else if (c == delimiter && !inQuotes) {
        fields.push_back(field);
        field.clear();
      } else {
        field += c;
      }
    }
    fields.push_back(field);
    return fields;
  }

  static std::string trim(const std::string &str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
  }
};

class CSVReader {
 public:
  CSVReader(const std::string &path);
  ~CSVReader();

  // Delete copy and move operations
  CSVReader(const CSVReader &) = delete;
  CSVReader(CSVReader &&) = delete;
  CSVReader &operator=(const CSVReader &) = delete;
  CSVReader &operator=(CSVReader &&) = delete;

  // Interface methods
  int open();
  int close();
  int read(Secondary_engine_execution_context *context, uchar *buffer, size_t length);
  int write(Secondary_engine_execution_context *context, uchar *buffer, size_t length);
  int records_in_range(Secondary_engine_execution_context *context, unsigned int index, key_range *min_key,
                       key_range *max_key);
  int index_read(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                 ha_rkey_function find_flag);
  int index_general(Secondary_engine_execution_context *context, uchar *buffer, size_t length);
  int index_next(Secondary_engine_execution_context *context, uchar *buffer, size_t length);
  int index_next_same(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                      ha_rkey_function find_flag);
  uchar *tell(uint field_index);
  uchar *seek(size_t offset);

  // Helper methods
  size_t getCurrentPosition() const;
  size_t getTotalRecords() const;
  const std::vector<std::string> &getHeaders() const;
  const std::vector<std::string> &getCurrentRecord() const;
  bool isEOF() const;

 private:
  std::string m_path;
  FILE *m_csv_fd;
  size_t m_current_pos;
  size_t m_total_records;
  bool m_is_opened;
  bool m_eof_reached;
  std::vector<std::string> m_headers;
  std::vector<std::string> m_current_record;

  // Private helper methods
  bool readHeader();
  void skipHeader();
  void countRecords();
  void serializeRecord(uchar *buffer, size_t length);
  int linearSearch(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                   ha_rkey_function find_flag);
  int findNextSame(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                   ha_rkey_function find_flag);
};

}  // namespace Reader
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CSV_READER_H__