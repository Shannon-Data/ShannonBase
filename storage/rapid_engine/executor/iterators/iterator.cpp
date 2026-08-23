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

   The fundmental code for imcs. It's based on mysql executor iterators.
*/
/** The basic iterator class for IMCS. All specific iterators are all inherited
 * from this.
 */
#include "storage/rapid_engine/executor/iterators/iterator.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"
#include "sql/table.h"  // TABLE

namespace ShannonBase {
namespace Executor {

ColumnChunk::ColumnChunk(Field *mysql_fld, size_t chunk_size)
    : m_source_fld(mysql_fld), m_current_size(0), m_chunk_size(chunk_size) {
  if (mysql_fld) {
    m_type = mysql_fld->type();
    m_field_width = Utils::Util::normalized_length(mysql_fld);
    m_field_index = static_cast<uint16_t>(mysql_fld->field_index());
    m_table = mysql_fld->table;
  }

  initialize_buffers();
}

ColumnChunk::ColumnChunk(const ColumnChunk &other)
    : m_source_fld(nullptr),
      m_field_index(0),
      m_table(nullptr),
      m_type(MYSQL_TYPE_NULL),
      m_field_width(0),
      m_current_size(0),
      m_chunk_size(0) {
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
      m_field_index(other.m_field_index),
      m_table(other.m_table),
      m_type(other.m_type),
      m_field_width(other.m_field_width),
      m_current_size(other.m_current_size),
      m_chunk_size(other.m_chunk_size),
      m_cols_buffer(std::move(other.m_cols_buffer)),
      m_cols_buffer_data(other.m_cols_buffer_data),
      m_null_mask(std::move(other.m_null_mask)),
      m_null_mask_data(other.m_null_mask_data) {
  other.m_source_fld = nullptr;
  other.m_type = MYSQL_TYPE_NULL;
  other.m_field_width = 0;
  other.m_field_index = 0;
  other.m_table = nullptr;
  other.m_current_size = 0;
  other.m_chunk_size = 0;
  other.m_cols_buffer_data = nullptr;
  other.m_null_mask_data = nullptr;
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
  std::swap(m_field_index, other.m_field_index);
  std::swap(m_table, other.m_table);
  std::swap(m_current_size, other.m_current_size);
  std::swap(m_chunk_size, other.m_chunk_size);
  std::swap(m_cols_buffer, other.m_cols_buffer);
  std::swap(m_cols_buffer_data, other.m_cols_buffer_data);
  std::swap(m_null_mask, other.m_null_mask);
  std::swap(m_null_mask_data, other.m_null_mask_data);
}

void ColumnChunk::initialize_buffers() {
  if (m_chunk_size == 0 || m_field_width == 0) {
    m_cols_buffer.reset();
    m_cols_buffer_data = nullptr;
    m_null_mask.reset();
    m_null_mask_data = nullptr;
    return;
  }

  const size_t buffer_size = m_chunk_size * m_field_width;
  // Always reallocate: m_chunk_size may have changed since last allocation
  // and unique_ptr doesn't expose the allocated size.  The previous check
  // "buffer_size != m_chunk_size * m_field_width" was a tautology (false).
  m_cols_buffer = std::make_unique<uchar[]>(buffer_size);
  m_cols_buffer_data = m_cols_buffer.get();

  if (!m_null_mask || m_null_mask->rows != m_chunk_size)
    m_null_mask = std::make_unique<ShannonBase::bit_array_t>(m_chunk_size);
  m_null_mask_data = m_null_mask ? m_null_mask->data : nullptr;
}

void ColumnChunk::copy_from(const ColumnChunk &other) {
  m_source_fld = other.m_source_fld;
  m_type = other.m_type;
  m_field_width = other.m_field_width;
  m_field_index = other.m_field_index;
  m_table = other.m_table;
  m_chunk_size = other.m_chunk_size;
  m_current_size = other.m_current_size;

  initialize_buffers();

  if (other.m_cols_buffer && m_cols_buffer && m_current_size != 0) {
    const size_t data_size = m_current_size * m_field_width;
    std::memcpy(m_cols_buffer.get(), other.m_cols_buffer.get(), data_size);
  }

  if (other.m_null_mask && m_null_mask && other.m_null_mask->data && m_null_mask->data && m_current_size != 0) {
    const size_t null_mask_size = (m_current_size + 7) / 8;
    std::memcpy(m_null_mask->data, other.m_null_mask->data, null_mask_size);
  }
}

void ColumnChunk::reset(Field *mysql_fld, size_t chunk_size) {
  if ((mysql_fld == m_source_fld) && (chunk_size == m_chunk_size)) {
    // Fast path: logical reset only. append() overwrites each touched null bit.
    m_current_size = 0;
    return;
  }

  m_source_fld = mysql_fld;
  m_chunk_size = chunk_size;

  if (mysql_fld) {
    m_type = mysql_fld->type();
    m_field_width = Utils::Util::normalized_length(mysql_fld);
    m_field_index = static_cast<uint16_t>(mysql_fld->field_index());
    m_table = mysql_fld->table;
  } else {
    m_type = MYSQL_TYPE_NULL;
    m_field_width = 0;
    m_field_index = 0;
    m_table = nullptr;
  }

  m_current_size = 0;
  initialize_buffers();  // realloc only when necessary
}

bool ColumnChunk::grow(size_t new_capacity) {
  if (new_capacity <= m_chunk_size) return true;
  if (!valid() || m_field_width == 0 || new_capacity > std::numeric_limits<size_t>::max() / m_field_width) return false;

  const size_t current = m_current_size;
  auto new_buffer = std::make_unique<uchar[]>(new_capacity * m_field_width);
  auto new_null_mask = std::make_unique<ShannonBase::bit_array_t>(new_capacity);
  if (current > 0) {
    std::memcpy(new_buffer.get(), m_cols_buffer.get(), current * m_field_width);
    std::memcpy(new_null_mask->data, m_null_mask->data, (current + 7) / 8);
  }

  m_cols_buffer = std::move(new_buffer);
  m_null_mask = std::move(new_null_mask);
  m_cols_buffer_data = m_cols_buffer.get();
  m_null_mask_data = m_null_mask->data;
  m_chunk_size = new_capacity;
  return true;
}

bool ColumnChunk::add(const uchar *data, size_t length, bool null) {
  if (!data && !null) return false;

  size_t actual_idx = m_current_size++;
  if (actual_idx >= m_chunk_size) {
    --m_current_size;
    return false;
  }

  uchar *dest = m_cols_buffer.get() + (actual_idx * m_field_width);
  if (null) {
    set_null(actual_idx);
  } else {
    ShannonBase::Utils::Util::bit_array_reset(m_null_mask.get(), actual_idx);
    const size_t copy_len = std::min(length, m_field_width);
    if (copy_len > 0) std::memcpy(dest, data, copy_len);
    if (copy_len < m_field_width) std::memset(dest + copy_len, 0, m_field_width - copy_len);
  }

  return true;
}

bool ColumnChunk::add_batch(const std::vector<std::pair<const uchar *, size_t>> &data_batch,
                            const std::vector<bool> &null_flags) {
  if (data_batch.size() != null_flags.size()) return false;

  size_t batch_size = data_batch.size();
  if (batch_size == 0) return true;

  // Reserve the range [start_idx, start_idx + batch_size). ColumnChunk is single-consumer.
  size_t start_idx = m_current_size;
  if (start_idx + batch_size > m_chunk_size) return false;
  m_current_size = start_idx + batch_size;

  for (size_t i = 0; i < batch_size; ++i) {
    size_t idx = start_idx + i;

    if (null_flags[i]) {
      set_null(idx);
    } else {
      ShannonBase::Utils::Util::bit_array_reset(m_null_mask.get(), idx);
      const uchar *src_data = data_batch[i].first;
      size_t src_length = data_batch[i].second;
      if (!src_data) {
        m_current_size = start_idx;
        return false;
      }

      const size_t copy_len = std::min(src_length, m_field_width);
      uchar *dest = m_cols_buffer.get() + (idx * m_field_width);
      if (copy_len > 0) std::memcpy(dest, src_data, copy_len);
      if (copy_len < m_field_width) std::memset(dest + copy_len, 0, m_field_width - copy_len);
    }
  }

  return true;
}

bool ColumnChunk::append_from(const ColumnChunk &source, size_t source_row) {
  if (!valid() || !source.valid() || source_row >= source.size()) return false;
  if (source.width() != width() || source.table() != table() || source.field_index() != field_index()) return false;
  const bool is_null = source.nullable_fast(source_row);
  return add(is_null ? nullptr : source.data_fast(source_row), is_null ? 0 : source.width(), is_null);
}

bool ColumnChunk::gather_in_place(const std::vector<uint32_t> &selection, size_t *bytes_moved) {
  if (!valid()) return selection.empty();
  const size_t old_size = m_current_size;
  if (selection.size() > old_size) return false;

  if (bytes_moved != nullptr) *bytes_moved = 0;
  bool identity = selection.size() == old_size;
  for (size_t i = 0; identity && i < selection.size(); ++i) identity = selection[i] == i;
  if (identity) return true;

  for (size_t dst = 0; dst < selection.size(); ++dst) {
    const size_t src = selection[dst];
    if (src >= old_size || (dst != 0 && selection[dst - 1] >= src)) return false;
    const bool is_null = nullable_fast(src);
    if (is_null) {
      ShannonBase::Utils::Util::bit_array_set(m_null_mask.get(), dst);
      continue;
    }

    ShannonBase::Utils::Util::bit_array_reset(m_null_mask.get(), dst);
    if (dst != src) {
      std::memmove(m_cols_buffer_data + dst * m_field_width, m_cols_buffer_data + src * m_field_width, m_field_width);
      if (bytes_moved != nullptr) *bytes_moved += m_field_width;
    }
  }
  m_current_size = selection.size();
  return true;
}

size_t ColumnChunk::compact() {
  if (!m_cols_buffer || !m_null_mask) return 0;

  size_t current_size = m_current_size;
  size_t write_idx = 0;

  for (size_t read_idx = 0; read_idx < current_size; ++read_idx) {
    if (!nullable(read_idx)) {
      if (write_idx != read_idx) {
        std::memcpy(mutable_data(write_idx), data(read_idx), m_field_width);
      }
      ++write_idx;
    }
  }

  m_current_size = write_idx;

  if (m_null_mask && m_null_mask->data) {
    const size_t null_mask_size = (m_chunk_size + 7) / 8;
    std::memset(m_null_mask->data, 0, null_mask_size);
  }

  return write_idx;
}

/**
 * @brief Calculate scale factor: 10^scale
 */
static int64_t get_scale_factor(uint scale) {
  static const int64_t scale_factors[] = {
      1LL,                    // 10^0
      10LL,                   // 10^1
      100LL,                  // 10^2
      1000LL,                 // 10^3
      10000LL,                // 10^4
      100000LL,               // 10^5
      1000000LL,              // 10^6
      10000000LL,             // 10^7
      100000000LL,            // 10^8
      1000000000LL,           // 10^9
      10000000000LL,          // 10^10
      100000000000LL,         // 10^11
      1000000000000LL,        // 10^12
      10000000000000LL,       // 10^13
      100000000000000LL,      // 10^14
      1000000000000000LL,     // 10^15
      10000000000000000LL,    // 10^16
      100000000000000000LL,   // 10^17
      1000000000000000000LL,  // 10^18
  };

  if (scale < sizeof(scale_factors) / sizeof(scale_factors[0])) return scale_factors[scale];

  // If scale is too large, fallback to loop calculation
  int64_t factor = 1;
  for (uint i = 0; i < scale; ++i) {
    factor *= 10;
  }
  return factor;
}

/**
 * @brief Get the scale (number of decimal places) from Field
 */
static inline uint get_decimal_scale(Field *field) {
  if (field && field->type() == MYSQL_TYPE_NEWDECIMAL) {
    // Field_new_decimal type provides decimals() information
    return field->decimals();
  }
  return 0;
}

/**
 * @brief Get the precision (total number of digits) from Field
 */
static uint get_decimal_precision(Field *field) {
  if (field && field->type() == MYSQL_TYPE_NEWDECIMAL) {
    Field_new_decimal *dec_field = static_cast<Field_new_decimal *>(field);
    return dec_field->precision;
  }
  return 0;
}

/**
 * @brief Check if decimal type can be safely represented using int64_t
 * @note int64_t range is [-9223372036854775808, 9223372036854775807]
 *       which corresponds to approximately 18-19 decimal digits
 */
static bool can_use_int64_for_decimal(Field *field) {
  if (!field || field->type() != MYSQL_TYPE_NEWDECIMAL) return false;

  uint precision = get_decimal_precision(field);
  // Conservative estimate: precision <= 18 can safely use int64_t
  return precision <= 18;
}

/**
 * @brief Convert int64_t back to my_decimal
 * @param val - input integer value
 * @param scale - number of decimal places
 * @param dec - output decimal value
 */
static void int64_to_my_decimal(int64_t val, uint scale, my_decimal *dec) {
  if (scale == 0) {
    // Simple case: no decimal places, direct conversion using MySQL API
    longlong2decimal(val, dec);
    return;
  }

  // Convert int64_t to decimal using MySQL API
  my_decimal val_dec;
  longlong2decimal(val, &val_dec);

  // Divide by scale_factor to restore original scale
  // Example: 12345 / 100 = 123.45 (with scale=2)
  my_decimal scale_factor_dec;
  int64_t scale_factor = get_scale_factor(scale);
  longlong2decimal(scale_factor, &scale_factor_dec);

  // Divide using MySQL API
  decimal_div(&val_dec, &scale_factor_dec, dec, scale);
}

namespace {

constexpr size_t kIntegerSumBatchRows = 4096;

inline __int128 abs_as_int128(int64_t value) {
  return value < 0 ? -static_cast<__int128>(value) : static_cast<__int128>(value);
}

inline bool int64_sum_overflow_safe(int64_t min_value, int64_t max_value, size_t count) {
  const __int128 bound = std::max(abs_as_int128(min_value), abs_as_int128(max_value));
  return bound * static_cast<__int128>(count) <= static_cast<__int128>(std::numeric_limits<int64_t>::max());
}

inline void add_decimal(my_decimal *total, const my_decimal &delta) {
  my_decimal result;
  my_decimal_add(E_DEC_FATAL_ERROR, &result, total, &delta);
  *total = result;
}

}  // namespace

/**
 * @brief Safe extraction with overflow checking
 */
static bool extract_decimal_for_simd_safe(const ColumnChunk &chunk, size_t row_count,
                                          std::vector<int64_t> &data_buffer) {
  data_buffer.clear();

  Field *source_field = chunk.source_field();
  if (!can_use_int64_for_decimal(source_field)) return false;

  data_buffer.resize(row_count, 0);
  uint scale = get_decimal_scale(source_field);

  for (size_t i = 0; i < row_count; ++i) {
    if (chunk.nullable(i)) {
      data_buffer[i] = 0;
      continue;
    }

    source_field->set_notnull();
    const uchar *row_data = chunk.data(i);
    size_t data_len = chunk.width();
    source_field->pack(const_cast<uchar *>(source_field->data_ptr()), row_data, data_len);
    my_decimal dec;
    source_field->val_decimal(&dec);

    // Convert using MySQL API
    longlong result_val = 0;

    if (scale == 0) {
      // Direct conversion
      int ret = decimal2longlong(&dec, &result_val);
      if (ret > E_DEC_TRUNCATED) return false;  // Overflow or fatal error, fallback to scalar
      data_buffer[i] = static_cast<int64_t>(result_val);
    } else {
      // Scale and convert
      my_decimal scaled_dec;
      my_decimal scale_factor_dec;
      int64_t scale_factor = get_scale_factor(scale);
      longlong2decimal(scale_factor, &scale_factor_dec);

      int mul_ret = decimal_mul(&dec, &scale_factor_dec, &scaled_dec);
      if (mul_ret > E_DEC_TRUNCATED) return false;  // Overflow detected

      int conv_ret = decimal2longlong(&scaled_dec, &result_val);
      if (conv_ret > E_DEC_TRUNCATED) return false;  // Conversion error
      data_buffer[i] = static_cast<int64_t>(result_val);
    }
  }

  return true;
}

static my_decimal scalar_sum_as_decimal(const ColumnChunk &chunk, size_t row_count) {
  my_decimal total;
  int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &total);

  Field *source_field = chunk.source_field();
  if (source_field == nullptr) return total;
  for (size_t row = 0; row < row_count; ++row) {
    if (chunk.nullable(row)) continue;
    source_field->set_notnull();
    source_field->pack(const_cast<uchar *>(source_field->data_ptr()), chunk.data(row), chunk.width());
    my_decimal value;
    source_field->val_decimal(&value);
    add_decimal(&total, value);
  }
  return total;
}

static my_decimal sum_integer_chunk_as_decimal(const ColumnChunk &chunk, size_t row_count) {
  my_decimal total;
  int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &total);
  if (row_count == 0 || !chunk.valid()) return total;

