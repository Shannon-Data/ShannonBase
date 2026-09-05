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

   Vectorized window function execution for the Rapid engine.
*/
#ifndef __SHANNONBASE_WINDOW_ITERATOR_H__
#define __SHANNONBASE_WINDOW_ITERATOR_H__

#include <cstdint>
#include <cstdio>
#include <vector>

#include "sql-common/my_decimal.h"
#include "sql/iterators/row_iterator.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"

class Field;
class Item;
class Item_sum;
class JOIN;
class TABLE;
class Temp_table_param;
class Window;

namespace ShannonBase {
namespace Executor {
/**
 * Vectorized replacement for MySQL's BufferingWindowIterator.
 *
 * MySQL evaluates a buffered window by materializing every input row into the
 * window's frame-buffer temporary table and then walking that table back once
 * per output row, calling Item_sum::add() per visited row. For the frames that
 * dominate analytic workloads -- "UNBOUNDED PRECEDING" to either CURRENT ROW or
 * UNBOUNDED FOLLOWING -- the whole frame walk is redundant: the aggregate over
 * such a frame is a running (prefix) aggregate, so a single pass suffices.
 *
 * This operator keeps MySQL's data flow verbatim (same input/output slices,
 * same copy_funcs() phases, results stored into the very same result fields)
 * but replaces the frame buffer with in-memory columnar buffers and the
 * per-row Item_sum::add() loop with column-at-a-time kernels:
 *
 *  - Window function arguments are evaluated once per input row into typed
 *    column vectors (int64 / double / my_decimal + a NULL bitmap).
 *  - Frames ending at UNBOUNDED FOLLOWING, and RANGE frames ending at CURRENT
 *    ROW, produce one value per partition / per peer group; those are computed
 *    with the SIMD reductions in Utils::SIMD over the column vector.
 *  - ROWS frames ending at CURRENT ROW need one value per row and use a tight
 *    prefix scan over the same column vector.
 *
 * Only the output record image (a fixed-length temp-table record) is buffered,
 * and only for as long as its result is not yet determined, so a running frame
 * streams with a bounded buffer. Whole-partition frames buffer the partition
 * and spill record images to a temporary file past a memory budget, exactly
 * where MySQL would have spilled its frame buffer to disk.
 *
 * Anything outside the supported subset (see CanVectorize()) keeps using
 * MySQL's own window iterators; this class is never constructed for those.
 */
class VectorizedWindowIterator final : public RowIterator {
 public:
  /// Rows ingested per vectorized pass.
  static constexpr size_t kBatchRows = 1024;
  /**
   * Default budget for buffered output record images before spilling.
   *
   * It bounds the record images and nothing else. The argument columns are
   * folded into the accumulators and dropped after every pass, and a computed
   * result is dropped with the rows that consumed it, so both stay bounded.
   * m_new_peer does not: it is only released when the buffer drains, which a
   * whole-partition frame does not do until the partition closes, so it grows
   * with the partition however small this budget is. It is one *bit* per
   * buffered row, so a partition just large enough to spill at 64MB with
   * 100-byte records holds ~84KB there; still linear, but a factor of eight
   * off what a byte per row would cost, and small enough beside the record
   * images that a separate budget would not buy anything. Bounding it properly
   * would mean spilling it alongside the record image.
   */
  static constexpr size_t kDefaultBufferMemoryLimit = 64ULL * 1024ULL * 1024ULL;

  /**
   * Whether this window step can be evaluated by this operator.
   *
   * Checked once while building iterators; a false answer leaves the plan on
   * MySQL's WindowIterator/BufferingWindowIterator, so the supported subset can
   * grow without any caller change.
   */
  static bool CanVectorize(const Temp_table_param *param);

  VectorizedWindowIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, Temp_table_param *temp_table_param,
                           JOIN *join, int output_slice, size_t buffer_memory_limit = kDefaultBufferMemoryLimit);
  ~VectorizedWindowIterator() override;

  bool Init() override;
  int Read() override;

  void SetNullRowFlag(bool is_null_row) override { m_source->SetNullRowFlag(is_null_row); }
  void StartPSIBatchMode() override { m_source->StartPSIBatchMode(); }
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }
  void UnlockRow() override {}

  /**
   * Per-execution counters: rows in/out, batches, SIMD vs scalar rows, spilled
   * rows and bytes, record bytes copied.
   *
   * PublishStats() folds them into the rapid_query_vectorized_window_*_total
   * status counters when the operator restarts or is destroyed, so the SIMD /
   * scalar split and the spill volume are observable per server rather than
   * only in a debugger.
   */
  const VectorizedOperatorStats &stats() const { return m_stats; }

 private:
  /// The window functions this operator evaluates natively.
  enum class WfKind : uint8_t { kRowNumber = 0, kRank, kDenseRank, kCount, kSum, kAvg, kMin, kMax };

  /// Value domain of an argument column or of a produced result.
  enum class ValueKind : uint8_t { kNone = 0, kInt, kUint, kReal, kDecimal };

  /// How many rows share one computed framing value.
  enum class Granularity : uint8_t {
    /// ROWS ... CURRENT ROW: a running value per row. Not reachable today --
    /// such a window does not need buffering, so it never gets this far; see
    /// the note in CanVectorize().
    kPerRow = 0,
    kPerPeerGroup,  ///< RANGE ... CURRENT ROW: one value per ORDER BY peer group.
    kPerPartition   ///< ... UNBOUNDED FOLLOWING: one value for the whole partition.
  };

