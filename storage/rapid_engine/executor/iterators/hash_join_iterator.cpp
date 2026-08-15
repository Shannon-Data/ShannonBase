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

   The fundmental code for imcs. It's based on mysql executor iterators.
*/
/** The basic iterator class for IMCS. All specific iterators are all inherited
 * from this.
 * vectorized/parallelized hash join iterator impl for rapid engine.
 */
#include "storage/rapid_engine/executor/iterators/hash_join_iterator.h"

#include <cstring>

#include <xxhash.h>
#include "sql/current_thd.h"
#include "sql/item_cmpfunc.h"  //Item_eq_base

#include "storage/rapid_engine/imcs/imcs.h"
namespace ShannonBase {
namespace Executor {
namespace {

size_t ColumnChunkStorageBytes(const ColumnChunk &chunk, size_t capacity) {
  if (!chunk.valid() || chunk.width() == 0 || capacity == 0) return 0;
  if (capacity > std::numeric_limits<size_t>::max() / chunk.width()) return std::numeric_limits<size_t>::max();
  const size_t data_bytes = capacity * chunk.width();
  const size_t null_bytes = (capacity + 7) / 8;
  if (data_bytes > std::numeric_limits<size_t>::max() - null_bytes) return std::numeric_limits<size_t>::max();
  return data_bytes + null_bytes;
}

size_t GrowthCapacity(size_t current, size_t required) {
  size_t capacity = current == 0 ? 64 : current;
  while (capacity < required) {
    if (capacity > std::numeric_limits<size_t>::max() / 2) return required;
    capacity *= 2;
  }
  return capacity;
}

}  // namespace

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
      m_hash_buckets(),
      m_hash_bucket_tails(),
      m_hash_slots(),
      m_hash_key_arena(),
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
    m_extra_condition = new (thd->mem_root) Item_cond_and(items);
    m_extra_condition->quick_fix_field();
    m_extra_condition->update_used_tables();
    m_extra_condition->apply_is_true();
  }
}

bool VectorizedHashJoinIterator::Init() {
  // Batch lookahead belongs to one execution. A re-execution (correlated
  // subquery/PS reuse) must never replay rows pushed back by the prior run.
  m_lookahead_count = 0;
  m_lookahead_start = 0;
  for (auto &chunk : m_lookahead_chunks) chunk.clear();
  m_chunk_map.clear();
  m_stats = PerformanceStats{};
  m_stats.build_memory_limit_bytes = m_max_memory_available;

  // CreateIteratorFromAccessPath only selects this iterator for no-spill hash
  // joins. Keep that invariant explicit here as well so a future caller cannot
  // accidentally route a spill-required plan into an implementation that has
  // no ordered external-spill protocol.
  if (m_allow_spill_to_disk) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join does not implement spill-to-disk");
    return true;
  }

  // Similar to original HashJoinIterator::Init()
  m_build_input->SetNullRowFlag(false);

  // Check for hash table reuse optimization
  if (m_hash_table_gen != nullptr && *m_hash_table_gen == m_last_hash_table_gen && !m_build_columns.empty()) {
    UpdateBuildMemoryPeak();
    if (BuildMemoryWouldExceed(0)) return ReportBuildMemoryLimit();
    m_state = State::PROBING_HASH_TABLE;
    m_curr_probe_size = 0;
    ResetProbeCursor();
    if (m_probe_input->Init()) return true;
    if (m_probe_input_batch_mode) m_probe_input->StartPSIBatchMode();
    return false;
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

  // When join conditions are empty (NLJ→HashJoin conversion from old
  // optimizer), produce a cartesian product: all build rows match all
  // probe rows.  BuildJoinKey() returns OK with an empty key, which
  // causes every row to land in the same hash bucket.
  if (m_join_conditions.empty()) {
    // Nothing to validate — empty-key hash join is well-defined.
  }

  // Initialize column chunks
  // Start small and grow from actual input. Cardinality is an estimate, not a
  // correctness boundary, and may be wrong by orders of magnitude.
  const size_t build_capacity = m_batch_size;
  if (InitializeColumnChunks(m_build_input_tables, m_build_columns, build_capacity, false) ||
      InitializeColumnChunks(m_probe_input_tables, m_probe_columns, m_batch_size, false) ||
      (m_build_batch_input != nullptr &&
       InitializeColumnChunks(m_build_input_tables, m_build_batch_columns, m_batch_size, true)) ||
      (m_probe_batch_input != nullptr &&
       InitializeColumnChunks(m_probe_input_tables, m_probe_batch_columns, m_batch_size, true))) {
    return true;
  }

  UpdateBuildMemoryPeak();
  if (BuildMemoryWouldExceed(0)) return ReportBuildMemoryLimit();

  // Enable batch mode if requested
  if (m_probe_input_batch_mode) {
    m_probe_input->StartPSIBatchMode();
  }

  m_state = State::BUILDING_HASH_TABLE;
  m_curr_probe_size = 0;
  ResetProbeCursor();

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
      if (!target.append_from(*input, row)) return true;
      if (!input->nullable_fast(row)) m_stats.bytes_copied += input->width();
    }
  }
  return false;
}

