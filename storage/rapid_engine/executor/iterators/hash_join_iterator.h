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
class VectorizedHashJoinIterator final : public RowIterator {
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

 private:
  enum class State { BUILDING_HASH_TABLE, PROBING_HASH_TABLE, READING_FROM_OUTPUT_BUFFER, END_OF_ROWS };

  // Build hash table from build input
  bool BuildHashTable();

  // Process probe batches
  bool ProcessProbeBatch();

  // Hash computation using actual HashJoinCondition interface
  uint64_t ComputeHashFromItem(Item_eq_base *condition, const uchar *row_data);

  uint64_t ComputeHashFromJoinConditions(const std::vector<ColumnChunk> &columns, size_t row_idx);

  // Read batches from input iterators
  bool ReadBuildBatch();
  bool ReadProbeBatch();

  // Extract data from table record buffers to column chunks
  bool ExtractRowToColumnChunks(const pack_rows::TableCollection &tables, std::vector<ColumnChunk> &chunks);

  // Load data from column chunks back to table record buffers
  bool LoadRowFromColumnChunks(const std::vector<ColumnChunk> &chunks, size_t row_idx,
                               const pack_rows::TableCollection &tables);

  // Join condition evaluation for vectorized processing
  bool EvaluateJoinConditions(const std::vector<ColumnChunk> &build_columns, size_t build_row,
                              const std::vector<ColumnChunk> &probe_columns, size_t probe_row);

  // Extra condition evaluation
  bool EvaluateExtraConditions();

  // Initialize column chunks based on table schema
  bool InitializeColumnChunks(const pack_rows::TableCollection &tables, std::vector<ColumnChunk> &chunks);

 private:
  unique_ptr_destroy_only<RowIterator> m_build_input;
  unique_ptr_destroy_only<RowIterator> m_probe_input;

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

  // Use ColumnChunk for vectorized data storage
  std::vector<ColumnChunk> m_build_columns;
  std::vector<ColumnChunk> m_probe_columns;
  std::vector<ColumnChunk> m_output_columns;

  // Hash table structure - use unordered_multimap for better performance
  struct HashEntry {
    std::vector<uchar> key_data;
    size_t build_row_idx;
    HashEntry *next;  // For chaining
  };

  std::vector<std::unique_ptr<HashEntry>> m_hash_table;
  static const size_t m_hash_table_size = 65536;  // Power of 2 for efficient modulo

  // Output buffer management
  struct OutputRow {
    size_t build_row_idx;
    size_t probe_row_idx;
    bool is_null_complemented;
  };

  std::vector<OutputRow> m_output_buffer;
  size_t m_current_output_pos;

  // Batch processing state
  size_t m_current_build_size;
  size_t m_current_probe_size;

  // Extra conditions
  Item *m_extra_condition;

  // Buffer for join key construction
  String m_join_key_buffer;

  // Hash table generation for optimization
  uint64_t *m_hash_table_generation;
  uint64_t m_last_hash_table_generation;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_HASH_JOIN_ITERATOR_H__