  /**
   * One evaluated argument expression, in columnar form.
   *
   * Scratch only: a column holds the rows of the current vectorized pass and is
   * cleared once they have been folded into the accumulators, so its footprint
   * is bounded by kBatchRows regardless of partition size.
   */
  struct ArgColumn {
    Item *item{nullptr};
    ValueKind kind{ValueKind::kNone};
    /// COUNT() only needs to know whether the argument was NULL.
    bool null_only{false};
    /// Values are at most 32 bits wide, so an int64 SIMD reduction is exact.
    bool narrow_int{false};

    std::vector<int64_t> ints;
    std::vector<double> reals;
    std::vector<my_decimal> decimals;
    /// Bit array, bit set == NULL, laid out as Utils::SIMD expects.
    std::vector<uint8_t> null_bits;
    size_t rows{0};
    size_t null_count{0};

    void Clear();
    void Reserve(size_t rows);
    void AppendNull();
  };

  /**
   * Exact-and-fast running sum of integer arguments.
   *
   * MySQL accumulates SUM()/AVG() over an integer argument in my_decimal, one
   * decimal addition per row. Integers add exactly in int64 until they
   * overflow, so accumulate there and fold into a decimal carry only on the
   * (astronomically rare) overflow. The final value is identical.
   */
  struct IntSumAccumulator {
    int64_t acc{0};
    my_decimal carry;
    bool carry_used{false};

    void Reset();
    void Add(int64_t value);
    void AddUnsigned(uint64_t value);
    void Value(my_decimal *out) const;

   private:
    void FoldIntoCarry();
  };

  /// One window function of this step plus its partition-scoped state.
  struct WindowFn {
    Item_sum *item{nullptr};
    Field *result_field{nullptr};
    WfKind kind{WfKind::kRowNumber};
    bool framing{false};
    size_t arg_col{SIZE_MAX};
    ValueKind result_kind{ValueKind::kNone};
    bool result_unsigned{false};
    uint prec_increment{4};

    // Partition-scoped accumulators.
    int64_t count{0};
    IntSumAccumulator int_sum;
    my_decimal dec_sum;
    double real_sum{0.0};
    bool has_extreme{false};
    int64_t ext_int{0};
    double ext_real{0.0};
    my_decimal ext_dec;

    // Computed values, indexed per Granularity.
    std::vector<uint8_t> res_null;
    std::vector<int64_t> res_int;
    std::vector<double> res_real;
    std::vector<my_decimal> res_dec;

    void ResetPartition();
    void ClearResults();
    void EraseResultPrefix(size_t count);
  };

  // setup
  bool SetupPlan();
  size_t AddArgColumn(Item *item, ValueKind kind, bool null_only);

  // pipeline
  int IngestBatch();
  bool EvaluateArguments();
  void EraseArgPrefix(size_t count);
  bool Compute();
  int EmitRow();
  void ErasePrefix(size_t rows);
  void StartNewPartition();

  // kernels
  void AccumulateSegment(WindowFn *fn, size_t from, size_t to);
  void AppendResult(WindowFn *fn);
  bool StoreResult(const WindowFn &fn, size_t index, int64_t row_number, int64_t rank, int64_t dense_rank);

  /// Fold m_stats into the global counters and start it over.
  void PublishStats();

  // record image buffer
  bool AppendRecord();
  bool LoadRecord(size_t row);
  bool SpillRecord(const uchar *record);
  void ResetRecordBuffer();

  unique_ptr_destroy_only<RowIterator> m_source;
  Temp_table_param *m_temp_table_param;
  Window *m_window;
  JOIN *m_join;
  int m_input_slice{-1};
  int m_output_slice;

  /// The temporary table holding this window step's output record.
  TABLE *m_out_table{nullptr};
  size_t m_record_length{0};

  std::vector<WindowFn> m_functions;
  std::vector<ArgColumn> m_arg_columns;

  Granularity m_granularity{Granularity::kPerRow};

  // Buffered rows of the current block.
  std::vector<uchar> m_records;
  /// One bit per buffered row: set when the row opens a new ORDER BY peer
  /// group (or a new partition). Same layout as ArgColumn::null_bits.
  std::vector<uint8_t> m_new_peer;
  size_t m_rows{0};
  /// Rows of the current partition ingested so far. Unlike m_rows this is not
  /// reset by a drain, so it is what tells a partition change apart from the
  /// very first row of the input.
  size_t m_part_rows{0};
  size_t m_accum_rows{0};
  size_t m_emit_end{0};
  size_t m_emit_row{0};
  bool m_partition_closed{false};
  bool m_eof{false};
  bool m_done{false};

  // Emit-time ranking counters (partition scoped).
  int64_t m_emit_part_rowno{1};
  int64_t m_emit_rank{1};
  int64_t m_emit_dense_rank{0};
  /// Index of the peer group the emit cursor is in, within the buffered block.
  size_t m_emit_group{0};

  /// The row that ended the previous partition, held back until that partition
  /// has been fully emitted. Its arguments already sit at the head of the
  /// argument columns.
  std::vector<uchar> m_carry_record;
  bool m_carry_valid{false};

  // Record images past the memory budget live here, written and read strictly
  // in row order. Only reachable for whole-partition frames, where no row is
  // emitted before the partition closes.
  size_t m_buffer_memory_limit;
  size_t m_mem_rows{0};
  std::FILE *m_spill_file{nullptr};
  size_t m_spill_rows{0};
  size_t m_spill_read{0};
  bool m_spill_reading{false};

  VectorizedOperatorStats m_stats;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  //__SHANNONBASE_WINDOW_ITERATOR_H__