bool VectorizedHashJoinIterator::EnsureBuildCapacity(size_t required_rows) {
  size_t current_capacity = m_batch_size;
  for (const ColumnChunk &chunk : m_build_columns) {
    if (chunk.valid()) {
      current_capacity = chunk.capacity();
      break;
    }
  }
  if (required_rows <= current_capacity) return false;

  // ColumnChunk::grow() allocates the replacement data/null buffers before the
  // old buffers are released. Simulate that transient allocation for every
  // column, not merely the retained delta, so join_buffer_size remains a real
  // build-side allocation boundary even during growth.
  auto growth_fits = [&](size_t new_capacity) -> bool {
    if (m_max_memory_available == 0) return true;
    size_t simulated = CurrentBuildMemoryUsage();
    if (simulated > m_max_memory_available) return false;

    for (const ColumnChunk &chunk : m_build_columns) {
      if (!chunk.valid()) continue;
      const size_t old_bytes = ColumnChunkStorageBytes(chunk, chunk.capacity());
      const size_t new_bytes = ColumnChunkStorageBytes(chunk, new_capacity);
      if (old_bytes == std::numeric_limits<size_t>::max() || new_bytes == std::numeric_limits<size_t>::max() ||
          new_bytes < old_bytes || new_bytes > m_max_memory_available - simulated) {
        return false;
      }
      // Peak while this column reallocates is simulated + new_bytes. Once the
      // replacement is installed, the old bytes are released.
      simulated += new_bytes;
      simulated -= old_bytes;
    }
    return true;
  };

  size_t new_capacity = GrowthCapacity(current_capacity, required_rows);
  // Spare capacity is only a performance optimization. If doubling would cross
  // the boundary, retry with exactly the rows needed by this batch.
  if (!growth_fits(new_capacity) && new_capacity != required_rows) new_capacity = required_rows;
  if (!growth_fits(new_capacity)) return ReportBuildMemoryLimit();

  try {
    for (ColumnChunk &chunk : m_build_columns) {
      if (chunk.valid() && !chunk.grow(new_capacity)) return ReportBuildMemoryLimit();
    }
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not grow build columns");
    m_build_error = true;
    return true;
  }

  UpdateBuildMemoryPeak();
  if (BuildMemoryWouldExceed(0)) return ReportBuildMemoryLimit();
  return false;
}