  Field *source_field = chunk.source_field();
  if (source_field == nullptr ||
      (source_field->type() != MYSQL_TYPE_LONG && source_field->type() != MYSQL_TYPE_LONGLONG))
    return scalar_sum_as_decimal(chunk, row_count);

  const size_t width = chunk.width();
  const uint8_t *null_mask = chunk.get_null_mask() ? chunk.get_null_mask()->data : nullptr;

  if (source_field->type() == MYSQL_TYPE_LONG && width == sizeof(int32_t)) {
    // Widen INT values before invoking the existing int64 SIMD sum. This
    // avoids the 32-bit SIMD lane overflow in Sum<int32_t>. A 4096-row batch
    // of UINT32_MAX values still fits comfortably in int64.
    std::vector<int64_t> widened(std::min(row_count, kIntegerSumBatchRows));
    for (size_t offset = 0; offset < row_count; offset += kIntegerSumBatchRows) {
      const size_t batch_rows = std::min(kIntegerSumBatchRows, row_count - offset);
      const bool is_unsigned = source_field->is_unsigned();
      for (size_t row = 0; row < batch_rows; ++row) {
        if (chunk.nullable(offset + row)) {
          widened[row] = 0;
          continue;
        }
        if (is_unsigned) {
          uint32_t value;
          memcpy(&value, chunk.data(offset + row), sizeof(value));
          widened[row] = static_cast<int64_t>(value);
        } else {
          int32_t value;
          memcpy(&value, chunk.data(offset + row), sizeof(value));
          widened[row] = static_cast<int64_t>(value);
        }
      }

      const int64_t batch_sum = Utils::SIMD::sum<int64_t>(widened.data(), nullptr, batch_rows);
      my_decimal delta;
      int64_to_my_decimal(batch_sum, 0, &delta);
      add_decimal(&total, delta);
    }
    return total;
  }

