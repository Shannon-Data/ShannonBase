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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>
#include "include/my_inttypes.h"
#include "sql-common/my_decimal.h"
#include "sql/field.h"

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/row_iterator.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/utils/SIMD.h"
#include "storage/rapid_engine/utils/utils.h"
class Field;
class Item;
namespace ShannonBase {
namespace Executor {

/*
 * Batch transport and the tiny execution kernels below are implementation
 * vocabulary of Rapid iterators. Keep them with the iterator abstraction
 * instead of exposing one-header batch/kernel subdirectories whose only
 * consumers are iterators.
 */
namespace Batch {
using SelectionVector = std::vector<uint32_t>;
}  // namespace Batch

namespace Kernels {
inline Utils::SIMD::SIMDType RuntimeSimdType() { return Utils::SIMD::simd_support(); }

inline bool HasRuntimeSimd() { return RuntimeSimdType() != Utils::SIMD::SIMDType::NONE; }

template <typename T>
inline T Min(const T *data, const uint8_t *null_mask, size_t rows) {
  return Utils::SIMD::min<T>(data, null_mask, rows);
}

template <typename T>
inline T Max(const T *data, const uint8_t *null_mask, size_t rows) {
  return Utils::SIMD::max<T>(data, null_mask, rows);
}

inline size_t FilterCompareInt32(const int32_t *data, const uint8_t *null_mask, size_t rows, int32_t value,
                                 Utils::SIMD::CompareOp op, std::vector<size_t> &selection) {
  return Utils::SIMD::filter_compare_int32(data, null_mask, rows, value, op, selection);
}

inline size_t FilterCompareInt64(const int64_t *data, const uint8_t *null_mask, size_t rows, int64_t value,
                                 Utils::SIMD::CompareOp op, std::vector<size_t> &selection) {
  return Utils::SIMD::filter_compare_int64(data, null_mask, rows, value, op, selection);
}

inline bool CompareUsesSimd(bool is_int32, Utils::SIMD::CompareOp op) {
  const auto isa = RuntimeSimdType();
  if (isa == Utils::SIMD::SIMDType::AVX2) return true;
  return is_int32 && op == Utils::SIMD::CompareOp::EQ && isa != Utils::SIMD::SIMDType::NONE &&
         isa != Utils::SIMD::SIMDType::NEON;
}
}  // namespace Kernels

/**
 * Common software counters for vectorized operators.
 *
 * Hardware PMCs (cycles/instructions/LLC/branch misses) intentionally stay in
 * the perf harness; reading perf_event counters in every hot loop would distort
 * the very path we are trying to measure.
 */
struct VectorizedOperatorStats {
  size_t rows_in{0};
  size_t rows_out{0};
  size_t batches{0};
  size_t row_materializations{0};
  size_t bytes_copied{0};
  size_t simd_rows{0};
  size_t scalar_fallback_rows{0};
  size_t hash_probes{0};
  size_t hash_collisions{0};
  size_t spill_rows{0};
  size_t spill_bytes{0};

  double avg_batch_rows() const {
    return batches == 0 ? 0.0 : static_cast<double>(rows_in) / static_cast<double>(batches);
  }
};

using filter_func_t = std::function<bool(const uchar *)>;

class ColumnChunk {
 public:
  // ColumnChunk is deliberately single-consumer. Parallel operators must own
  // disjoint chunks/partitions and merge at an operator boundary; sharing one
  // chunk between workers is not supported. This keeps the hot append path free
  // of per-value atomics.
  // ctor
  ColumnChunk(Field *mysql_fld, size_t size);

  // Same, but with an explicit element width instead of the IMCS normalized
  // width. Callers that store a full MySQL record image (Field::pack_length())
  // rather than IMCS column bytes must use this: for VARCHAR/CHAR the two
  // differ (normalized_length() is the 4-byte dictionary id), and silently
  // clamping a record image to 4 bytes loses the value.
  ColumnChunk(Field *mysql_fld, size_t size, size_t field_width);

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

  inline bool valid() const { return m_source_fld && (m_cols_buffer.get() != nullptr); }
  // set the row-indexth is null.
  inline void set_null(size_t index) {
    assert(m_null_mask.get());
    assert(index < m_chunk_size);
    ShannonBase::Utils::Util::bit_array_set(m_null_mask.get(), index);
  }

  // to tell indexth is null or not.
  inline bool nullable(size_t index) const {
    assert(m_null_mask.get());
    assert(index < m_chunk_size);
    return ShannonBase::Utils::Util::bit_array_get(m_null_mask.get(), index);
  }