size_t VectorizedHashJoinIterator::CurrentBuildMemoryUsage() const {
  size_t total = 0;
  auto add = [&total](size_t bytes) -> bool {
    if (bytes == std::numeric_limits<size_t>::max() || total > std::numeric_limits<size_t>::max() - bytes) {
      total = std::numeric_limits<size_t>::max();
      return false;
    }
    total += bytes;
    return true;
  };

  if (m_build_columns.capacity() > std::numeric_limits<size_t>::max() / sizeof(ColumnChunk))
    return std::numeric_limits<size_t>::max();
  if (!add(m_build_columns.capacity() * sizeof(ColumnChunk))) return total;
  for (const ColumnChunk &chunk : m_build_columns) {
    if (!add(ColumnChunkStorageBytes(chunk, chunk.capacity()))) return total;
  }
  if (m_hash_buckets.capacity() > std::numeric_limits<size_t>::max() / sizeof(size_t) ||
      m_hash_bucket_tails.capacity() > std::numeric_limits<size_t>::max() / sizeof(size_t) ||
      m_hash_slots.capacity() > std::numeric_limits<size_t>::max() / sizeof(HashSlot))
    return std::numeric_limits<size_t>::max();
  if (!add(m_hash_buckets.capacity() * sizeof(size_t))) return total;
  if (!add(m_hash_bucket_tails.capacity() * sizeof(size_t))) return total;
  if (!add(m_hash_slots.capacity() * sizeof(HashSlot))) return total;
  (void)add(m_hash_key_arena.capacity());
  return total;
}

bool VectorizedHashJoinIterator::BuildMemoryWouldExceed(size_t additional_bytes) const {
  if (m_max_memory_available == 0) return false;  // Preserve the historical unlimited sentinel.
  const size_t used = CurrentBuildMemoryUsage();
  return used > m_max_memory_available || additional_bytes > m_max_memory_available - used;
}

bool VectorizedHashJoinIterator::ReportBuildMemoryLimit() {
  ++m_stats.build_memory_limit_hits;
  m_build_error = true;
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
           "Rapid no-spill hash join exceeded its build memory budget; ordered hash spill is not implemented");
  return true;
}

void VectorizedHashJoinIterator::UpdateBuildMemoryPeak() {
  m_stats.build_memory_peak_bytes = std::max(m_stats.build_memory_peak_bytes, CurrentBuildMemoryUsage());
}

bool VectorizedHashJoinIterator::EnsureHashSlotCapacity(size_t required_slots) {
  if (required_slots <= m_hash_slots.capacity()) return false;

  size_t desired = GrowthCapacity(m_hash_slots.capacity(), required_slots);
  auto allocation_bytes = [](size_t capacity) -> size_t {
    if (capacity > std::numeric_limits<size_t>::max() / sizeof(HashSlot)) return std::numeric_limits<size_t>::max();
    return capacity * sizeof(HashSlot);
  };

  // vector::reserve() holds the old allocation until the replacement allocation
  // succeeds, so charge the complete new allocation as transient memory.
  size_t bytes = allocation_bytes(desired);
  if ((bytes == std::numeric_limits<size_t>::max() || BuildMemoryWouldExceed(bytes)) && desired != required_slots) {
    desired = required_slots;
    bytes = allocation_bytes(desired);
  }
  if (bytes == std::numeric_limits<size_t>::max() || BuildMemoryWouldExceed(bytes)) return ReportBuildMemoryLimit();

  try {
    m_hash_slots.reserve(desired);
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not grow hash slots");
    m_build_error = true;
    return true;
  }
  UpdateBuildMemoryPeak();
  return BuildMemoryWouldExceed(0) ? ReportBuildMemoryLimit() : false;
}