  if (source_field->type() != MYSQL_TYPE_LONGLONG || width != sizeof(int64_t))
    return scalar_sum_as_decimal(chunk, row_count);

  for (size_t offset = 0; offset < row_count; offset += kIntegerSumBatchRows) {
    const size_t batch_rows = std::min(kIntegerSumBatchRows, row_count - offset);
    const uint8_t *batch_mask = null_mask ? null_mask + offset / 8 : nullptr;
    const size_t non_null = Utils::SIMD::count_non_null(batch_mask, batch_rows);
    if (non_null == 0) continue;

    const int64_t *data = reinterpret_cast<const int64_t *>(chunk.data(offset));
    bool simd_safe = false;
    if (source_field->is_unsigned()) {
      uint64_t max_value = 0;
      for (size_t row = 0; row < batch_rows; ++row) {
        if (chunk.nullable(offset + row)) continue;
        uint64_t value;
        memcpy(&value, chunk.data(offset + row), sizeof(value));
        max_value = std::max(max_value, value);
      }
      simd_safe = static_cast<__int128>(max_value) * static_cast<__int128>(non_null) <=
                  static_cast<__int128>(std::numeric_limits<int64_t>::max());
    } else {
      const int64_t min_value = Utils::SIMD::min<int64_t>(data, batch_mask, batch_rows);
      const int64_t max_value = Utils::SIMD::max<int64_t>(data, batch_mask, batch_rows);
      simd_safe = int64_sum_overflow_safe(min_value, max_value, non_null);
    }

    if (simd_safe) {
      const int64_t batch_sum = Utils::SIMD::sum<int64_t>(data, batch_mask, batch_rows);
      my_decimal delta;
      int64_to_my_decimal(batch_sum, 0, &delta);
      add_decimal(&total, delta);
      continue;
    }

    // Values in this batch could overflow an int64 SIMD lane. Convert only
    // this rare batch through Field::val_decimal and accumulate in DECIMAL.
    for (size_t row = offset; row < offset + batch_rows; ++row) {
      if (chunk.nullable(row)) continue;
      source_field->set_notnull();
      source_field->pack(const_cast<uchar *>(source_field->data_ptr()), chunk.data(row), width);
      my_decimal value;
      source_field->val_decimal(&value);
      add_decimal(&total, value);
    }
  }
  return total;
}

template <>
my_decimal ColumnChunkOper::genericSum<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  return scalar_sum_as_decimal(chunk, row_count);
}

