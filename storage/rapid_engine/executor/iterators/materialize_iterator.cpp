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
#include "storage/rapid_engine/executor/iterators/materialize_iterator.h"
#include <array>
#include "scope_guard.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/materialize_path_parameters.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/resource_management/io_scheduler.h"

namespace ShannonBase {
namespace Executor {
bool VectorizedMaterializeIterator::CanVectorize(const AccessPath *path) {
  if (path == nullptr || path->type != AccessPath::MATERIALIZE) return false;

  DBUG_EXECUTE_IF("rapid_disable_vectorized_materialize", { return false; });

  const auto &materialize = path->materialize();
  const auto *param = materialize.param;
  if (param == nullptr || param->table == nullptr || param->cte != nullptr || param->rematerialize ||
      param->invalidators != nullptr || materialize.table_path == nullptr ||
      materialize.table_path->type != AccessPath::TABLE_SCAN)
    return false;

  for (const auto &operand : param->m_operands) {
    if (operand.is_recursive_reference) return false;
  }

  TABLE *table = param->table;
  if (table->s->fields > SHANNON_MAX_COLUMNS || table->read_set == nullptr || bitmap_is_clear_all(table->read_set))
    return false;

  for (uint i = 0; i < table->s->fields; ++i) {
    const Field *field = table->field[i];
    // Temp-table strings have no immutable IMCS dictionary. Preserve the SQL
    // path until their encoding has an explicitly budgeted lifetime.
    if (Utils::IsOffPageField(field) || Utils::Util::is_string(field->type()) ||
        Utils::Util::normalized_length(field) != field->pack_length())
      return false;
  }
  return true;
}

VectorizedMaterializeIterator::VectorizedMaterializeIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                             TABLE *table, size_t budget, ha_rows limit)
    : RowIterator(thd),
      m_reservation(ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kBatchTransport, budget)),
      m_memory(m_reservation.bytes()),
      m_source(std::move(source)),
      m_table(table),
      m_columns(&m_memory),
      m_tail(&m_memory),
      m_input(&m_memory),
      m_encoded(&m_memory),
      m_limit(limit) {}

bool VectorizedMaterializeIterator::WriteBlock() {
  if (m_rows == 0) return false;

  if (!m_file) {
    struct Owner {
      ResMgmt::BoundedMemoryResource::Reservation charge;
      std::array<char, 1024> buffer{};
      FILE *file{nullptr};
      explicit Owner(ResMgmt::BoundedMemoryResource *memory)
          : charge(memory, sizeof(FILE)), file(Utils::Util::create_spill_file("rapid_materialize")) {
        if (file != nullptr && setvbuf(file, buffer.data(), _IOFBF, buffer.size())) {
          Utils::Util::close_spill_file(file);
          file = nullptr;
        }
      }
      ~Owner() { Utils::Util::close_spill_file(file); }
    };

    std::shared_ptr<Owner> owner;
    if (!TryAllocation(
            [&] { owner = std::allocate_shared<Owner>(std::pmr::polymorphic_allocator<Owner>(&m_memory), &m_memory); }))
      return Error("Rapid materialization spool descriptor allocation failed");
    if (owner->file == nullptr) return Error("Rapid materialization could not create its column spool");
    m_file = std::shared_ptr<FILE>(owner, owner->file);
  }

  uint64_t bytes = sizeof(uint32_t);
  for (const auto &column : m_columns) {
    if (column.valid()) bytes += m_rows * (column.width() + 1);
  }

  const uint64_t limit = shannon_rpd_engine_cfg.materialize_spill_size_max;
  if (bytes > limit || m_spill_bytes > limit - bytes)
    return Error("Rapid materialization exceeded rapid_materialize_spill_size_max");

  ResMgmt::SpillIoScope io(thd(), m_file.get());
  if (!io) return true;

  const uint32_t count = static_cast<uint32_t>(m_rows);
  if (fwrite(&count, sizeof(count), 1, m_file.get()) != 1) return Error("Rapid materialization spool write failed");

  for (auto &column : m_columns) {
    if (!column.valid()) continue;
    const size_t width = column.width();
    m_encoded.assign(m_rows * (width + 1), 0);
    for (size_t row = 0; row < m_rows; ++row) {
      uchar *to = m_encoded.data() + row * (width + 1);
      to[0] = column.nullable_fast(row);
      if (to[0] == 0) memcpy(to + 1, column.data_fast(row), width);
    }
    if (fwrite(m_encoded.data(), 1, m_encoded.size(), m_file.get()) != m_encoded.size())
      return Error("Rapid materialization column write failed");
    column.clear();
  }
  m_spill_bytes += bytes;
  m_rows = 0;
  return false;
}

