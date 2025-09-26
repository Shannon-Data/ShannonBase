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

#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/SIMD.h"
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
  virtual ~ColumnChunk() = default;

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
    assert(index < m_chunk_size);
    ShannonBase::Utils::Util::bit_array_set(m_null_mask.get(), index);
  }

  // to tell indexth is null or not.
  inline bool nullable(size_t index) {
    assert(m_null_mask.get());
    assert(index < m_chunk_size);
    return ShannonBase::Utils::Util::bit_array_get(m_null_mask.get(), index);
  }

  bool add(uchar *data, size_t length, bool null);
  bool add_batch(const std::vector<std::pair<const uchar *, size_t>> &data_batch, const std::vector<bool> &null_flags);

  inline const uchar *data(size_t index) const {
    assert(index < m_current_size.load(std::memory_order_relaxed));
    return m_cols_buffer.get() + (index * m_field_width);
  }

  inline uchar *mutable_data(size_t index) {
    assert(index < m_current_size.load(std::memory_order_relaxed));
    return m_cols_buffer.get() + (index * m_field_width);
  }

  inline bool empty() const { return m_current_size.load(std::memory_order_relaxed) == 0; }

  inline bool full() const { return m_current_size.load(std::memory_order_relaxed) >= m_chunk_size; }

  inline size_t size() const { return m_current_size.load(std::memory_order_relaxed); }

  inline size_t capacity() const { return m_chunk_size; }

  inline size_t width() const { return m_field_width; }

  inline size_t remaining() const { return m_chunk_size - m_current_size.load(std::memory_order_relaxed); }

  inline enum_field_types field_type() const { return m_type; }

  inline Field *source_field() const { return m_source_fld; }

  void clear() {
    m_current_size.store(0, std::memory_order_release);

    if (m_null_mask) {
      memset(m_null_mask.get()->data, 0x0, m_null_mask.get()->size);
    }

#ifndef NDEBUG
    if (m_cols_buffer) {
      std::memset((void *)m_cols_buffer.get(), 0, m_chunk_size * m_field_width);
    }
#endif
  }

  void resize(size_t new_size) {
    assert(new_size <= m_chunk_size);
    m_current_size.store(new_size, std::memory_order_release);
  }

  bool reserve(size_t additional_space) {
    size_t current = m_current_size.load(std::memory_order_relaxed);
    return (current + additional_space) <= m_chunk_size;
  }

  size_t compact();

  struct MemoryUsage {
    size_t data_buffer_bytes;
    size_t null_mask_bytes;
    size_t total_bytes;
    double utilization_ratio;
  };

  MemoryUsage get_memory_usage() const {
    MemoryUsage usage{};
    usage.data_buffer_bytes = m_chunk_size * m_field_width;
    usage.null_mask_bytes = (m_chunk_size + 7) / 8;
    usage.total_bytes = usage.data_buffer_bytes + usage.null_mask_bytes + sizeof(*this);

    size_t current = m_current_size.load(std::memory_order_relaxed);
    usage.utilization_ratio = m_chunk_size > 0 ? static_cast<double>(current) / m_chunk_size : 0.0;

    return usage;
  }

  inline ShannonBase::bit_array_t *get_null_mask() { return m_null_mask.get(); }

 private:
  inline void initialize_buffers();

  void copy_from(const ColumnChunk &other);

  void swap(ColumnChunk &other);

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

/**
 * Usage examples for ColumnChunkOper:
 *
 * ColumnChunk chunk(...);
 *
 * // Sum operation
 * double sum = ColumnChunkOper::Sum<double>(chunk, row_count);
 *
 * // Non-null count
 * size_t non_null_count = ColumnChunkOper::CountNonNull(chunk, row_count);
 *
 * // Filter operation using filter_func_t
 * std::vector<size_t> indices;
 * filter_func_t pred = [](const uchar* data) {
 *     return *reinterpret_cast<const int*>(data) > 100;
 * };
 * size_t filtered_count = ColumnChunkOper::Filter(chunk, row_count, pred, indices);
 *
 * // Filter operation using template version
 * std::vector<size_t> indices2;
 * size_t filtered_count2 = ColumnChunkOper::Filter<int>(chunk, row_count,
 *     [](int val) { return val > 100; }, indices2);
 *
 * // Memory usage statistics
 * ColumnChunkOper::PrintMemoryUsage(chunk);
 */
