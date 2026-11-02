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
#include "storage/rapid_engine/executor/iterators/nested_loop_iterator.h"
#include "storage/rapid_engine/executor/iterators/materialize_iterator.h"

#include <utility>

#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/opt_trace.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/optimizer/utils.h"

namespace ShannonBase {
namespace Executor {
bool VectorizedNestedLoopIterator::CanVectorize(AccessPath *path) {
  bool supported = path && path->type == AccessPath::NESTED_LOOP_JOIN;
  if (!supported) return false;

  const JoinType type = path->nested_loop_join().join_type;
  if (type != JoinType::INNER && type != JoinType::OUTER && type != JoinType::SEMI && type != JoinType::ANTI)
    return false;

  size_t tables{0};
  WalkTablesUnderAccessPath(
      path,
      [&](TABLE *table) {
        // Partition_helper supplies native rows for scans and lookups. Price
        // that boundary up front instead of selecting a batch core that will
        // fall back to a native NLJ during Init().
        if (!table || !table->s || !table->read_set || table->part_info != nullptr || ++tables > 64) {
          supported = false;
          return false;
        }
        for (uint i = 0; i < table->s->fields; ++i) {
          if (!bitmap_is_set(table->read_set, i)) continue;
          Field *field = table->field[i];
          if (!field || Utils::IsOffPageField(field) ||
              (Utils::Util::normalized_length(field) != field->pack_length() && !RecordDictionary(field)))
            supported = false;
          // A four-byte CHAR/VARCHAR still holds SQL bytes, not a dictionary ID.
          if (field && Utils::Util::is_string(field->type()) && field->real_type() != MYSQL_TYPE_ENUM &&
              field->real_type() != MYSQL_TYPE_SET && !RecordDictionary(field))
            supported = false;
        }
        return false;
      },
      true);
  return supported;
}

bool SelectVectorizedNestedLoop(THD *thd, AccessPath *path, bool batch_consumer) {
  if (!thd || !VectorizedNestedLoopIterator::CanVectorize(path)) return false;

  bool force{false};
  DBUG_EXECUTE_IF("rapid_disable_vectorized_nlj", { return false; });
  DBUG_EXECUTE_IF("rapid_force_vectorized_nlj", { force = true; });

  if (!batch_consumer && !force) return false;

  double columns{0}, strings{0}, width{0}, record_width{0};
  WalkTablesUnderAccessPath(
      path,
      [&](TABLE *table) {
        record_width += table->s->null_bytes + 1;
        for (uint i = 0; i < table->s->fields; ++i) {
          if (!bitmap_is_set(table->read_set, i)) continue;
          const Field *field = table->field[i];
          ++columns;
          width += Utils::Util::normalized_length(field);
          record_width += field->pack_length();
          strings += RecordDictionary(field) != nullptr;
        }
        return false;
      },
      true);
  using NestedLoopBatchEstimator = ShannonBase::Optimizer::RpdCostEstimator::NestedLoopBatchEstimator;
  using ShannonBase::Optimizer::RapidCostConstants;
  const double budget = std::min<size_t>(Utils::Util::hash_join_memory_budget(thd), 2 * 1024 * 1024);
  const auto work = NestedLoopBatchEstimator::estimate(
      path->num_output_rows(), columns, strings, width, record_width, budget, RapidCostConstants::kHashBuildPerRow,
      RapidCostConstants::kVectorDecompCostPerCell, RapidCostConstants::kHashLookupPerRow,
      RapidCostConstants::kArtRangeSetupCost, RapidCostConstants::kHashBuildCopyPerByte);
  const bool selected = force || work.choose_batches();
  Opt_trace_object trace(&thd->opt_trace, "rapid_nested_loop_transport");
  trace.add("batch_consumer", batch_consumer)
      .add("estimates_valid", work.valid)
      .add("output_rows", path->num_output_rows())
      .add("projected_columns", columns)
      .add("dictionary_columns", strings)
      .add("retained_bytes", work.retained_bytes)
      .add("budget_bytes", budget)
      .add("scalar_cost", work.scalar)
      .add("batch_cost", work.batches)
      .add("chosen_batches", selected)
      .add("forced_for_test", force);
  return selected;
}

namespace detail {
BatchNestedLoop::BatchNestedLoop(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                                 unique_ptr_destroy_only<RowIterator> inner, JoinType type, bool pfs,
                                 const Prealloced_array<TABLE *, 4> &tables,
                                 const Prealloced_array<TABLE *, 4> &outer_tables,
                                 const Prealloced_array<TABLE *, 4> &inner_tables, size_t budget)
    : RowIterator(thd),
      m_outer(std::move(outer)),
      m_inner(std::move(inner)),
      m_native(thd, NewIterator<BorrowedRows>(thd, thd->mem_root, m_outer.get(), &m_skip_outer_init),
               NewIterator<BorrowedRows>(thd, thd->mem_root, m_inner.get()), type, pfs),
      m_type(type),
      m_pfs(pfs),
      m_tables(tables, false, 0, ~table_map{0}),
      m_outer_tables(outer_tables, false, 0, ~table_map{0}),
      m_inner_tables(inner_tables, false, 0, ~table_map{0}),
      m_reservation(ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kNestedLoop, budget)),
      m_memory(m_reservation.bytes()),
      m_outer_chunks(&m_memory),
      m_inner_chunks(&m_memory),
      m_output_columns(&m_memory),
      m_outer_selection(&m_memory),
      m_inner_selection(&m_memory),
      m_outer_nulls(&m_memory),
      m_inner_nulls(&m_memory),
      m_output_nulls(&m_memory),
      m_decode(&m_memory) {}

bool BatchNestedLoop::Init() {
  if (m_pfs) m_inner->EndPSIBatchModeIfStarted();
  m_skip_outer_init = false;
  m_enabled = false;
  m_outer_pos = m_outer_rows = m_inner_pos = m_inner_rows = 0;
  m_outer_eof = m_inner_eof = m_probe = m_found = false;
  m_output_nulls.clear();
  m_inner_block.clear();
  m_inner_block_rows = 0;
  m_inner_block_bytes = 0;
  m_inner_active = nullptr;
  m_outer_batch = dynamic_cast<BatchReadable *>(m_outer->real_iterator());
  m_inner_batch = dynamic_cast<BatchReadable *>(m_inner->real_iterator());
  // The inner side is initialized per outer row. Its execution-dependent
  // SupportsBatchRead() is checked then; no input may be consumed before it.
  if (m_outer_tables.tables().empty() || m_inner_tables.tables().empty() || !m_outer_batch || !m_inner_batch)
    return m_native.Init();

  if (m_outer->Init()) return true;

  // Capability negotiation already initialized the outer input. A native
  // fallback must reuse that execution, including any Init() side effects.
  m_skip_outer_init = true;
  if (!m_outer_batch->SupportsBatchRead()) return m_native.Init();

  for (const auto &entry : m_outer_tables.tables()) {
    if (entry.table->is_nullable() && !m_outer_batch->PreservesBatchNullRows()) return m_native.Init();
  }
  for (const auto &entry : m_inner_tables.tables()) {
    if (entry.table->is_nullable() && !m_inner_batch->PreservesBatchNullRows()) return m_native.Init();
  }

  if (!m_reservation) return AllocationError("Rapid batch nested loop workspace allocation failed");

  size_t width{32}, decode{0};
  for (const auto &entry : m_tables.tables()) {
    for (const auto &column : entry.columns) {
      width += Utils::Util::normalized_length(column.field) + 1;
      decode = std::max(decode, static_cast<size_t>(column.field->pack_length()));
    }
  }
  m_capacity = SelectBatchRows(width, m_reservation.bytes(), 4);
  if (m_capacity == 0) return AllocationError("Rapid batch nested loop row exceeds its workspace");

  if (!Prepare(m_outer_tables, m_outer_chunks) || !Prepare(m_inner_tables, m_inner_chunks) ||
      !Reserve(m_outer_nulls, m_capacity) || !Reserve(m_inner_nulls, m_capacity) ||
      !Reserve(m_output_nulls, m_capacity) || !Reserve(m_decode, decode))
    return AllocationError("Rapid batch nested loop buffers allocation failed");

  m_decode.resize(decode);
  m_enabled = true;
  return false;
}

bool BatchNestedLoop::SupportsBatchRead() const { return m_enabled; }

bool BatchNestedLoop::PreservesBatchNullRows() const { return true; }

int BatchNestedLoop::Read() { return m_native.Read(); }

void BatchNestedLoop::SetNullRowFlag(bool value) {
  m_outer->SetNullRowFlag(value);
  m_inner->SetNullRowFlag(value);
}

void BatchNestedLoop::UnlockRow() {
  if (m_enabled)
    m_inner->UnlockRow();
  else
    m_native.UnlockRow();
}

void BatchNestedLoop::EndPSIBatchModeIfStarted() {
  m_outer->EndPSIBatchModeIfStarted();
  m_inner->EndPSIBatchModeIfStarted();
}

bool BatchNestedLoop::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) { return true; }

bool BatchNestedLoop::BatchNullRow(TABLE *table, size_t row) const {
  size_t bit = 0;
  for (const auto &entry : m_tables.tables()) {
    if (entry.table == table) return row < m_output_nulls.size() && (m_output_nulls[row] & (uint64_t{1} << bit));
    ++bit;
  }
  return false;
}

int BatchNestedLoop::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  const int result = ReadBatchImpl(chunks, capacity, rows);
  if (result != 0 && result != HA_ERR_END_OF_FILE) EndPSIBatchModeIfStarted();
  return result;
}

