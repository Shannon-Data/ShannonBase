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
      m_probe_input(std::move(probe_input)),
      m_build_input_tables(build_input_tables, store_rowids, tables_to_get_rowid_for, 0),
      m_probe_input_tables(probe_input_tables, store_rowids, tables_to_get_rowid_for, 0),
      m_join_conditions(join_conditions),
      m_join_type(join_type),
      m_max_memory_available(max_memory_available),
      m_batch_size(std::min<size_t>(1024, max_memory_available / (1024 * 1024))),  // Adaptive batch size
      m_allow_spill_to_disk(allow_spill_to_disk),
      m_probe_input_batch_mode(probe_input_batch_mode),
      m_tables_to_get_rowid_for(tables_to_get_rowid_for),
      m_first_input(first_input),
      m_state(State::BUILDING_HASH_TABLE),
      m_hash_table(m_hash_table_size),
      m_curr_output_pos(0),
      m_curr_build_size(0),
      m_curr_probe_size(0),
      m_extra_condition(nullptr),
      m_hash_table_gen(hash_table_generation),
      m_last_hash_table_gen(0) {
  // Handle extra conditions similar to original implementation
  if (extra_conditions.size() == 1) {
    m_extra_condition = extra_conditions[0];
  } else if (extra_conditions.size() > 1) {
    List<Item> items;
    for (Item *cond : extra_conditions) {
      items.push_back(cond);
    }
    m_extra_condition = new Item_cond_and(items);
    m_extra_condition->quick_fix_field();
    m_extra_condition->update_used_tables();
    m_extra_condition->apply_is_true();
  }

  // Reserve output buffer
  m_output_buffer.reserve(m_batch_size);
}

bool VectorizedHashJoinIterator::Init() {
  // Similar to original HashJoinIterator::Init()
  m_build_input->SetNullRowFlag(false);

  // Check for hash table reuse optimization
  if (m_hash_table_gen != nullptr && *m_hash_table_gen == m_last_hash_table_gen && !m_build_columns.empty()) {
    m_state = State::PROBING_HASH_TABLE;
    m_curr_output_pos = 0;
    m_output_buffer.clear();
    return m_probe_input->Init();
  }

  if (m_build_input->Init() || m_probe_input->Init()) {
    return true;
  }

  // Initialize column chunks
  if (InitializeColumnChunks(m_build_input_tables, m_build_columns) ||
      InitializeColumnChunks(m_probe_input_tables, m_probe_columns)) {
    return true;
  }

  // Enable batch mode if requested
  if (m_probe_input_batch_mode) {
    m_probe_input->StartPSIBatchMode();
  }

  m_state = State::BUILDING_HASH_TABLE;
  m_curr_output_pos = 0;
  m_output_buffer.clear();

  return false;
}

bool VectorizedHashJoinIterator::InitializeColumnChunks(const pack_rows::TableCollection &tables,
                                                        std::vector<ColumnChunk> &chunks) {
  chunks.clear();
  chunks.reserve(tables.tables().size() * 10);  // Estimate columns per table

  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      try {
        chunks.emplace_back(column.field, m_batch_size);
      } catch (const std::exception &e) {
        my_error(ER_OUTOFMEMORY, MYF(0), m_batch_size * column.field->pack_length());
        return true;
      }
    }
  }

  return false;
}

int VectorizedHashJoinIterator::Read() {
  while (true) {
    switch (m_state) {
      case State::BUILDING_HASH_TABLE:
        if (BuildHashTable()) return 1;  // Error
        m_state = State::PROBING_HASH_TABLE;
        continue;

      case State::PROBING_HASH_TABLE:
        if (!ReadProbeBatch()) {
          m_state = State::END_OF_ROWS;
          continue;
        }
        if (ProcessProbeBatch()) return 1;  // Error
        if (!m_output_buffer.empty()) {
          m_state = State::READING_FROM_OUTPUT_BUFFER;
          m_curr_output_pos = 0;
        }
        continue;

      case State::READING_FROM_OUTPUT_BUFFER:
        if (m_curr_output_pos < m_output_buffer.size()) {
          const OutputRow &output_row = m_output_buffer[m_curr_output_pos++];

          // Load probe row data
          if (LoadRowFromColumnChunks(m_probe_columns, output_row.probe_row_idx, m_probe_input_tables)) {
            return 1;
          }

          if (output_row.is_null_complemented) {
            m_build_input->SetNullRowFlag(true);
          } else {
            // Load build row data
            if (LoadRowFromColumnChunks(m_build_columns, output_row.build_row_idx, m_build_input_tables)) {
              return 1;
            }
            m_build_input->SetNullRowFlag(false);
          }

          return 0;  // Success, row ready
        } else {
          // Output buffer exhausted, read more probe data
          m_state = State::PROBING_HASH_TABLE;
          m_output_buffer.clear();
          continue;
        }

      case State::END_OF_ROWS:
        return -1;  // EOF
    }
  }
}