template <>
my_decimal ColumnChunkOper::genericMin<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  my_decimal min_val;
  bool found = false;

  Field *source_field = chunk.source_field();
  for (size_t i = 0; i < row_count; ++i) {
    if (chunk.nullable(i))
      source_field->set_null();
    else {
      source_field->set_notnull();
      const uchar *row_data = chunk.data(i);
      size_t data_len = chunk.width();
      source_field->pack(const_cast<uchar *>(source_field->data_ptr()), row_data, data_len);

      my_decimal val;
      source_field->val_decimal(&val);
      if (!found || decimal_cmp(&val, &min_val) < 0) {
        min_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &min_val);
  }
  return min_val;
}

template <>
my_decimal ColumnChunkOper::genericMax<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  my_decimal max_val;
  bool found = false;

  Field *source_field = chunk.source_field();
  for (size_t i = 0; i < row_count; ++i) {
    // Skip NULL values, process non-NULL values
    if (chunk.nullable(i))
      source_field->set_null();
    else {
      source_field->set_notnull();
      const uchar *row_data = chunk.data(i);
      size_t data_len = chunk.width();
      source_field->pack(const_cast<uchar *>(source_field->data_ptr()), row_data, data_len);
      my_decimal val;
      source_field->val_decimal(&val);
      if (!found || decimal_cmp(&val, &max_val) > 0) {
        max_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &max_val);
  }
  return max_val;
}

