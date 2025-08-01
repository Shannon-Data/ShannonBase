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

#include "include/my_base.h"  //key_range
#include "include/ut0dbg.h"

namespace ShannonBase {
namespace Reader {
// CSVReader implementation
CSVReader::CSVReader(const std::string &path)
    : m_path(path), m_csv_fd(nullptr), m_current_pos(0), m_total_records(0), m_is_opened(false), m_eof_reached(false) {}

int CSVReader::open() {
  if (m_is_opened) {
    return SHANNON_SUCCESS;
  }

  m_csv_fd = fopen(m_path.c_str(), "r");
  if (!m_csv_fd) {
    return HA_ERR_GENERIC;
  }

  if (!readHeader()) {
    fclose(m_csv_fd);
    m_csv_fd = nullptr;
    return HA_ERR_GENERIC;
  }

  countRecords();
  fseek(m_csv_fd, 0, SEEK_SET);
  skipHeader();

  m_current_pos = 0;
  m_is_opened = true;
  m_eof_reached = false;

  return ShannonBase::SHANNON_SUCCESS;
}

int CSVReader::close() {
  if (!m_is_opened) {
    return ShannonBase::SHANNON_SUCCESS;
  }

  if (m_csv_fd) {
    fclose(m_csv_fd);
    m_csv_fd = nullptr;
  }

  m_headers.clear();
  m_current_record.clear();
  m_current_pos = 0;
  m_total_records = 0;
  m_is_opened = false;
  m_eof_reached = false;

  return ShannonBase::SHANNON_SUCCESS;
}

int CSVReader::read(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  if (!m_is_opened || !m_csv_fd || m_eof_reached) {
    return HA_ERR_END_OF_FILE;
  }

  char line[8192];
  if (!fgets(line, sizeof(line), m_csv_fd)) {
    m_eof_reached = true;
    return HA_ERR_END_OF_FILE;
  }

  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n') {
    line[len - 1] = '\0';
  }

  std::string lineStr(line);
  m_current_record = CSVParser::parseLine(lineStr);

  if (buffer && length > 0) {
    serializeRecord(buffer, length);
  }

  m_current_pos++;
  return ShannonBase::SHANNON_SUCCESS;
}

int CSVReader::write(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  return HA_ERR_GENERIC;
}

int CSVReader::records_in_range(Secondary_engine_execution_context *context, unsigned int index, key_range *min_key,
                                key_range *max_key) {
  return static_cast<int>(m_total_records);
}

int CSVReader::index_read(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                          ha_rkey_function find_flag) {
  return linearSearch(context, buffer, key, key_len, find_flag);
}

int CSVReader::index_general(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  return read(context, buffer, length);
}

int CSVReader::index_next(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  return read(context, buffer, length);
}

int CSVReader::index_next_same(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                               ha_rkey_function find_flag) {
  return findNextSame(context, buffer, key, key_len, find_flag);
}

uchar *CSVReader::tell(uint field_index) {
  if (field_index >= m_current_record.size()) {
    return nullptr;
  }
  return reinterpret_cast<uchar *>(const_cast<char *>(m_current_record[field_index].c_str()));
}

uchar *CSVReader::seek(size_t offset) {
  if (!m_is_opened || !m_csv_fd) {
    return nullptr;
  }

  fseek(m_csv_fd, 0, SEEK_SET);
  skipHeader();

  for (size_t i = 0; i < offset && !feof(m_csv_fd); ++i) {
    char line[8192];
    if (!fgets(line, sizeof(line), m_csv_fd)) {
      break;
    }
  }

  m_current_pos = offset;
  m_eof_reached = feof(m_csv_fd);

  return m_csv_fd ? reinterpret_cast<uchar *>(m_csv_fd) : nullptr;
}

size_t CSVReader::getCurrentPosition() const { return m_current_pos; }

size_t CSVReader::getTotalRecords() const { return m_total_records; }

const std::vector<std::string> &CSVReader::getHeaders() const { return m_headers; }

const std::vector<std::string> &CSVReader::getCurrentRecord() const { return m_current_record; }

bool CSVReader::isEOF() const { return m_eof_reached; }

bool CSVReader::readHeader() {
  if (!m_csv_fd) return false;

  char line[8192];
  if (!fgets(line, sizeof(line), m_csv_fd)) {
    return false;
  }

  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n') {
    line[len - 1] = '\0';
  }

  std::string headerLine(line);
  m_headers = CSVParser::parseLine(headerLine);

  for (auto &header : m_headers) {
    header = CSVParser::trim(header);
  }

  return !m_headers.empty();
}

void CSVReader::skipHeader() {
  if (!m_csv_fd) return;

  char line[8192];
  auto ret [[maybe_unused]] = fgets(line, sizeof(line), m_csv_fd);
}

void CSVReader::countRecords() {
  if (!m_csv_fd) return;

  long current_pos = ftell(m_csv_fd);
  fseek(m_csv_fd, 0, SEEK_SET);
  skipHeader();

  m_total_records = 0;
  char line[8192];
  while (fgets(line, sizeof(line), m_csv_fd)) {
    m_total_records++;
  }

  fseek(m_csv_fd, current_pos, SEEK_SET);
}

void CSVReader::serializeRecord(uchar *buffer, size_t length) {
  if (!buffer || length == 0) return;

  size_t offset = 0;
  for (size_t i = 0; i < m_current_record.size() && offset < length; ++i) {
    const std::string &field = m_current_record[i];
    size_t field_len = std::min(field.length(), length - offset);

    std::memcpy(buffer + offset, field.c_str(), field_len);
    offset += field_len;

    if (offset < length) {
      buffer[offset++] = '\0';
    }
  }
}

int CSVReader::linearSearch(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                            ha_rkey_function find_flag) {
  if (!m_is_opened || !m_csv_fd) {
    return -1;
  }

  fseek(m_csv_fd, 0, SEEK_SET);
  skipHeader();

  char line[8192];
  std::string search_key(reinterpret_cast<char *>(key), key_len);

  while (fgets(line, sizeof(line), m_csv_fd)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    std::string lineStr(line);
    std::vector<std::string> record = CSVParser::parseLine(lineStr);

    if (!record.empty() && record[0] == search_key) {
      m_current_record = record;
      if (buffer) {
        serializeRecord(buffer, key_len);
      }
      return ShannonBase::SHANNON_SUCCESS;
    }
  }

  return HA_ERR_GENERIC;
}

int CSVReader::findNextSame(Secondary_engine_execution_context *context, uchar *buffer, uchar *key, uint key_len,
                            ha_rkey_function find_flag) {
  if (!m_is_opened || !m_csv_fd) {
    return HA_ERR_GENERIC;
  }

  std::string search_key(reinterpret_cast<char *>(key), key_len);
  char line[8192];

  while (fgets(line, sizeof(line), m_csv_fd)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    std::string lineStr(line);
    std::vector<std::string> record = CSVParser::parseLine(lineStr);

    if (!record.empty() && record[0] == search_key) {
      m_current_record = record;
      if (buffer) {
        serializeRecord(buffer, key_len);
      }
      m_current_pos++;
      return ShannonBase::SHANNON_SUCCESS;
    }
  }

  m_eof_reached = true;
  return HA_ERR_END_OF_FILE;
}

}  // namespace Reader
}  // namespace ShannonBase