bool VectorizedHashJoinIterator::EnsureHashKeyCapacity(size_t required_bytes) {
  if (required_bytes <= m_hash_key_arena.capacity()) return false;

  size_t desired = GrowthCapacity(m_hash_key_arena.capacity(), required_bytes);
  // As with slots, reserve() temporarily retains the old arena while allocating
  // the replacement, so charge the complete requested capacity.
  if (BuildMemoryWouldExceed(desired) && desired != required_bytes) desired = required_bytes;
  if (BuildMemoryWouldExceed(desired)) return ReportBuildMemoryLimit();

  try {
    m_hash_key_arena.reserve(desired);
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not grow hash key arena");
    m_build_error = true;
    return true;
  }
  UpdateBuildMemoryPeak();
  return BuildMemoryWouldExceed(0) ? ReportBuildMemoryLimit() : false;
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
    if (m_state == State::BUILDING_HASH_TABLE) {
      if (BuildHashTable()) return 1;
      m_state = State::PROBING_HASH_TABLE;
      m_curr_probe_size = 0;
      ResetProbeCursor();
    }

    if (m_state == State::END_OF_ROWS) return -1;

    if (m_probe_cursor_row >= m_curr_probe_size) {
      const int result = ReadProbeBatch();
      if (result != 0) {
        if (result != -1) return result;
        m_state = State::END_OF_ROWS;
        continue;
      }
    }

    OutputRow output_row;
    const int result = NextProbeOutput(&output_row);
    if (result == -1) {
      m_curr_probe_size = 0;
      continue;
    }
    if (result != 0) return result;

    if (LoadRowFromColumnChunks(m_probe_columns, output_row.probe_row_idx, m_probe_input_tables)) return 1;
    ++m_stats.output_row_materializations;

    if (output_row.is_null_complemented) {
      m_build_input->SetNullRowFlag(true);
    } else {
      if (LoadRowFromColumnChunks(m_build_columns, output_row.build_row_idx, m_build_input_tables)) return 1;
      ++m_stats.output_row_materializations;
      m_build_input->SetNullRowFlag(false);
    }
    return 0;
  }
}

