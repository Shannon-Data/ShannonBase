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
#include "storage/rapid_engine/reader/parquet_reader.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>
#include <cstring>
#include <iostream>

#include "include/my_base.h"  //key_range

namespace ShannonBase {
namespace Reader {
int ParquetReader::open() {
  arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>> file_result =
      arrow::io::ReadableFile::Open(m_path, arrow::default_memory_pool());

  if (!file_result.ok()) {
    std::cerr << "Failed to open file: " << file_result.status().ToString() << std::endl;
    return -1;
  }
  m_file = file_result.ValueOrDie();

  arrow::Status status = parquet::arrow::OpenFile(m_file, arrow::default_memory_pool(), &m_parquet_reader);
  if (!status.ok()) {
    std::cerr << "Failed to create parquet reader: " << status.ToString() << std::endl;
    return -1;
  }

  status = load_table();
  if (!status.ok()) {
    std::cerr << "Failed to load table: " << status.ToString() << std::endl;
    return -1;
  }

  m_current_row = 0;
  std::cout << "Successfully opened parquet file: " << m_path << std::endl;
  std::cout << "Total rows: " << m_total_rows << std::endl;

  return 0;
}

int ParquetReader::close() {
  try {
    m_parquet_reader.reset();
    m_table.reset();
    m_schema.reset();
    m_file.reset();
    m_current_row = 0;
    m_total_rows = 0;

    std::cout << "Parquet file closed successfully" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Exception in close(): " << e.what() << std::endl;
    return -1;
  }
}

int ParquetReader::read(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  if (!m_table || !buffer) {
    return -1;
  }

  if (m_current_row >= m_total_rows) {
    return 0;  // EOF
  }

  arrow::Status status = convert_row_to_buffer(m_current_row, buffer, length);
  if (status.ok()) {
    m_current_row++;
    return 1;
  }

  std::cerr << "Failed to convert row to buffer: " << status.ToString() << std::endl;
  return -1;
}

int ParquetReader::write(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  std::cerr << "Write operation not supported for Parquet files" << std::endl;
  return -1;
}

int ParquetReader::records_in_range(Secondary_engine_execution_context *context, unsigned int index, key_range *min,
                                    key_range *max) {
  return static_cast<int>(m_total_rows);
}

int ParquetReader::index_read(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                              ha_rkey_function find_flag) {
  m_current_row = 0;
  return read(context, buff, 0);
}

int ParquetReader::index_next(Secondary_engine_execution_context *context, uchar *buff, size_t length) {
  return read(context, buff, length);
}

int ParquetReader::index_next_same(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                                   ha_rkey_function find_flag) {
  return read(context, buff, 0);
}

int ParquetReader::index_general(Secondary_engine_execution_context *context, uchar *buff, size_t length) {
  return read(context, buff, length);
}

uchar *ParquetReader::tell(uint field_index) {
  static uchar position_info[sizeof(size_t)];
  std::memcpy(position_info, &m_current_row, sizeof(size_t));
  return position_info;
}

uchar *ParquetReader::seek(size_t offset) {
  if (offset < m_total_rows) {
    m_current_row = offset;
    return tell(0);
  }
  return nullptr;
}

arrow::Status ParquetReader::load_table() {
  arrow::Result<std::shared_ptr<arrow::Table>> table_result = m_parquet_reader->ReadTable();
  if (!table_result.ok()) {
    return table_result.status();
  }

  m_table = table_result.ValueOrDie();
  m_schema = m_table->schema();
  m_total_rows = m_table->num_rows();

  std::cout << "Loaded table with " << m_table->num_columns() << " columns and " << m_total_rows << " rows"
            << std::endl;

  for (int i = 0; i < m_schema->num_fields(); i++) {
    auto field = m_schema->field(i);
    std::cout << "Column " << i << ": " << field->name() << " (type: " << field->type()->ToString() << ")" << std::endl;
  }

  return arrow::Status::OK();
}

arrow::Status ParquetReader::convert_row_to_buffer(size_t row_index, uchar *buffer, size_t buffer_length) {
  if (!is_valid_row_index(row_index) || !buffer) {
    return arrow::Status::Invalid("Invalid row index or buffer");
  }

  size_t buffer_offset = 0;

  for (int col_idx = 0; col_idx < m_table->num_columns(); col_idx++) {
    auto column = m_table->column(col_idx);

    size_t current_row = row_index;
    std::shared_ptr<arrow::Array> array;

    for (int chunk_idx = 0; chunk_idx < column->num_chunks(); chunk_idx++) {
      auto chunk = column->chunk(chunk_idx);
      if (current_row < static_cast<size_t>(chunk->length())) {
        array = chunk;
        break;
      }
      current_row -= chunk->length();
    }

    if (!array) {
      return arrow::Status::Invalid("Row not found in any chunk");
    }

    if (buffer_length > 0 && buffer_offset >= buffer_length) {
      return arrow::Status::Invalid("Buffer overflow");
    }

    switch (array->type_id()) {
      case arrow::Type::INT32: {
        auto typed_array = std::static_pointer_cast<arrow::Int32Array>(array);
        if (!typed_array->IsNull(current_row)) {
          int32_t value = typed_array->Value(current_row);
          std::memcpy(buffer + buffer_offset, &value, sizeof(int32_t));
          buffer_offset += sizeof(int32_t);
        }
        break;
      }
      case arrow::Type::INT64: {
        auto typed_array = std::static_pointer_cast<arrow::Int64Array>(array);
        if (!typed_array->IsNull(current_row)) {
          int64_t value = typed_array->Value(current_row);
          std::memcpy(buffer + buffer_offset, &value, sizeof(int64_t));
          buffer_offset += sizeof(int64_t);
        }
        break;
      }
      case arrow::Type::DOUBLE: {
        auto typed_array = std::static_pointer_cast<arrow::DoubleArray>(array);
        if (!typed_array->IsNull(current_row)) {
          double value = typed_array->Value(current_row);
          std::memcpy(buffer + buffer_offset, &value, sizeof(double));
          buffer_offset += sizeof(double);
        }
        break;
      }
      case arrow::Type::STRING: {
        auto typed_array = std::static_pointer_cast<arrow::StringArray>(array);
        if (!typed_array->IsNull(current_row)) {
          std::string value = typed_array->GetString(current_row);
          size_t str_len = value.length();
          std::memcpy(buffer + buffer_offset, &str_len, sizeof(size_t));
          buffer_offset += sizeof(size_t);
          std::memcpy(buffer + buffer_offset, value.c_str(), str_len);
          buffer_offset += str_len;
        }
        break;
      }
      default:
        return arrow::Status::NotImplemented("Unsupported data type: " + array->type()->ToString());
    }
  }

  return arrow::Status::OK();
}

size_t ParquetReader::get_row_size() const {
  size_t row_size = 0;

  if (!m_schema) {
    return 0;
  }

  for (int i = 0; i < m_schema->num_fields(); i++) {
    auto field = m_schema->field(i);
    auto type = field->type();

    switch (type->id()) {
      case arrow::Type::INT32:
        row_size += sizeof(int32_t);
        break;
      case arrow::Type::INT64:
        row_size += sizeof(int64_t);
        break;
      case arrow::Type::DOUBLE:
        row_size += sizeof(double);
        break;
      case arrow::Type::STRING:
        row_size += sizeof(size_t) + 256;
        break;
      default:
        row_size += 8;
        break;
    }
  }

  return row_size;
}

bool ParquetReader::is_valid_row_index(size_t row_index) const { return row_index < m_total_rows; }
}  // namespace Reader
}  // namespace ShannonBase