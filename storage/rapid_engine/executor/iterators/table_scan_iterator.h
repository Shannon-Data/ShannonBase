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
#ifndef __SHANNONBASE_TABLE_SCAN_ITERATOR_H__
#define __SHANNONBASE_TABLE_SCAN_ITERATOR_H__

#include <memory>
#include <mutex>

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/row_iterator.h"
#include "sql/mem_root_array.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/imcs/table0view.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {
class ART;
}
}  // namespace Imcs
}  // namespace ShannonBase

namespace ShannonBase {
namespace Executor {
namespace Morsel {

class ParallelScan final : public ParallelSource {
 public:
  ParallelScan(ResMgmt::BoundedMemoryResource *memory, const Rapid_scan_context *snapshot,
               const std::vector<std::shared_ptr<Imcs::Imcu>> &imcus,
               const std::vector<std::unique_ptr<Imcs::Predicate>> &predicates, const std::pmr::vector<Field *> &fields,
               bool storage_index);
  size_t tasks() const override { return m_queue.size(); }
  uint64_t max_ordinal() const override { return m_max_ordinal; }
  size_t extra_scratch_bytes() const override { return m_storage_scratch_bytes; }
  bool ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                   const Consumer &consume) override;

 private:
  // The foreground cursor owns the snapshot, reader pins and predicates and
  // cannot be advanced/destroyed until all workers have joined AND this
  // borrowed source has been destroyed. Workers only read the shared ReadView.
  const Rapid_scan_context *m_snapshot;
  const std::vector<std::shared_ptr<Imcs::Imcu>> &m_imcus;
  const std::vector<std::unique_ptr<Imcs::Predicate>> &m_predicates;
  std::pmr::vector<Field *> m_fields;
  std::pmr::vector<uint64_t> m_ordinal_bases;
  ResMgmt::BoundedMemoryResource::Reservation m_projection_charge;
  std::vector<uint32_t> m_projection;
  ParallelQueue m_queue;
  bool m_storage_index;
  uint64_t m_max_ordinal{0};
  size_t m_storage_scratch_bytes{0};
};

/**
 * Index-driven variant of ParallelScan.
 *
 * The ART cursor is single-threaded and stateful, so the candidate walk stays
 * on the coordinator: it is turned into per-IMCU runs of physical offsets
 * before any worker starts. Workers then materialize one run each, which is
 * where the cost is, and hand rows back with the ordinal the candidate had in
 * index order. Consumers merge on that ordinal, so runs may finish in any
 * order.
 *
 * Only an index without versioned keys may use this: the serial path then has
 * nothing to verify per candidate (visible_index_key_matches() returns early),
 * so a run of offsets is equivalent to the same run of ART entries.
 */
class ParallelIndexScan final : public ParallelSource {
 public:
  struct Run {
    size_t imcu;          // Index into the cursor's IMCU list.
    uint64_t ordinal;     // Index-order position of offsets[begin].
    size_t begin, count;  // Slice of the shared offset array.
  };

  // The ART walk that produces the runs is deferred to Collect(): doing it in
  // the constructor paid for a full index traversal even when the caller then
  // abandoned the parallel phase, which is exactly what a cost gate decides.
  ParallelIndexScan(ResMgmt::BoundedMemoryResource *memory, const Rapid_scan_context *snapshot,
                    const std::vector<std::shared_ptr<Imcs::Imcu>> &imcus,
                    const std::vector<std::unique_ptr<Imcs::Predicate>> &predicates,
                    const std::pmr::vector<Field *> &fields, uint32_t rows_per_imcu, Imcs::Index::ART *index,
                    size_t row_bound);
  size_t tasks() const override {
    const size_t runs = m_ready_runs.load(std::memory_order_acquire);
    // Before collection the candidate count is unknown; the row bound is a safe
    // upper estimate for worker sizing.
    return runs != 0 ? runs : std::max<size_t>(1, m_row_bound / ParallelQueue::kClaimRows);
  }
  uint64_t max_ordinal() const override { return m_max_ordinal.load(std::memory_order_acquire); }
  size_t extra_scratch_bytes() const override { return m_storage_scratch_bytes; }
  bool ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                   const Consumer &consume) override;

 private:
  // Walks the index once, on whichever worker arrives first. Later callers block
  // until it finishes, then read the finished run list.
  void Collect();

