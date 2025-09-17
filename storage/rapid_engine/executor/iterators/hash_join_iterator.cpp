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
 * vectorized/parallelized hash join iterator impl for rapid engine. In
 */
#include "storage/rapid_engine/executor/iterators/hash_join_iterator.h"

#include <xxhash.h>
#include "sql/current_thd.h"
#include "sql/item_cmpfunc.h"  //Item_eq_base

#include "storage/rapid_engine/imcs/imcs.h"
namespace ShannonBase {
namespace Executor {
VectorizedHashJoinIterator::VectorizedHashJoinIterator(
    THD *thd, unique_ptr_destroy_only<RowIterator> build_input, const Prealloced_array<TABLE *, 4> &build_input_tables,
    double estimated_build_rows, unique_ptr_destroy_only<RowIterator> probe_input,
    const Prealloced_array<TABLE *, 4> &probe_input_tables, bool store_rowids, table_map tables_to_get_rowid_for,
    size_t max_memory_available, const std::vector<HashJoinCondition> &join_conditions, bool allow_spill_to_disk,
    JoinType join_type, const Mem_root_array<Item *> &extra_conditions, HashJoinInput first_input,
    bool probe_input_batch_mode, uint64_t *hash_table_generation)
    : RowIterator(thd),
      m_build_input(std::move(build_input)),
      m_probe_input(std::move(probe_input))
// For (LEFT)OUTER and ANTI-join we may have to return rows even if the
// build input is empty. Therefore we check the probe input for emptiness
// first. If 'probe' is empty, there is no need to read from 'build',
// while the converse is not the case.
{}

bool VectorizedHashJoinIterator::Init() {
  if (m_build_input->Init() || m_probe_input->Init()) {
    return true;
  }

  m_state = State::BUILDING_HASH_TABLE;
  return false;
}

int VectorizedHashJoinIterator::Read() {
  while (true) {
    switch (m_state) {
      case State::BUILDING_HASH_TABLE:
        if (BuildHashTable()) return 1;
        m_state = State::PROBING_HASH_TABLE;
        continue;

      case State::PROBING_HASH_TABLE:
        if (!ReadProbeBatch()) {
          m_state = State::END_OF_ROWS;
          continue;
        }
        if (ProcessProbeBatch()) return 1;
        if (m_current_output_size > 0) {
          return 0;  // Success, data ready
        }
        continue;

      case State::END_OF_ROWS:
        return -1;  // EOF
    }
  }
}

bool VectorizedHashJoinIterator::BuildHashTable() {
  // Read all build input data
  while (true) {
    int result = m_build_input->Read();
    if (result == -1) break;       // EOF
    if (result == 1) return true;  // Error

    // Process current row - need to extract data based on actual interface
    // This would need to be implemented based on how the build input provides data
  }

  return false;
}

bool VectorizedHashJoinIterator::ReadProbeBatch() {
  m_current_output_size = 0;

  for (size_t i = 0; i < m_batch_size; ++i) {
    int result = m_probe_input->Read();
    if (result == -1) break;        // EOF
    if (result == 1) return false;  // Error

    // Process probe data
    // Implementation depends on actual data access interface
  }

  return m_current_output_size > 0;
}

bool VectorizedHashJoinIterator::ProcessProbeBatch() {
  // Process probe batch and perform hash join
  return false;
}

uint64_t VectorizedHashJoinIterator::ComputeHashFromItem(Item_eq_base *condition, const uchar *row_data) {
  // Use the actual HashJoinCondition interface
  // Based on access_path.cpp, HashJoinCondition is constructed from Item_eq_base
  // Need to access the underlying Item to get key values

  return XXH64(row_data, 8, 0);  // Simplified hash computation
}

void VectorizedHashJoinIterator::SetNullRowFlag(bool is_null_row) {
  m_build_input->SetNullRowFlag(is_null_row);
  m_probe_input->SetNullRowFlag(is_null_row);
}

void VectorizedHashJoinIterator::UnlockRow() {
  // Forward to appropriate input based on current state
  if (m_state == State::PROBING_HASH_TABLE) {
    m_probe_input->UnlockRow();
  }
}
}  // namespace Executor
}  // namespace ShannonBase