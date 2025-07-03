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
#ifndef __SHANNONBASE_ITERATOR_H__
#define __SHANNONBASE_ITERATOR_H__

#include <atomic>
#include <functional>
#include "include/my_inttypes.h"

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/row_iterator.h"

#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/utils.h"

class Field;
namespace ShannonBase {
namespace Executor {

using filter_func_t = std::function<bool(const uchar *)>;

class ColumnChunk {
 public:
  // ctor
  ColumnChunk(Field *mysql_fld, size_t size);

  // Destructor (default is fine since we use smart pointers)
  ~ColumnChunk() = default;

  // Copy ctor operator
  ColumnChunk(const ColumnChunk &other);

  // Copy assignment operator
  ColumnChunk &operator=(const ColumnChunk &other);

  // Move constructor
  ColumnChunk(ColumnChunk &&other) noexcept;

  // Move assignment operator
  ColumnChunk &operator=(ColumnChunk &&other) noexcept;

  // set the indexth is null.
  inline void set_null(size_t index) {
    assert(m_null_mask.get());
    ShannonBase::Utils::Util::bit_array_set(m_null_mask.get(), index);
  }

  // to tell indexth is null or not.
  inline bool nullable(size_t index) {
    assert(m_null_mask.get());
    return ShannonBase::Utils::Util::bit_array_get(m_null_mask.get(), index);
  }

  inline void add(uchar *data, size_t length, bool null) {
    if (null) {
      ShannonBase::Utils::Util::bit_array_set(m_null_mask.get(), m_current_size);
    } else {
      auto len = (length > m_field_width) ? m_field_width : length;
      memcpy(m_cols_buffer.get() + (m_current_size * m_field_width), data, len);
    }
    m_current_size.fetch_add(1);
  }
  inline uchar *data(size_t index = 0) const { return m_cols_buffer.get() + (index * m_field_width); }

  inline bool empty() const { return (m_current_size == 0); }

  inline bool full() const { return (m_current_size == m_chunk_size); }

  inline size_t size() const { return m_current_size; }

  inline size_t width() const { return m_field_width; }

 private:
  // source field.
  Field *m_source_fld;

  // data type in mysql.
  enum_field_types m_type{MYSQL_TYPE_NULL};

  size_t m_field_width{0};

  // current rows number of this chunk.
  std::atomic<size_t> m_current_size{0};

  // VECTOR_WIDTH
  size_t m_chunk_size{0};

  // to keep the every column data in VECTOR_WIDTH count. the order is same with field order.
  std::unique_ptr<uchar[]> m_cols_buffer{nullptr};

  // null bitmap of all data in this Column Chunk.
  std::unique_ptr<ShannonBase::bit_array_t> m_null_mask{nullptr};
};

class Iterator : public RowIterator {};

}  // namespace Executor
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ITERATOR_H__