  const Rapid_scan_context *m_snapshot;
  const std::vector<std::shared_ptr<Imcs::Imcu>> &m_imcus;
  const std::vector<std::unique_ptr<Imcs::Predicate>> &m_predicates;
  std::pmr::vector<Field *> m_fields;
  std::vector<uint32_t> m_projection;
  std::pmr::vector<uint32_t> m_offsets;
  std::pmr::vector<Run> m_runs;
  std::once_flag m_collect;
  std::atomic<size_t> m_ready_runs{0};
  std::atomic<size_t> m_next_run{0};
  Imcs::Index::ART *m_index;
  size_t m_row_bound;
  uint32_t m_rows_per_imcu;
  std::atomic<uint64_t> m_max_ordinal{0};
  size_t m_storage_scratch_bytes{0};
};
}  // namespace Morsel
}  // namespace Executor
}  // namespace ShannonBase
/**
 * Usage:
 *
 * // 1. create and initialization.
 * auto iterator = std::make_unique<VectorizedTableScanIterator>(thd, table, expected_rows, &examined_rows);
 * if (iterator->Init()) {
 *   return HA_ERR_INITIALIZATION;
 * }
 *
 * // 2. read the data
 * while (true) {
 *   int result = iterator->Read();
 *   if (result == HA_ERR_END_OF_FILE) break;
 *   if (result != 0) return result;
 *
 *   // data has been fill up into table->field.
 *   ProcessRow(table);
 * }
 *
 * // 3. option：the performance metrics
 * size_t total_rows, total_batches;
 * double avg_batch_time, throughput;
 * iterator->GetPerformanceStats(&total_rows, &total_batches, &avg_batch_time, &throughput);
 */

class TABLE;
struct RapidShare;
namespace ShannonBase {
namespace Executor {
/**
 * VectorizedTableScanIterator - A vectorized table scan iterator that processes data in batches
 *
 * This iterator implements a vectorized execution model for table scanning, where data is
 * processed in batches rather than row-by-row. It leverages columnar storage and SIMD
 * optimizations to improve cache locality and CPU efficiency.
 */
class VectorizedTableScanIterator final : public TableRowIterator, public BatchReadable {
 public:
  bool PreservesBatchNullRows() const override { return true; }
  VectorizedTableScanIterator(THD *thd, TABLE *mtable, double expected_rows, ha_rows *examined_rows,
                              std::unique_ptr<Imcs::Predicate> predicate = nullptr,
                              const std::vector<uint32_t> &projection = {}, ha_rows limit = HA_POS_ERROR,
                              ha_rows offset = 0, bool use_storage_index = false, int index_no = -1,
                              bool reverse = false);

  ~VectorizedTableScanIterator() override;
  bool Init() override;
  int Read() override;

  /**
   * Fill col_chunks with up to `capacity` rows directly from ha_rapid,
   * bypassing table->field row-format conversion entirely.
   * col_chunks must contain one slot per TABLE field in field-index order;
   * unused fields are represented by invalid ColumnChunks.
   * Drains internal lookahead buffer first if non-empty.
   */
  int ReadBatch(std::pmr::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) override;
  bool SupportsLateMaterialization() const;
  int ReadPredicateBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows,
                         const std::pmr::vector<Field *> &fields);
  int CompleteProjection(std::pmr::vector<ColumnChunk> &chunks, const std::pmr::vector<uint32_t> &selection);

  // Synchronous consumer: the callback and its owner never escape this call.
  // 1 keeps the row, 0 rejects it, and -1 reports an execution error.
  using RuntimeFilter = int (*)(void *, const std::pmr::vector<ColumnChunk> &, size_t);
  bool SupportsRuntimeFilter() const { return m_limit == HA_POS_ERROR && m_offset == 0 && !m_filter; }
  int ReadFilteredBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows_read, void *owner,
                        RuntimeFilter filter);

  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override;
  std::unique_ptr<Morsel::ParallelSource> CreateParallelSource(ResMgmt::BoundedMemoryResource *memory,
                                                               const std::pmr::vector<Field *> &fields) override;