bool VectorizedHashJoinIterator::BuildHashTable() {
  // A rebuild must not inherit capacity from the previous execution; retained
  // capacity is real memory and therefore part of the no-spill budget.
  std::vector<size_t>().swap(m_hash_buckets);
  std::vector<size_t>().swap(m_hash_bucket_tails);
  std::vector<HashSlot>().swap(m_hash_slots);
  std::vector<uchar>().swap(m_hash_key_arena);

  // Bucket heads and temporary build tails are charged to the same budget as
  // build columns, slots and key bytes. Bucket count remains a performance
  // heuristic; correctness never depends on the estimate.
  size_t desired = static_cast<size_t>(std::max(0.0, m_estimated_build_rows)) / kTargetLoadFactor;
  desired = std::max<size_t>(1024, std::min<size_t>(desired, 1ULL << 28));

  if (m_max_memory_available > 0) {
    const size_t used = CurrentBuildMemoryUsage();
    if (used >= m_max_memory_available) return ReportBuildMemoryLimit();
    const size_t remaining = m_max_memory_available - used;
    // Heads + tails need two size_t arrays. Do not let bucket metadata consume
    // the whole budget; leave most of the currently available bytes for build
    // rows, slots and key data.
    const size_t max_bucket_bytes = remaining / 4;
    const size_t max_buckets = max_bucket_bytes / (2 * sizeof(size_t));
    if (max_buckets == 0) return ReportBuildMemoryLimit();
    desired = std::min(desired, max_buckets);
  }

  // Keep a power-of-two count without rounding above the memory-derived ceiling
  // (the old round-up could exceed its own bucket budget by almost 2x).
  m_hash_table_size = 1;
  while (m_hash_table_size <= desired / 2) m_hash_table_size <<= 1;

  if (m_hash_table_size > std::numeric_limits<size_t>::max() / (2 * sizeof(size_t))) return ReportBuildMemoryLimit();
  const size_t bucket_bytes = m_hash_table_size * sizeof(size_t) * 2;
  if (BuildMemoryWouldExceed(bucket_bytes)) return ReportBuildMemoryLimit();
  try {
    m_hash_buckets.assign(m_hash_table_size, kInvalidHashSlot);
    m_hash_bucket_tails.assign(m_hash_table_size, kInvalidHashSlot);
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not allocate hash buckets");
    m_build_error = true;
    return true;
  }
  UpdateBuildMemoryPeak();
  if (BuildMemoryWouldExceed(0)) return ReportBuildMemoryLimit();

  for (auto &chunk : m_build_columns) chunk.clear();
  m_curr_build_size = 0;
  m_build_error = false;

  size_t total_build_rows = 0;
  while (ReadBuildBatch()) {
    for (size_t i = total_build_rows; i < total_build_rows + m_curr_build_size; ++i) {
      ++m_stats.build_rows;
      JoinKeyResult key_result = BuildJoinKey(m_build_columns, i, m_build_input_tables);
      if (key_result == JoinKeyResult::ERROR) {
        m_build_error = true;
        return true;
      }
      if (key_result == JoinKeyResult::NULL_KEY) continue;

      const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
      const size_t bucket_idx = hash & (m_hash_table_size - 1);
      const size_t key_offset = m_hash_key_arena.size();
      const size_t key_length = m_join_key_buffer.length();
      if (key_length > std::numeric_limits<size_t>::max() - key_offset) return ReportBuildMemoryLimit();

      // Never let std::vector grow implicitly. Every retained-state growth is
      // negotiated against join_buffer_size before allocation.
      if (EnsureHashSlotCapacity(m_hash_slots.size() + 1) || EnsureHashKeyCapacity(key_offset + key_length))
        return true;

      if (key_length != 0) {
        const auto *begin = pointer_cast<const uchar *>(m_join_key_buffer.ptr());
        m_hash_key_arena.insert(m_hash_key_arena.end(), begin, begin + key_length);
      }

      const size_t slot_idx = m_hash_slots.size();
      m_hash_slots.push_back({hash, key_offset, key_length, i, kInvalidHashSlot});
      m_stats.key_bytes += key_length;
      if (m_hash_buckets[bucket_idx] == kInvalidHashSlot) {
        m_hash_buckets[bucket_idx] = slot_idx;
      } else {
        m_hash_slots[m_hash_bucket_tails[bucket_idx]].next = slot_idx;
      }
      m_hash_bucket_tails[bucket_idx] = slot_idx;
    }
    total_build_rows += m_curr_build_size;
  }

  if (m_hash_table_gen != nullptr) m_last_hash_table_gen = *m_hash_table_gen;
  if (m_build_error) return true;

  // Tails are construction-only state. Free them before probing.
  std::vector<size_t>().swap(m_hash_bucket_tails);
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
    const size_t existing_rows = m_build_columns.empty() ? 0 : m_build_columns.front().size();
    if (EnsureBuildCapacity(existing_rows + rows)) {
      m_build_error = true;
      return false;
    }
    if (CopyBatchColumns(m_build_batch_columns, rows, m_build_columns)) {
      m_build_error = true;
      return false;
    }
    m_curr_build_size = rows;
    ++m_stats.build_batches;
    return true;
  }

  size_t existing_rows = 0;
  for (const ColumnChunk &chunk : m_build_columns) {
    if (chunk.valid()) {
      existing_rows = chunk.size();
      break;
    }
  }
  if (EnsureBuildCapacity(existing_rows + m_batch_size)) {
    m_build_error = true;
    return false;
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

  if (m_curr_build_size > 0) ++m_stats.build_batches;
  return m_curr_build_size > 0;
}

int VectorizedHashJoinIterator::ReadProbeBatch() {
  m_curr_probe_size = 0;
  ResetProbeCursor();

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
    ++m_stats.probe_batches;
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

  if (m_curr_probe_size > 0) ++m_stats.probe_batches;
  return m_curr_probe_size > 0 ? 0 : -1;
}

void VectorizedHashJoinIterator::ResetProbeCursor() {
  m_probe_cursor_row = 0;
  m_probe_cursor_slot = kInvalidHashSlot;
  m_probe_cursor_hash = 0;
  m_probe_cursor_key_ready = false;
  m_probe_cursor_found_match = false;
}

