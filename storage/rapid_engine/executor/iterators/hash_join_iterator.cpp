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
      m_build_input_tables(build_input_tables, store_rowids, store_rowids ? tables_to_get_rowid_for : 0, 0),
      m_probe_input_tables(probe_input_tables, store_rowids, store_rowids ? tables_to_get_rowid_for : 0, 0),
      m_join_conditions(join_conditions),
      m_join_type(join_type),
      m_max_memory_available(max_memory_available),
      m_batch_size(std::max<size_t>(128, std::min<size_t>(1024, max_memory_available / 1024))),  // Adaptive batch size
      m_allow_spill_to_disk(allow_spill_to_disk),
      m_probe_input_batch_mode(probe_input_batch_mode),
      m_tables_to_get_rowid_for(store_rowids ? tables_to_get_rowid_for : 0),
      m_first_input(first_input),
      m_state(State::BUILDING_HASH_TABLE),
      m_hash_table(),
      m_curr_output_pos(0),
      m_curr_build_size(0),
      m_curr_probe_size(0),
      m_extra_condition(nullptr),
      m_hash_table_gen(hash_table_generation),
      m_last_hash_table_gen(0),
      m_estimated_build_rows(estimated_build_rows) {
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

  m_build_batch_input = dynamic_cast<BatchReadable *>(m_build_input.get());
  if (m_build_batch_input == nullptr)
    m_build_batch_input = dynamic_cast<BatchReadable *>(m_build_input->real_iterator());
  m_probe_batch_input = dynamic_cast<BatchReadable *>(m_probe_input.get());
  if (m_probe_batch_input == nullptr)
    m_probe_batch_input = dynamic_cast<BatchReadable *>(m_probe_input->real_iterator());

  // Rapid scans expose encoded string payloads in their chunks. Keep those
  // inputs on the scalar path until the batch interface carries decoded data.
  if (!SupportsDirectBatchInput(m_build_input_tables)) m_build_batch_input = nullptr;
  if (!SupportsDirectBatchInput(m_probe_input_tables)) m_probe_batch_input = nullptr;

  if (m_join_conditions.empty()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "VectorizedHashJoinIterator: join_conditions is empty, cannot "
             "determine hash / comparison columns reliably.");
    return true;
  }

  // Initialize column chunks
  const size_t build_capacity = std::max<size_t>(static_cast<size_t>(m_estimated_build_rows), m_batch_size);
  if (InitializeColumnChunks(m_build_input_tables, m_build_columns, build_capacity, false) ||
      InitializeColumnChunks(m_probe_input_tables, m_probe_columns, m_batch_size, false) ||
      (m_build_batch_input != nullptr &&
       InitializeColumnChunks(m_build_input_tables, m_build_batch_columns, m_batch_size, true)) ||
      (m_probe_batch_input != nullptr &&
       InitializeColumnChunks(m_probe_input_tables, m_probe_batch_columns, m_batch_size, true))) {
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
                                                        std::vector<ColumnChunk> &chunks, size_t capacity,
                                                        bool input_layout) {
  chunks.clear();
  if (input_layout && tables.tables().size() == 1) {
    const pack_rows::Table &packed_table = tables.tables()[0];
    TABLE *table = packed_table.table;
    chunks.reserve(table->s->fields);
    for (uint field_idx = 0; field_idx < table->s->fields; ++field_idx) {
      Field *field = table->field[field_idx];
      bool required = bitmap_is_set(table->read_set, field_idx);
      for (const pack_rows::Column &column : packed_table.columns) required = required || column.field == field;
      if (required && !field->is_flag_set(NOT_SECONDARY_FLAG))
        chunks.emplace_back(field, capacity);
      else
        chunks.emplace_back(nullptr, 0);
    }
    return false;
  }

  chunks.reserve(tables.tables().size() * 10);

  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      chunks.emplace_back(column.field, capacity);
    }
  }

  return false;
}