bool VectorizedMaterializeIterator::Init() { return InitImpl(); }

bool VectorizedMaterializeIterator::InitImpl() {
  if (m_ready && m_query_id == thd()->query_id) {
    m_tail.clear();
    m_tail_start = m_tail_count = m_position = m_emitted = 0;
    if (m_file) {
      m_rows = 0;
      if (fseek(m_file.get(), 0, SEEK_SET)) return Error("Rapid materialization cache rewind failed");
    }
    return false;
  }
  m_ready = false;
  m_file.reset();
  m_columns.clear();
  m_tail.clear();
  m_input.clear();
  m_encoded.clear();
  m_rows = m_position = m_total = m_emitted = m_tail_start = m_tail_count = 0;
  m_spill_bytes = 0;
  if (!m_reservation) return Error("Rapid materialization memory reservation exhausted");
  if (m_source->Init()) return true;

  size_t width{0};
  size_t max_width{0};
  for (uint i = 0; i < m_table->s->fields; ++i) {
    if (!bitmap_is_set(m_table->read_set, i)) continue;
    const size_t field_width = m_table->field[i]->pack_length() + 1;
    width += field_width;
    max_width = std::max(max_width, field_width);
  }

  if (width == 0 || width > m_reservation.bytes() / 8)
    return Error("Rapid materialization row exceeds its memory budget");

  // Leave room for growth overlap, input chunks, encoded IO and lookahead.
  m_max_rows = std::min<size_t>(UINT32_MAX, m_reservation.bytes() / (8 * width));
  DBUG_EXECUTE_IF("rapid_materialize_tiny_cache", { m_max_rows = std::min<size_t>(m_max_rows, 8); });
  size_t capacity = std::min<size_t>(1024, m_max_rows);
  // Spool IO reuses this buffer, so replay never has to allocate.
  const auto ensure_encoded = [&](size_t rows) {
    const size_t bytes = rows * max_width;
    if (!TryReserve(m_encoded, bytes)) return false;
    return true;
  };
  if (!ensure_encoded(capacity)) return Error("Rapid materialization spool buffer allocation failed");
  if (!TryReserve(m_columns, m_table->s->fields))
    return Error("Rapid materialization column descriptors allocation failed");
  for (uint i = 0; i < m_table->s->fields; ++i) {
    m_columns.emplace_back(bitmap_is_set(m_table->read_set, i) ? m_table->field[i] : nullptr,
                           bitmap_is_set(m_table->read_set, i) ? capacity : 0, &m_memory);
    if (m_columns.back().failed()) return Error("Rapid materialization could not retain a column");
  }

  auto *batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (batch != nullptr && !batch->SupportsBatchRead()) batch = nullptr;
  if (batch != nullptr) {
    if (!TryReserve(m_input, m_table->s->fields))
      return Error("Rapid materialization input descriptors allocation failed");
    for (uint i = 0; i < m_table->s->fields; ++i) {
      m_input.emplace_back(bitmap_is_set(m_table->read_set, i) ? m_table->field[i] : nullptr,
                           bitmap_is_set(m_table->read_set, i) ? std::min<size_t>(1024, capacity) : 0, &m_memory);
      if (m_input.back().failed()) return Error("Rapid materialization could not allocate an input chunk");
    }
  }
  for (;;) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return true;
    }
    if (m_total >= m_limit) break;
    if (m_rows == capacity) {
      if (capacity < m_max_rows) {
        const size_t next = std::min(m_max_rows, capacity * 2);
        for (auto &column : m_columns)
          if (column.valid() && !column.grow(next)) return Error("Rapid materialization column growth failed");
        capacity = next;
        if (!ensure_encoded(capacity)) return Error("Rapid materialization spool buffer growth failed");
      } else if (WriteBlock())
        return true;
    }

    if (batch != nullptr) {
      for (auto &column : m_input) column.clear();
      size_t rows = 0;
      const size_t requested = std::min<size_t>({1024, capacity - m_rows, m_limit - m_total});
      const int result = batch->ReadBatch(m_input, requested, rows);
      if (result != 0 && result != HA_ERR_END_OF_FILE) return true;
      for (size_t i = 0; i < m_columns.size(); ++i) {
        if (!m_columns[i].valid()) continue;
        for (size_t row = 0; row < rows; ++row)
          if (!m_columns[i].append_from(m_input[i], row)) return Error("Rapid materialization batch append failed");
      }
      m_rows += rows;
      m_total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
      if (rows == 0) return Error("Rapid materialization input batch made no progress");
      continue;
    }

    const int result = m_source->Read();
    if (result == -1) break;
    if (result != 0) return true;

    for (auto &column : m_columns) {
      if (!column.valid()) continue;
      Field *field = column.source_field();
      const bool is_null = field->is_null();
      if (!column.add(is_null ? nullptr : field->field_ptr(), is_null ? 0 : field->pack_length(), is_null))
        return Error("Rapid materialization could not retain a column");
    }
    ++m_rows;
    ++m_total;
  }
  if (m_file) {
    if (WriteBlock() || fflush(m_file.get()) || fseek(m_file.get(), 0, SEEK_SET))
      return Error("Rapid materialization could not rewind its column spool");
  }
  m_query_id = thd()->query_id;
  m_ready = true;
  return false;
}

