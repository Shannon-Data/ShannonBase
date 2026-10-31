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
#include "storage/rapid_engine/executor/iterators/limit_iterator.h"
#include "sql/iterators/timing_iterator.h"

namespace ShannonBase {
namespace Executor {
namespace detail {
BatchLimitIterator::BatchLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit,
                                       ha_rows offset, ha_rows *skipped, const Prealloced_array<TABLE *, 4> &tables,
                                       size_t budget)
    : RowIterator(thd),
      m_reservation(ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kBatchTransport, budget)),
      m_memory(m_reservation.bytes()),
      m_source(std::move(source)),
      m_native(thd, NewIterator<BorrowedRows>(thd, thd->mem_root, m_source.get(), &m_skip_source_init), limit, offset,
               false, false, skipped),
      m_tables(tables, false, 0, ~table_map{0}),
      m_limit(limit),
      m_offset(offset),
      m_skipped(skipped),
      m_discard(&m_memory) {}

int BatchLimitIterator::Error() {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid LIMIT could not preserve its batch input");
  return 1;
}

bool BatchLimitIterator::Init() {
  m_skip_source_init = false;
  m_seen = 0;
  m_eof = false;
  m_batch = nullptr;
  m_discard.clear();

  if (!m_reservation) return Error();
  if (m_source->Init()) return true;

  m_skip_source_init = true;
  m_batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());

  if (m_batch && !m_batch->SupportsBatchRead()) m_batch = nullptr;
  if (m_batch) {
    for (const auto &entry : m_tables.tables()) {
      if (entry.table->is_nullable() && !m_batch->PreservesBatchNullRows()) {
        m_batch = nullptr;
        break;
      }
      for (uint i = 0; i < entry.table->s->fields; ++i) {
        if (!bitmap_is_set(entry.table->read_set, i)) continue;
        Field *field = entry.table->field[i];
        if (Utils::Util::is_string(field->type()) && field->real_type() != MYSQL_TYPE_ENUM &&
            field->real_type() != MYSQL_TYPE_SET && !m_batch->Dictionary(field) && !RecordDictionary(field)) {
          m_batch = nullptr;
          break;
        }
      }
      if (!m_batch) break;
    }
  }

  if (!m_batch) return m_native.Init();

  if (m_offset) {
    size_t columns = 0;
    for (const auto &entry : m_tables.tables()) columns += entry.table->s->fields;

    if (!TryReserve(m_discard, columns)) return Error();

    for (const auto &entry : m_tables.tables()) {
      TABLE *table = entry.table;
      for (uint i = 0; i < table->s->fields; ++i) {
        Field *field = bitmap_is_set(table->read_set, i) ? table->field[i] : nullptr;
        m_discard.emplace_back(field, field ? std::min<ha_rows>(kMaxBatchRows, m_offset) : 0, &m_memory);

        if (m_discard.back().failed()) return Error();
      }
    }
  }
  return false;
}

int BatchLimitIterator::Read() { return m_native.Read(); }

bool BatchLimitIterator::SupportsBatchRead() const { return m_batch != nullptr; }

bool BatchLimitIterator::PreservesBatchNullRows() const { return m_batch && m_batch->PreservesBatchNullRows(); }

bool BatchLimitIterator::BatchNullRow(TABLE *table, size_t row) const {
  return m_batch && m_batch->BatchNullRow(table, row);
}

std::shared_ptr<Compress::Dictionary> BatchLimitIterator::Dictionary(const Field *field) const {
  return m_batch ? m_batch->Dictionary(field) : nullptr;
}

void BatchLimitIterator::SetNullRowFlag(bool value) { m_source->SetNullRowFlag(value); }

void BatchLimitIterator::UnlockRow() { m_source->UnlockRow(); }

void BatchLimitIterator::EndPSIBatchModeIfStarted() { m_source->EndPSIBatchModeIfStarted(); }

int BatchLimitIterator::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  if (m_batch == nullptr) return Error();
  if (capacity == 0) return 0;

  while (!m_eof && m_seen < m_offset) {
    for (auto &chunk : m_discard) chunk.clear();

    size_t skipped{0};
    const int result = m_batch->ReadBatch(m_discard, std::min<ha_rows>(kMaxBatchRows, m_offset - m_seen), skipped);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    m_seen += skipped;

    if (m_skipped) *m_skipped += skipped;
    if (result == HA_ERR_END_OF_FILE) m_eof = true;
    if (skipped == 0 && !m_eof) return Error();
  }

  if (m_eof || m_seen >= m_limit) return HA_ERR_END_OF_FILE;
  const int result = m_batch->ReadBatch(chunks, std::min<ha_rows>({kMaxBatchRows, capacity, m_limit - m_seen}), rows);
  m_seen += rows;

  if (result == HA_ERR_END_OF_FILE) m_eof = true;
  if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
  return m_eof || m_seen >= m_limit ? HA_ERR_END_OF_FILE : 0;
}

bool BatchLimitIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) { return true; }
}  // namespace detail

bool VectorizedLimitIterator::CanVectorize(const Prealloced_array<TABLE *, 4> &tables) {
  for (TABLE *table : tables) {
    if (!table || !table->s || !table->read_set) return false;
    for (uint i = 0; i < table->s->fields; ++i) {
      if (!bitmap_is_set(table->read_set, i)) continue;
      Field *field = table->field[i];
      if (Utils::IsOffPageField(field)) return false;
      if (!Utils::Util::is_string(field->type()) && Utils::Util::normalized_length(field) != field->pack_length())
        return false;
    }
  }
  return true;
}

VectorizedLimitIterator::VectorizedLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit,
                                                 ha_rows offset, ha_rows *skipped,
                                                 const Prealloced_array<TABLE *, 4> &tables, size_t budget)
    : VectorizedRecordIterator(thd,
                               NewIterator<detail::BatchLimitIterator>(thd, thd->mem_root, std::move(source), limit,
                                                                       offset, skipped, tables, budget / 2),
                               tables, budget - budget / 2, -1.0, "limit_offset") {}
}  // namespace Executor
}  // namespace ShannonBase