void VectorizedHashJoinIterator::AdvanceProbeRow() {
  ++m_probe_cursor_row;
  m_probe_cursor_slot = kInvalidHashSlot;
  m_probe_cursor_hash = 0;
  m_probe_cursor_key_ready = false;
  m_probe_cursor_found_match = false;
}

int VectorizedHashJoinIterator::NextProbeOutput(OutputRow *out) {
  assert(out != nullptr);

  // Probe-major order is preserved exactly: finish every build match for one
  // probe row before advancing. Return after one output and retain the next
  // hash slot so join fanout never becomes materialized operator state.
  while (m_probe_cursor_row < m_curr_probe_size) {
    const size_t probe_idx = m_probe_cursor_row;

    if (!m_probe_cursor_key_ready) {
      ++m_stats.probe_rows;
      m_probe_cursor_found_match = false;

      const JoinKeyResult key_result = BuildJoinKey(m_probe_columns, probe_idx, m_probe_input_tables);
      if (key_result == JoinKeyResult::ERROR) return 1;

      m_probe_cursor_key_ready = true;
      m_probe_cursor_slot = kInvalidHashSlot;
      if (key_result == JoinKeyResult::OK) {
        m_probe_cursor_hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
        const size_t bucket_idx = m_probe_cursor_hash & (m_hash_table_size - 1);
        m_probe_cursor_slot = m_hash_buckets[bucket_idx];
      }
    }

    while (m_probe_cursor_slot != kInvalidHashSlot) {
      const size_t slot_idx = m_probe_cursor_slot;
      const HashSlot &slot = m_hash_slots[slot_idx];
      // Advance before any return so the next call resumes at the next slot.
      m_probe_cursor_slot = slot.next;

      const bool same_key = slot.hash == m_probe_cursor_hash && slot.key_length == m_join_key_buffer.length() &&
                            (slot.key_length == 0 || std::memcmp(m_hash_key_arena.data() + slot.key_offset,
                                                                 m_join_key_buffer.ptr(), slot.key_length) == 0);
      ++m_stats.hash_slots_visited;
      if (!same_key) {
        ++m_stats.hash_key_mismatches;
        continue;
      }

      bool passes_extra_conditions = true;
      if (m_extra_condition != nullptr) {
        if (LoadRowFromColumnChunks(m_build_columns, slot.build_row_idx, m_build_input_tables) ||
            LoadRowFromColumnChunks(m_probe_columns, probe_idx, m_probe_input_tables))
          return 1;
        m_stats.extra_condition_row_materializations += 2;
        passes_extra_conditions = EvaluateExtraConditions();
        if (thd()->is_error()) return 1;
      }
      if (!passes_extra_conditions) continue;

      m_probe_cursor_found_match = true;
      if (m_join_type == JoinType::ANTI) {
        AdvanceProbeRow();
        break;
      }

      if (m_join_type == JoinType::SEMI) {
        *out = {slot.build_row_idx, probe_idx, false};
        ++m_stats.output_rows;
        AdvanceProbeRow();
        return 0;
      }

      *out = {slot.build_row_idx, probe_idx, false};
      ++m_stats.output_rows;
      return 0;
    }

    // ANTI matched and already advanced to the next probe row.
    if (m_probe_cursor_row != probe_idx) continue;

    if (!m_probe_cursor_found_match && (m_join_type == JoinType::OUTER || m_join_type == JoinType::ANTI)) {
      *out = {0, probe_idx, true};
      ++m_stats.output_rows;
      AdvanceProbeRow();
      return 0;
    }

    AdvanceProbeRow();
  }

  return -1;
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
      if (!is_null) m_stats.bytes_copied += field->pack_length();
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

      if (chunk.nullable_fast(row_idx)) {
        field->set_null();
      } else {
        field->set_notnull();
        const bool normalized_batch = (&chunks == &m_build_columns && m_build_batch_input != nullptr) ||
                                      (&chunks == &m_probe_columns && m_probe_batch_input != nullptr);
        if (normalized_batch) {
          // Direct BatchReadable children expose IMCS normalized column bytes,
          // not necessarily a byte-for-byte MySQL record image. Use the same
          // fixed-width adapter contract as VectorizedTableScanIterator.
          field->pack(const_cast<uchar *>(field->data_ptr()), chunk.data_fast(row_idx), chunk.width());
        } else {
          // Row input was captured from Field::data_ptr(), so it is already a
          // MySQL record image and must be restored byte-for-byte.
          std::memcpy(const_cast<uchar *>(field->data_ptr()), chunk.data_fast(row_idx), chunk.width());
        }
        m_stats.bytes_copied += chunk.width();
      }

      chunk_idx++;
    }
  }

  return false;
}

