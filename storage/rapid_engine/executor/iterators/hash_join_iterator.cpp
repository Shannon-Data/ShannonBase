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
 * vectorized hash join iterator impl for rapid engine.
 *
 * Vectorized, not parallelized: the whole executor is single-threaded, and
 * ColumnChunk is deliberately single-consumer (see iterator.h).
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

  // Spill state is per-execution: a re-execution (correlated subquery / PS
  // reuse) must not inherit another run's partitions or merge cursors.
  m_spilled = false;
  m_spill_output_ready = false;
  m_probe_ordinal_counter = 0;
  for (auto &f : m_build_partitions) f.reset();
  for (auto &f : m_probe_partitions) f.reset();
  m_output_runs.clear();
  m_unmatched_run.reset();
  m_merge_heads.clear();
  m_probe_ordinals.clear();
  m_build_capacity_exceeded = false;
  m_spill_rows = 0;
  m_spill_bytes = 0;

  // This operator spills, but only through its own order-preserving protocol
  // (grace-hash partitioning plus an ordinal merge), entered on demand from
  // BuildHashTable(). It does not implement MySQL's unordered
  // chunk-file protocol, which is what allow_spill_to_disk selects and which
  // the optimizer routes to the server's own iterator instead
  // (CreateIteratorFromAccessPath only picks this class when the flag is off).
  // Keep the invariant explicit so a future caller cannot route a plan built
  // for the unordered protocol into this implementation.
  if (m_allow_spill_to_disk) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Rapid vectorized hash join implements only ordered spill-to-disk, not the unordered protocol");
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
  // A child may implement BatchReadable yet be unable to serve batches for this
  // execution (e.g. a nested hash join holding record images).
  if (m_build_batch_input != nullptr && !m_build_batch_input->SupportsBatchRead()) m_build_batch_input = nullptr;
  if (m_probe_batch_input != nullptr && !m_probe_batch_input->SupportsBatchRead()) m_probe_batch_input = nullptr;

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
  // A row-mode input is captured from Field::field_ptr(), i.e. a MySQL record
  // image, so its retained chunks must be pack_length()-wide. Sizing them by
  // Util::normalized_length() truncates every VARCHAR/CHAR value to the 4-byte
  // dictionary-id width and restores it over a stale record buffer.
  const bool build_row_image = (m_build_batch_input == nullptr);
  const bool probe_row_image = (m_probe_batch_input == nullptr);
  m_columns_hold_row_images = build_row_image || probe_row_image;
  // Remembered per side: the two can differ (one input batch-mode, the other
  // row-mode), and the spill merge must rebuild its scratch rows with exactly
  // the layout each side was written with.
  m_build_row_image = build_row_image;
  m_probe_row_image = probe_row_image;
  if (InitializeColumnChunks(m_build_input_tables, m_build_columns, build_capacity, false, build_row_image) ||
      InitializeColumnChunks(m_probe_input_tables, m_probe_columns, m_batch_size, false, probe_row_image) ||
      (m_build_batch_input != nullptr &&
       InitializeColumnChunks(m_build_input_tables, m_build_batch_columns, m_batch_size, true, false)) ||
      (m_probe_batch_input != nullptr &&
       InitializeColumnChunks(m_probe_input_tables, m_probe_batch_columns, m_batch_size, true, false))) {
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
                                                        bool input_layout, bool row_image) {
  chunks.clear();
  const auto make_chunk = [row_image, capacity](std::vector<ColumnChunk> &out, Field *field) {
    if (row_image)
      out.emplace_back(field, capacity, field->pack_length());
    else
      out.emplace_back(field, capacity);
  };

  if (input_layout && tables.tables().size() == 1) {
    const pack_rows::Table &packed_table = tables.tables()[0];
    TABLE *table = packed_table.table;
    chunks.reserve(table->s->fields);
    for (uint field_idx = 0; field_idx < table->s->fields; ++field_idx) {
      Field *field = table->field[field_idx];
      bool required = bitmap_is_set(table->read_set, field_idx);
      for (const pack_rows::Column &column : packed_table.columns) required = required || column.field == field;
      if (required && !field->is_flag_set(NOT_SECONDARY_FLAG))
        make_chunk(chunks, field);
      else
        chunks.emplace_back(nullptr, 0);
    }
    return false;
  }

  chunks.reserve(tables.tables().size() * 10);

  for (const pack_rows::Table &table : tables.tables()) {
    for (const pack_rows::Column &column : table.columns) {
      make_chunk(chunks, column.field);
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
  // Out of budget is no longer fatal: report it upwards so BuildHashTable()
  // can switch to the partitioned external join instead of failing.
  if (!growth_fits(new_capacity)) return SignalBuildOverflow();

  try {
    for (ColumnChunk &chunk : m_build_columns) {
      if (chunk.valid() && !chunk.grow(new_capacity)) return SignalBuildOverflow();
    }
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not grow build columns");
    m_build_error = true;
    return true;
  }

  UpdateBuildMemoryPeak();
  if (BuildMemoryWouldExceed(0)) return SignalBuildOverflow();
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

// Ordered external spill
VectorizedHashJoinIterator::SpillFile::SpillFile() : file(std::tmpfile()) {}

VectorizedHashJoinIterator::SpillFile::~SpillFile() {
  if (file != nullptr) std::fclose(file);
}

bool VectorizedHashJoinIterator::SpillFile::RewindForRead() {
  if (file == nullptr) return true;
  // fseek() is the required synchronization point when turning an update
  // stream from writing to reading.
  std::clearerr(file);
  return std::fseek(file, 0, SEEK_SET) != 0;
}

size_t VectorizedHashJoinIterator::SerializedRowBytes(const std::vector<ColumnChunk> &chunks) {
  size_t bytes = 0;
  for (const ColumnChunk &chunk : chunks) bytes += 1 + chunk.width();
  return bytes;
}

bool VectorizedHashJoinIterator::WriteSpillRaw(SpillFile *file, const void *data, size_t length) {
  if (file == nullptr || file->file == nullptr) return true;
  if (length == 0) return false;
  if (std::fwrite(data, 1, length, file->file) != length) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not write its spill file");
    return true;
  }
  file->bytes_written += length;
  // Deliberately not m_stats.bytes_copied: that counter means "bytes moved
  // between in-memory chunks" everywhere else. Spill traffic is reported
  // separately as spill_bytes.
  m_spill_bytes += length;
  return false;
}

bool VectorizedHashJoinIterator::WriteSpillRow(SpillFile *file, const std::vector<ColumnChunk> &chunks,
                                               size_t row_idx) {
  for (const ColumnChunk &chunk : chunks) {
    const uint8_t is_null = chunk.nullable(row_idx) ? 1 : 0;
    if (WriteSpillRaw(file, &is_null, sizeof(is_null))) return true;
    // The payload is written even for a NULL so every row occupies the same
    // number of bytes; a variable-length encoding would buy little here and
    // would make a short read impossible to distinguish from corruption.
    if (WriteSpillRaw(file, chunk.data(row_idx), chunk.width())) return true;
  }
  ++file->records;
  ++m_spill_rows;
  return false;
}

bool VectorizedHashJoinIterator::ReadSpillRow(SpillFile *file, std::vector<ColumnChunk> &chunks, bool *eof) {
  assert(eof != nullptr);
  *eof = false;
  if (file == nullptr || file->file == nullptr) {
    *eof = true;
    return false;
  }

  for (size_t i = 0; i < chunks.size(); ++i) {
    ColumnChunk &chunk = chunks[i];
    uint8_t is_null = 0;
    if (std::fread(&is_null, 1, sizeof(is_null), file->file) != sizeof(is_null)) {
      // A clean EOF is only legitimate before the first column of a row.
      if (i == 0 && std::feof(file->file)) {
        *eof = true;
        return false;
      }
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return true;
    }

    m_spill_row_buffer.resize(std::max(m_spill_row_buffer.size(), chunk.width()));
    if (chunk.width() > 0 && std::fread(m_spill_row_buffer.data(), 1, chunk.width(), file->file) != chunk.width()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return true;
    }
    if (!chunk.add(m_spill_row_buffer.data(), chunk.width(), is_null != 0)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not rebuild a spilled row");
      return true;
    }
  }
  return false;
}

bool VectorizedHashJoinIterator::SignalBuildOverflow() {
  m_build_capacity_exceeded = true;
  return true;
}

bool VectorizedHashJoinIterator::ReportBuildMemoryLimit() {
  ++m_stats.build_memory_limit_hits;
  m_build_error = true;
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
           "Rapid hash join could not fit even its smallest hash table in the join buffer; "
           "raise join_buffer_size");
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
      if (m_spilled) {
        // The build side did not fit. Everything is on disk now: partition the probe side the same way, join partition
        // by partition, then merge the per-partition results back into probe order.
        if (PartitionProbeInput()) return 1;
        if (ProcessSpilledPartitions() != 0) return 1;
      }
      m_state = State::PROBING_HASH_TABLE;
      m_curr_probe_size = 0;
      ResetProbeCursor();
    }

    if (m_state == State::END_OF_ROWS) return -1;

    if (m_spilled) {
      bool have_row = false;
      if (NextMergedOutput(&have_row) != 0) return 1;
      if (!have_row) {
        m_state = State::END_OF_ROWS;
        return -1;
      }
      return 0;
    }

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

bool VectorizedHashJoinIterator::ResetHashTable(double expected_rows) {
  std::vector<size_t>().swap(m_hash_buckets);
  std::vector<size_t>().swap(m_hash_bucket_tails);
  std::vector<HashSlot>().swap(m_hash_slots);
  std::vector<uchar>().swap(m_hash_key_arena);

  size_t desired = static_cast<size_t>(std::max(0.0, expected_rows)) / kTargetLoadFactor;
  desired = std::max<size_t>(1024, std::min<size_t>(desired, 1ULL << 28));

  if (m_max_memory_available > 0) {
    const size_t used = CurrentBuildMemoryUsage();
    if (used >= m_max_memory_available) return true;
    const size_t remaining = m_max_memory_available - used;
    const size_t max_bucket_bytes = remaining / 4;
    const size_t max_buckets = max_bucket_bytes / (2 * sizeof(size_t));
    if (max_buckets == 0) return true;
    desired = std::min(desired, max_buckets);
  }

  m_hash_table_size = 1;
  while (m_hash_table_size <= desired / 2) m_hash_table_size <<= 1;

  if (m_hash_table_size > std::numeric_limits<size_t>::max() / (2 * sizeof(size_t))) return true;
  try {
    m_hash_buckets.assign(m_hash_table_size, kInvalidHashSlot);
    m_hash_bucket_tails.assign(m_hash_table_size, kInvalidHashSlot);
  } catch (const std::bad_alloc &) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized hash join could not allocate hash buckets");
    return true;
  }
  return false;
}