class ColumnChunkOper {
 public:
  // Constructors / Destructor
  ColumnChunkOper() = default;
  ~ColumnChunkOper() = default;

  // Disable copy & move operations
  ColumnChunkOper(const ColumnChunkOper &) = delete;
  ColumnChunkOper &operator=(const ColumnChunkOper &) = delete;
  ColumnChunkOper(ColumnChunkOper &&) = delete;
  ColumnChunkOper &operator=(ColumnChunkOper &&) = delete;

  /**
   * @brief Compute the sum of all non-null values in the column.
   * @tparam T - arithmetic type (int, double, etc.)
   * @param chunk - input column data
   * @param row_count - number of rows to process
   */
  template <typename T>
  static T Sum(const ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");

    // Extract contiguous data buffer for potential SIMD optimization
    std::vector<T> data_buffer;
    std::vector<uint8_t> null_mask_buffer;
    extract_data_for_simd(chunk, row_count, data_buffer, null_mask_buffer);

    if (!data_buffer.empty()) {
      // Use SIMD accelerated sum if data is available
      return Utils::SIMD::sum(data_buffer.data(), null_mask_buffer.empty() ? nullptr : null_mask_buffer.data(),
                              row_count);
    }

    // Fallback to generic scalar implementation
    return genericSum<T>(chunk, row_count);
  }

  /**
   * @brief Count the number of non-null rows.
   * @param chunk - input column data
   * @param row_count - number of rows to process
   */
  static size_t CountNonNull(ColumnChunk &chunk, size_t row_count) {
    if (!chunk.get_null_mask()) {
      return row_count;  // No null mask means all rows are non-null
    }

    // Use SIMD-optimized popcount if null mask is present
    return Utils::SIMD::count_non_null(chunk.get_null_mask()->data, row_count);
  }

  /**
   * @brief Compute the minimum value in the column (ignoring nulls).
   */
  template <typename T>
  static T Min(const ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");

    std::vector<T> data_buffer;
    std::vector<uint8_t> null_mask_buffer;
    extract_data_for_simd(chunk, row_count, data_buffer, null_mask_buffer);

    if (!data_buffer.empty()) {
      return Utils::SIMD::min(data_buffer.data(), null_mask_buffer.empty() ? nullptr : null_mask_buffer.data(),
                              row_count);
    }

    return genericMin<T>(chunk, row_count);
  }

  /**
   * @brief Compute the maximum value in the column (ignoring nulls).
   */
  template <typename T>
  static T Max(const ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");

    std::vector<T> data_buffer;
    std::vector<uint8_t> null_mask_buffer;
    extract_data_for_simd(chunk, row_count, data_buffer, null_mask_buffer);

    if (!data_buffer.empty()) {
      return Utils::SIMD::max(data_buffer.data(), null_mask_buffer.empty() ? nullptr : null_mask_buffer.data(),
                              row_count);
    }

    return genericMax<T>(chunk, row_count);
  }

  /**
   * @brief Filter rows using a raw pointer-based predicate function.
   * @return number of matched rows
   */
  static size_t Filter(ColumnChunk &chunk, size_t row_count, filter_func_t predicate,
                       std::vector<size_t> &output_indices) {
    return genericFilter(chunk, row_count, predicate, output_indices);
  }

  /**
   * @brief Filter rows using a templated std::function-based predicate.
   *        If possible, extracts data for SIMD-based filtering.
   */
  template <typename T>
  static size_t Filter(const ColumnChunk &chunk, size_t row_count, std::function<bool(T)> predicate,
                       std::vector<size_t> &output_indices) {
    std::vector<T> data_buffer;
    std::vector<uint8_t> null_mask_buffer;
    extract_data_for_simd(chunk, row_count, data_buffer, null_mask_buffer);

    if (!data_buffer.empty()) {
      return Utils::SIMD::filter(data_buffer.data(), null_mask_buffer.empty() ? nullptr : null_mask_buffer.data(),
                                 row_count, predicate, output_indices);
    }

    return genericFilter<T>(chunk, row_count, predicate, output_indices);
  }

