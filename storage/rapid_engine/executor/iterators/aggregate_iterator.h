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

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

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
enum class AggregateStrategy : uint8_t;

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
  static constexpr size_t kDefaultHashMemoryLimit = 64ULL * 1024ULL * 1024ULL;

  VectorizedAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, JOIN *join,
                              pack_rows::TableCollection tables, bool rollup, AggregateStrategy strategy,
                              ORDER *hash_output_order = nullptr, double expected_rows = 0.0,
                              size_t hash_memory_limit = kDefaultHashMemoryLimit);

  ~VectorizedAggregateIterator() override = default;

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void StartPSIBatchMode() override;
  void EndPSIBatchModeIfStarted() override;
  void UnlockRow() override;

  // Performance monitoring
  struct VectorizationStats {
    size_t rows_in{0};
    size_t rows_out{0};
    size_t total_batches_processed{0};
    size_t total_rows_vectorized{0};
    size_t traditional_fallbacks{0};
    double avg_batch_processing_time_ms{0.0};
    double total_vectorized_time_ms{0.0};
    size_t cache_hits{0};
    size_t cache_misses{0};
    size_t hash_memory_limit_bytes{0};
    size_t hash_memory_peak_bytes{0};
    size_t hash_memory_limit_hits{0};
    size_t hash_batch_input_rows{0};
    size_t hash_row_input_rows{0};
    size_t hash_batch_direct_rows{0};
    size_t hash_row_materializations{0};
    size_t hash_new_group_materializations{0};
    size_t row_materializations{0};
    size_t bytes_copied{0};
    size_t simd_rows{0};
    size_t scalar_fallback_rows{0};
    size_t hash_probes{0};
    size_t hash_collisions{0};
    size_t hash_spill_rows{0};
    size_t hash_spill_groups{0};
    size_t hash_spill_partitions{0};
    size_t hash_spill_repartitions{0};
    size_t hash_spill_bytes_written{0};

    double avg_batch_rows() const {
      return total_batches_processed == 0 ? 0.0
                                          : static_cast<double>(rows_in) / static_cast<double>(total_batches_processed);
    }
  };

  const VectorizationStats &GetStats() const { return m_stats; }

 private:
  // Core members (identical to original AggregateIterator)
  unique_ptr_destroy_only<RowIterator> m_source;
  JOIN *m_join;
  const bool m_rollup;
  pack_rows::TableCollection m_tables;
  AggregateStrategy m_strategy;
  ORDER *m_hash_output_order;

  BatchReadable *m_batch_source{nullptr};
  bool m_source_supports_batch{false};

  /*
   * AccessPath::vectorized chooses this Rapid iterator implementation; it
   * does not promise that the child edge is batch-readable. HASH aggregation
   * therefore has two ingestion modes while keeping the same HASH algorithm.
   */
  enum class HashInputMode : uint8_t { ROW, BATCH };
  HashInputMode m_hash_input_mode{HashInputMode::ROW};

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
      uint16_t source_field_table_index;                // Retained: for row-by-row path reference only,
                                                        // no longer used in batch path addressing.
      size_t field_index;                               // Index in column chunks / aggregate_infos
      size_t batch_chunk_idx{static_cast<size_t>(-1)};  // Resolved index into m_batch_col_chunks
                                                        // by SetupBatchChunks().
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
    template <typename T>
    inline bool Sum(Item_sum_sum *sum_item, Field *source_field, T value) {
      source_field->set_notnull();
      Item *arg = sum_item->get_arg(0);
      if (arg != nullptr) arg->null_value = false;
      return sum_item->add_value(value);
    }
  };

  VectorizedGroupProcessor m_vectorizer;
  VectorizationStats m_stats;

  /*
   * All retained HASH aggregate state is allocated through this resource.
   *
   * The previous implementation used ordinary std::unordered_map/std::vector
   * containers, so memory usage grew with group cardinality until process OOM.
   * This resource turns the configured per-operator budget into a hard
   * allocation boundary: an allocation that would cross the boundary throws
   * std::bad_alloc before the upstream allocator is called.
   */
  class HashMemoryResource final : public std::pmr::memory_resource {
   public:
    explicit HashMemoryResource(size_t limit_bytes)
        : m_limit_bytes(limit_bytes), m_upstream(std::pmr::new_delete_resource()) {}

    size_t limit_bytes() const { return m_limit_bytes; }
    size_t used_bytes() const { return m_used_bytes; }
    size_t peak_bytes() const { return m_peak_bytes; }
    size_t failed_allocation_bytes() const { return m_failed_allocation_bytes; }

   private:
    void *do_allocate(size_t bytes, size_t alignment) override {
      // Overflow-safe hard-boundary check. We reject the allocation before
      // touching the upstream allocator, so retained HASH state can never
      // cross m_limit_bytes.
      if (bytes > m_limit_bytes || m_used_bytes > m_limit_bytes - bytes) {
        m_failed_allocation_bytes = bytes;
        throw std::bad_alloc();
      }

      void *ptr = nullptr;
      try {
        ptr = m_upstream->allocate(bytes, alignment);
      } catch (const std::bad_alloc &) {
        m_failed_allocation_bytes = bytes;
        throw;
      }
      m_used_bytes += bytes;
      m_peak_bytes = std::max(m_peak_bytes, m_used_bytes);
      return ptr;
    }

    void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
      m_upstream->deallocate(ptr, bytes, alignment);
      assert(bytes <= m_used_bytes);
      // Clamp in release too: an accounting drift must not wrap m_used_bytes to
      // ~SIZE_MAX, which would make every later do_allocate() reject and turn a
      // bookkeeping slip into a permanent spill.
      m_used_bytes -= std::min(bytes, m_used_bytes);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

    size_t m_limit_bytes{0};
    size_t m_used_bytes{0};
    size_t m_peak_bytes{0};
    size_t m_failed_allocation_bytes{0};
    std::pmr::memory_resource *m_upstream{nullptr};
  };

  struct HashAggregateState {
    explicit HashAggregateState(std::pmr::memory_resource *resource) : extremum(resource) {}

    uint64_t count{0};
    bool has_value{false};
    bool decimal_value{false};
    double real_sum{0.0};
    my_decimal decimal_sum{};
    std::pmr::vector<uchar> extremum;
  };

  struct HashGroupOrderValue {
    explicit HashGroupOrderValue(std::pmr::memory_resource *resource) : data(resource) {}

    bool is_null{true};
    std::pmr::vector<uchar> data;
  };

  struct HashGroupState {
    explicit HashGroupState(std::pmr::memory_resource *resource)
        : key(resource), representative_row(resource), aggregates(resource), order_values(resource) {}

    std::pmr::string key;
    std::pmr::vector<uchar> representative_row;
    std::pmr::vector<HashAggregateState> aggregates;
    std::pmr::vector<HashGroupOrderValue> order_values;
  };

  struct HashArena {
    explicit HashArena(size_t limit_bytes) : memory(limit_bytes), index(&memory), groups(&memory) {
      index.max_load_factor(0.80F);
    }

    HashMemoryResource memory;

    /*
     * Store only the 64-bit hash in the index and keep the canonical key once
     * in HashGroupState. Collisions are resolved by comparing the full key.
     * This avoids retaining two copies of every GROUP BY key.
     */
    std::pmr::unordered_multimap<uint64_t, size_t> index;
    std::pmr::vector<HashGroupState> groups;
  };

  enum class HashSpillRecordType : uint8_t { ROW = 1, STATE = 2 };

  struct SpillAggregateState {
    uint64_t count{0};
    bool has_value{false};
    bool decimal_value{false};
    double real_sum{0.0};
    my_decimal decimal_sum{};
    std::vector<uchar> extremum;
  };

  struct SpillOrderValue {
    bool is_null{true};
    std::vector<uchar> data;
  };

  struct SpillGroupState {
    std::string key;
    std::vector<uchar> representative_row;
    std::vector<SpillAggregateState> aggregates;
    std::vector<SpillOrderValue> order_values;

    void clear() {
      key.clear();
      representative_row.clear();
      aggregates.clear();
      order_values.clear();
    }
  };

  struct HashSpillFile {
    HashSpillFile();
    ~HashSpillFile();

    HashSpillFile(const HashSpillFile &) = delete;
    HashSpillFile &operator=(const HashSpillFile &) = delete;

    bool valid() const { return file != nullptr; }
    bool RewindForRead();

    std::FILE *file{nullptr};
    uint64_t records{0};
    size_t bytes_written{0};
  };

  static constexpr int kHashNeedSpill = 2;
  static constexpr size_t kHashSpillFanout = 16;
  static constexpr size_t kHashMaxSpillDepth = 8;

  bool m_hash_groups_built{false};
  size_t m_hash_group_output_idx{0};
  size_t m_hash_memory_limit{kDefaultHashMemoryLimit};
  std::unique_ptr<HashArena> m_hash_arena;

  bool m_hash_spilled{false};
  uint64_t m_hash_spill_output_read{0};
  std::array<std::unique_ptr<HashSpillFile>, kHashSpillFanout> m_hash_spill_partitions;
  std::unique_ptr<HashSpillFile> m_hash_spill_output;

  std::vector<ColumnChunk> m_batch_col_chunks;
  bool m_batch_chunks_initialized{false};

  std::unordered_map<Field *, size_t> m_field_to_batch_chunk_idx;
  std::string m_hash_key_scratch;

  // Configuration
  size_t m_max_batch_size{4096};
  size_t m_min_batch_size{64};
  double m_target_batch_time_ms{10.0};
  bool m_vectorization_enabled{true};

  // Core processing methods
  void SetRollupLevel(int level);

  int ProcessCurrentGroupTraditional();
  int ProcessGroupVectorized();
  int ProcessGroupScalar();
  bool ValidateHashAggregatePlan() const;
  int ReadHashAggregate();
  int BuildHashGroups();
  int BuildHashGroupsBatch();
  int BuildHashGroupsRow();
  int ConsumeHashRow(size_t packed_row_capacity);
  int ConsumeHashBatchRow(size_t row_idx, size_t packed_row_capacity);
  int HashMemoryLimitExceeded();
  int BeginHashSpill(size_t packed_row_capacity);
  int SpillCurrentInputRow(size_t packed_row_capacity);
  int FinalizeHashSpill();
  int ProcessHashSpillPartition(std::unique_ptr<HashSpillFile> partition, size_t depth, HashSpillFile *unordered_output,
                                std::unique_ptr<HashSpillFile> *ordered_output);
  int RepartitionHashSpillFile(HashSpillFile *source, size_t depth,
                               std::array<std::unique_ptr<HashSpillFile>, kHashSpillFanout> *children);
  int LoadHashSpillState(const SpillGroupState &state);
  int AccumulateOrderedSpillRun(std::unique_ptr<HashSpillFile> run, std::unique_ptr<HashSpillFile> *ordered_output);
  int MergeSortedSpillRuns(std::unique_ptr<HashSpillFile> left, std::unique_ptr<HashSpillFile> right,
                           std::unique_ptr<HashSpillFile> *merged);
  bool WriteHashSpillRow(HashSpillFile *file, const std::string &key, const uchar *row, size_t row_length);
  bool WriteHashSpillState(HashSpillFile *file, const HashGroupState &group);
  bool WriteHashSpillState(HashSpillFile *file, const SpillGroupState &group);
  bool ReadHashSpillRecord(HashSpillFile *file, HashSpillRecordType *type, SpillGroupState *state,
                           std::vector<uchar> *row);
  bool WriteHashSpillRaw(HashSpillFile *file, const void *data, size_t length);
  bool ReadHashSpillRaw(HashSpillFile *file, void *data, size_t length) const;
  bool WriteHashSpillBlob(HashSpillFile *file, const uchar *data, size_t length);
  bool ReadHashSpillBlob(HashSpillFile *file, std::vector<uchar> *data) const;
  // my_decimal carries an internal buffer that its `buf` member points into, so
  // it is not safe to (de)serialize by raw bytes -- doing so leaves `buf`
  // dangling and trips my_decimal::sanity_check(). These write/read only the
  // value (intg/frac/sign + digit words) and keep the destination's own buffer.
  bool WriteHashSpillDecimal(HashSpillFile *file, const my_decimal &value);
  bool ReadHashSpillDecimal(HashSpillFile *file, my_decimal *value) const;
  bool ReadHashSpillString(HashSpillFile *file, std::string *data) const;
  size_t HashSpillPartitionForKey(const std::string &key, size_t depth) const;
  bool SpillGroupLess(const SpillGroupState &left, const SpillGroupState &right) const;
  int MaterializeSpillGroup(const SpillGroupState &group);
  int ReportHashSpillError(const char *reason);
  std::unique_ptr<HashSpillFile> CreateHashSpillFile();

  bool HashKeysEqual(const std::pmr::string &left, const std::string &right) const;
  bool RestoreHashBatchRow(size_t row_idx);
  bool RestoreBatchField(Field *field, size_t row_idx);
  bool CanMaterializeBatchRows() const;
  bool CanBuildHashGroupKeyFromBatch() const;
  bool CanUseBatchGrouping() const;
  bool BuildHashGroupKey(std::string *key) const;
  bool BuildHashGroupKeyFromBatch(size_t row_idx, std::string *key);
  bool CaptureHashGroupOrderValues(HashGroupState *group) const;
  void SortHashGroupsForOutput();
  bool UpdateHashGroup(HashGroupState *group);
  bool UpdateHashGroupFromBatch(HashGroupState *group, size_t row_idx);
  int MaterializeHashGroup(const HashGroupState &group);

  // Helper: copy one row from m_batch_col_chunks[boundary] into table->field.
  bool RestoreBoundaryRowToTableFields(size_t boundary);

  // Vectorization setup and analysis
  void InitializeVectorization();
  bool AnalyzeAggregatesForVectorization();
  void SetupColumnChunks();
  bool EnsureBatchCapacity(size_t capacity);
  void UpdateBatchSizeFromPerformance(double processing_time_ms);

  void SetupBatchChunks();
  bool RestoreGroupKeyField(size_t row_idx);

  // Batch processing
  int ReadRowsIntoCurrentBatch();
  int ProcessVectorizedAggregates();
  int ProcessVectorizedAggregates(const std::vector<ColumnChunk> &col_chunks, size_t row_count);
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

  void AppendCurrentRowToChunks();
};