/**
 * Index one already-resident build row into the hash table.
 *
 * `overflowed` distinguishes "this row does not fit in the budget" from a hard
 * error, so the caller can choose to start spilling rather than fail.
 */
bool VectorizedHashJoinIterator::IndexBuildRow(size_t row_idx, bool *overflowed, uint64_t *out_hash) {
  if (overflowed != nullptr) *overflowed = false;

  const JoinKeyResult key_result = BuildJoinKey(m_build_columns, row_idx, m_build_input_tables);
  if (key_result == JoinKeyResult::ERROR) return true;
  if (key_result == JoinKeyResult::NULL_KEY) return false;  // never matches

  const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
  if (out_hash != nullptr) *out_hash = hash;

  const size_t bucket_idx = hash & (m_hash_table_size - 1);
  const size_t key_offset = m_hash_key_arena.size();
  const size_t key_length = m_join_key_buffer.length();
  if (key_length > std::numeric_limits<size_t>::max() - key_offset) return true;

  const size_t slot_bytes = (m_hash_slots.size() + 1) * sizeof(HashSlot);
  if (overflowed != nullptr &&
      (BuildMemoryWouldExceed(slot_bytes) || BuildMemoryWouldExceed(key_offset + key_length))) {
    *overflowed = true;
    return false;
  }
  if (EnsureHashSlotCapacity(m_hash_slots.size() + 1) || EnsureHashKeyCapacity(key_offset + key_length)) return true;

  if (key_length != 0) {
    const auto *begin = pointer_cast<const uchar *>(m_join_key_buffer.ptr());
    m_hash_key_arena.insert(m_hash_key_arena.end(), begin, begin + key_length);
  }

  const size_t slot_idx = m_hash_slots.size();
  m_hash_slots.push_back({hash, key_offset, key_length, row_idx, kInvalidHashSlot});
  m_stats.key_bytes += key_length;
  if (m_hash_buckets[bucket_idx] == kInvalidHashSlot) {
    m_hash_buckets[bucket_idx] = slot_idx;
  } else {
    m_hash_slots[m_hash_bucket_tails[bucket_idx]].next = slot_idx;
  }
  m_hash_bucket_tails[bucket_idx] = slot_idx;
  return false;
}

