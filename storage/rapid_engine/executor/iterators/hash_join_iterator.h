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
#ifndef __SHANNONBASE_HASH_JOIN_ITERATOR_H__
#define __SHANNONBASE_HASH_JOIN_ITERATOR_H__
#include <limits>
#include <memory>
#include <vector>

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_buffer.h"
#include "sql/iterators/hash_join_chunk.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/join_type.h"  //JoinType
#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/cu.h"

class Item_eq_base;
namespace ShannonBase {
namespace Executor {
// this vectorized version of HashJoinIterator. The More Hash Iterator, refere to HashJoinIterator.
class VectorizedHashJoinIterator final : public RowIterator, public BatchReadable {
 public:
  VectorizedHashJoinIterator(THD *thd, unique_ptr_destroy_only<RowIterator> build_input,
                             const Prealloced_array<TABLE *, 4> &build_input_tables, double estimated_build_rows,
                             unique_ptr_destroy_only<RowIterator> probe_input,
                             const Prealloced_array<TABLE *, 4> &probe_input_tables, bool store_rowids,
                             table_map tables_to_get_rowid_for, size_t max_memory_available,
                             const std::vector<HashJoinCondition> &join_conditions, bool allow_spill_to_disk,
                             JoinType join_type, const Mem_root_array<Item *> &extra_conditions,
                             HashJoinInput first_input, bool probe_input_batch_mode, uint64_t *hash_table_generation);

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override;
  void EndPSIBatchModeIfStarted() override;

  struct PerformanceStats {
    size_t build_rows{0};
    size_t probe_rows{0};
    size_t build_batches{0};
    size_t probe_batches{0};
    size_t output_rows{0};
    size_t output_row_materializations{0};
    size_t hash_slots_visited{0};
    size_t hash_key_mismatches{0};
    size_t extra_condition_row_materializations{0};
    size_t join_key_row_materializations{0};
    size_t direct_join_key_rows{0};
    size_t key_bytes{0};
    size_t bytes_copied{0};
    size_t build_memory_limit_bytes{0};
    size_t build_memory_peak_bytes{0};
    size_t build_memory_limit_hits{0};
  };
  const PerformanceStats &GetPerformanceStats() const { return m_stats; }
  VectorizedOperatorStats GetOperatorStats() const {
    VectorizedOperatorStats stats;
    stats.rows_in = m_stats.build_rows + m_stats.probe_rows;
    stats.rows_out = m_stats.output_rows;
    stats.batches = m_stats.build_batches + m_stats.probe_batches;
    stats.row_materializations = m_stats.output_row_materializations + m_stats.extra_condition_row_materializations +
                                 m_stats.join_key_row_materializations;
    stats.bytes_copied = m_stats.bytes_copied;
    stats.hash_probes = m_stats.hash_slots_visited;
    stats.hash_collisions = m_stats.hash_key_mismatches;
    stats.scalar_fallback_rows = m_stats.extra_condition_row_materializations + m_stats.join_key_row_materializations;
    return stats;
  }

  // BatchReadable interface
  bool SupportsBatchRead() const override { return !m_columns_hold_row_images; }
  int ReadBatch(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) override;
  bool PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row, size_t total_rows) override;

 private:
  enum class State { BUILDING_HASH_TABLE, PROBING_HASH_TABLE, END_OF_ROWS };

  struct OutputRow {
    size_t build_row_idx{0};
    size_t probe_row_idx{0};
    bool is_null_complemented{false};
  };

  // Build hash table from build input
  bool BuildHashTable();

  // Emit at most one joined row and retain the exact match position so a high
  // fanout probe row never needs a materialized output vector.
  int NextProbeOutput(OutputRow *out);
  void ResetProbeCursor();
  void AdvanceProbeRow();

  // Read batches from input iterators
  bool ReadBuildBatch();
  int ReadProbeBatch();

  // Extract data from table record buffers to column chunks
  bool ExtractRowToColumnChunks(const pack_rows::TableCollection &tables, std::vector<ColumnChunk> &chunks);

  // Load data from column chunks back to table record buffers
  bool LoadRowFromColumnChunks(const std::vector<ColumnChunk> &chunks, size_t row_idx,
                               const pack_rows::TableCollection &tables);

  enum class JoinKeyResult { OK, NULL_KEY, ERROR };
  JoinKeyResult BuildJoinKey(const std::vector<ColumnChunk> &columns, size_t row_idx,
                             const pack_rows::TableCollection &tables);
  bool TryBuildDirectJoinKey(const std::vector<ColumnChunk> &columns, size_t row_idx, JoinKeyResult *result);

  // Extra condition evaluation
  bool EvaluateExtraConditions();

  // Initialize column chunks based on table schema
  // `row_image` selects the element width of the produced chunks: true stores a
  // full MySQL record image (Field::pack_length()), false stores IMCS
  // normalized column bytes (Util::normalized_length()). They differ for
  // VARCHAR/CHAR, so the retained build/probe columns must be built with the
  // width matching whichever input path actually fills them.
  bool InitializeColumnChunks(const pack_rows::TableCollection &tables, std::vector<ColumnChunk> &chunks,
                              size_t capacity, bool input_layout, bool row_image);
  bool SupportsDirectBatchInput(const pack_rows::TableCollection &tables) const;
  bool CopyBatchColumns(const std::vector<ColumnChunk> &source, size_t rows, std::vector<ColumnChunk> &destination);
  bool EnsureBuildCapacity(size_t required_rows);

