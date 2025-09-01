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

ColumnChunk::ColumnChunk(Field *mysql_fld, size_t chunk_size)
    : m_source_fld(mysql_fld), m_current_size(0), m_chunk_size(chunk_size) {
  if (mysql_fld) {
    m_type = mysql_fld->type();
    m_field_width = Utils::Util::normalized_length(mysql_fld);
  }

  initialize_buffers();
}

ColumnChunk::ColumnChunk(const ColumnChunk &other)
    : m_source_fld(nullptr), m_type(MYSQL_TYPE_NULL), m_field_width(0), m_current_size(0), m_chunk_size(0) {
  copy_from(other);
}

// Copy assignment operator
ColumnChunk &ColumnChunk::operator=(const ColumnChunk &other) {
  if (this != &other) {
    ColumnChunk temp(other);
    swap(temp);
  }

  return *this;
}

ColumnChunk::ColumnChunk(ColumnChunk &&other) noexcept
    : m_source_fld(other.m_source_fld),
      m_type(other.m_type),
      m_field_width(other.m_field_width),
      m_current_size(other.m_current_size.load(std::memory_order_relaxed)),
      m_chunk_size(other.m_chunk_size),
      m_cols_buffer(std::move(other.m_cols_buffer)),
      m_null_mask(std::move(other.m_null_mask)) {
  other.m_source_fld = nullptr;
  other.m_type = MYSQL_TYPE_NULL;
  other.m_field_width = 0;
  other.m_current_size.store(0, std::memory_order_relaxed);
  other.m_chunk_size = 0;
}

ColumnChunk &ColumnChunk::operator=(ColumnChunk &&other) noexcept {
  if (this != &other) {
    ColumnChunk temp(std::move(other));
    swap(temp);
  }

  return *this;
}

void ColumnChunk::swap(ColumnChunk &other) {
  std::swap(m_source_fld, other.m_source_fld);
  std::swap(m_type, other.m_type);
  std::swap(m_field_width, other.m_field_width);

  auto temp_current = m_current_size.load(std::memory_order_relaxed);
  m_current_size.store(other.m_current_size.load(std::memory_order_relaxed), std::memory_order_relaxed);
  other.m_current_size.store(temp_current, std::memory_order_relaxed);

  std::swap(m_chunk_size, other.m_chunk_size);
  std::swap(m_cols_buffer, other.m_cols_buffer);
  std::swap(m_null_mask, other.m_null_mask);
}

void ColumnChunk::initialize_buffers() {
  if (m_chunk_size == 0 || m_field_width == 0) {
    return;
  }

  size_t buffer_size = m_chunk_size * m_field_width;
  m_cols_buffer = std::make_unique<uchar[]>(buffer_size);
  std::memset(m_cols_buffer.get(), 0, buffer_size);

  size_t null_mask_size = (m_chunk_size + 7) / 8;
  m_null_mask = std::make_unique<ShannonBase::bit_array_t>(null_mask_size);
}

void ColumnChunk::copy_from(const ColumnChunk &other) {
  m_source_fld = other.m_source_fld;
  m_type = other.m_type;
  m_field_width = other.m_field_width;
  m_chunk_size = other.m_chunk_size;
  m_current_size.store(other.m_current_size.load(std::memory_order_relaxed), std::memory_order_relaxed);

  initialize_buffers();

  if (other.m_cols_buffer && m_cols_buffer) {
    size_t data_size = m_chunk_size * m_field_width;
    std::memcpy(m_cols_buffer.get(), other.m_cols_buffer.get(), data_size);
  }

  if (other.m_null_mask && m_null_mask && other.m_null_mask->data && m_null_mask->data) {
    size_t null_mask_size = (m_chunk_size + 7) / 8;
    std::memcpy(m_null_mask->data, other.m_null_mask->data, null_mask_size);
  }
}

bool ColumnChunk::add(uchar *data, size_t length, bool null) {
  if (!data && !null) {
    return false;
  }

  size_t current_idx = m_current_size.load(std::memory_order_relaxed);
  if (current_idx >= m_chunk_size) {
    return false;
  }

  size_t actual_idx = m_current_size.fetch_add(1, std::memory_order_acq_rel);
  if (actual_idx >= m_chunk_size) {
    m_current_size.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }

  if (null) {
    set_null(actual_idx);
  } else {
    const size_t copy_len = std::min(length, m_field_width);
    uchar *dest = m_cols_buffer.get() + (actual_idx * m_field_width);

    if (copy_len > 0) {
      std::memcpy(dest, data, copy_len);
    }

    if (copy_len < m_field_width) {
      std::memset(dest + copy_len, 0, m_field_width - copy_len);
    }
  }

  return true;
}

bool ColumnChunk::add_batch(const std::vector<std::pair<const uchar *, size_t>> &data_batch,
                            const std::vector<bool> &null_flags) {
  if (data_batch.size() != null_flags.size()) {
    return false;
  }

  size_t batch_size = data_batch.size();
  if (batch_size == 0) {
    return true;
  }

  size_t current_idx = m_current_size.load(std::memory_order_relaxed);
  if (current_idx + batch_size > m_chunk_size) {
    return false;
  }

  size_t start_idx = m_current_size.fetch_add(batch_size, std::memory_order_acq_rel);
  if (start_idx + batch_size > m_chunk_size) {
    m_current_size.fetch_sub(batch_size, std::memory_order_acq_rel);
    return false;
  }

  for (size_t i = 0; i < batch_size; ++i) {
    size_t idx = start_idx + i;

    if (null_flags[i]) {
      set_null(idx);
    } else {
      const uchar *src_data = data_batch[i].first;
      size_t src_length = data_batch[i].second;

      if (!src_data) {
        m_current_size.store(start_idx, std::memory_order_release);
        return false;
      }

      const size_t copy_len = std::min(src_length, m_field_width);
      uchar *dest = m_cols_buffer.get() + (idx * m_field_width);

      if (copy_len > 0) {
        std::memcpy(dest, src_data, copy_len);
      }
      if (copy_len < m_field_width) {
        std::memset(dest + copy_len, 0, m_field_width - copy_len);
      }
    }
  }

  return true;
}

size_t ColumnChunk::compact() {
  if (!m_cols_buffer || !m_null_mask) {
    return 0;
  }

  size_t current_size = m_current_size.load(std::memory_order_relaxed);
  size_t write_idx = 0;

  for (size_t read_idx = 0; read_idx < current_size; ++read_idx) {
    if (!nullable(read_idx)) {
      if (write_idx != read_idx) {
        std::memcpy(mutable_data(write_idx), data(read_idx), m_field_width);
      }
      ++write_idx;
    }
  }

  m_current_size.store(write_idx, std::memory_order_release);

  if (m_null_mask && m_null_mask->data) {
    const size_t null_mask_size = (m_chunk_size + 7) / 8;
    std::memset(m_null_mask->data, 0, null_mask_size);
  }

  return write_idx;
}

}  // namespace Executor
}  // namespace ShannonBase