bool VectorizedHashJoinIterator::BuildHashTable() {
  // A rebuild must not inherit capacity from the previous execution; retained
  // capacity is real memory and therefore part of the budget.
  if (ResetHashTable(m_estimated_build_rows)) return ReportBuildMemoryLimit();
  UpdateBuildMemoryPeak();
  if (BuildMemoryWouldExceed(0)) return ReportBuildMemoryLimit();

  for (auto &chunk : m_build_columns) chunk.clear();
  m_curr_build_size = 0;
  m_build_error = false;

  size_t total_build_rows = 0;
  for (;;) {
    m_build_capacity_exceeded = false;
    if (!ReadBuildBatch()) {
      if (m_build_capacity_exceeded && !m_spilled && !m_build_error) {
        // The build buffer cannot grow any further. Move what is buffered to
        // disk and keep reading; from here the build side is partitioned.
        if (BeginBuildSpill(total_build_rows)) {
          m_build_error = true;
          return true;
        }
        for (auto &chunk : m_build_columns) chunk.clear();
        total_build_rows = 0;
        continue;
      }
      break;
    }

    if (m_spilled) {
      // Already partitioning: this batch goes straight to disk and the buffer
      // is reused, so build-side memory stays flat from here on.
      if (SpillBuildRange(0, m_curr_build_size)) {
        m_build_error = true;
        return true;
      }
      for (auto &chunk : m_build_columns) chunk.clear();
      continue;
    }

    for (size_t i = total_build_rows; i < total_build_rows + m_curr_build_size; ++i) {
      ++m_stats.build_rows;
      bool overflowed = false;
      if (IndexBuildRow(i, &overflowed, nullptr)) {
        m_build_error = true;
        return true;
      }
      if (!overflowed) continue;

      // Out of budget. Rather than aborting the query, switch to a partitioned external join. Everything buffered so
      // far, including this row, moves to disk.
      if (BeginBuildSpill(total_build_rows + m_curr_build_size)) {
        m_build_error = true;
        return true;
      }
      break;
    }

    if (m_spilled) {
      for (auto &chunk : m_build_columns) chunk.clear();
      total_build_rows = 0;
      continue;
    }
    total_build_rows += m_curr_build_size;
  }

  if (m_hash_table_gen != nullptr) m_last_hash_table_gen = *m_hash_table_gen;
  if (m_build_error) return true;

  // Tails are construction-only state. Free them before probing.
  std::vector<size_t>().swap(m_hash_bucket_tails);
  return false;
}

size_t VectorizedHashJoinIterator::SpillPartitionIndex(uint64_t hash, size_t depth) {
  // Partition on the high bits and bucket on the low bits, so the two uses of
  // the same hash stay independent. Each recursion level consumes the next
  // nibble, which redistributes a partition that was too skewed to fit.
  const unsigned shift = 32u + static_cast<unsigned>(4 * depth);
  return static_cast<size_t>((hash >> shift) & (kSpillFanout - 1));
}

bool VectorizedHashJoinIterator::SpillBuildRange(size_t first_row, size_t row_count) {
  for (size_t i = first_row; i < first_row + row_count; ++i) {
    const JoinKeyResult key_result = BuildJoinKey(m_build_columns, i, m_build_input_tables);
    if (key_result == JoinKeyResult::ERROR) return true;
    // A NULL join key can never match, so it need not be partitioned at all.
    if (key_result == JoinKeyResult::NULL_KEY) continue;

    const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
    const size_t partition = SpillPartitionIndex(hash, 0);
    if (m_build_partitions[partition] == nullptr) {
      m_build_partitions[partition] = std::make_unique<SpillFile>();
      if (!m_build_partitions[partition]->valid()) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
        return true;
      }
    }
    if (WriteSpillRow(m_build_partitions[partition].get(), m_build_columns, i)) return true;
  }
  return false;
}