bool VectorizedHashJoinIterator::CopyBatchColumns(const std::vector<ColumnChunk> &source, size_t rows,
                                                  std::vector<ColumnChunk> &destination) {
  for (ColumnChunk &target : destination) {
    const ColumnChunk *input = nullptr;
    for (const ColumnChunk &candidate : source) {
      if (candidate.valid() && candidate.table() == target.table() && candidate.field_index() == target.field_index()) {
        input = &candidate;
        break;
      }
    }
    if (input == nullptr || input->size() < rows) return true;

    for (size_t row = 0; row < rows; ++row) {
      const bool is_null = input->nullable(row);
      if (!target.add(is_null ? nullptr : input->data(row), is_null ? 0 : input->width(), is_null)) return true;
    }
  }
  return false;
}

bool VectorizedHashJoinIterator::SupportsDirectBatchInput(const pack_rows::TableCollection &tables) const {
  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      const enum_field_types type = column.field->type();
      if (Utils::Util::is_string(type) || Utils::Util::is_varlen(type)) return false;
    }
  }
  return true;
}

int VectorizedHashJoinIterator::Read() {
  while (true) {
    switch (m_state) {
      case State::BUILDING_HASH_TABLE:
        if (BuildHashTable()) return 1;  // Error
        m_state = State::PROBING_HASH_TABLE;
        continue;

      case State::PROBING_HASH_TABLE:
        if (int result = ReadProbeBatch(); result != 0) {
          if (result != -1) return result;
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
  // Compute hash table size dynamically from estimated build rows.
  size_t desired = static_cast<size_t>(m_estimated_build_rows) / kTargetLoadFactor;
  desired = (desired < 1024) ? 1024 : ((desired > (1ULL << 28)) ? (1ULL << 28) : desired);

  // Round up to next power of two.
  m_hash_table_size = 1;
  while (m_hash_table_size < desired) m_hash_table_size <<= 1;

  m_hash_table.clear();
  m_hash_table.resize(m_hash_table_size);

  const size_t build_capacity = std::max<size_t>(static_cast<size_t>(m_estimated_build_rows), m_batch_size);
  for (auto &chunk : m_build_columns) {
    if (chunk.valid() && chunk.capacity() != build_capacity)
      chunk.reset(chunk.source_field(), build_capacity);
    else
      chunk.clear();
  }

  m_curr_build_size = 0;
  m_build_error = false;

  // Read all build input data in batches
  size_t total_build_rows = 0;
  while (ReadBuildBatch()) {
    for (size_t i = total_build_rows; i < total_build_rows + m_curr_build_size; ++i) {
      JoinKeyResult key_result = BuildJoinKey(m_build_columns, i, m_build_input_tables);
      if (key_result == JoinKeyResult::ERROR) {
        m_build_error = true;
        return true;
      }
      if (key_result == JoinKeyResult::NULL_KEY) continue;

      uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
      size_t bucket_idx = hash % m_hash_table_size;

      // Create new hash entry
      auto entry = std::make_unique<HashEntry>();
      if (m_join_key_buffer.length() > 0) {
        const auto *key_begin = pointer_cast<const uchar *>(m_join_key_buffer.ptr());
        entry->key_data.assign(key_begin, key_begin + m_join_key_buffer.length());
      }
      entry->build_row_idx = i;
      entry->next.reset(m_hash_table[bucket_idx].release());

      m_hash_table[bucket_idx] = std::move(entry);
    }
    total_build_rows += m_curr_build_size;
  }

  if (m_hash_table_gen != nullptr) {
    m_last_hash_table_gen = *m_hash_table_gen;
  }

  if (m_build_error) return true;  // propagate: a real error, not clean EOF

  return false;
}

bool VectorizedHashJoinIterator::ReadBuildBatch() {
  m_curr_build_size = 0;

  if (m_build_batch_input != nullptr) {
    for (ColumnChunk &chunk : m_build_batch_columns) chunk.clear();
    size_t rows = 0;
    int result = m_build_batch_input->ReadBatch(m_build_batch_columns, m_batch_size, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) {
      m_build_error = true;
      return false;
    }
    if (rows == 0) return false;
    if (CopyBatchColumns(m_build_batch_columns, rows, m_build_columns)) {
      m_build_error = true;
      return false;
    }
    m_curr_build_size = rows;
    return true;
  }

  for (size_t i = 0; i < m_batch_size; ++i) {
    int result = m_build_input->Read();
    if (result == -1) break;  // EOF
    if (result != 0) {
      m_build_error = true;
      return false;  // Error from child iterator
    }

    // Extract current row to column chunks
    if (ExtractRowToColumnChunks(m_build_input_tables, m_build_columns)) {
      m_build_error = true;  // capacity exceeded / extraction error
      return false;
    }

    m_curr_build_size++;
  }

  return m_curr_build_size > 0;
}

int VectorizedHashJoinIterator::ReadProbeBatch() {
  m_curr_probe_size = 0;

  // Clear probe columns
  for (auto &chunk : m_probe_columns) {
    chunk.clear();
  }

  if (m_probe_batch_input != nullptr) {
    for (ColumnChunk &chunk : m_probe_batch_columns) chunk.clear();
    size_t rows = 0;
    int result = m_probe_batch_input->ReadBatch(m_probe_batch_columns, m_batch_size, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    if (rows == 0) return -1;
    if (CopyBatchColumns(m_probe_batch_columns, rows, m_probe_columns)) return 1;
    m_curr_probe_size = rows;
    return 0;
  }

  for (size_t i = 0; i < m_batch_size; ++i) {
    int result = m_probe_input->Read();
    if (result == -1) break;
    if (result != 0) return result;

    // Extract current row to column chunks
    if (ExtractRowToColumnChunks(m_probe_input_tables, m_probe_columns)) {
      return 1;
    }

    m_curr_probe_size++;
  }

  return m_curr_probe_size > 0 ? 0 : -1;
}

bool VectorizedHashJoinIterator::ProcessProbeBatch() {
  m_output_buffer.clear();

  // Process each probe row
  for (size_t probe_idx = 0; probe_idx < m_curr_probe_size; ++probe_idx) {
    bool found_match = false;

    JoinKeyResult key_result = BuildJoinKey(m_probe_columns, probe_idx, m_probe_input_tables);
    if (key_result == JoinKeyResult::ERROR) return true;

    if (key_result == JoinKeyResult::OK) {
      const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
      const size_t bucket_idx = hash % m_hash_table_size;

      // The key is generated by MySQL's HashJoinCondition machinery, so byte
      // equality here implements the comparison type, collation and NULL-safe
      // semantics selected by the server.
      HashEntry *entry = m_hash_table[bucket_idx].get();
      while (entry != nullptr) {
        const bool same_key = entry->key_data.size() == m_join_key_buffer.length() &&
                              (entry->key_data.empty() || std::memcmp(entry->key_data.data(), m_join_key_buffer.ptr(),
                                                                      entry->key_data.size()) == 0);
        if (same_key) {
          // Load rows for extra condition evaluation.
          if (LoadRowFromColumnChunks(m_build_columns, entry->build_row_idx, m_build_input_tables) ||
              LoadRowFromColumnChunks(m_probe_columns, probe_idx, m_probe_input_tables)) {
            return true;
          }

          if (EvaluateExtraConditions()) {
            found_match = true;

            switch (m_join_type) {
              case JoinType::INNER:
              case JoinType::OUTER:
              case JoinType::SEMI:
                m_output_buffer.push_back({entry->build_row_idx, probe_idx, false});
                break;
              case JoinType::ANTI:
              default:
                break;
            }

            if (m_join_type == JoinType::SEMI || m_join_type == JoinType::ANTI) break;
          }
        }
        entry = entry->next.get();
      }
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
      bool ok;

      if (is_null) {
        ok = chunks[chunk_idx].add(nullptr, 0, true);
      } else {
        auto data = const_cast<uchar *>(field->data_ptr());
        size_t length = field->pack_length();
        ok = chunks[chunk_idx].add(data, length, false);
      }

      if (!ok) return true;  // capacity exceeded — do not silently continue
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
      if (chunk_idx >= chunks.size() || row_idx >= chunks[chunk_idx].size()) {
        return true;  // Error
      }
      const auto &chunk = chunks[chunk_idx];

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

VectorizedHashJoinIterator::JoinKeyResult VectorizedHashJoinIterator::BuildJoinKey(
    const std::vector<ColumnChunk> &columns, size_t row_idx, const pack_rows::TableCollection &tables) {
  if (LoadRowFromColumnChunks(columns, row_idx, tables)) return JoinKeyResult::ERROR;

  m_join_key_buffer.length(0);
  for (const HashJoinCondition &condition : m_join_conditions) {
    const bool is_null = condition.join_condition()->append_join_key_for_hash_join(
        thd(), tables.tables_bitmap(), condition, m_join_conditions.size() > 1, &m_join_key_buffer);
    if (thd()->is_error()) return JoinKeyResult::ERROR;
    if (is_null) return JoinKeyResult::NULL_KEY;
  }
  return JoinKeyResult::OK;
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

int VectorizedHashJoinIterator::ReadBatch(std::vector<ColumnChunk> &col_chunks, size_t capacity, size_t &rows_read) {
  rows_read = 0;

  if (m_lookahead_count > 0) {
    size_t to_copy = std::min(m_lookahead_count, capacity);
    for (size_t ci = 0; ci < m_lookahead_chunks.size() && ci < col_chunks.size(); ++ci) {
      if (!m_lookahead_chunks[ci].valid()) continue;
      for (size_t r = 0; r < to_copy; ++r) {
        size_t src_row = m_lookahead_start + r;
        bool is_null = m_lookahead_chunks[ci].nullable(src_row);
        const uchar *data = is_null ? nullptr : m_lookahead_chunks[ci].data(src_row);
        size_t width = is_null ? 0 : m_lookahead_chunks[ci].width();
        if (!col_chunks[ci].add(data, width, is_null)) return 1;
      }
    }
    m_lookahead_start += to_copy;
    m_lookahead_count -= to_copy;
    if (m_lookahead_count == 0) {
      m_lookahead_start = 0;
      for (auto &c : m_lookahead_chunks) c.clear();
    }
    rows_read = to_copy;
    return 0;
  }

  bool needs_reinit = (m_chunk_map.size() != col_chunks.size());
  for (size_t ci = 0; !needs_reinit && ci < col_chunks.size(); ++ci) {
    if (col_chunks[ci].source_field() != nullptr &&
        (ci >= m_chunk_map.size() || m_chunk_map[ci].source_columns == nullptr))
      needs_reinit = true;
  }

  if (needs_reinit) {
    m_chunk_map.resize(col_chunks.size());
    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      auto &mapping = m_chunk_map[ci];
      mapping.source_columns = nullptr;
      Field *target = col_chunks[ci].source_field();
      if (target == nullptr) continue;

      // Match by stable identity: (table, field_index).
      TABLE *target_tbl = col_chunks[ci].table();
      uint16_t target_idx = col_chunks[ci].field_index();

      for (size_t j = 0; j < m_build_columns.size(); ++j) {
        if (m_build_columns[j].table() == target_tbl && m_build_columns[j].field_index() == target_idx) {
          mapping.source_columns = &m_build_columns;
          mapping.source_col_idx = j;
          break;
        }
      }
      if (mapping.source_columns != nullptr) continue;
      for (size_t j = 0; j < m_probe_columns.size(); ++j) {
        if (m_probe_columns[j].table() == target_tbl && m_probe_columns[j].field_index() == target_idx) {
          mapping.source_columns = &m_probe_columns;
          mapping.source_col_idx = j;
          break;
        }
      }
    }
  }

  if (m_state == State::BUILDING_HASH_TABLE) {
    if (BuildHashTable()) return 1;
    m_state = State::PROBING_HASH_TABLE;
  }

  size_t produced = 0;
  while (produced < capacity) {
    if (m_curr_output_pos >= m_output_buffer.size()) {
      m_output_buffer.clear();
      m_curr_output_pos = 0;

      if (m_state == State::END_OF_ROWS) break;

      if (m_state == State::PROBING_HASH_TABLE) {
        int result = ReadProbeBatch();
        if (result != 0) {
          if (result != -1) return result;
          m_state = State::END_OF_ROWS;
          break;
        }
        if (ProcessProbeBatch()) return 1;
        if (m_output_buffer.empty()) continue;
      } else if (m_state == State::READING_FROM_OUTPUT_BUFFER) {
        // Output buffer drained — transition back to probing for more data.
        m_state = State::PROBING_HASH_TABLE;
        continue;
      } else {
        // Should not happen — BUILDING_HASH_TABLE is handled by Init/Read.
        break;
      }
    }

    const OutputRow &out = m_output_buffer[m_curr_output_pos++];

    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      const auto &mapping = m_chunk_map[ci];
      if (mapping.source_columns == nullptr) {
        if (!col_chunks[ci].add(nullptr, 0, true)) return 1;
        continue;
      }

      const auto &src_cols = *mapping.source_columns;
      size_t src_idx = mapping.source_col_idx;
      size_t src_row;

      if (mapping.source_columns == &m_build_columns) {
        if (out.is_null_complemented) {
          if (!col_chunks[ci].add(nullptr, 0, true)) return 1;
          continue;
        }
        src_row = out.build_row_idx;
      } else {
        src_row = out.probe_row_idx;
      }

      const ColumnChunk &src = src_cols[src_idx];
      if (src_row >= src.size() || src.nullable(src_row)) {
        if (!col_chunks[ci].add(nullptr, 0, true)) return 1;
      } else {
        if (!col_chunks[ci].add(src.data(src_row), src.width(), false)) return 1;
      }
    }
    ++produced;
  }

  rows_read = produced;
  if (produced == 0 && m_state == State::END_OF_ROWS) return HA_ERR_END_OF_FILE;
  return 0;
}

void VectorizedHashJoinIterator::PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row,
                                                   size_t total_rows) {
  assert(from_row <= total_rows);
  size_t tail_len = total_rows - from_row;
  if (tail_len == 0) return;

  // (Re-)initialise the lookahead buffer to match the layout of `chunks`.
  bool rebuild = m_lookahead_chunks.size() != chunks.size();
  for (size_t i = 0; !rebuild && i < chunks.size(); ++i) {
    if (!chunks[i].valid()) continue;
    rebuild = !m_lookahead_chunks[i].valid() || m_lookahead_chunks[i].capacity() < tail_len ||
              m_lookahead_chunks[i].table() != chunks[i].table() ||
              m_lookahead_chunks[i].field_index() != chunks[i].field_index();
  }

  if (rebuild) {
    m_lookahead_chunks.clear();
    for (const auto &src : chunks) {
      if (src.valid()) {
        m_lookahead_chunks.emplace_back(src.source_field(), tail_len);
      } else {
        m_lookahead_chunks.emplace_back(nullptr, 0);
      }
    }
  } else {
    for (auto &chunk : m_lookahead_chunks) chunk.clear();
  }

  for (size_t ci = 0; ci < chunks.size(); ++ci) {
    if (!chunks[ci].valid()) continue;
    for (size_t r = from_row; r < total_rows; ++r) {
      bool is_null = chunks[ci].nullable(r);
      const uchar *data = is_null ? nullptr : chunks[ci].data(r);
      size_t width = is_null ? 0 : chunks[ci].width();
      if (!m_lookahead_chunks[ci].add(data, width, is_null)) {
        m_lookahead_count = 0;
        m_lookahead_start = 0;
        return;
      }
    }
  }

  m_lookahead_start = 0;
  m_lookahead_count = tail_len;
}
}  // namespace Executor
}  // namespace ShannonBase