bool VectorizedHashJoinIterator::BuildHashTable() {
  // Clear previous hash table
  for (auto &bucket : m_hash_table) {
    bucket.reset();
  }

  // Clear build columns
  for (auto &chunk : m_build_columns) {
    chunk.clear();
  }

  m_curr_build_size = 0;

  // Read all build input data in batches
  while (ReadBuildBatch()) {
    // Process the batch and add to hash table
    for (size_t i = 0; i < m_curr_build_size; ++i) {
      uint64_t hash = ComputeHashFromJoinConditions(m_build_columns, i);
      size_t bucket_idx = hash % m_hash_table_size;

      // Create new hash entry
      auto entry = std::make_unique<HashEntry>();
      entry->build_row_idx = i;
      entry->next = m_hash_table[bucket_idx].release();

      // Store join key for comparison
      m_join_key_buffer.length(0);
      // Extract join key from current row - this would need proper implementation
      // based on the actual HashJoinCondition interface

      m_hash_table[bucket_idx] = std::move(entry);
    }
  }

  if (m_hash_table_gen != nullptr) {
    m_last_hash_table_gen = *m_hash_table_gen;
  }

  return false;
}

bool VectorizedHashJoinIterator::ReadBuildBatch() {
  m_curr_build_size = 0;

  for (size_t i = 0; i < m_batch_size; ++i) {
    int result = m_build_input->Read();
    if (result == -1) break;        // EOF
    if (result == 1) return false;  // Error

    // Extract current row to column chunks
    if (ExtractRowToColumnChunks(m_build_input_tables, m_build_columns)) {
      return false;  // Error
    }

    m_curr_build_size++;
  }

  return m_curr_build_size > 0;
}

bool VectorizedHashJoinIterator::ReadProbeBatch() {
  m_curr_probe_size = 0;

  // Clear probe columns
  for (auto &chunk : m_probe_columns) {
    chunk.clear();
  }

  for (size_t i = 0; i < m_batch_size; ++i) {
    int result = m_probe_input->Read();
    if (result == -1) break;        // EOF
    if (result == 1) return false;  // Error

    // Extract current row to column chunks
    if (ExtractRowToColumnChunks(m_probe_input_tables, m_probe_columns)) {
      return false;  // Error
    }

    m_curr_probe_size++;
  }

  return m_curr_probe_size > 0;
}

bool VectorizedHashJoinIterator::ProcessProbeBatch() {
  m_output_buffer.clear();

  // Process each probe row
  for (size_t probe_idx = 0; probe_idx < m_curr_probe_size; ++probe_idx) {
    uint64_t hash = ComputeHashFromJoinConditions(m_probe_columns, probe_idx);
    size_t bucket_idx = hash % m_hash_table_size;

    bool found_match = false;

    // Look for matches in hash table
    HashEntry *entry = m_hash_table[bucket_idx].get();
    while (entry != nullptr) {
      if (EvaluateJoinConditions(m_build_columns, entry->build_row_idx, m_probe_columns, probe_idx)) {
        // Load rows for extra condition evaluation
        if (LoadRowFromColumnChunks(m_build_columns, entry->build_row_idx, m_build_input_tables) ||
            LoadRowFromColumnChunks(m_probe_columns, probe_idx, m_probe_input_tables)) {
          return true;  // Error
        }

        if (EvaluateExtraConditions()) {
          found_match = true;

          // Add to output buffer based on join type
          switch (m_join_type) {
            case JoinType::INNER:
            case JoinType::OUTER:
              m_output_buffer.push_back({entry->build_row_idx, probe_idx, false});
              break;
            case JoinType::SEMI:
              m_output_buffer.push_back({entry->build_row_idx, probe_idx, false});
              break;  // Only first match for semijoin
            case JoinType::ANTI:
              // Don't add to output for antijoin if match found
              break;
            default:
              break;
          }

          if (m_join_type == JoinType::SEMI || m_join_type == JoinType::ANTI) {
            break;  // Only need first match
          }
        }
      }
      entry = entry->next;
    }

    // Handle unmatched probe rows
    if (!found_match) {
      switch (m_join_type) {
        case JoinType::OUTER:
          // Add NULL-complemented row
          m_output_buffer.push_back({0, probe_idx, true});
          break;
        case JoinType::ANTI:
          // Add unmatched row for antijoin
          m_output_buffer.push_back({0, probe_idx, true});
          break;
        default:
          break;
      }
    }
  }

  return false;
}