bool VectorizedHashJoinIterator::BeginBuildSpill(size_t buffered_rows) {
  m_spilled = true;
  // Counted in m_stats only: build_memory_limit_hits records that the operator
  // had to partition, and spill_rows/spill_bytes below record how much moved.
  // Nothing about the spill is published outside the operator.
  ++m_stats.build_memory_limit_hits;

  // Stand-in image for the build side of a null-complemented row. Its size is
  // fixed by the build column layout, so build it once for the whole spill
  // rather than per emitted row.
  m_spill_zero_build.assign(SerializedRowBytes(m_build_columns), 0);

  if (SpillBuildRange(0, buffered_rows)) return true;

  // The in-memory table is now redundant: every build row lives in a
  // partition. Release it so the per-partition rebuild starts from an empty
  // budget instead of inheriting this one.
  std::vector<size_t>().swap(m_hash_buckets);
  std::vector<size_t>().swap(m_hash_bucket_tails);
  std::vector<HashSlot>().swap(m_hash_slots);
  std::vector<uchar>().swap(m_hash_key_arena);
  m_hash_table_size = 0;
  return false;
}

/**
 * Drain the probe input once, in order, writing each row to the partition its
 * join key hashes to. The ordinal recorded with the row is its position in the
 * original probe stream; that is what later restores probe order.
 */
bool VectorizedHashJoinIterator::PartitionProbeInput() {
  m_probe_ordinal_counter = 0;

  for (;;) {
    const int result = ReadProbeBatch();
    if (result == -1) break;  // clean EOF
    if (result != 0) return true;

    for (size_t row = 0; row < m_curr_probe_size; ++row) {
      const uint64_t ordinal = m_probe_ordinal_counter++;
      ++m_stats.probe_rows;

      const JoinKeyResult key_result = BuildJoinKey(m_probe_columns, row, m_probe_input_tables);
      if (key_result == JoinKeyResult::ERROR) return true;

      SpillFile *target = nullptr;
      if (key_result == JoinKeyResult::NULL_KEY) {
        // Never matches. Only OUTER/ANTI still owe an output for it, and that
        // output has to land at this ordinal, so it goes to the run that is
        // merged alongside the partition results.
        if (m_join_type != JoinType::OUTER && m_join_type != JoinType::ANTI) continue;
        if (m_unmatched_run == nullptr) {
          m_unmatched_run = std::make_unique<SpillFile>();
          if (!m_unmatched_run->valid()) {
            my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
            return true;
          }
        }
        target = m_unmatched_run.get();
      } else {
        const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
        const size_t partition = SpillPartitionIndex(hash, 0);
        if (m_probe_partitions[partition] == nullptr) {
          m_probe_partitions[partition] = std::make_unique<SpillFile>();
          if (!m_probe_partitions[partition]->valid()) {
            my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
            return true;
          }
        }
        target = m_probe_partitions[partition].get();
      }

      if (WriteSpillRaw(target, &ordinal, sizeof(ordinal))) return true;
      if (target == m_unmatched_run.get()) {
        const uint8_t null_complemented = 1;
        if (WriteSpillRaw(target, &null_complemented, sizeof(null_complemented))) return true;
      }
      if (WriteSpillRow(target, m_probe_columns, row)) return true;
      if (target == m_unmatched_run.get()) {
        // Keep the unmatched run record-compatible with an output run: a
        // zero-filled build image follows, so the merge can read every run the
        // same way.
        if (!m_spill_zero_build.empty() && WriteSpillRaw(target, m_spill_zero_build.data(), m_spill_zero_build.size()))
          return true;
      }
    }
    m_curr_probe_size = 0;
  }
  return false;
}

/**
 * Join one build/probe partition pair in memory and append the results to
 * `output`, each tagged with its probe ordinal.
 *
 * When the build side of a partition still does not fit, the partition is
 * split again on the next nibble of the hash (bounded by kMaxSpillDepth) --
 * the standard recursive grace-hash response to skew.
 */
VectorizedHashJoinIterator::SpillFile *VectorizedHashJoinIterator::NewOutputRun() {
  auto run = std::make_unique<SpillFile>();
  if (!run->valid()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
    return nullptr;
  }
  m_output_runs.push_back(std::move(run));
  return m_output_runs.back().get();
}

int VectorizedHashJoinIterator::ProcessPartitionPair(SpillFile *build_file, SpillFile *probe_file, size_t depth) {
  if (probe_file == nullptr || probe_file->records == 0) return 0;  // nothing can match
  if (build_file == nullptr || build_file->records == 0) {
    // No build rows: only OUTER/ANTI produce anything, and NextProbeOutput()
    // handles that once the (empty) table is in place.
    if (m_join_type != JoinType::OUTER && m_join_type != JoinType::ANTI) return 0;
  }

  if (ResetHashTable(build_file != nullptr ? static_cast<double>(build_file->records) : 0.0)) {
    ReportBuildMemoryLimit();  // sets the error; returning 1 alone would leave none
    return 1;
  }

  // load the build partition
  for (auto &chunk : m_build_columns) chunk.clear();
  size_t build_rows = 0;
  bool build_overflowed = false;
  if (build_file != nullptr) {
    if (build_file->RewindForRead()) return 1;
    for (;;) {
      m_build_capacity_exceeded = false;
      if (EnsureBuildCapacity(build_rows + 1)) {
        if (!m_build_capacity_exceeded) return 1;
        build_overflowed = true;
        break;
      }
      bool eof = false;
      if (ReadSpillRow(build_file, m_build_columns, &eof)) return 1;
      if (eof) break;
      bool overflowed = false;
      if (IndexBuildRow(build_rows, &overflowed, nullptr)) return 1;
      if (overflowed) {
        build_overflowed = true;
        break;
      }
      ++build_rows;
    }
  }

  if (build_overflowed) {
    if (depth + 1 >= kMaxSpillDepth) {
      // Re-splitting redistributes distinct keys, so it cannot break up a
      // partition whose rows all share one join key -- the depth limit is
      // where skew stops being a partitioning problem. Finish the pair by
      // consuming the build side in blocks instead.
      return ProcessPartitionPairBlockwise(build_file, probe_file);
    }
    return RepartitionAndProcess(build_file, probe_file, depth + 1);
  }

  std::vector<size_t>().swap(m_hash_bucket_tails);
  return StreamProbePartition(probe_file);
}