  /**
   * @brief Compute the average value (mean) ignoring nulls.
   */
  template <typename T>
  static double Average(ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");

    T sum = Sum<T>(chunk, row_count);
    size_t non_null_count = CountNonNull(chunk, row_count);

    if (non_null_count == 0) return 0.0;
    return static_cast<double>(sum) / non_null_count;
  }

  /**
   * @brief Compute sample standard deviation (N-1 denominator).
   */
  template <typename T>
  static double StdDev(ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");

    double mean = Average<T>(chunk, row_count);
    double sum_sq = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        T val = *reinterpret_cast<const T *>(row_data);
        double diff = static_cast<double>(val) - mean;
        sum_sq += diff * diff;
        count++;
      }
    }

    if (count <= 1) return 0.0;
    return std::sqrt(sum_sq / (count - 1));
  }

  /**
   * @brief Print memory usage statistics for the column.
   */
  static void PrintMemoryUsage(ColumnChunk &chunk) {
    auto usage = chunk.get_memory_usage();
    size_t non_null_count = CountNonNull(chunk, chunk.size());

    printf("Memory Usage:\n");
    printf("  Data Buffer: %zu bytes\n", usage.data_buffer_bytes);
    printf("  Null Mask: %zu bytes\n", usage.null_mask_bytes);
    printf("  Total: %zu bytes\n", usage.total_bytes);
    printf("  Utilization: %.2f%%\n", usage.utilization_ratio * 100);
    printf("  Non-null Count: %zu\n", non_null_count);
    printf("  Null Count: %zu\n", chunk.size() - non_null_count);
  }

 private:
  /**
   * @brief Extract contiguous data + null mask for SIMD operations.
   *        This allows vectorized processing for Sum/Min/Max/Filter.
   */
  template <typename T>
  static void extract_data_for_simd(ColumnChunk &chunk, size_t row_count, std::vector<T> &data_buffer,
                                    std::vector<uint8_t> &null_mask_buffer) {
    data_buffer.resize(row_count);
    null_mask_buffer.resize((row_count + 7) / 8, 0xFF);  // Assume all non-null initially

    for (size_t i = 0; i < row_count; ++i) {
      const uchar *row_data = chunk.data(i);
      data_buffer[i] = *reinterpret_cast<const T *>(row_data);

      // Clear the corresponding bit if row is null
      if (chunk.nullable(i)) {
        size_t byte_index = i / 8;
        size_t bit_index = i % 8;
        null_mask_buffer[byte_index] &= ~(1 << bit_index);
      }
    }
  }

  // === Generic fallback implementations (scalar) ===

  template <typename T>
  static T genericSum(ColumnChunk &chunk, size_t row_count) {
    T sum = 0;
    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        sum += *reinterpret_cast<const T *>(row_data);
      }
    }
    return sum;
  }

  template <typename T>
  static T genericMin(ColumnChunk &chunk, size_t row_count) {
    T min_val = std::numeric_limits<T>::max();
    bool found = false;

    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        T val = *reinterpret_cast<const T *>(row_data);
        if (val < min_val) {
          min_val = val;
          found = true;
        }
      }
    }

    return found ? min_val : static_cast<T>(0);
  }

  template <typename T>
  static T genericMax(ColumnChunk &chunk, size_t row_count) {
    T max_val = std::numeric_limits<T>::lowest();
    bool found = false;

    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        T val = *reinterpret_cast<const T *>(row_data);
        if (val > max_val) {
          max_val = val;
          found = true;
        }
      }
    }

    return found ? max_val : static_cast<T>(0);
  }

  static size_t genericFilter(ColumnChunk &chunk, size_t row_count, filter_func_t predicate,
                              std::vector<size_t> &output_indices) {
    size_t count = 0;
    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        if (predicate(row_data)) {
          output_indices.push_back(i);
          count++;
        }
      }
    }
    return count;
  }

  template <typename T>
  static size_t genericFilter(ColumnChunk &chunk, size_t row_count, std::function<bool(T)> predicate,
                              std::vector<size_t> &output_indices) {
    size_t count = 0;
    for (size_t i = 0; i < row_count; ++i) {
      if (!chunk.nullable(i)) {
        const uchar *row_data = chunk.data(i);
        T val = *reinterpret_cast<const T *>(row_data);
        if (predicate(val)) {
          output_indices.push_back(i);
          count++;
        }
      }
    }
    return count;
  }
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ITERATOR_H__