int VectorizedMaterializeIterator::EnsureBlock() {
  if (m_position < m_rows) return 0;
  if (m_emitted >= m_total) return -1;
  if (!m_file) return Error("Rapid materialization lost its retained columns");

  ResMgmt::SpillIoScope io(thd(), m_file.get());
  if (!io) return 1;
  uint32_t rows{0};
  if (fread(&rows, sizeof(rows), 1, m_file.get()) != 1 || rows == 0 || rows > m_max_rows)
    return Error("Rapid materialization spool header is invalid");

  for (auto &column : m_columns) {
    if (!column.valid()) continue;
    column.clear();
    const size_t width = column.width();
    m_encoded.resize(rows * (width + 1));
    if (fread(m_encoded.data(), 1, m_encoded.size(), m_file.get()) != m_encoded.size())
      return Error("Rapid materialization spool column is truncated");
    for (size_t row = 0; row < rows; ++row) {
      const uchar *value = m_encoded.data() + row * (width + 1);
      if (!column.add(value + 1, width, value[0] != 0)) return Error("Rapid materialization column replay failed");
    }
  }
  m_rows = rows;
  m_position = 0;
  return 0;
}

bool VectorizedMaterializeIterator::Restore(const std::pmr::vector<ColumnChunk> &chunks, size_t row) {
  m_table->reset_null_row();
  for (const auto &column : chunks) {
    if (!column.valid()) continue;

    Field *field = column.source_field();
    if (column.nullable_fast(row))
      field->set_null();
    else {
      field->set_notnull();
      memcpy(field->field_ptr(), column.data_fast(row), field->pack_length());
    }
  }
  return false;
}

int VectorizedMaterializeIterator::Read() {
  if (m_tail_count != 0) {
    Restore(m_tail, m_tail_start++);
    --m_tail_count;
    return 0;
  }
  const int result = EnsureBlock();
  if (result != 0) return result;
  Restore(m_columns, m_position++);
  ++m_emitted;
  return 0;
}

int VectorizedMaterializeIterator::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  auto report_batches = create_scope_guard([&] {
    if (rows != 0)
      RapidMonitor::rapid_counters.query_vectorized_materialize_batch_rows_total.fetch_add(rows,
                                                                                           std::memory_order_relaxed);
  });
  capacity = std::min<size_t>(capacity, 1024);
  while (rows < capacity) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }
    const auto *source = &m_columns;
    size_t position = m_position;
    if (m_tail_count != 0) {
      source = &m_tail;
      position = m_tail_start;
    } else {
      const int result = EnsureBlock();
      if (result != 0) return result == -1 ? HA_ERR_END_OF_FILE : result;
      position = m_position;
    }
    for (auto &target : chunks) {
      if (!target.valid()) continue;
      const ColumnChunk *column = nullptr;
      for (const auto &candidate : *source)
        if (candidate.valid() && candidate.source_field() == target.source_field()) column = &candidate;
      if (column == nullptr || !target.append_from(*column, position))
        return Error("Rapid materialization batch replay failed");
    }
    if (m_tail_count != 0) {
      ++m_tail_start;
      --m_tail_count;
    } else {
      ++m_position;
      ++m_emitted;
    }
    ++rows;
  }
  return 0;
}

bool VectorizedMaterializeIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                      size_t total) {
  return PushbackBatchTailShared(chunks, from, total, &m_tail, &m_tail_start, &m_tail_count, nullptr, &m_memory);
}
}  // namespace Executor
}  // namespace ShannonBase