int BatchNestedLoop::ReadBatchImpl(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  m_output_nulls.clear();
  capacity = std::min(capacity, m_capacity);
  if (capacity == 0) return 0;
  // Consumers may change their projection or column order between calls.
  // Bind by Field identity once per batch, not by scanning both input schemas
  // for every emitted row. Store indices so input-buffer growth is harmless.
  m_output_columns.clear();
  if (!Reserve(m_output_columns, chunks.size()))
    return AllocationError("Rapid nested loop output mapping allocation failed");
  for (size_t target = 0; target < chunks.size(); ++target) {
    if (!chunks[target].valid()) continue;
    bool found = false;
    for (bool inner : {false, true}) {
      const auto &source = inner ? m_inner_chunks : m_outer_chunks;
      for (size_t col = 0; col < source.size(); ++col) {
        if (source[col].source_field() != chunks[target].source_field()) continue;
        m_output_columns.push_back({target, col, inner});
        found = true;
        break;
      }
      if (found) break;
    }
    if (!found) return 1;
  }
  while (rows < capacity) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }
    if (!m_probe) {
      if (m_outer_pos == m_outer_rows) {
        if (m_outer_eof) return HA_ERR_END_OF_FILE;
        const int error =
            Fetch(m_outer_batch, m_outer_chunks, m_outer_selection, m_outer_nulls, m_outer_rows, m_outer_eof);
        if (error) return error;
        m_outer_pos = 0;
        if (!m_outer_rows) return HA_ERR_END_OF_FILE;
      }
      if (BindOuter()) return 1;

      m_inner->SetNullRowFlag(false);
      if (m_pfs) m_inner->StartPSIBatchMode();
      if (m_inner->Init()) return 1;

      m_inner_active = m_inner_batch->SupportsBatchRead() ? m_inner_batch : nullptr;
      m_inner_pos = m_inner_rows = 0;
      m_inner_eof = m_found = false;
      m_probe = true;
    }

    if (m_inner_pos == m_inner_rows && !m_inner_eof) {
      // A consumer may overwrite every TABLE buffer after each output batch.
      if (BindOuter()) return 1;
      const int error = m_inner_active ? Fetch(m_inner_active, m_inner_chunks, m_inner_selection, m_inner_nulls,
                                               m_inner_rows, m_inner_eof)
                                       : FetchInnerRows();
      if (error) return error;
      m_inner_pos = 0;
    }

    if (m_inner_pos == m_inner_rows) {
      const bool complement = (m_type == JoinType::OUTER && !m_found) || m_type == JoinType::ANTI;
      if (complement && !Emit(chunks, true)) return 1;
      FinishProbe();
      if (complement) ++rows;
      continue;
    }

    m_found = true;
    if (m_type == JoinType::ANTI) {
      FinishProbe();
      continue;
    }
    if (!Emit(chunks, false)) return 1;

    ++rows;
    ++m_inner_pos;
    if (m_type == JoinType::SEMI) FinishProbe();
  }
  return 0;
}

