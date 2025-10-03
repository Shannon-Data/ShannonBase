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

  m_null_mask = std::make_unique<ShannonBase::bit_array_t>(m_chunk_size);
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

  if (scale < sizeof(scale_factors) / sizeof(scale_factors[0])) {
    return scale_factors[scale];
  }

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
  if (!field || field->type() != MYSQL_TYPE_NEWDECIMAL) {
    return false;
  }

  uint precision = get_decimal_precision(field);
  // Conservative estimate: precision <= 18 can safely use int64_t
  return precision <= 18;
}

/**
 * @brief Convert my_decimal to int64_t for SIMD processing
 * @param dec - input decimal value
 * @param scale - number of decimal places
 * @note This conversion scales the decimal value to an integer, preserving original precision
 */
static int64_t my_decimal_to_int64(const my_decimal *dec, uint scale) {
  longlong base_val = 0;
  if (decimal2longlong(dec, &base_val) == E_DEC_OK && scale == 0) {
    // When scale = 0, direct conversion is possible
    return static_cast<int64_t>(base_val);
  }

  // Otherwise, use binary conversion to avoid double precision loss
  uchar bin_buf[DECIMAL_MAX_PRECISION];  // Sufficient to hold all decimal values
  // int bin_size = decimal_bin_size(dec->precision(), dec->frac);
  decimal2bin(dec, bin_buf, dec->precision(), dec->frac);

  // Directly extract the integer part and scale according to the scale factor
  int64_t result = 0;
  my_decimal tmp;
  bin2decimal(bin_buf, &tmp, dec->precision(), dec->frac);

  longlong scaled_val = 0;
  decimal2longlong(&tmp, &scaled_val);  // Already converted to integer
  int64_t scale_factor = get_scale_factor(scale);

  result = static_cast<int64_t>(scaled_val * scale_factor);
  return result;
}

/**
 * @brief Convert int64_t back to my_decimal
 * @param val - input integer value
 * @param scale - number of decimal places
 * @param dec - output decimal value
 */
static void int64_to_my_decimal(int64_t val, uint scale, my_decimal *dec) {
  my_decimal tmp;
  longlong2decimal(val, &tmp);

  if (scale == 0) {
    *dec = tmp;
    return;
  }

  // dive by scale_factor, get the original decimal.
  int64_t scale_factor = get_scale_factor(scale);
  longlong scaled_int = static_cast<longlong>(val / scale_factor);
  longlong2decimal(scaled_int, dec);
}

/**
 * @brief my_decimal specialization: Extract data for SIMD processing
 * @note Converts my_decimal to int64_t to enable vectorized operations
 *       Uses Field information to obtain accurate scale and precision
 */
static void extract_decimal_for_simd(const ColumnChunk &chunk, size_t row_count, std::vector<int64_t> &data_buffer) {
  data_buffer.clear();

  // Check if int64_t is suitable for SIMD processing
  Field *source_field = chunk.source_field();
  if (!can_use_int64_for_decimal(source_field)) {
    // Precision too large, cannot safely use int64_t, return empty buffer
    // Caller will fallback to scalar implementation
    return;
  }
  data_buffer.resize(row_count);

  // Get decimal scale information from Field
  uint scale = get_decimal_scale(source_field);
  for (size_t i = 0; i < row_count; ++i) {
    if (chunk.nullable(i)) continue;

    const uchar *row_data = chunk.data(i);
    // Convert to int64_t using correct scale
    my_decimal dec;
    source_field->set_field_ptr(const_cast<uchar *>(row_data));
    source_field->val_decimal(&dec);
    data_buffer[i] = my_decimal_to_int64(&dec, scale);
  }
}

template <>
my_decimal ColumnChunkOper::genericSum<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  my_decimal sum;
  int2my_decimal(E_DEC_FATAL_ERROR, 0, false, &sum);  // Initialize to 0

  Field *source_field = chunk.source_field();
  for (size_t i = 0; i < row_count; ++i) {
    if (chunk.nullable(i)) continue;

    const uchar *row_data = chunk.data(i);
    my_decimal val;
    source_field->set_field_ptr(const_cast<uchar *>(row_data));
    source_field->val_decimal(&val);

    // Use decimal_add for addition
    my_decimal temp_result;
    decimal_add(&sum, &val, &temp_result);
    sum = temp_result;
  }
  return sum;
}

template <>
my_decimal ColumnChunkOper::genericMin<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  my_decimal min_val;
  bool found = false;

  Field *source_field = chunk.source_field();
  for (size_t i = 0; i < row_count; ++i) {
    if (chunk.nullable(i)) continue;

    const uchar *row_data = chunk.data(i);
    my_decimal val;
    source_field->set_field_ptr(const_cast<uchar *>(row_data));
    source_field->val_decimal(&val);

    if (!found || decimal_cmp(&val, &min_val) < 0) {
      min_val = val;
      found = true;
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
    if (!chunk.nullable(i)) continue;
    const uchar *row_data = chunk.data(i);
    my_decimal val;
    source_field->set_field_ptr(const_cast<uchar *>(row_data));
    source_field->val_decimal(&val);

    if (!found || decimal_cmp(&val, &max_val) > 0) {
      max_val = val;
      found = true;
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
    if (!chunk.nullable(i)) continue;

    const uchar *row_data = chunk.data(i);
    my_decimal val;
    source_field->set_field_ptr(const_cast<uchar *>(row_data));
    source_field->val_decimal(&val);

    if (predicate(val)) {
      output_indices.push_back(i);
      count++;
    }
  }
  return count;
}

/**
 * @brief my_decimal specialization: Sum operation
 */
template <>
my_decimal ColumnChunkOper::Sum<my_decimal>(const ColumnChunk &chunk, size_t row_count) {
  // Get Field scale information
  Field *source_field = chunk.source_field();
  uint scale = get_decimal_scale(source_field);

  // Extract data for SIMD acceleration
  std::vector<int64_t> data_buffer;
  extract_decimal_for_simd(chunk, row_count, data_buffer);

  my_decimal result;
  if (!data_buffer.empty()) {
    // Use SIMD-accelerated sum (for int64_t)
    int64_t sum = Utils::SIMD::sum<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);

    // Convert back to my_decimal using correct scale
    int64_to_my_decimal(sum, scale, &result);
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
  extract_decimal_for_simd(chunk, row_count, data_buffer);

  my_decimal result;
  if (!data_buffer.empty()) {
    int64_t min_val = Utils::SIMD::min<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);

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
  extract_decimal_for_simd(chunk, row_count, data_buffer);

  my_decimal result;
  if (!data_buffer.empty()) {
    int64_t max_val = Utils::SIMD::max<int64_t>(data_buffer.data(), chunk.get_null_mask()->data, row_count);

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
double ColumnChunkOper::Average<my_decimal>(ColumnChunk &chunk, size_t row_count) {
  my_decimal sum = Sum<my_decimal>(chunk, row_count);
  size_t non_null_count = CountNonNull(chunk, row_count);

  if (non_null_count == 0) return 0.0;

  // Convert my_decimal to double
  double sum_val = 0.0;
  decimal2double(&sum, &sum_val);

  return sum_val / non_null_count;
}
}  // namespace Executor
}  // namespace ShannonBase