  // Hot-path version: uses cached raw pointer + bitwise ops, no bounds check.
  // Caller guarantees index < m_chunk_size and m_null_mask_data is valid.
  inline bool nullable_fast(size_t index) const {
    return ShannonBase::Utils::Util::bit_array_get_fast(m_null_mask_data, index);
  }

  // Hot-path version: uses cached raw pointer, no atomic load for assert.
  // Caller guarantees index < current batch size (stable during PopulateCurrentRow).
  inline const uchar *data_fast(size_t index) const { return m_cols_buffer_data + (index * m_field_width); }

  bool add(const uchar *data, size_t length, bool null);
  bool add_batch(const std::vector<std::pair<const uchar *, size_t>> &data_batch, const std::vector<bool> &null_flags);

  // Append one row without repeated metadata lookups. Single-consumer only.
  bool append_from(const ColumnChunk &source, size_t source_row);

  // Compact rows in-place according to a monotonically increasing selection.
  // Used by batch-preserving filters; no temporary column buffers are needed.
  bool gather_in_place(const std::vector<uint32_t> &selection, size_t *bytes_moved = nullptr);

  inline uchar *mutable_data_base() noexcept { return m_cols_buffer_data; }
  inline uint8_t *mutable_null_mask_data() noexcept { return m_null_mask_data; }

  // remove the last row data.
  inline bool remove() {
    if (m_current_size == 0) return true;
    --m_current_size;
    return true;
  }

  inline const uchar *data(size_t index) const {
    assert(index < m_current_size);
    return m_cols_buffer.get() + (index * m_field_width);
  }

  inline uchar *mutable_data(size_t index) {
    assert(index < m_current_size);
    return m_cols_buffer.get() + (index * m_field_width);
  }

  inline bool empty() const { return m_current_size == 0; }

  inline bool full() const { return m_current_size >= m_chunk_size; }

  inline size_t size() const { return m_current_size; }

  inline size_t capacity() const { return m_chunk_size; }

  inline size_t width() const { return m_field_width; }

  inline size_t remaining() const { return m_chunk_size - m_current_size; }

  inline enum_field_types field_type() const { return m_type; }

  inline Field *source_field() const { return m_source_fld; }

  inline uint field_index() const { return m_field_index; }
  inline TABLE *table() const { return m_table; }

  /**
   * Logical reset only. Every append explicitly sets or resets its null bit and
   * overwrites every byte of a non-NULL fixed-width value, so clearing the whole
   * capacity here only burns memory bandwidth on the analytical hot path.
   */
  inline void clear() noexcept { m_current_size = 0; }

  void resize(size_t new_size) {
    assert(new_size <= m_chunk_size);
    m_current_size = new_size;
  }

  bool reserve(size_t additional_space) { return (m_current_size + additional_space) <= m_chunk_size; }

  // Grow the backing buffers while preserving existing rows. Capacity is an
  // execution detail and must not be constrained by optimizer estimates.
  bool grow(size_t new_capacity);

  void reset(Field *mysql_fld, size_t chunk_size);

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

    size_t current = m_current_size;
    usage.utilization_ratio = m_chunk_size > 0 ? static_cast<double>(current) / m_chunk_size : 0.0;

    return usage;
  }

  inline ShannonBase::bit_array_t *get_null_mask() const { return m_null_mask.get(); }

 private:
  inline void initialize_buffers();

  void copy_from(const ColumnChunk &other);

  void swap(ColumnChunk &other);

 private:
  // source field pointer (may dangle; use field_index/table for metadata).
  Field *m_source_fld;

  uint16_t m_field_index{0};
  TABLE *m_table{nullptr};

  // data type in mysql.
  enum_field_types m_type{MYSQL_TYPE_NULL};

  size_t m_field_width{0};

  // current rows number of this chunk.
  size_t m_current_size{0};

  // VECTOR_WIDTH
  size_t m_chunk_size{0};

  // to keep the every column data in VECTOR_WIDTH count. the order is same with field order.
  std::unique_ptr<uchar[]> m_cols_buffer{nullptr};

  // Cached raw pointer to m_cols_buffer data — avoids unique_ptr::get() on hot path.
  uchar *m_cols_buffer_data{nullptr};

  // null bitmap of all data in this Column Chunk.
  std::unique_ptr<ShannonBase::bit_array_t> m_null_mask{nullptr};

  // Cached raw pointer to m_null_mask->data — avoids unique_ptr::get() + ->data
  // indirection on the nullable hot path.
  uint8_t *m_null_mask_data{nullptr};
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
#if defined(SHANNON_VECTORIZE_SUPPORT)
    if (row_count == 0 || !chunk.valid()) return T{};
    return Utils::SIMD::sum<T>(reinterpret_cast<const T *>(chunk.data(0)), chunk.get_null_mask()->data, row_count);
#else
    return genericSum<T>(chunk, row_count);
#endif
  }