bool VectorizedHashJoinIterator::ReadProbePartitionBatch(SpillFile *probe_file) {
  for (auto &chunk : m_probe_columns) chunk.clear();
  m_probe_ordinals.clear();
  m_curr_probe_size = 0;

  for (size_t i = 0; i < m_batch_size; ++i) {
    uint64_t ordinal = 0;
    const size_t read = std::fread(&ordinal, 1, sizeof(ordinal), probe_file->file);
    if (read == 0 && std::feof(probe_file->file)) break;
    if (read != sizeof(ordinal)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return true;
    }
    bool eof = false;
    if (ReadSpillRow(probe_file, m_probe_columns, &eof)) return true;
    if (eof) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return true;
    }
    m_probe_ordinals.push_back(ordinal);
    ++m_curr_probe_size;
  }
  return false;
}

int VectorizedHashJoinIterator::WriteOutputRecord(SpillFile *output, uint64_t ordinal, bool null_complemented,
                                                  size_t probe_row_idx, size_t build_row_idx) {
  const uint8_t marker = null_complemented ? 1 : 0;
  if (WriteSpillRaw(output, &ordinal, sizeof(ordinal))) return 1;
  if (WriteSpillRaw(output, &marker, sizeof(marker))) return 1;
  if (WriteSpillRow(output, m_probe_columns, probe_row_idx)) return 1;
  if (null_complemented) {
    if (!m_spill_zero_build.empty() && WriteSpillRaw(output, m_spill_zero_build.data(), m_spill_zero_build.size()))
      return 1;
  } else {
    if (WriteSpillRow(output, m_build_columns, build_row_idx)) return 1;
  }
  return 0;
}

/**
 * Join `probe_file` against the build rows currently resident in the hash
 * table, appending every result to a run of this call's own.
 */
int VectorizedHashJoinIterator::StreamProbePartition(SpillFile *probe_file, SpillFile *in_flags, SpillFile *out_flags,
                                                     JoinType logical_type) {
  // Rows enter the run in probe order, so it is ordinal-ascending by
  // construction and the merge can rely on that. The run is created on the
  // first output, so a pass that emits nothing -- an ANTI block pass, or a
  // partition with no matches -- leaves no empty run behind.
  SpillFile *output = nullptr;
  const bool track_matches = (out_flags != nullptr);

  if (probe_file->RewindForRead()) return 1;
  if (in_flags != nullptr && in_flags->RewindForRead()) return 1;

  for (;;) {
    if (ReadProbePartitionBatch(probe_file)) return 1;
    if (m_curr_probe_size == 0) break;

    if (track_matches) {
      m_block_prev_flags.assign(m_curr_probe_size, 0);
      // The flag stream is written in partition order by the previous pass and
      // read back in the same order here, so the byte counts line up exactly.
      if (in_flags != nullptr &&
          std::fread(m_block_prev_flags.data(), 1, m_curr_probe_size, in_flags->file) != m_curr_probe_size) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
        return 1;
      }
      m_block_matched.assign(m_curr_probe_size, 0);
    }

    ResetProbeCursor();
    for (;;) {
      OutputRow out;
      const int rc = NextProbeOutput(&out);
      if (rc == -1) break;  // batch exhausted
      if (rc != 0) return rc;

      bool emit = true;
      if (track_matches) {
        // The blockwise caller runs NextProbeOutput() as an INNER join, so
        // what comes back is a raw match. Every per-type decision is made
        // here instead, from state that spans all the blocks.
        const bool matched_earlier = m_block_prev_flags[out.probe_row_idx] != 0;
        switch (logical_type) {
          case JoinType::ANTI:
            // A match means this row produces nothing at all; the unmatched
            // pass at the end emits the rows that never matched.
            emit = false;
            break;
          case JoinType::SEMI:
            // Exactly one row per matching probe row, no matter how many
            // blocks or build rows it matches.
            emit = !matched_earlier && m_block_matched[out.probe_row_idx] == 0;
            break;
          default:
            break;  // INNER and OUTER emit every match
        }
        m_block_matched[out.probe_row_idx] = 1;
      }
      if (!emit) {
        --m_stats.output_rows;  // NextProbeOutput() counted a row we do not emit
        continue;
      }

      if (output == nullptr) {
        output = NewOutputRun();
        if (output == nullptr) return 1;
      }
      if (WriteOutputRecord(output, m_probe_ordinals[out.probe_row_idx], out.is_null_complemented, out.probe_row_idx,
                            out.build_row_idx))
        return 1;
    }

    if (track_matches) {
      for (size_t row = 0; row < m_curr_probe_size; ++row) {
        m_block_prev_flags[row] = static_cast<uint8_t>(m_block_prev_flags[row] | m_block_matched[row]);
      }
      if (WriteSpillRaw(out_flags, m_block_prev_flags.data(), m_curr_probe_size)) return 1;
    }
  }
  return 0;
}