template <>
size_t ColumnChunkOper::genericFilter<my_decimal>(const ColumnChunk &chunk, size_t row_count,
                                                  std::function<bool(my_decimal)> predicate,
                                                  std::vector<size_t> &output_indices) {
  size_t count = 0;
  Field *source_field = chunk.source_field();

  for (size_t i = 0; i < row_count; ++i) {
    // Skip NULL values correctly
    if (chunk.nullable(i))
      source_field->set_null();
    else {
      source_field->set_notnull();
      const uchar *row_data = chunk.data(i);
      size_t data_len = chunk.width();
      source_field->pack(const_cast<uchar *>(source_field->data_ptr()), row_data, data_len);
      my_decimal val;
      source_field->val_decimal(&val);

      if (predicate(val)) {
        output_indices.push_back(i);
        count++;
      }
    }
  }
  return count;
}

/**
 * @brief my_decimal specialization: Sum operation
 */
template <>
my_decimal ColumnChunkOper::Sum<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  Field *source_field = chunk.source_field();
  if (source_field != nullptr &&
      (source_field->type() == MYSQL_TYPE_LONG || source_field->type() == MYSQL_TYPE_LONGLONG))
    return sum_integer_chunk_as_decimal(chunk, row_count);

  uint scale = get_decimal_scale(source_field);

  std::vector<int64_t> data_buffer;
  bool simd_safe = extract_decimal_for_simd_safe(chunk, row_count, data_buffer);

  my_decimal result;
  const size_t non_null = CountNonNull(chunk, row_count);
  if (simd_safe && !data_buffer.empty() && non_null > 0) {
    const int64_t min_value = Utils::SIMD::min<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);
    const int64_t max_value = Utils::SIMD::max<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);
    if (!int64_sum_overflow_safe(min_value, max_value, non_null)) return genericSum<my_decimal>(chunk, row_count);
    // Use SIMD-accelerated sum
    int64_t sum = Utils::SIMD::sum<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);
    // Convert back using MySQL API
    int64_to_my_decimal(sum, scale, &result);
    return result;
  }

  if (non_null == 0) {
    int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &result);
    return result;
  }

  // Fallback to scalar implementation
  return genericSum<my_decimal>(chunk, row_count);
}

/**
 * @brief my_decimal specialization: Min operation
 */
template <>
my_decimal ColumnChunkOper::Min<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  Field *source_field = chunk.source_field();
  uint scale = get_decimal_scale(source_field);

  std::vector<int64_t> data_buffer;
  bool simd_safe = extract_decimal_for_simd_safe(chunk, row_count, data_buffer);

  my_decimal result;
  if (simd_safe && !data_buffer.empty()) {
    // Use SIMD-accelerated min
    int64_t min_val = Utils::SIMD::min<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);
    // Convert back using MySQL API
    int64_to_my_decimal(min_val, scale, &result);
    return result;
  }

  // Fallback to scalar implementation
  return genericMin<my_decimal>(chunk, row_count);
}

/**
 * @brief my_decimal specialization: Max operation
 */
template <>
my_decimal ColumnChunkOper::Max<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  Field *source_field = chunk.source_field();
  uint scale = get_decimal_scale(source_field);

  std::vector<int64_t> data_buffer;
  bool simd_safe = extract_decimal_for_simd_safe(chunk, row_count, data_buffer);

  my_decimal result;
  if (simd_safe && !data_buffer.empty()) {
    // Use SIMD-accelerated max
    int64_t max_val = Utils::SIMD::max<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);
    // Convert back using MySQL API
    int64_to_my_decimal(max_val, scale, &result);
    return result;
  }

  // Fallback to scalar implementation
  return genericMax<my_decimal>(chunk, row_count);
}

/**
 * @brief my_decimal specialization: Filter operation
 */
template <>
size_t ColumnChunkOper::Filter<my_decimal>(const ColumnChunk &chunk, size_t row_count,
                                           std::function<bool(my_decimal)> predicate,
                                           std::vector<size_t> &output_indices) {
  // Filter operation for my_decimal is not suitable for direct SIMD, use scalar implementation
  return genericFilter<my_decimal>(chunk, row_count, predicate, output_indices);
}