/**
 * The legacy optimizer plans an unsorted GROUP BY as
 *   Table scan on <temporary>  <-  Aggregate using temporary table  <-  scan
 * and rebinds the SELECT list to the temp table's Fields.  Core MySQL fills
 * that temp table with a per-row hash-probe + ha_update_row loop, which the
 * profiler showed dominating grouped analytical queries.
 *
 * This iterator keeps the exact same plan shape (so no Item/Field rebinding is
 * needed and it works identically under the legacy and hypergraph optimizers),
 * but produces the group rows with Rapid's vectorized hash/stream aggregate and
 * writes each FINISHED group into the temp table exactly once -- mirroring the
 * "insert new row" branch of TemptableAggregateIterator, with no update branch.
 * Read() then just scans the materialized temp table.
 */
class VectorizedTemptableAggregateIterator final : public TableRowIterator {
 public:
  VectorizedTemptableAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> subquery_iterator,
                                       Temp_table_param *temp_table_param, TABLE *table,
                                       unique_ptr_destroy_only<RowIterator> table_iterator, JOIN *join, int ref_slice,
                                       pack_rows::TableCollection tables, bool rollup, AggregateStrategy strategy,
                                       ORDER *hash_output_order, double expected_rows, size_t hash_memory_limit);

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override { m_table_iterator->SetNullRowFlag(is_null_row); }
  void EndPSIBatchModeIfStarted() override { m_table_iterator->EndPSIBatchModeIfStarted(); }
  void UnlockRow() override {}

 private:
  // Write the group currently held by m_agg_iterator (keys in the base-table
  // Fields, aggregates in the JOIN's Item_sum objects) into the temp table.
  bool WriteCurrentGroup();

  unique_ptr_destroy_only<RowIterator> m_agg_iterator;    // VectorizedAggregateIterator over the subquery
  unique_ptr_destroy_only<RowIterator> m_table_iterator;  // scan of the filled temp table
  Temp_table_param *m_temp_table_param;
  JOIN *const m_join;
  const int m_ref_slice;
  bool m_materialized{false};

  bool using_hash_key() const { return table()->hash_field != nullptr; }
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_AGGREGATE_ITERATOR_H__