bool BatchNestedLoop::Prepare(const pack_rows::TableCollection &tables, std::pmr::vector<ColumnChunk> &chunks) {
  chunks.clear();
  size_t count = 0;
  for (const auto &entry : tables.tables()) count += entry.table->s->fields;
  if (!Reserve(chunks, count)) return false;
  for (const auto &entry : tables.tables())
    for (uint i = 0; i < entry.table->s->fields; ++i) {
      chunks.emplace_back(bitmap_is_set(entry.table->read_set, i) ? entry.table->field[i] : nullptr, m_capacity,
                          &m_memory);
      if (chunks.back().failed()) return false;
    }
  return true;
}

int BatchNestedLoop::Fetch(BatchReadable *source, std::pmr::vector<ColumnChunk> &chunks,
                           std::pmr::vector<uint32_t> &selection, std::pmr::vector<uint64_t> &nulls, size_t &rows,
                           bool &eof) {
  for (auto &chunk : chunks) chunk.clear();

  const int result = source->ReadSelectedBatch(chunks, m_capacity, rows, selection);
  if (result != 0 && result != HA_ERR_END_OF_FILE) return result;

  eof = result == HA_ERR_END_OF_FILE || rows == 0;
  nulls.clear();
  for (size_t row = 0; row < rows; ++row) {
    uint64_t flags{0};
    size_t bit{0};
    for (const auto &entry : m_tables.tables()) {
      if (source->BatchNullRow(entry.table, row)) flags |= uint64_t{1} << bit;
      ++bit;
    }
    nulls.push_back(flags);
  }
  return 0;
}