/**
 * @brief my_decimal specialization: Average operation
 */
template <>
double ColumnChunkOper::Average<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  my_decimal sum = Sum<my_decimal>(chunk, row_count);
  size_t non_null_count = CountNonNull(chunk, row_count);

  if (non_null_count == 0) return 0.0;

  // Convert my_decimal to double using MySQL API
  double sum_val = 0.0;
  decimal2double(&sum, &sum_val);

  return sum_val / non_null_count;
}

VectorizedFilterIterator::VectorizedFilterIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                   Item *condition)
    : RowIterator(thd), m_source(std::move(source)), m_condition(condition) {}

void VectorizedFilterIterator::CollectConditionFields() {
  m_condition_fields.clear();
  if (m_condition == nullptr) return;
  WalkItem(m_condition, enum_walk::POSTFIX, [this](Item *item) -> bool {
    if (item == nullptr || item->type() != Item::FIELD_ITEM) return false;
    Field *field = down_cast<Item_field *>(item)->field;
    if (field != nullptr &&
        std::find(m_condition_fields.begin(), m_condition_fields.end(), field) == m_condition_fields.end()) {
      m_condition_fields.push_back(field);
    }
    return false;
  });
}

void VectorizedFilterIterator::CompileSimplePredicate() {
  m_simple_predicate = SimplePredicate{};
  if (m_condition == nullptr || m_condition->type() != Item::FUNC_ITEM) return;

  auto *func = down_cast<Item_func *>(m_condition);
  if (func->argument_count() != 2) return;

  SimplePredicateOp op = SimplePredicateOp::NONE;
  switch (func->functype()) {
    case Item_func::EQ_FUNC:
      op = SimplePredicateOp::EQ;
      break;
    case Item_func::NE_FUNC:
      op = SimplePredicateOp::NE;
      break;
    case Item_func::LT_FUNC:
      op = SimplePredicateOp::LT;
      break;
    case Item_func::LE_FUNC:
      op = SimplePredicateOp::LE;
      break;
    case Item_func::GT_FUNC:
      op = SimplePredicateOp::GT;
      break;
    case Item_func::GE_FUNC:
      op = SimplePredicateOp::GE;
      break;
    default:
      return;
  }

  Item *left = func->arguments()[0]->real_item();
  Item *right = func->arguments()[1]->real_item();
  Item_field *field_item = nullptr;
  Item *constant = nullptr;
  bool reversed = false;
  if (left->type() == Item::FIELD_ITEM && right->const_item()) {
    field_item = down_cast<Item_field *>(left);
    constant = right;
  } else if (right->type() == Item::FIELD_ITEM && left->const_item()) {
    field_item = down_cast<Item_field *>(right);
    constant = left;
    reversed = true;
  } else {
    return;
  }

  Field *field = field_item->field;
  if (field == nullptr || field->is_unsigned()) return;
  if (field->type() != MYSQL_TYPE_LONG && field->type() != MYSQL_TYPE_LONGLONG) return;

  // Do not guess MySQL coercion for string/decimal/temporal constants. Those
  // stay on Item::val_int() row semantics; the fast kernel is intentionally
  // limited to an integer field compared with an integer constant.
  switch (constant->data_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      break;
    default:
      return;
  }

  const longlong constant_value = constant->val_int();
  if (constant->null_value || thd()->is_error()) return;

  if (reversed) {
    switch (op) {
      case SimplePredicateOp::LT:
        op = SimplePredicateOp::GT;
        break;
      case SimplePredicateOp::LE:
        op = SimplePredicateOp::GE;
        break;
      case SimplePredicateOp::GT:
        op = SimplePredicateOp::LT;
        break;
      case SimplePredicateOp::GE:
        op = SimplePredicateOp::LE;
        break;
      default:
        break;
    }
  }

  m_simple_predicate.op = op;
  m_simple_predicate.field = field;
  m_simple_predicate.integer_constant = constant_value;
  m_simple_predicate.valid = true;
}

bool VectorizedFilterIterator::EvaluateSimplePredicateBatch(const std::vector<ColumnChunk> &chunks, size_t rows) {
  if (!m_simple_predicate.valid || m_simple_predicate.chunk_idx >= chunks.size()) return false;
  const ColumnChunk &chunk = chunks[m_simple_predicate.chunk_idx];
  if (!chunk.valid() || rows > chunk.size()) return false;

  m_selection.clear();
  Utils::SIMD::CompareOp op;
  switch (m_simple_predicate.op) {
    case SimplePredicateOp::EQ:
      op = Utils::SIMD::CompareOp::EQ;
      break;
    case SimplePredicateOp::NE:
      op = Utils::SIMD::CompareOp::NE;
      break;
    case SimplePredicateOp::LT:
      op = Utils::SIMD::CompareOp::LT;
      break;
    case SimplePredicateOp::LE:
      op = Utils::SIMD::CompareOp::LE;
      break;
    case SimplePredicateOp::GT:
      op = Utils::SIMD::CompareOp::GT;
      break;
    case SimplePredicateOp::GE:
      op = Utils::SIMD::CompareOp::GE;
      break;
    default:
      return false;
  }

  if (m_simple_predicate.field->type() == MYSQL_TYPE_LONG && chunk.width() == sizeof(int32_t)) {
    if (m_simple_predicate.integer_constant < std::numeric_limits<int32_t>::min() ||
        m_simple_predicate.integer_constant > std::numeric_limits<int32_t>::max())
      return false;
    m_simd_selection.clear();
    const auto *data = reinterpret_cast<const int32_t *>(chunk.data_fast(0));
    Kernels::FilterCompareInt32(data, chunk.get_null_mask()->data, rows,
                                static_cast<int32_t>(m_simple_predicate.integer_constant), op, m_simd_selection);
    m_selection.reserve(m_simd_selection.size());
    for (size_t row : m_simd_selection) m_selection.push_back(static_cast<uint32_t>(row));
  } else if (m_simple_predicate.field->type() == MYSQL_TYPE_LONGLONG && chunk.width() == sizeof(int64_t)) {
    m_simd_selection.clear();
    const auto *data = reinterpret_cast<const int64_t *>(chunk.data_fast(0));
    Kernels::FilterCompareInt64(data, chunk.get_null_mask()->data, rows,
                                static_cast<int64_t>(m_simple_predicate.integer_constant), op, m_simd_selection);
    m_selection.reserve(m_simd_selection.size());
    for (size_t row : m_simd_selection) m_selection.push_back(static_cast<uint32_t>(row));
  } else {
    return false;
  }

  if (Kernels::CompareUsesSimd(m_simple_predicate.field->type() == MYSQL_TYPE_LONG, op))
    m_stats.simd_rows += rows;
  else
    m_stats.scalar_fallback_rows += rows;
  return true;
}

