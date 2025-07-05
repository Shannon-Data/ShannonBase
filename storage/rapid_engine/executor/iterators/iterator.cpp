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
/** The basic iterator class for IMCS. All specific iterators are all inherited
 * from this.
 */
#include "storage/rapid_engine/executor/iterators/iterator.h"

#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/table.h"  // TABLE

namespace ShannonBase {
namespace Executor {

ColumnChunk::ColumnChunk(Field *mysql_fld, size_t size)
    : m_source_fld(mysql_fld), m_type(mysql_fld->type()), m_field_width(mysql_fld->pack_length()), m_chunk_size(size) {
  m_cols_buffer.reset(new (std::nothrow) uchar[m_chunk_size * m_field_width]);
  m_null_mask.reset(new (std::nothrow) ShannonBase::bit_array_t(m_chunk_size));
}

ColumnChunk::ColumnChunk(const ColumnChunk &other) {
  this->m_chunk_size = other.m_chunk_size;
  this->m_current_size.store(other.m_current_size);
  this->m_field_width = other.m_field_width;
  this->m_type = other.m_type;

  m_cols_buffer.reset(new (std::nothrow) uchar[m_chunk_size * m_field_width]);
  if (m_cols_buffer && other.m_cols_buffer) {
    std::memcpy(m_cols_buffer.get(), other.m_cols_buffer.get(), m_chunk_size * m_field_width);
  }

  if (other.m_null_mask) {
    m_null_mask.reset(new ShannonBase::bit_array_t(*other.m_null_mask));
  } else {
    m_null_mask = nullptr;
  }
}
// Copy assignment operator
ColumnChunk &ColumnChunk::operator=(const ColumnChunk &other) {
  if (this == &other)  // smame one.
    return *this;
  this->m_chunk_size = other.m_chunk_size;
  this->m_current_size.store(other.m_current_size);
  this->m_field_width = other.m_field_width;
  this->m_type = other.m_type;

  m_cols_buffer.reset(new (std::nothrow) uchar[m_chunk_size * m_field_width]);
  if (m_cols_buffer && other.m_cols_buffer) {
    std::memcpy(m_cols_buffer.get(), other.m_cols_buffer.get(), m_chunk_size * m_field_width);
  }

  if (other.m_null_mask) {
    this->m_null_mask.reset(new (std::nothrow) ShannonBase::bit_array_t(m_chunk_size));
  } else {
    m_null_mask = nullptr;
  }

  return *this;
}

ColumnChunk::ColumnChunk(ColumnChunk &&other) noexcept
    : m_type(other.m_type),
      m_field_width(other.m_field_width),
      m_chunk_size(other.m_chunk_size),
      m_cols_buffer(std::move(other.m_cols_buffer)),
      m_null_mask(std::move(other.m_null_mask)) {
  m_current_size.store(other.m_current_size);
  other.m_chunk_size = 0;
  other.m_current_size = 0;
  other.m_field_width = 0;
  other.m_type = MYSQL_TYPE_NULL;
}

ColumnChunk &ColumnChunk::operator=(ColumnChunk &&other) noexcept {
  if (this == &other) return *this;

  m_type = other.m_type;
  m_field_width = other.m_field_width;
  m_current_size.store(other.m_current_size);
  m_chunk_size = other.m_chunk_size;

  m_cols_buffer = std::move(other.m_cols_buffer);
  m_null_mask = std::move(other.m_null_mask);

  other.m_chunk_size = 0;
  other.m_current_size = 0;
  other.m_field_width = 0;
  other.m_type = MYSQL_TYPE_NULL;

  return *this;
}

}  // namespace Executor
}  // namespace ShannonBase
