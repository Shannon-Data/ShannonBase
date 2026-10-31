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
#ifndef __SHANNONBASE_LIMIT_ITERATOR_H__
#define __SHANNONBASE_LIMIT_ITERATOR_H__
#include "sql/iterators/composite_iterators.h"
#include "storage/rapid_engine/executor/iterators/record_iterator.h"

namespace ShannonBase {
namespace Executor {
namespace detail {
// Owns LIMIT/OFFSET state once. Row-only input uses MySQL's native implementation.
class BatchLimitIterator final : public RowIterator, public BatchReadable {
 public:
  BatchLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit, ha_rows offset,
                     ha_rows *skipped, const Prealloced_array<TABLE *, 4> &tables, size_t budget);
  bool Init() override;
  int Read() override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
  bool SupportsBatchRead() const override;
  bool PreservesBatchNullRows() const override;
  bool BatchNullRow(TABLE *table, size_t row) const override;
  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override;
  void SetNullRowFlag(bool value) override;
  void UnlockRow() override;
  void EndPSIBatchModeIfStarted() override;

 private:
  static constexpr size_t kMaxBatchRows = 1024;
  int Error();

  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;

  unique_ptr_destroy_only<RowIterator> m_source;
  bool m_skip_source_init{false};
  LimitOffsetIterator m_native;
  pack_rows::TableCollection m_tables;
  BatchReadable *m_batch{nullptr};
  ha_rows m_limit{HA_POS_ERROR}, m_offset{HA_POS_ERROR}, m_seen{HA_POS_ERROR}, *m_skipped{nullptr};
  std::pmr::vector<ColumnChunk> m_discard;
  bool m_eof{false};
};
}  // namespace detail

// Shared record adapter provides Read(), ReadBatch(), dictionaries and lookahead.
// The LIMIT algorithm itself lives only in the batch core or MySQL's fallback.
class VectorizedLimitIterator final : public VectorizedRecordIterator {
 public:
  static bool CanVectorize(const Prealloced_array<TABLE *, 4> &tables);
  VectorizedLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit, ha_rows offset,
                          ha_rows *skipped, const Prealloced_array<TABLE *, 4> &tables, size_t budget);
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