bool BatchNestedLoop::MaterializeInnerBlock() {
  // Only the batch path can be drained here, and only tables that cannot produce
  // NULL rows: the parallel transport carries no per-row table NULL channel.
  auto *batch = (m_inner_batch != nullptr && m_inner_batch->SupportsBatchRead()) ? m_inner_batch : nullptr;
  if (batch == nullptr) return false;

  constexpr size_t capacity = Morsel::ParallelSource::kBatchRows;
  std::pmr::vector<ColumnChunk> read(&m_memory);
  if (!Prepare(m_inner_tables, read)) return false;
  for (auto &chunk : read)
    if (chunk.valid() && !chunk.grow(capacity)) return false;

  m_inner_block.clear();
  m_inner_block_rows = 0;
  m_inner_block_bytes = 0;
  if (!Reserve(m_inner_block, read.size())) return false;
  for (const auto &chunk : read) {
    m_inner_block.emplace_back(chunk.valid() ? chunk.source_field() : nullptr, chunk.valid() ? capacity : 0, &m_memory);
    if (m_inner_block.back().failed()) return false;
  }

  // The whole reservation may be spent here: the block is the only thing the
  // parallel phase needs to keep alive.
  const size_t budget = m_reservation.bytes();
  for (;;) {
    for (auto &chunk : read) chunk.clear();
    size_t rows{0};
    const int result = batch->ReadBatch(read, capacity, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return false;

    size_t row_bytes = 0;
    for (const auto &chunk : m_inner_block)
      if (chunk.valid()) row_bytes += chunk.width() + 1;
    if (rows != 0 && budget != 0 && m_inner_block_bytes + rows * row_bytes > budget) return false;

    if (rows != 0) {
      const size_t needed = m_inner_block_rows + rows;
      for (auto &chunk : m_inner_block)
        if (chunk.valid() && chunk.capacity() < needed && !chunk.grow(std::max(needed, chunk.capacity() * 2)))
          return false;
      for (size_t row = 0; row < rows; ++row)
        for (size_t col = 0; col < m_inner_block.size(); ++col)
          if (m_inner_block[col].valid() && !m_inner_block[col].append_from(read[col], row)) return false;
    }
    m_inner_block_rows += rows;
    m_inner_block_bytes += rows * row_bytes;
    if (result == HA_ERR_END_OF_FILE) break;
    if (rows == 0) return false;
  }
  return m_inner_block_rows > 0;
}

int BatchNestedLoop::FetchInnerRows() {
  for (auto &chunk : m_inner_chunks) chunk.clear();

  m_inner_selection.clear();
  m_inner_nulls.clear();
  m_inner_rows = 0;
  while (m_inner_rows < m_capacity) {
    const int result = m_inner->Read();
    if (result != 0) {
      if (result == -1) {
        m_inner_eof = true;
        return 0;
      }
      return result;
    }

    for (auto &chunk : m_inner_chunks) {
      if (!chunk.valid()) continue;

      Field *field = chunk.source_field();
      if (field->is_null()) {
        if (!chunk.add(nullptr, 0, true)) return 1;
        continue;
      }
      auto dictionary = RecordDictionary(field);
      if (dictionary) {
        String view;
        const String *value = field->val_str(&view);
        if (!value) return 1;
        int64 id = -1;
        if (field->type() == MYSQL_TYPE_STRING)
          id = dictionary->find_id(
              std::string_view(reinterpret_cast<const char *>(field->field_ptr()), field->pack_length()));
        if (id < 0) id = dictionary->find_id(std::string_view(value->ptr(), value->length()));
        if (id < 0 || static_cast<uint64_t>(id) > UINT32_MAX) return 1;
        const uint32 code = static_cast<uint32>(id);
        if (!chunk.add(reinterpret_cast<const uchar *>(&code), sizeof(code), false)) return 1;
      } else if (!chunk.add(field->field_ptr(), field->pack_length(), false))
        return 1;
    }
    uint64_t flags{0};
    size_t bit{0};
    for (const auto &entry : m_tables.tables()) {
      if (entry.table->has_null_row()) flags |= uint64_t{1} << bit;
      ++bit;
    }
    m_inner_nulls.push_back(flags);
    ++m_inner_rows;
  }
  return 0;
}

bool BatchNestedLoop::BindOuter() {
  const size_t row = m_outer_selection.empty() ? m_outer_pos : m_outer_selection[m_outer_pos];
  size_t bit{0};
  for (const auto &entry : m_tables.tables()) {
    bool outer{false};
    for (const auto &candidate : m_outer_tables.tables()) outer |= candidate.table == entry.table;
    if (outer) {
      if (m_outer_nulls[m_outer_pos] & (uint64_t{1} << bit))
        entry.table->set_null_row();
      else
        entry.table->reset_null_row();
    }
    ++bit;
  }

  for (const auto &chunk : m_outer_chunks) {
    if (!chunk.valid()) continue;

    Field *field = chunk.source_field();
    if (field->table->has_null_row()) continue;
    if (chunk.nullable_fast(row)) {
      field->set_null();
      continue;
    }
    field->set_notnull();

    auto dictionary = m_outer_batch->Dictionary(field);
    if (dictionary) {
      uint32 code;
      std::memcpy(&code, chunk.data_fast(row), sizeof(code));
      const auto length = dictionary->get(code, m_decode.data(), m_decode.size());
      Utils::ColumnMapGuard write_guard(field->table, Utils::ColumnMapGuard::TYPE::WRITE);
      if (!length || field->store(m_decode.data(), *length, field->charset()) != TYPE_OK) return true;
    } else
      std::memcpy(field->field_ptr(), chunk.data_fast(row), field->pack_length());
  }
  return false;
}

bool BatchNestedLoop::Emit(std::pmr::vector<ColumnChunk> &output, bool complement) {
  const size_t outer_row = m_outer_selection.empty() ? m_outer_pos : m_outer_selection[m_outer_pos];
  const size_t inner_row = complement ? 0 : m_inner_selection.empty() ? m_inner_pos : m_inner_selection[m_inner_pos];
  for (const auto &column : m_output_columns) {
    auto &target = output[column.target];
    const auto &source = column.inner ? m_inner_chunks[column.source] : m_outer_chunks[column.source];
    if (column.inner && complement) {
      if (!target.add(nullptr, 0, true)) return false;
    } else if (!target.append_from(source, column.inner ? inner_row : outer_row)) {
      return false;
    }
  }

  uint64_t flags = m_outer_nulls[m_outer_pos];
  if (complement) {
    size_t bit = 0;
    for (const auto &entry : m_tables.tables()) {
      for (const auto &inner : m_inner_tables.tables()) {
        if (entry.table == inner.table) flags |= uint64_t{1} << bit;
      }
      ++bit;
    }
  } else
    flags |= m_inner_nulls[m_inner_pos];

  m_output_nulls.push_back(flags);
  return true;
}

void BatchNestedLoop::FinishProbe() {
  if (m_pfs) m_inner->EndPSIBatchModeIfStarted();
  ++m_outer_pos;
  m_probe = false;
}

namespace {
// DuckDB's shape for a parallel nested loop: materialize the inner side once,
// then let each worker rescan that immutable block with its own position. The
// outer side supplies the parallelism and the ordinals; the inner side is pure
// data, so no cursor is ever shared and none has to be cloned.
class ParallelNestedLoop final : public Morsel::ParallelSource {
 public:
  struct OutputColumn {
    size_t target;  // Index in the consumer's chunk vector.
    size_t source;  // Index in the outer or inner source's field list.
    bool inner;
  };

  ParallelNestedLoop(ResMgmt::BoundedMemoryResource *memory, std::unique_ptr<Morsel::ParallelSource> outer,
                     Morsel::ReplayableParallelSource *inner, const std::pmr::vector<Field *> &output_fields,
                     const std::pmr::vector<Field *> &outer_fields, const std::pmr::vector<Field *> &inner_fields,
                     const std::pmr::vector<OutputColumn> &output_columns, size_t inner_rows)
      : m_outer(std::move(outer)),
        m_inner(inner),
        m_output_fields(output_fields, memory),
        m_outer_fields(outer_fields, memory),
        m_inner_fields(inner_fields, memory),
        m_output_columns(output_columns, memory),
        m_inner_rows(inner_rows) {}

  size_t tasks() const override { return m_outer->tasks(); }
  uint64_t max_ordinal() const override {
    const uint64_t outer_count = m_outer->max_ordinal() + 1;
    // Zero when the inner source produced no rows; the product is the ordinal count otherwise.
    const uint64_t ordinals = outer_count * m_inner_rows;
    return ordinals == 0 ? 0 : ordinals - 1;
  }
  size_t extra_scratch_bytes() const override { return m_outer->extra_scratch_bytes(); }

  bool ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                   const Consumer &consume) override {
    constexpr size_t capacity = kBatchRows;
    std::pmr::vector<ColumnChunk> inner_chunks(memory);
    std::pmr::vector<ColumnChunk> output(memory);
    std::pmr::vector<uint32_t> selection(memory);
    std::pmr::vector<uint64_t> ordinals(memory);
    if (!Reserve(inner_chunks, m_inner_fields.size()) || !Reserve(output, m_output_fields.size())) return true;
    for (Field *field : m_inner_fields) {
      inner_chunks.emplace_back(field, capacity, memory);
      if (inner_chunks.back().failed()) return true;
    }
    for (Field *field : m_output_fields) {
      output.emplace_back(field, capacity, memory);
      if (output.back().failed()) return true;
    }
    if (!Reserve(ordinals, capacity)) return true;

    bool failed = false;
    const auto flush = [&]() -> bool {
      if (ordinals.empty()) return false;
      if (consume(output, ordinals.size(), selection, ordinals)) return true;
      for (auto &chunk : output) chunk.clear();
      ordinals.clear();
      return false;
    };

    const bool error = m_outer->Execute(
        worker, memory, cancelled,
        [&](const std::pmr::vector<ColumnChunk> &outer_chunks, size_t rows, const std::pmr::vector<uint32_t> &chosen,
            const std::pmr::vector<uint64_t> &outer_ordinals) {
          if (failed) return true;
          const size_t outer_rows = chosen.empty() ? rows : chosen.size();
          for (size_t position = 0; position < outer_rows; ++position) {
            if (cancelled.load(std::memory_order_acquire)) return true;
            const size_t outer_row = chosen.empty() ? position : chosen[position];
            const uint64_t base = outer_ordinals[position] * m_inner_rows;
            // The inner block can be larger than one transport batch, so it is
            // replayed in slices; each slice refills only the inner chunks.
            for (size_t start = 0; start < m_inner_rows; start += capacity) {
              const size_t count = std::min(capacity, m_inner_rows - start);
              for (auto &chunk : inner_chunks) chunk.clear();
              if (!m_inner->Replay(start, count, inner_chunks)) {
                failed = true;
                return true;
              }
              for (size_t i = 0; i < count; ++i) {
                for (const auto &column : m_output_columns) {
                  auto &target = output[column.target];
                  if (!target.valid()) continue;
                  const ColumnChunk &source = column.inner ? inner_chunks[column.source] : outer_chunks[column.source];
                  const size_t row = column.inner ? i : outer_row;
                  const bool is_null = source.nullable_fast(row);
                  if (!target.add(is_null ? nullptr : source.data_fast(row), is_null ? 0 : source.width(), is_null)) {
                    failed = true;
                    return true;
                  }
                }
                ordinals.push_back(base + start + i);
                if (ordinals.size() == capacity && flush()) {
                  failed = true;
                  return true;
                }
              }
            }
          }
          return false;
        });
    if (error || failed) return true;
    if (flush()) return true;
    return false;
  }

 private:
  std::unique_ptr<Morsel::ParallelSource> m_outer;
  Morsel::ReplayableParallelSource *m_inner;
  std::pmr::vector<Field *> m_output_fields, m_outer_fields, m_inner_fields;
  std::pmr::vector<OutputColumn> m_output_columns;
  size_t m_inner_rows{0};
};
}  // namespace

