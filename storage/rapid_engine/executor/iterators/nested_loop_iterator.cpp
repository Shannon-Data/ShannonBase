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
        if (!table || !table->s || !table->read_set || ++tables > 64) {
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
      !TryReserve(m_outer_nulls, m_capacity) || !TryReserve(m_inner_nulls, m_capacity) ||
      !TryReserve(m_output_nulls, m_capacity) || !TryReserve(m_decode, decode))
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
  if (!TryReserve(m_output_columns, chunks.size()))
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
  if (!TryReserve(chunks, count)) return false;
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