/**
 * Emit what OUTER/ANTI owe to probe rows that matched in no block: one
 * null-complemented row each, at their own ordinal. Written in partition
 * order, so this run is ordinal-ascending like any other.
 */
int VectorizedHashJoinIterator::EmitUnmatchedProbeRows(SpillFile *probe_file, SpillFile *flags) {
  if (probe_file->RewindForRead()) return 1;
  if (flags != nullptr && flags->RewindForRead()) return 1;

  SpillFile *output = nullptr;
  for (;;) {
    if (ReadProbePartitionBatch(probe_file)) return 1;
    if (m_curr_probe_size == 0) break;

    m_block_prev_flags.assign(m_curr_probe_size, 0);
    if (flags != nullptr &&
        std::fread(m_block_prev_flags.data(), 1, m_curr_probe_size, flags->file) != m_curr_probe_size) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return 1;
    }

    for (size_t row = 0; row < m_curr_probe_size; ++row) {
      if (m_block_prev_flags[row] != 0) continue;
      if (output == nullptr) {
        output = NewOutputRun();
        if (output == nullptr) return 1;
      }
      ++m_stats.output_rows;
      if (WriteOutputRecord(output, m_probe_ordinals[row], /*null_complemented=*/true, row, 0)) return 1;
    }
  }
  return 0;
}

/**
 * Finish a partition pair that re-splitting cannot break up -- in practice a
 * single join-key value whose build rows alone outgrow the join buffer.
 *
 * The build partition is consumed in memory-sized blocks; the probe partition
 * is streamed once per block, into a run of that block's own. Every run is
 * still ordinal-ascending, because each pass reads the probe partition in
 * ordinal order, so the merge restores exact probe order as usual. Rows of one
 * probe row that matched in different blocks land in different runs, but they
 * share an ordinal and therefore still come out adjacent.
 *
 * OUTER/SEMI/ANTI need something an individual block cannot answer: whether a
 * probe row matched *anywhere*. That is carried between blocks as one byte per
 * probe row of the partition, written in partition order, so it costs
 * sequential I/O rather than memory proportional to the partition: each pass
 * reads the previous block's flags and writes their union with its own
 * matches. OUTER and ANTI then owe a null-complemented row to whatever the
 * final flags still leave at zero, and SEMI uses the flags to emit one row per
 * matching probe row rather than one per block that matched.
 */
int VectorizedHashJoinIterator::ProcessPartitionPairBlockwise(SpillFile *build_file, SpillFile *probe_file) {
  if (build_file == nullptr || build_file->records == 0) return 0;  // nothing can match
  if (probe_file == nullptr || probe_file->records == 0) return 0;
  ++m_stats.build_memory_limit_hits;
  if (build_file->RewindForRead()) return 1;

  const JoinType logical_type = m_join_type;
  // A block sees only part of the build side, so it can decide nothing except
  // "these rows match". Run the probe passes as an INNER join and let
  // StreamProbePartition() apply the real semantics from the flags; restore
  // the type on every exit path.
  struct JoinTypeRestorer {
    VectorizedHashJoinIterator *self;
    JoinType saved;
    ~JoinTypeRestorer() { self->m_join_type = saved; }
  } restorer{this, logical_type};
  m_join_type = JoinType::INNER;

  const bool needs_flags = (logical_type != JoinType::INNER);
  std::unique_ptr<SpillFile> matched_flags;

  for (;;) {
    if (ResetHashTable(static_cast<double>(build_file->records))) {
      ReportBuildMemoryLimit();
      return 1;
    }
    for (auto &chunk : m_build_columns) chunk.clear();

    size_t build_rows = 0;
    bool exhausted = false;
    for (;;) {
      // Where this row starts, so a row that does not fit in the current block
      // can be re-read as the first row of the next one.
      const long row_pos = std::ftell(build_file->file);
      if (row_pos < 0) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not position a spill file");
        return 1;
      }

      m_build_capacity_exceeded = false;
      if (EnsureBuildCapacity(build_rows + 1)) {
        if (!m_build_capacity_exceeded) return 1;
        break;  // nothing was read, so the file already points at this row
      }

      bool eof = false;
      if (ReadSpillRow(build_file, m_build_columns, &eof)) return 1;
      if (eof) {
        exhausted = true;
        break;
      }

      bool overflowed = false;
      if (IndexBuildRow(build_rows, &overflowed, nullptr)) return 1;
      if (overflowed) {
        if (std::fseek(build_file->file, row_pos, SEEK_SET) != 0) {
          my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not position a spill file");
          return 1;
        }
        break;
      }
      ++build_rows;
    }

    if (build_rows == 0) {
      if (exhausted) break;
      // Not even one build row fits: no amount of blocking helps.
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Rapid hash join could not fit a single build row in the join buffer; raise join_buffer_size");
      return 1;
    }

    std::vector<size_t>().swap(m_hash_bucket_tails);

    std::unique_ptr<SpillFile> next_flags;
    if (needs_flags) {
      next_flags = std::make_unique<SpillFile>();
      if (!next_flags->valid()) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
        return 1;
      }
    }
    const int rc = StreamProbePartition(probe_file, matched_flags.get(), next_flags.get(), logical_type);
    if (rc != 0) return rc;
    if (needs_flags) matched_flags = std::move(next_flags);

    if (exhausted) break;
  }

  if (logical_type == JoinType::OUTER || logical_type == JoinType::ANTI) {
    return EmitUnmatchedProbeRows(probe_file, matched_flags.get());
  }
  return 0;
}