  /**
   * @brief Count the number of non-null rows.
   * @param chunk - input column data
   * @param row_count - number of rows to process
   */
  static size_t CountNonNull(const ColumnChunk &chunk, size_t row_count) {
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
    static_assert(std::is_arithmetic_v<T> || std::is_same_v<std::decay_t<T>, my_decimal>,
                  "T must be arithmetic or my_decimal type");

#if defined(SHANNON_VECTORIZE_SUPPORT)
    return Utils::SIMD::min((T *)chunk.data(0), chunk.get_null_mask()->data, row_count);
#else
    return genericMin<T>(chunk, row_count);
#endif
  }

  /**
   * @brief Compute the maximum value in the column (ignoring nulls).
   */
  template <typename T>
  static T Max(const ColumnChunk &chunk, size_t row_count) {
    static_assert(std::is_arithmetic_v<T> || std::is_same_v<std::decay_t<T>, my_decimal>, "T must be arithmetic type");

#if defined(SHANNON_VECTORIZE_SUPPORT)
    return Utils::SIMD::max((T *)chunk.data(0), chunk.get_null_mask()->data, row_count);
#else
    return genericMax<T>(chunk, row_count);
#endif
  }

  /**
   * @brief Filter rows using a raw pointer-based predicate function.
   * @return number of matched rows
   */
  static size_t Filter(const ColumnChunk &chunk, size_t row_count, filter_func_t predicate,
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
#if defined(SHANNON_VECTORIZE_SUPPORT)
    return Utils::SIMD::filter((T *)chunk.data(0), chunk.get_null_mask()->data, row_count, predicate, output_indices);
#else
    return genericFilter<T>(chunk, row_count, predicate, output_indices);
#endif
  }

