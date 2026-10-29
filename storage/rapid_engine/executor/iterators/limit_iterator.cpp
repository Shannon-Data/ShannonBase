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
namespace ShannonBase {
namespace Executor {
VectorizedLimitIterator::VectorizedLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit,
                                                 ha_rows offset, ha_rows *skipped,
                                                 const Prealloced_array<TABLE *, 4> &tables, size_t budget)
    : RowIterator(thd),
      m_reservation(ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kBatchTransport, budget)),
      m_memory(m_reservation.bytes()),
      m_source(std::move(source)),
      m_limit(limit),
      m_offset(offset),
      m_skipped(skipped),
      m_fields(&m_memory),
      m_discard(&m_memory) {
  if (offset != 0) {
    for (TABLE *table : tables) {
      for (uint i = 0; i < table->s->fields; ++i) {
        m_fields.push_back(bitmap_is_set(table->read_set, i) ? table->field[i] : nullptr);
      }
    }
  }
}

bool VectorizedLimitIterator::Init() {
  m_seen = 0;
  m_eof = false;
  m_batch = nullptr;
  m_discard.clear();

  if (!m_reservation) return Error();
  if (m_source->Init()) return true;

  m_batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (m_batch != nullptr && !m_batch->SupportsBatchRead()) m_batch = nullptr;
  if (m_batch != nullptr) {
    if (!TryReserve(m_discard, m_fields.size())) return Error();
    for (Field *field : m_fields) {
      m_discard.emplace_back(field, field ? std::min<ha_rows>(1024, m_offset) : 0, &m_memory);
      if (m_discard.back().failed()) return Error();
    }
  }
  return false;
}

int VectorizedLimitIterator::Read() {
  while (!m_eof && m_seen < m_offset) {
    const int result = m_source->Read();
    if (result != 0) {
      m_eof = result == -1;
      return result;
    }
    ++m_seen;
    if (m_skipped) ++*m_skipped;
    m_source->UnlockRow();
  }

  if (m_eof || m_seen >= m_limit) return -1;

  const int result = m_source->Read();

  if (result == 0) ++m_seen;
  if (result == -1) m_eof = true;
  return result;
}

int VectorizedLimitIterator::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  if (m_batch == nullptr) return Error();
  if (capacity == 0) return 0;

  while (!m_eof && m_seen < m_offset) {
    for (auto &chunk : m_discard) chunk.clear();

    size_t skipped{0};
    const int result = m_batch->ReadBatch(m_discard, std::min<ha_rows>(1024, m_offset - m_seen), skipped);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    m_seen += skipped;

    if (m_skipped) *m_skipped += skipped;
    if (result == HA_ERR_END_OF_FILE) m_eof = true;
    if (skipped == 0 && !m_eof) return Error();
  }

  if (m_eof || m_seen >= m_limit) return HA_ERR_END_OF_FILE;
  const int result = m_batch->ReadBatch(chunks, std::min<ha_rows>({1024, capacity, m_limit - m_seen}), rows);
  m_seen += rows;

  if (result == HA_ERR_END_OF_FILE) m_eof = true;
  if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
  return m_eof || m_seen >= m_limit ? HA_ERR_END_OF_FILE : 0;
}

bool VectorizedLimitIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                size_t total) {
  if (from >= total) return false;
  if (m_batch == nullptr || m_seen < m_offset || total - from > m_seen - m_offset) return Error();
  if (m_batch->PushbackBatchTail(chunks, from, total)) return true;

  m_seen -= total - from;
  m_eof = false;
  return false;
}
}  // namespace Executor
}  // namespace ShannonBase