/**
 * Split one skewed partition pair on the next nibble of the hash and process
 * the children. Rows are re-read one at a time, so this needs no more memory
 * than a single row plus the child file handles.
 */
int VectorizedHashJoinIterator::RepartitionAndProcess(SpillFile *build_file, SpillFile *probe_file, size_t depth) {
  ++m_stats.build_memory_limit_hits;
  std::array<std::unique_ptr<SpillFile>, kSpillFanout> child_build;
  std::array<std::unique_ptr<SpillFile>, kSpillFanout> child_probe;

  auto ensure = [&](std::array<std::unique_ptr<SpillFile>, kSpillFanout> &slots, size_t idx) -> SpillFile * {
    if (slots[idx] == nullptr) {
      slots[idx] = std::make_unique<SpillFile>();
      if (!slots[idx]->valid()) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join could not create a spill file");
        return nullptr;
      }
    }
    return slots[idx].get();
  };

  if (build_file != nullptr && build_file->records > 0) {
    if (build_file->RewindForRead()) return 1;
    for (;;) {
      for (auto &chunk : m_build_columns) chunk.clear();
      bool eof = false;
      if (ReadSpillRow(build_file, m_build_columns, &eof)) return 1;
      if (eof) break;
      const JoinKeyResult key_result = BuildJoinKey(m_build_columns, 0, m_build_input_tables);
      if (key_result == JoinKeyResult::ERROR) return 1;
      if (key_result == JoinKeyResult::NULL_KEY) continue;
      const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
      SpillFile *dest = ensure(child_build, SpillPartitionIndex(hash, depth));
      if (dest == nullptr) return 1;
      if (WriteSpillRow(dest, m_build_columns, 0)) return 1;
    }
  }

  if (probe_file->RewindForRead()) return 1;
  for (;;) {
    uint64_t ordinal = 0;
    const size_t read = std::fread(&ordinal, 1, sizeof(ordinal), probe_file->file);
    if (read == 0 && std::feof(probe_file->file)) break;
    if (read != sizeof(ordinal)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
      return 1;
    }
    for (auto &chunk : m_probe_columns) chunk.clear();
    bool eof = false;
    if (ReadSpillRow(probe_file, m_probe_columns, &eof) || eof) return 1;
    const JoinKeyResult key_result = BuildJoinKey(m_probe_columns, 0, m_probe_input_tables);
    if (key_result == JoinKeyResult::ERROR) return 1;
    if (key_result == JoinKeyResult::NULL_KEY) continue;  // already routed to the unmatched run
    const uint64_t hash = XXH64(m_join_key_buffer.ptr(), m_join_key_buffer.length(), 0);
    SpillFile *dest = ensure(child_probe, SpillPartitionIndex(hash, depth));
    if (dest == nullptr) return 1;
    if (WriteSpillRaw(dest, &ordinal, sizeof(ordinal))) return 1;
    if (WriteSpillRow(dest, m_probe_columns, 0)) return 1;
  }

  for (size_t i = 0; i < kSpillFanout; ++i) {
    const int rc = ProcessPartitionPair(child_build[i].get(), child_probe[i].get(), depth);
    if (rc != 0) return rc;
  }
  return 0;
}

int VectorizedHashJoinIterator::ProcessSpilledPartitions() {
  for (size_t i = 0; i < kSpillFanout; ++i) {
    if (m_probe_partitions[i] == nullptr) continue;
    const int rc = ProcessPartitionPair(m_build_partitions[i].get(), m_probe_partitions[i].get(), 0);
    if (rc != 0) return rc;
  }

  // Every run is ordinal-sorted because probe rows entered their partition in
  // probe order, so a k-way merge over them yields the original probe order.
  m_merge_heads.clear();
  for (auto &run : m_output_runs) {
    if (run == nullptr) continue;
    if (run->RewindForRead()) return 1;
    m_merge_heads.push_back(MergeHead{false, 0, false, run.get()});
  }
  if (m_unmatched_run != nullptr) {
    if (m_unmatched_run->RewindForRead()) return 1;
    m_merge_heads.push_back(MergeHead{false, 0, false, m_unmatched_run.get()});
  }

  for (MergeHead &head : m_merge_heads) {
    if (RefillMergeHead(&head)) return 1;
  }

  // Single-row scratch used to rehydrate a merged record.
  // Must mirror the per-side layout used when the rows were written. Using the
  // OR of the two flags here made the scratch chunks a different width from
  // m_build_columns/m_probe_columns whenever the two inputs disagreed, which
  // desynchronised the record stream: each read consumed the wrong number of
  // bytes, so the next header picked up payload as if it were an ordinal and
  // the merge saw plausible-looking but wrong ordinals.
  if (InitializeColumnChunks(m_probe_input_tables, m_merge_probe_row, 1, false, m_probe_row_image) ||
      InitializeColumnChunks(m_build_input_tables, m_merge_build_row, 1, false, m_build_row_image))
    return 1;

  m_spill_output_ready = true;
  return 0;
}