  /**
   * @brief Compute the average value (mean) ignoring nulls.
   */
  template <typename T>
  static double Average(const ColumnChunk &chunk, size_t row_count) {
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
  static double StdDev(const ColumnChunk &chunk, size_t row_count) {
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
  static void PrintMemoryUsage(const ColumnChunk &chunk) {
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
  static void extract_data_for_simd(const ColumnChunk &, size_t, std::vector<T> &, std::vector<uint8_t> &) {}

  // === Generic fallback implementations (scalar) ===
  template <typename T>
  static T genericSum(const ColumnChunk &chunk, size_t row_count) {
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
  static T genericMin(const ColumnChunk &chunk, size_t row_count) {
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
  static T genericMax(const ColumnChunk &chunk, size_t row_count) {
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

  static size_t genericFilter(const ColumnChunk &chunk, size_t row_count, filter_func_t predicate,
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
  static size_t genericFilter(const ColumnChunk &chunk, size_t row_count, std::function<bool(T)> predicate,
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

template <>
my_decimal ColumnChunkOper::genericSum<my_decimal>(const ColumnChunk &chunk, size_t row_count);
template <>
my_decimal ColumnChunkOper::genericMin<my_decimal>(const ColumnChunk &chunk, size_t row_count);

template <>
my_decimal ColumnChunkOper::genericMax<my_decimal>(const ColumnChunk &chunk, size_t row_count);

template <>
size_t ColumnChunkOper::genericFilter<my_decimal>(const ColumnChunk &chunk, size_t row_count,
                                                  std::function<bool(my_decimal)> predicate,
                                                  std::vector<size_t> &output_indices);

template <>
my_decimal ColumnChunkOper::Sum<my_decimal>(const ColumnChunk &chunk, size_t row_count);

template <>
my_decimal ColumnChunkOper::Min<my_decimal>(const ColumnChunk &chunk, size_t row_count);

template <>
my_decimal ColumnChunkOper::Max<my_decimal>(const ColumnChunk &chunk, size_t row_count);

template <>
size_t ColumnChunkOper::Filter<my_decimal>(const ColumnChunk &chunk, size_t row_count,
                                           std::function<bool(my_decimal)> predicate,
                                           std::vector<size_t> &output_indices);

template <>
double ColumnChunkOper::Average<my_decimal>(const ColumnChunk &chunk, size_t row_count);

/**
 * Iterators that can deliver columnar data in batches implement this interface.
 * Callers probe via dynamic_cast<BatchReadable*> once during Init();
 */
class BatchReadable {
 public:
  virtual ~BatchReadable() = default;

  /**
   * Whether this iterator can currently serve ReadBatch() at all.
   *
   * Implementing the interface is a compile-time property; being able to
   * produce columnar batches is a per-execution one. An operator whose retained
   * columns hold full MySQL record images (rather than IMCS normalized column
   * bytes) cannot hand them to a caller that laid out its chunks by
   * Util::normalized_length(). Consumers must check this right after the
   * dynamic_cast probe and stay on the row path when it returns false.
   */
  virtual bool SupportsBatchRead() const { return true; }

  /**
   * Fill col_chunks with up to `capacity` rows directly from columnar storage.
   * Caller must clear each chunk before calling.
   *
   * @param col_chunks  Pre-allocated column chunks (caller owns).
   * @param capacity    Maximum rows to fill.
   * @param rows_read   [out] Rows actually written.
   * @return  0                   success, rows_read > 0
   *          HA_ERR_END_OF_FILE  EOF; rows_read may be > 0 (last partial batch)
   *          other               error
   */
  virtual int ReadBatch(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) = 0;

  /**
   * Push rows [from_row, total_rows) back into an internal lookahead buffer
   * so the next ReadBatch() re-delivers them first.
   *
   * Called by VectorizedAggregateIterator when a GROUP BY boundary is detected
   * inside a batch: rows after the boundary belong to the next group and must
   * not be lost.
   *
   * @return true if the tail could not be buffered. The caller must treat this
   *         as an error: the rows are dropped, and silently continuing would
   *         truncate the query result.
   */
  virtual bool PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows) = 0;
};

/**
 * Shared implementation for BatchReadable::PushbackBatchTail(). Copies rows [from_row, total_rows)
 * from `chunks` into `*lookahead_chunks`, (re)allocating it to match `chunks`' layout only when
 * the existing buffer can't hold the tail or its layout has drifted, and resets
 * `*lookahead_start`/`*lookahead_count` to describe the buffered range. On a failed append, resets
 * the lookahead state to empty rather than leaving a partially-written buffer.
 *
 * Every BatchReadable iterator (table scan, filter, hash join) needs identical pushback logic;
 * this is the single implementation they all call instead of each carrying its own copy.
 *
 * @param bytes_copied  Optional running byte-count accumulator (e.g. an iterator's
 *                       VectorizedOperatorStats::bytes_copied); pass nullptr to skip tracking.
 */
bool PushbackBatchTailShared(const std::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows,
                             std::vector<ColumnChunk> *lookahead_chunks, size_t *lookahead_start,
                             size_t *lookahead_count, size_t *bytes_copied);

/**
 * Batch-preserving FILTER adapter.
 *
 * The fast path asks a BatchReadable child for a complete batch, evaluates the
 * residual predicate while only materializing fields referenced by the predicate,
 * then compacts all output columns in-place. Unsupported layouts transparently
 * use the ordinary RowIterator path but still return a batch to the parent.
 */
class VectorizedFilterIterator final : public RowIterator, public BatchReadable {
 public:
  VectorizedFilterIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, Item *condition);

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void StartPSIBatchMode() override;
  void EndPSIBatchModeIfStarted() override;
  void UnlockRow() override;

  int ReadBatch(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) override;
  bool PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows) override;

  const VectorizedOperatorStats &stats() const { return m_stats; }
  size_t row_materializations() const { return m_stats.row_materializations; }
  size_t vectorized_predicate_rows() const { return m_stats.simd_rows + m_stats.scalar_fallback_rows; }

 private:
  enum class SimplePredicateOp : uint8_t { NONE, EQ, NE, LT, LE, GT, GE };
  struct SimplePredicate {
    SimplePredicateOp op{SimplePredicateOp::NONE};
    Field *field{nullptr};
    longlong integer_constant{0};
    size_t chunk_idx{static_cast<size_t>(-1)};
    bool valid{false};
  };

  void CollectConditionFields();
  void CompileSimplePredicate();
  bool EvaluateSimplePredicateBatch(const std::vector<ColumnChunk> &chunks, size_t rows);
  bool BuildConditionChunkMap(const std::vector<ColumnChunk> &chunks);
  bool MaterializeConditionRow(const std::vector<ColumnChunk> &chunks, size_t row);
  int ReadBatchViaRows(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read);
  bool DrainLookahead(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t *rows_read);

  unique_ptr_destroy_only<RowIterator> m_source;
  Item *m_condition{nullptr};
  BatchReadable *m_batch_source{nullptr};
  std::vector<Field *> m_condition_fields;
  std::vector<size_t> m_condition_chunk_map;
  Batch::SelectionVector m_selection;
  std::vector<size_t> m_simd_selection;
  SimplePredicate m_simple_predicate;

  std::vector<ColumnChunk> m_lookahead_chunks;
  size_t m_lookahead_start{0};
  size_t m_lookahead_count{0};
  VectorizedOperatorStats m_stats;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ITERATOR_H__