std::unique_ptr<Morsel::ParallelSource> BatchNestedLoop::CreateParallelSource(ResMgmt::BoundedMemoryResource *memory,
                                                                              const std::pmr::vector<Field *> &fields) {
  const auto isolation = ResMgmt::work_governor().mode();
  // Only a fresh inner-join probe: OUTER/ANTI also emit complement rows, and a
  // started probe has already consumed outer rows the runs would redo.
  if (isolation == ResMgmt::IsolationMode::AUTO || isolation == ResMgmt::IsolationMode::STRICT_CPU || !m_enabled ||
      m_type != JoinType::INNER || m_probe || m_outer_pos != 0 || m_outer_rows != 0 || m_outer_eof)
    return {};
  // The parallel transport has no per-row table NULL channel, so any table that
  // could produce NULL rows would be silently wrong here.
  for (const auto &entry : m_outer_tables.tables())
    if (entry.table->is_nullable()) return {};
  for (const auto &entry : m_inner_tables.tables())
    if (entry.table->is_nullable()) return {};

  const auto belongs_to = [](const pack_rows::TableCollection &tables, const Field *field) {
    for (const auto &entry : tables.tables())
      if (entry.table == field->table) return true;
    return false;
  };

  std::pmr::vector<Field *> outer_fields(memory), inner_fields(memory);
  std::pmr::vector<ParallelNestedLoop::OutputColumn> output_columns(memory);
  if (!Reserve(output_columns, fields.size())) return {};
  for (Field *field : fields) {
    if (belongs_to(m_outer_tables, field)) {
      output_columns.push_back({output_columns.size(), outer_fields.size(), false});
      outer_fields.push_back(field);
    } else if (belongs_to(m_inner_tables, field)) {
      output_columns.push_back({output_columns.size(), inner_fields.size(), true});
      inner_fields.push_back(field);
    } else
      return {};
  }
  if (outer_fields.empty() || inner_fields.empty()) return {};

  auto outer = m_outer_batch->CreateParallelSource(memory, outer_fields);
  if (!outer || outer->tasks() < 2) return {};

  // Materializing the inner side once is exactly what makes the rescan safe:
  // the block is immutable and outer-independent by construction (the batch
  // materializer refuses rematerialize/correlated shapes).
  if (m_inner->Init()) return {};
  std::unique_ptr<Morsel::ParallelSource> inner = m_inner_batch->CreateParallelSource(memory, inner_fields);
  // No split at all means the inner is a per-outer-row lookup: it must be
  // re-driven for every outer row, so nothing here may be materialized.
  if (inner == nullptr) return {};
  auto *replay = dynamic_cast<Morsel::ReplayableParallelSource *>(inner.get());
  if (replay == nullptr || replay->replay_row_count() < 1) {
    // Stream-shaped inner. Only a correlation-free inner is BatchReadable here:
    // REF and dependent-subquery inners are not, so those stay native. Drain it
    // once, as a hash join drains its build side, and let workers replay a block.
    inner.reset();
    if (!MaterializeInnerBlock()) return {};
    inner = std::make_unique<Morsel::MaterializedSource>(memory, m_inner_block, m_inner_block_rows, inner_fields);
    replay = dynamic_cast<Morsel::ReplayableParallelSource *>(inner.get());
    if (replay == nullptr || replay->replay_row_count() < 1) return {};
  }
  if (replay->replay_row_count() > (UINT64_MAX / std::max<uint64_t>(1, outer->max_ordinal() + 1))) return {};

  return std::make_unique<ParallelNestedLoop>(memory, std::move(outer), replay, fields, outer_fields, inner_fields,
                                              output_columns, replay->replay_row_count());
}
}  // namespace detail

