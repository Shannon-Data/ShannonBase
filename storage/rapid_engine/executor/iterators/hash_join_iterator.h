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
#include "sql/join_type.h"  //JoinType
#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/imcs/cu.h"

class Item_eq_base;
namespace ShannonBase {
namespace Executor {
class VectorizedHashJoinIterator final : public RowIterator {
 public:
  VectorizedHashJoinIterator(THD *thd, unique_ptr_destroy_only<RowIterator> build_input,
                             unique_ptr_destroy_only<RowIterator> probe_input,
                             const std::vector<HashJoinCondition> &join_conditions, JoinType join_type,
                             size_t max_memory_available = 256 * 1024 * 1024, size_t batch_size = 4096);

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override;

 private:
  enum class State { BUILDING_HASH_TABLE, PROBING_HASH_TABLE, END_OF_ROWS };

  // Build hash table from build input
  bool BuildHashTable();

  // Process probe batches
  bool ProcessProbeBatch();

  // Hash computation using actual HashJoinCondition interface
  uint64_t ComputeHashFromItem(Item_eq_base *condition, const uchar *row_data);

  // Read batches from input iterators
  bool ReadBuildBatch();
  bool ReadProbeBatch();

  unique_ptr_destroy_only<RowIterator> m_build_input;
  unique_ptr_destroy_only<RowIterator> m_probe_input;
  std::vector<HashJoinCondition> m_join_conditions;
  JoinType m_join_type;
  size_t m_max_memory_available;
  size_t m_batch_size;

  State m_state;

  // Use ColumnChunk for vectorized data storage
  std::vector<ColumnChunk> m_build_columns;
  std::vector<ColumnChunk> m_probe_columns;
  std::vector<ColumnChunk> m_output_columns;

  // Simplified hash table structure
  struct HashEntry {
    std::vector<uchar> key_data;
    size_t build_row_idx;
    std::unique_ptr<HashEntry> next;
  };

  std::vector<std::unique_ptr<HashEntry>> m_hash_table;
  static const uint m_hash_table_size{356};

  size_t m_current_output_size;
  size_t m_current_output_pos;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_HASH_JOIN_ITERATOR_H__