bool VectorizedFilterIterator::Init() {
  m_lookahead_start = 0;
  m_lookahead_count = 0;
  for (ColumnChunk &chunk : m_lookahead_chunks) chunk.clear();
  m_condition_chunk_map.clear();
  m_selection.clear();
  m_simd_selection.clear();
  m_stats = VectorizedOperatorStats{};

  if (m_source->Init()) return true;
  m_batch_source = dynamic_cast<BatchReadable *>(m_source.get());
  if (m_batch_source == nullptr) m_batch_source = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  CollectConditionFields();
  CompileSimplePredicate();
  return false;
}

int VectorizedFilterIterator::Read() {
  for (;;) {
    const int result = m_source->Read();
    if (result != 0) return result;
    ++m_stats.rows_in;
    if (m_condition == nullptr) {
      ++m_stats.rows_out;
      return 0;
    }

    const bool matched = m_condition->val_int() != 0;
    if (thd()->is_error()) return 1;
    if (matched && !m_condition->null_value) {
      ++m_stats.rows_out;
      return 0;
    }
    m_source->UnlockRow();
  }
}

void VectorizedFilterIterator::SetNullRowFlag(bool is_null_row) { m_source->SetNullRowFlag(is_null_row); }
void VectorizedFilterIterator::StartPSIBatchMode() { m_source->StartPSIBatchMode(); }
void VectorizedFilterIterator::EndPSIBatchModeIfStarted() { m_source->EndPSIBatchModeIfStarted(); }
void VectorizedFilterIterator::UnlockRow() { m_source->UnlockRow(); }

bool VectorizedFilterIterator::BuildConditionChunkMap(const std::vector<ColumnChunk> &chunks) {
  m_condition_chunk_map.clear();
  m_condition_chunk_map.reserve(m_condition_fields.size());
  for (Field *field : m_condition_fields) {
    size_t found = std::numeric_limits<size_t>::max();
    for (size_t idx = 0; idx < chunks.size(); ++idx) {
      if (!chunks[idx].valid()) continue;
      if (chunks[idx].table() == field->table && chunks[idx].field_index() == field->field_index()) {
        found = idx;
        break;
      }
    }
    if (found == std::numeric_limits<size_t>::max()) return false;

    // IMCS variable-length payloads may be dictionary ids/VarlenReference rather
    // than a MySQL Field image. Keep those conditions on the ordinary row path.
    if (Utils::Util::is_string(field->type()) || Utils::Util::is_varlen(field->type())) return false;
    m_condition_chunk_map.push_back(found);
    if (m_simple_predicate.valid && m_simple_predicate.field == field) m_simple_predicate.chunk_idx = found;
  }
  return true;
}

bool VectorizedFilterIterator::MaterializeConditionRow(const std::vector<ColumnChunk> &chunks, size_t row) {
  if (m_condition_chunk_map.size() != m_condition_fields.size()) return true;
  for (size_t i = 0; i < m_condition_fields.size(); ++i) {
    Field *field = m_condition_fields[i];
    const ColumnChunk &chunk = chunks[m_condition_chunk_map[i]];
    if (row >= chunk.size()) return true;
    if (chunk.nullable_fast(row)) {
      field->set_null();
      continue;
    }
    field->set_notnull();
    field->pack(const_cast<uchar *>(field->data_ptr()), chunk.data_fast(row), chunk.width());
    m_stats.bytes_copied += chunk.width();
  }
  ++m_stats.row_materializations;
  return false;
}

bool VectorizedFilterIterator::DrainLookahead(std::vector<ColumnChunk> &col_chunks, size_t capacity,
                                              size_t *rows_read) {
  if (rows_read == nullptr || m_lookahead_count == 0) return false;
  const size_t count = std::min(capacity, m_lookahead_count);
  if (col_chunks.size() != m_lookahead_chunks.size()) return false;
  for (size_t col = 0; col < col_chunks.size(); ++col) {
    if (!col_chunks[col].valid()) continue;
    const ColumnChunk &source = m_lookahead_chunks[col];
    if (!source.valid()) return false;
    for (size_t row = 0; row < count; ++row) {
      if (!col_chunks[col].append_from(source, m_lookahead_start + row)) return false;
      if (!source.nullable_fast(m_lookahead_start + row)) m_stats.bytes_copied += source.width();
    }
  }
  m_lookahead_start += count;
  m_lookahead_count -= count;
  if (m_lookahead_count == 0) {
    m_lookahead_start = 0;
    for (ColumnChunk &chunk : m_lookahead_chunks) chunk.clear();
  }
  *rows_read = count;
  return true;
}

