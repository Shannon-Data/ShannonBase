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

#include "include/my_base.h"  //key_range
#include "my_dbug.h"          // DBUG_PRINT

namespace ShannonBase {
namespace Reader {
int ParquetReader::open() {
  DBUG_TRACE;

  arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>> file_result =
      arrow::io::ReadableFile::Open(m_path, arrow::default_memory_pool());

  if (!file_result.ok()) {
    DBUG_PRINT("error", ("Failed to open file: %s", file_result.status().ToString().c_str()));
    return -1;
  }
  m_file = file_result.ValueOrDie();

  arrow::Status status = parquet::arrow::OpenFile(m_file, arrow::default_memory_pool(), &m_parquet_reader);
  if (!status.ok()) {
    DBUG_PRINT("error", ("Failed to create parquet reader: %s", status.ToString().c_str()));
    return -1;
  }

  status = load_table();
  if (!status.ok()) {
    DBUG_PRINT("error", ("Failed to load table: %s", status.ToString().c_str()));
    return -1;
  }

  m_current_row = 0;
  DBUG_PRINT("info", ("Successfully opened parquet file: %s", m_path.c_str()));
  DBUG_PRINT("info", ("Total rows: %zu", m_total_rows));

  return 0;
}

int ParquetReader::close() {
  DBUG_TRACE;

  m_parquet_reader.reset();
  m_table.reset();
  m_schema.reset();
  m_file.reset();
  m_current_row = 0;
  m_total_rows = 0;

  DBUG_PRINT("info", ("Parquet file closed successfully"));
  return 0;
}

int ParquetReader::read(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  DBUG_TRACE;

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

  DBUG_PRINT("error", ("Failed to convert row to buffer: %s", status.ToString().c_str()));
  return -1;
}

int ParquetReader::write(Secondary_engine_execution_context *context, uchar *buffer, size_t length) {
  DBUG_PRINT("error", ("Write operation not supported for Parquet files"));
  return -1;
}

int ParquetReader::records_in_range(Secondary_engine_execution_context *context, unsigned int index, key_range *min,
                                    key_range *max) {
  DBUG_TRACE;
  return static_cast<int>(m_total_rows);
}

int ParquetReader::index_read(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                              ha_rkey_function find_flag) {
  DBUG_TRACE;
  m_current_row = 0;
  return read(context, buff, 0);
}

int ParquetReader::index_next(Secondary_engine_execution_context *context, uchar *buff, size_t length) {
  DBUG_TRACE;
  return read(context, buff, length);
}

int ParquetReader::index_next_same(Secondary_engine_execution_context *context, uchar *buff, uchar *key, uint key_len,
                                   ha_rkey_function find_flag) {
  DBUG_TRACE;
  return read(context, buff, 0);
}

int ParquetReader::index_general(Secondary_engine_execution_context *context, uchar *buff, size_t length) {
  DBUG_TRACE;
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
  DBUG_TRACE;

  arrow::Result<std::shared_ptr<arrow::Table>> table_result = m_parquet_reader->ReadTable();
  if (!table_result.ok()) {
    return table_result.status();
  }

  m_table = table_result.ValueOrDie();
  m_schema = m_table->schema();
  m_total_rows = m_table->num_rows();

  DBUG_PRINT("info", ("Loaded table with %d columns and %zu rows", m_table->num_columns(), m_total_rows));

  for (int i = 0; i < m_schema->num_fields(); i++) {
    auto field = m_schema->field(i);
    DBUG_PRINT("info", ("Column %d: %s (type: %s)", i, field->name().c_str(), field->type()->ToString().c_str()));
  }

  return arrow::Status::OK();
}

arrow::Status ParquetReader::convert_row_to_buffer(size_t row_index, uchar *buffer, size_t buffer_length) {
  DBUG_TRACE;

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