bool VectorizedHashJoinIterator::RefillMergeHead(MergeHead *head) {
  head->valid = false;
  uint64_t ordinal = 0;
  const size_t read = std::fread(&ordinal, 1, sizeof(ordinal), head->run->file);
  if (read == 0 && std::feof(head->run->file)) return false;  // run exhausted
  if (read != sizeof(ordinal)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
    return true;
  }
  uint8_t null_complemented = 0;
  if (std::fread(&null_complemented, 1, sizeof(null_complemented), head->run->file) != sizeof(null_complemented)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash join read a truncated spill record");
    return true;
  }
  head->ordinal = ordinal;
  head->null_complemented = null_complemented != 0;
  head->valid = true;
  return false;
}

int VectorizedHashJoinIterator::NextMergedOutput(bool *have_row, bool *null_complemented, bool materialize) {
  *have_row = false;
  if (null_complemented != nullptr) *null_complemented = false;

  MergeHead *winner = nullptr;
  for (MergeHead &head : m_merge_heads) {
    if (!head.valid) continue;
    if (winner == nullptr || head.ordinal < winner->ordinal) winner = &head;
  }
  if (winner == nullptr) return 0;  // all runs drained

  for (auto &chunk : m_merge_probe_row) chunk.clear();
  for (auto &chunk : m_merge_build_row) chunk.clear();

  bool eof = false;
  if (ReadSpillRow(winner->run, m_merge_probe_row, &eof) || eof) return 1;
  if (ReadSpillRow(winner->run, m_merge_build_row, &eof) || eof) return 1;

  if (null_complemented != nullptr) *null_complemented = winner->null_complemented;

  if (materialize) {
    if (LoadRowFromColumnChunks(m_merge_probe_row, 0, m_probe_input_tables)) return 1;
    ++m_stats.output_row_materializations;
    if (winner->null_complemented) {
      m_build_input->SetNullRowFlag(true);
    } else {
      if (LoadRowFromColumnChunks(m_merge_build_row, 0, m_build_input_tables)) return 1;
      ++m_stats.output_row_materializations;
      m_build_input->SetNullRowFlag(false);
    }
  }

  if (RefillMergeHead(winner)) return 1;
  *have_row = true;
  return 0;
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
      if (!m_build_capacity_exceeded) m_build_error = true;
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
    if (!m_build_capacity_exceeded) m_build_error = true;
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
        auto data = field->field_ptr();
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
          field->pack(field->field_ptr(), chunk.data_fast(row_idx), chunk.width());
        } else {
          std::memcpy(field->field_ptr(), chunk.data_fast(row_idx), chunk.width());
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
    if (m_spilled) {
      // Same ordered-spill protocol as Read(). It has to be handled here as
      // well: the parent decided to consume batches back in its own Init(),
      // when nothing had been read yet and no spill could have happened, so
      // there is no falling back to Read() now. Probing the hash table instead
      // would index the buckets BeginBuildSpill() has already released.
      if (PartitionProbeInput()) return 1;
      if (ProcessSpilledPartitions() != 0) return 1;
    }
    m_state = State::PROBING_HASH_TABLE;
    m_curr_probe_size = 0;
    ResetProbeCursor();
  }

  size_t produced = 0;
  while (produced < capacity) {
    if (m_state == State::END_OF_ROWS) break;

    OutputRow out;
    // Where this output row's columns live. On the spilled path the merge
    // rehydrates one row at a time into scratch chunks that mirror the layout
    // of m_build_columns/m_probe_columns, so the chunk map built above stays
    // valid; only the vector and the row index differ.
    const std::vector<ColumnChunk> *build_source = &m_build_columns;
    const std::vector<ColumnChunk> *probe_source = &m_probe_columns;

    if (m_spilled) {
      bool have_row = false;
      bool null_complemented = false;
      if (NextMergedOutput(&have_row, &null_complemented, /*materialize=*/false) != 0) return 1;
      if (!have_row) {
        m_state = State::END_OF_ROWS;
        break;
      }
      out = {0, 0, null_complemented};
      build_source = &m_merge_build_row;
      probe_source = &m_merge_probe_row;
    } else {
      if (m_probe_cursor_row >= m_curr_probe_size) {
        const int result = ReadProbeBatch();
        if (result != 0) {
          if (result != -1) return result;
          m_state = State::END_OF_ROWS;
          break;
        }
      }

      const int result = NextProbeOutput(&out);
      if (result == -1) {
        m_curr_probe_size = 0;
        continue;
      }
      if (result != 0) return result;
    }

    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      if (!col_chunks[ci].valid()) continue;

      const auto &mapping = m_chunk_map[ci];
      if (mapping.source_columns == nullptr || mapping.source_col_idx >= mapping.source_columns->size()) {
        // A valid requested output column must map to exactly one input column.
        // Returning NULL here would silently turn a layout bug into wrong data.
        return 1;
      }

      const size_t src_idx = mapping.source_col_idx;
      const std::vector<ColumnChunk> *src_cols;
      size_t src_row;

      if (mapping.source_columns == &m_build_columns) {
        if (out.is_null_complemented) {
          if (!col_chunks[ci].add(nullptr, 0, true)) return 1;
          continue;
        }
        src_cols = build_source;
        src_row = out.build_row_idx;
      } else {
        src_cols = probe_source;
        src_row = out.probe_row_idx;
      }
      if (src_idx >= src_cols->size()) return 1;

      const ColumnChunk &src = (*src_cols)[src_idx];
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

bool VectorizedHashJoinIterator::PushbackBatchTail(const std::vector<ColumnChunk> &chunks, size_t from_row,
                                                   size_t total_rows) {
  assert(from_row <= total_rows);
  return PushbackBatchTailShared(chunks, from_row, total_rows, &m_lookahead_chunks, &m_lookahead_start,
                                 &m_lookahead_count, &m_stats.bytes_copied);
}
}  // namespace Executor
}  // namespace ShannonBase