int VectorizedFilterIterator::ReadBatchViaRows(std::vector<ColumnChunk> &col_chunks, size_t capacity,
                                               size_t &rows_read) {
  rows_read = 0;
  while (rows_read < capacity) {
    const int result = Read();
    if (result != 0) return result == -1 ? HA_ERR_END_OF_FILE : result;

    for (ColumnChunk &chunk : col_chunks) {
      if (!chunk.valid()) continue;
      Field *field = chunk.source_field();
      if (field == nullptr) return 1;
      const bool is_null = field->is_null();
      if (!chunk.add(is_null ? nullptr : field->field_ptr(), is_null ? 0 : field->pack_length(), is_null)) return 1;
      if (!is_null) m_stats.bytes_copied += field->pack_length();
    }
    ++rows_read;
  }
  return 0;
}

int VectorizedFilterIterator::ReadBatch(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) {
  rows_read = 0;
  if (capacity == 0) return 0;
  if (m_lookahead_count != 0) {
    if (!DrainLookahead(col_chunks, capacity, &rows_read)) return 1;
    ++m_stats.batches;
    return 0;
  }

  const bool direct_batch = m_batch_source != nullptr && BuildConditionChunkMap(col_chunks);
  if (!direct_batch) {
    const int result = ReadBatchViaRows(col_chunks, capacity, rows_read);
    if (rows_read != 0) ++m_stats.batches;
    return result;
  }

  for (;;) {
    for (ColumnChunk &chunk : col_chunks) chunk.clear();
    size_t input_rows = 0;
    const int result = m_batch_source->ReadBatch(col_chunks, capacity, input_rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    m_stats.rows_in += input_rows;
    ++m_stats.batches;

    const bool used_vector_predicate = EvaluateSimplePredicateBatch(col_chunks, input_rows);
    if (!used_vector_predicate) {
      m_stats.scalar_fallback_rows += input_rows;
      m_selection.clear();
      m_selection.reserve(input_rows);
      for (size_t row = 0; row < input_rows; ++row) {
        if (MaterializeConditionRow(col_chunks, row)) return 1;
        const bool matched = m_condition == nullptr || (m_condition->val_int() != 0 && !m_condition->null_value);
        if (thd()->is_error()) return 1;
        if (matched) m_selection.push_back(static_cast<uint32_t>(row));
      }
    }

    if (!m_selection.empty()) {
      for (ColumnChunk &chunk : col_chunks) {
        if (!chunk.valid()) continue;
        size_t bytes_moved = 0;
        if (!chunk.gather_in_place(m_selection, &bytes_moved)) return 1;
        m_stats.bytes_copied += bytes_moved;
      }
      rows_read = m_selection.size();
      m_stats.rows_out += rows_read;
      return result;
    }

    if (result == HA_ERR_END_OF_FILE || input_rows == 0) return HA_ERR_END_OF_FILE;
  }
}

void PushbackBatchTailShared(const std::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows,
                             std::vector<ColumnChunk> *lookahead_chunks, size_t *lookahead_start,
                             size_t *lookahead_count, size_t *bytes_copied) {
  if (from_row >= total_rows) return;
  const size_t tail = total_rows - from_row;
  bool rebuild = lookahead_chunks->size() != chunks.size();
  for (size_t idx = 0; !rebuild && idx < chunks.size(); ++idx) {
    if (chunks[idx].valid() != (*lookahead_chunks)[idx].valid()) {
      rebuild = true;
      break;
    }
    if (!chunks[idx].valid()) continue;
    rebuild = (*lookahead_chunks)[idx].capacity() < tail || (*lookahead_chunks)[idx].table() != chunks[idx].table() ||
              (*lookahead_chunks)[idx].field_index() != chunks[idx].field_index();
  }

  if (rebuild) {
    lookahead_chunks->clear();
    lookahead_chunks->reserve(chunks.size());
    for (const ColumnChunk &source : chunks)
      lookahead_chunks->emplace_back(source.valid() ? source.source_field() : nullptr, source.valid() ? tail : 0);
  } else {
    for (ColumnChunk &chunk : *lookahead_chunks) chunk.clear();
  }

  for (size_t col = 0; col < chunks.size(); ++col) {
    if (!chunks[col].valid()) continue;
    for (size_t row = from_row; row < total_rows; ++row) {
      if (!(*lookahead_chunks)[col].append_from(chunks[col], row)) {
        *lookahead_start = 0;
        *lookahead_count = 0;
        return;
      }
      if (bytes_copied != nullptr && !chunks[col].nullable_fast(row)) *bytes_copied += chunks[col].width();
    }
  }
  *lookahead_start = 0;
  *lookahead_count = tail;
}

void VectorizedFilterIterator::PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row,
                                                 size_t total_rows) {
  PushbackBatchTailShared(chunks, from_row, total_rows, &m_lookahead_chunks, &m_lookahead_start, &m_lookahead_count,
                          &m_stats.bytes_copied);
}

}  // namespace Executor
}  // namespace ShannonBase