VectorizedNestedLoopIterator::VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                                                           unique_ptr_destroy_only<RowIterator> inner, JoinType type,
                                                           bool pfs_batch_mode,
                                                           const Prealloced_array<TABLE *, 4> &tables, size_t budget,
                                                           double estimate)
    : VectorizedRecordIterator(
          thd,
          NewIterator<NestedLoopIterator>(thd, thd->mem_root, std::move(outer), std::move(inner), type, pfs_batch_mode),
          tables, budget, estimate, "nested_loop_join", ResMgmt::QueryMemoryOperator::kNestedLoop) {}
VectorizedNestedLoopIterator::VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                                                           unique_ptr_destroy_only<RowIterator> inner, JoinType type,
                                                           bool pfs, const Prealloced_array<TABLE *, 4> &tables,
                                                           const Prealloced_array<TABLE *, 4> &outer_tables,
                                                           const Prealloced_array<TABLE *, 4> &inner_tables,
                                                           size_t budget, double estimate)
    : VectorizedRecordIterator(
          thd,
          NewIterator<detail::BatchNestedLoop>(thd, thd->mem_root, std::move(outer), std::move(inner), type, pfs,
                                               tables, outer_tables, inner_tables, budget / 2),
          tables, budget / 2, estimate, "nested_loop_join", ResMgmt::QueryMemoryOperator::kNestedLoop) {}
}  // namespace Executor
}  // namespace ShannonBase
