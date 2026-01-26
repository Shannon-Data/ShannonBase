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
#ifndef __SHANNONBASE_TABLE_AGGREGATE_ITERATOR_H__
#define __SHANNONBASE_TABLE_AGGREGATE_ITERATOR_H__

#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/row_iterator.h"
#include "sql/mem_root_array.h"
#include "sql/pack_rows.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/imcs/table0view.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"
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
namespace ShannonBase {
namespace Executor {

/**
  Handles aggregation (typically used for GROUP BY) for the case where the rows
  are already properly grouped coming in, ie., all rows that are supposed to be
  part of the same group are adjacent in the input stream. (This could be
  because they were sorted earlier, because we are scanning an index that
  already gives us the rows in a group-compatible order, or because there is no
  grouping.)

  AggregateIterator needs to be able to save and restore rows; it doesn't know
  when a group ends until it's seen the first row that is part of the _next_
  group. When that happens, it needs to tuck away that next row, and then
  restore the previous row so that the output row gets the correct grouped
  values. A simple example, doing SELECT a, SUM(b) FROM t1 GROUP BY a:

    t1.a  t1.b                                       SUM(b)
     1     1     <-- first row, save it                1
     1     2                                           3
     1     3                                           6
     2     1     <-- group changed, save row
    [1     1]    <-- restore first row, output         6
                     reset aggregate              -->  0
    [2     1]    <-- restore new row, process it       1
     2    10                                          11
                 <-- EOF, output                      11

  To save and restore rows like this, it uses the infrastructure from
  pack_rows.h to pack and unpack all relevant rows into record[0] of every input
  table. (Currently, there can only be one input table, but this may very well
  change in the future.) It would be nice to have a more abstract concept of
  sending a row around and taking copies of it if needed, as opposed to it
  implicitly staying in the table's buffer. (This would also solve some
  issues in EQRefIterator and when synthesizing NULL rows for outer joins.)
  However, that's a large refactoring.
 */
/**
 * Vectorized version of AggregateIterator that maintains full compatibility
 * with the original while adding performance optimizations for simple aggregates.
 *
 * Key design principles:
 * 1. Preserve original state machine and semantics exactly
 * 2. Use vectorization only for simple aggregates within groups
 * 3. Fall back to traditional processing for complex cases
 * 4. Maintain all rollup and group change detection logic
 */
class VectorizedAggregateIterator final : public RowIterator {
 public:
  VectorizedAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, JOIN *join,
                              pack_rows::TableCollection tables, bool rollup, double expected_rows = 0.0);

  ~VectorizedAggregateIterator() override = default;

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void StartPSIBatchMode() override;
  void EndPSIBatchModeIfStarted() override;
  void UnlockRow() override;

  // Performance monitoring
  struct VectorizationStats {
    size_t total_batches_processed{0};
    size_t total_rows_vectorized{0};
    size_t traditional_fallbacks{0};
    double avg_batch_processing_time_ms{0.0};
    double total_vectorized_time_ms{0.0};
    size_t cache_hits{0};
    size_t cache_misses{0};
  };

  const VectorizationStats &GetStats() const { return m_stats; }

 private:
  // Core members (identical to original AggregateIterator)
  unique_ptr_destroy_only<RowIterator> m_source;
  JOIN *m_join;
  const bool m_rollup;
  pack_rows::TableCollection m_tables;

  // Keep original state machine
  enum State {
    READING_FIRST_ROW,
    PROCESSING_CURRENT_GROUP,
    LAST_ROW_STARTED_NEW_GROUP,
    OUTPUTTING_ROLLUP_ROWS,
    DONE_OUTPUTTING_ROWS
  } m_state;

  bool m_seen_eof;
  table_map m_save_nullinfo;

  int m_last_unchanged_grp_item_idx;
  int m_current_rollup_pos;
  String m_first_row_this_grp;
  String m_first_row_next_grp;
  int m_output_slice;

  // Vectorization infrastructure
  struct VectorizedGroupProcessor {
    // Batch storage for current group
    struct RowBatch {
      std::vector<ColumnChunk> column_chunks;
      size_t row_count{0};
      size_t capacity{0};
      bool initialized{false};

      void clear() {
        for (auto &chunk : column_chunks) {
          chunk.clear();
        }
        row_count = 0;
      }

      bool full() const { return row_count >= capacity; }
    };

    RowBatch current_batch;

    // Aggregate function analysis
    struct AggregateInfo {
      Item_sum *item;
      Item_sum::Sumfunctype type;
      Field *source_field;  // Primary field for this aggregate
      bool vectorizable;
      size_t field_index;  // Index in column chunks
    };

    std::vector<AggregateInfo> aggregate_infos;
    bool can_vectorize_curr_grp{false};
    bool analysis_complete{false};

    // Performance state
    size_t opt_batch_size{1024};
    double recent_processing_times[10]{0.0};
    size_t time_index{0};

    void reset() {
      current_batch.clear();
      can_vectorize_curr_grp = false;
    }
  };

  VectorizedGroupProcessor m_vectorizer;
  VectorizationStats m_stats;

  // Configuration
  size_t m_max_batch_size{4096};
  size_t m_min_batch_size{64};
  double m_target_batch_time_ms{10.0};
  bool m_vectorization_enabled{true};

  // Core processing methods
  void SetRollupLevel(int level);
  int ProcessCurrentGroupTraditional();
  int ProcessCurrentGroupVectorized();

  // Vectorization setup and analysis
  void InitializeVectorization();
  bool AnalyzeAggregatesForVectorization();
  void SetupColumnChunks();
  void UpdateBatchSizeFromPerformance(double processing_time_ms);

  // Batch processing
  int ReadRowsIntoCurrentBatch();
  int ProcessVectorizedAggregates();
  void RestoreFirstRowOfCurrentGroup();
  void RestoreRowFromBatch(size_t row_idx, size_t agg_idx);

  // Aggregate type handlers
  int ProcessCountAggregates(const std::vector<size_t> &count_indices);
  int ProcessSumAggregates(const std::vector<size_t> &sum_indices);
  int ProcessMinMaxAggregates(const std::vector<size_t> &minmax_indices);
  int ProcessAvgAggregates(const std::vector<size_t> &avg_indices);

  // Utility methods
  bool IsSimpleAggregate(Item_sum *item) const;
  Field *GetPrimaryFieldForAggregate(Item_sum *item) const;
  void LogPerformanceMetrics();
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_AGGREGATE_ITERATOR_H__