  /**
   * Cache rows [from_row, total_rows) into lookahead buffer so the next
   * ReadBatch() re-delivers them.  Called by VectorizedAggregateIterator
   * when a GROUP BY boundary is found mid-batch.
   */
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows) override;

  void set_filter(filter_func_t filter) { m_filter = filter; }
  size_t GetCurrentBatchSize() const { return m_batch_size; }
  const VectorizedOperatorStats &GetOperatorStats() const { return m_operator_stats; }

 private:
  size_t EstimateRowSize() const;

  /**
   * Calculate the optimal batch size based on system characteristics
   * Considers cache line size, row size, and expected row count
   * @param expected_rows Estimated number of rows to process
   * @return Optimal batch size in number of rows
   */
  size_t CalculateOptimalBatchSize(double expected_rows);

  void CacheActiveFields();

  /// Resolve the IMCU owning `global_row_id`, refreshing the cached snapshot
  /// once if the current one does not contain it. Returns nullptr if unknown.
  Imcs::Imcu *LocateImcuForRow(row_id_t global_row_id);

  /**
   * Preallocate memory for column chunks to avoid runtime allocations
   * Sets up the columnar storage buffers for batch processing
   */
  bool PreallocateColumnChunks();

  int ReadNextBatch();

  inline void ClearBatchData() {
    for (auto &chunk : m_col_chunks) chunk.clear();
  }

  void UpdatePerformanceMetrics(std::chrono::high_resolution_clock::time_point start_time);

  /**
   * Adapt batch size based on runtime performance characteristics
   * Implements dynamic batch size adjustment for optimal performance
   */
  void AdaptBatchSize();

  /**
   * Populate the current row from batch data to MySQL row format
   * @return 0 on success, error code on failure
   */
  int PopulateCurrentRow();

  /**
   * Process field data from column chunk to MySQL field format
   * Dispatches to specialized handlers based on field type
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  /** @return false when the value could not be produced; the scan must fail. */
  inline bool ProcessFieldData(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    if (Utils::Util::is_string(field->type()) || Utils::IsOffPageField(field)) {
      return ProcessStringField(field, col_chunk, rowid);
    }
    ProcessNumericField(field, col_chunk, rowid);
    return true;
  }

  /**
   * Process string field data with dictionary encoding support
   * Handles ENUM types and dictionary-encoded strings
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  bool ProcessStringField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid);

  /**
   * Process fixed-width field data through the MySQL Field adapter
   * Handles integer, float, and other fixed-width types efficiently
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  inline void ProcessNumericField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    ut_a(col_chunk.width() == field->pack_length());
    field->pack(field->field_ptr(), col_chunk.data_fast(rowid), col_chunk.width());
  }

 private:
  std::shared_ptr<RapidShare> m_share;
  Imcs::RpdTable *m_rpd_table{nullptr};

  //  these fields came from optimized plan.
  std::unique_ptr<Imcs::Predicate> m_pushed_predicate{nullptr};
  std::vector<uint32_t> m_projected_columns;
  ha_rows m_limit{HA_POS_ERROR};
  ha_rows m_offset{0};
  bool m_use_storage_index{false};

  // >= 0 when the plan asked for an ordered index scan: the batch is then read
  // in key order off the ART index rather than in rowid order.
  int m_index_no{-1};
  bool m_reverse{false};

  /// Cached IMCU snapshot used to resolve out-of-line varlen references.
  /// RpdTable::get_imcus() copies the whole vector (and bumps every shared_ptr
  /// refcount) under a lock, so calling it per row made varlen materialization
  /// O(rows x IMCUs) in allocations as well as comparisons. Refreshed lazily on
  /// a lookup miss so IMCUs added by concurrent population are still found.
  std::vector<std::shared_ptr<Imcs::Imcu>> m_imcu_snapshot;
  bool m_imcu_snapshot_valid{false};

  std::pmr::vector<ShannonBase::Executor::ColumnChunk> m_col_chunks;  ///< Column chunks for batch processing
  std::vector<row_id_t> m_batch_row_ids;  ///< Real physical row id per position in m_col_chunks' current batch;

  filter_func_t m_filter;  ///< Optional filter function for row-level filtering

  size_t m_batch_size;            ///< Current batch size being used
  size_t m_opt_batch_size;        ///< Calculated optimal batch size
  size_t m_curr_batch_size{0};    ///< Size of the current batch being processed
  size_t m_curr_row_in_batch{0};  ///< Current row index within the batch
  bool m_batch_exhausted{true};   ///< Flag indicating if current batch is fully processed
  bool m_eof_reached{false};      ///< Flag indicating end of data has been reached

  std::vector<Field *> m_active_fields;  ///< Cached pointers to active fields for faster access
  std::vector<uint> m_field_indices;     ///< Field indices for column mapping
  bool m_fields_cached{false};           ///< Flag indicating if field caching is complete
  std::string m_str_buf;
  /**
   * Performance metrics structure for monitoring and adaptive optimization
   * Tracks timing, throughput, and error statistics for dynamic tuning
   */
  struct PerformanceMetrics {
    std::chrono::high_resolution_clock::time_point start_time;       ///< Start time of current operation
    std::chrono::high_resolution_clock::time_point last_batch_time;  ///< Time when last batch completed
    size_t total_rows{0};                                            ///< Total rows processed so far
    size_t total_batches{0};                                         ///< Total batches processed so far
    size_t error_count{0};                                           ///< Count of errors encountered
    double avg_batch_time{0.0};                                      ///< Average time per batch in milliseconds
    double total_read_time{0.0};                                     ///< Total time spent reading data

    /**
     * Reset all metrics to initial state
     * Typically called at the start of a new scanning operation
     */
    void reset() {
      start_time = std::chrono::high_resolution_clock::now();
      last_batch_time = start_time;
      total_rows = 0;
      total_batches = 0;
      error_count = 0;
      avg_batch_time = 0.0;
      total_read_time = 0.0;
    }
  };

  mutable PerformanceMetrics m_metrics;  ///< Instance of performance metrics for this iterator
  VectorizedOperatorStats m_operator_stats;
  CardinalityObservation m_cardinality;

  // When VectorizedAggregateIterator detects a GROUP BY boundary mid-batch,
  // it pushes the tail rows back here.  The next ReadBatch() drains this
  // buffer before issuing a new rnd_next_batch() to the handler.
  std::pmr::vector<ColumnChunk> m_lookahead_chunks;  ///< Copy of pushed-back chunk tails
  size_t m_lookahead_start{0};                       ///< First unconsumed row in lookahead
  size_t m_lookahead_count{0};                       ///< Number of rows remaining in lookahead
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_SCAN_ITERATOR_H__