  // The Rapid hash join is deliberately no-spill when probe ordering is part
  // of the physical contract. Keep all build-side retained state inside the
  // join-buffer budget and fail the operator instead of silently falling back
  // to a different join algorithm/ordering when that budget is exhausted.
  size_t CurrentBuildMemoryUsage() const;
  bool BuildMemoryWouldExceed(size_t additional_bytes) const;
  bool ReportBuildMemoryLimit();
  void UpdateBuildMemoryPeak();
  bool EnsureHashSlotCapacity(size_t required_slots);
  bool EnsureHashKeyCapacity(size_t required_bytes);

 private:
  unique_ptr_destroy_only<RowIterator> m_build_input;
  unique_ptr_destroy_only<RowIterator> m_probe_input;
  BatchReadable *m_build_batch_input{nullptr};
  BatchReadable *m_probe_batch_input{nullptr};

  // Table collections for managing input tables
  pack_rows::TableCollection m_build_input_tables;
  pack_rows::TableCollection m_probe_input_tables;

  std::vector<HashJoinCondition> m_join_conditions;
  JoinType m_join_type;
  size_t m_max_memory_available;
  size_t m_batch_size;
  bool m_allow_spill_to_disk;
  bool m_probe_input_batch_mode;
  table_map m_tables_to_get_rowid_for;
  HashJoinInput m_first_input;

  State m_state;
  PerformanceStats m_stats;

  // Use ColumnChunk for vectorized data storage
  std::vector<ColumnChunk> m_build_columns;
  std::vector<ColumnChunk> m_probe_columns;
  std::vector<ColumnChunk> m_build_batch_columns;
  std::vector<ColumnChunk> m_probe_batch_columns;

  // Cache-friendly chained hash table. Buckets and entries are dense arrays;
  // keys live in one byte arena and are addressed by offsets. This removes one
  // heap allocation per build row and pointer-chasing through unique_ptr nodes.
  static constexpr size_t kInvalidHashSlot = std::numeric_limits<size_t>::max();
  struct HashSlot {
    uint64_t hash{0};
    size_t key_offset{0};
    size_t key_length{0};
    size_t build_row_idx{0};
    size_t next{kInvalidHashSlot};
  };

  std::vector<size_t> m_hash_buckets;
  // Temporary tails used only while building. Keeping them as a member makes
  // their allocation visible to build-memory accounting. They are released
  // immediately after the hash table has been built.
  std::vector<size_t> m_hash_bucket_tails;
  std::vector<HashSlot> m_hash_slots;
  std::vector<uchar> m_hash_key_arena;
  size_t m_hash_table_size{0};  // Number of buckets, power-of-two.

  // Target load factor (rows per bucket); trade-off between memory and probe cost.
  static constexpr size_t kTargetLoadFactor = 4;

  // Resumable probe cursor.  Retained state is O(1) regardless of join
  // fanout; m_join_key_buffer remains stable until the current probe row is
  // fully consumed.
  size_t m_probe_cursor_row{0};
  size_t m_probe_cursor_slot{kInvalidHashSlot};
  uint64_t m_probe_cursor_hash{0};
  bool m_probe_cursor_key_ready{false};
  bool m_probe_cursor_found_match{false};

  // Batch processing state
  size_t m_curr_build_size;
  size_t m_curr_probe_size;
  // Set when ReadBuildBatch()/ExtractRowToColumnChunks() hits a real error
  // (e.g. ColumnChunk capacity exceeded because m_estimated_build_rows
  // under-estimated the build side).  Distinguishes "real error" from
  // "clean EOF" so BuildHashTable() doesn't silently proceed with a
  // partially-built hash table.
  bool m_build_error{false};
  // Extra conditions
  Item *m_extra_condition;
  // Buffer for join key construction
  String m_join_key_buffer;
  // Hash table generation for optimization
  uint64_t *m_hash_table_gen;
  uint64_t m_last_hash_table_gen;

  // Estimated build row count (from optimizer), used for dynamic bucket sizing.
  double m_estimated_build_rows{0.0};

  // True when m_build_columns/m_probe_columns store MySQL record images because
  // the corresponding input is row-mode. Such chunks cannot be handed to a
  // ReadBatch() caller that sized its own chunks by Util::normalized_length().
  bool m_columns_hold_row_images{false};

  // ---- BatchReadable lookahead buffer ----
  // When PushbackBatchTail pushes rows back, they are stored here so the
  // next ReadBatch() drains them first.
  std::vector<ColumnChunk> m_lookahead_chunks;
  size_t m_lookahead_count{0};
  size_t m_lookahead_start{0};

  // Pre-built index mapping: col_chunks[i] → (source vector, column index).
  // Built once per ReadBatch call (when the caller-supplied chunk layout
  // changes) and reused across rows.
  struct ChunkMapping {
    const std::vector<ColumnChunk> *source_columns;  // &m_build_columns or &m_probe_columns
    size_t source_col_idx;                           // index within source_columns
  };
  std::vector<ChunkMapping> m_chunk_map;  // indexed by col_chunks position
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_HASH_JOIN_ITERATOR_H__