bool VectorizedHashJoinIterator::TryBuildDirectJoinKey(const std::vector<ColumnChunk> &columns, size_t row_idx,
                                                       JoinKeyResult *result) {
  if (result == nullptr || m_join_conditions.empty()) return false;
  m_join_key_buffer.length(0);

  auto find_chunk = [&columns](Field *field) -> const ColumnChunk * {
    if (field == nullptr) return nullptr;
    for (const ColumnChunk &chunk : columns) {
      if (chunk.valid() && chunk.table() == field->table && chunk.field_index() == field->field_index()) return &chunk;
    }
    return nullptr;
  };

  for (const HashJoinCondition &condition : m_join_conditions) {
    Item_eq_base *eq = condition.join_condition();
    if (eq == nullptr || eq->functype() != Item_func::EQ_FUNC || eq->argument_count() != 2) return false;

    Item *left = eq->arguments()[0]->real_item();
    Item *right = eq->arguments()[1]->real_item();
    if (left->type() != Item::FIELD_ITEM || right->type() != Item::FIELD_ITEM) return false;
    Field *left_field = down_cast<Item_field *>(left)->field;
    Field *right_field = down_cast<Item_field *>(right)->field;
    if (left_field == nullptr || right_field == nullptr || left_field->type() != right_field->type() ||
        left_field->is_unsigned() || right_field->is_unsigned())
      return false;
    if (left_field->type() != MYSQL_TYPE_LONG && left_field->type() != MYSQL_TYPE_LONGLONG) return false;

    const ColumnChunk *left_chunk = find_chunk(left_field);
    const ColumnChunk *right_chunk = find_chunk(right_field);
    if ((left_chunk == nullptr) == (right_chunk == nullptr)) return false;
    const ColumnChunk *chunk = left_chunk != nullptr ? left_chunk : right_chunk;
    Field *field = left_chunk != nullptr ? left_field : right_field;
    if (row_idx >= chunk->size()) {
      *result = JoinKeyResult::ERROR;
      return true;
    }
    if (chunk->nullable_fast(row_idx)) {
      *result = JoinKeyResult::NULL_KEY;
      return true;
    }

    const uint8_t type = static_cast<uint8_t>(field->type());
    if (m_join_key_buffer.append(pointer_cast<const char *>(&type), sizeof(type))) {
      *result = JoinKeyResult::ERROR;
      return true;
    }

    longlong value = 0;
    if (field->type() == MYSQL_TYPE_LONG && chunk->width() == sizeof(int32_t)) {
      int32_t raw;
      std::memcpy(&raw, chunk->data_fast(row_idx), sizeof(raw));
      value = static_cast<longlong>(raw);
    } else if (field->type() == MYSQL_TYPE_LONGLONG && chunk->width() == sizeof(int64_t)) {
      int64_t raw;
      std::memcpy(&raw, chunk->data_fast(row_idx), sizeof(raw));
      value = static_cast<longlong>(raw);
    } else {
      return false;
    }
    if (m_join_key_buffer.append(pointer_cast<const char *>(&value), sizeof(value))) {
      *result = JoinKeyResult::ERROR;
      return true;
    }
  }

  *result = JoinKeyResult::OK;
  ++m_stats.direct_join_key_rows;
  return true;
}