bool VectorizedHashJoinIterator::ExtractRowToColumnChunks(const pack_rows::TableCollection &tables,
                                                          std::vector<ColumnChunk> &chunks) {
  size_t chunk_idx = 0;

  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      if (chunk_idx >= chunks.size()) {
        return true;  // Error: not enough chunks
      }

      Field *field = column.field;
      bool is_null = field->is_null();

      if (is_null) {
        chunks[chunk_idx].add(nullptr, 0, true);
      } else {
        auto data = const_cast<uchar *>(field->data_ptr());
        size_t length = field->pack_length();
        chunks[chunk_idx].add(data, length, false);
      }

      chunk_idx++;
    }
  }

  return false;
}

bool VectorizedHashJoinIterator::LoadRowFromColumnChunks(const std::vector<ColumnChunk> &chunks, size_t row_idx,
                                                         const pack_rows::TableCollection &tables) {
  size_t chunk_idx = 0;

  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      auto &chunk = chunks[chunk_idx];
      if (chunk_idx >= chunks.size() || row_idx >= chunks[chunk_idx].size()) {
        return true;  // Error
      }

      Field *field = column.field;

      if (chunk.nullable(row_idx)) {
        field->set_null();
      } else {
        field->set_notnull();
        const uchar *data = chunks[chunk_idx].data(row_idx);
        memcpy((void *)(field->data_ptr()), data, chunks[chunk_idx].width());
      }

      chunk_idx++;
    }
  }

  return false;
}

uint64_t VectorizedHashJoinIterator::ComputeHashFromJoinConditions(const std::vector<ColumnChunk> &columns,
                                                                   size_t row_idx) {
  // This is a simplified implementation
  // In practice, you'd need to extract the actual join key values
  // from the columns based on the join conditions

  uint64_t hash = 0;
  for (size_t i = 0; i < std::min(columns.size(), size_t(4)); ++i) {
    auto &col = columns[i];
    if (row_idx < col.size() && !col.nullable(row_idx)) {
      const uchar *data = col.data(row_idx);
      size_t width = col.width();
      hash ^= XXH64(data, width, i);
    }
  }

  return hash;
}

bool VectorizedHashJoinIterator::EvaluateJoinConditions(const std::vector<ColumnChunk> &build_columns, size_t build_row,
                                                        const std::vector<ColumnChunk> &probe_columns,
                                                        size_t probe_row) {
  // Simplified join condition evaluation
  // In practice, you'd need to implement proper comparison based on
  // the actual HashJoinCondition specifications

  // for (const HashJoinCondition &condition : m_join_conditions) {
  //   // This would need proper implementation based on the condition type
  //   // For now, assume equality comparison
  // }

  return true;  // Simplified - assume match
}

bool VectorizedHashJoinIterator::EvaluateExtraConditions() {
  if (m_extra_condition == nullptr) {
    return true;
  }

  return m_extra_condition->val_int() != 0;
}

void VectorizedHashJoinIterator::EndPSIBatchModeIfStarted() {
  m_build_input->EndPSIBatchModeIfStarted();
  m_probe_input->EndPSIBatchModeIfStarted();
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