VectorizedHashJoinIterator::JoinKeyResult VectorizedHashJoinIterator::BuildJoinKey(
    const std::vector<ColumnChunk> &columns, size_t row_idx, const pack_rows::TableCollection &tables) {
  JoinKeyResult direct_result = JoinKeyResult::ERROR;
  if (TryBuildDirectJoinKey(columns, row_idx, &direct_result)) return direct_result;

  if (LoadRowFromColumnChunks(columns, row_idx, tables)) return JoinKeyResult::ERROR;
  ++m_stats.join_key_row_materializations;

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
    if (col_chunks.size() != m_lookahead_chunks.size()) return 1;

    size_t to_copy = std::min(m_lookahead_count, capacity);
    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      if (!col_chunks[ci].valid()) continue;  // caller does not request this slot

      const ColumnChunk &src = m_lookahead_chunks[ci];
      if (!src.valid() || src.table() != col_chunks[ci].table() || src.field_index() != col_chunks[ci].field_index() ||
          m_lookahead_start + to_copy > src.size()) {
        return 1;
      }
      for (size_t r = 0; r < to_copy; ++r) {
        const size_t source_row = m_lookahead_start + r;
        if (!col_chunks[ci].append_from(src, source_row)) return 1;
        if (!src.nullable_fast(source_row)) m_stats.bytes_copied += src.width();
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
    const ColumnChunk &target = col_chunks[ci];
    const auto &mapping = m_chunk_map[ci];
    if (!target.valid()) {
      needs_reinit = mapping.source_columns != nullptr;
      continue;
    }
    if (mapping.source_columns == nullptr || mapping.source_col_idx >= mapping.source_columns->size()) {
      needs_reinit = true;
      continue;
    }
    const ColumnChunk &source = (*mapping.source_columns)[mapping.source_col_idx];
    needs_reinit = source.table() != target.table() || source.field_index() != target.field_index();
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
    if (m_state == State::END_OF_ROWS) break;

    if (m_probe_cursor_row >= m_curr_probe_size) {
      const int result = ReadProbeBatch();
      if (result != 0) {
        if (result != -1) return result;
        m_state = State::END_OF_ROWS;
        break;
      }
    }

    OutputRow out;
    const int result = NextProbeOutput(&out);
    if (result == -1) {
      m_curr_probe_size = 0;
      continue;
    }
    if (result != 0) return result;

    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      if (!col_chunks[ci].valid()) continue;

      const auto &mapping = m_chunk_map[ci];
      if (mapping.source_columns == nullptr || mapping.source_col_idx >= mapping.source_columns->size()) {
        // A valid requested output column must map to exactly one input column.
        // Returning NULL here would silently turn a layout bug into wrong data.
        return 1;
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
      if (src_row >= src.size()) return 1;
      if (!col_chunks[ci].append_from(src, src_row)) return 1;
      if (!src.nullable_fast(src_row)) m_stats.bytes_copied += src.width();
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
    if (chunks[i].valid() != m_lookahead_chunks[i].valid()) {
      rebuild = true;
      break;
    }
    if (!chunks[i].valid()) continue;
    rebuild = m_lookahead_chunks[i].capacity() < tail_len || m_lookahead_chunks[i].table() != chunks[i].table() ||
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
      if (!m_lookahead_chunks[ci].append_from(chunks[ci], r)) {
        m_lookahead_count = 0;
        m_lookahead_start = 0;
        return;
      }
      if (!chunks[ci].nullable_fast(r)) m_stats.bytes_copied += chunks[ci].width();
    }
  }

  m_lookahead_start = 0;
  m_lookahead_count = tail_len;
}
}  // namespace Executor
}  // namespace ShannonBase
