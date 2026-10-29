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
#include "storage/rapid_engine/executor/iterators/iterator.h"
namespace ShannonBase {
namespace Executor {
// The scalar-cardinality and SQL_CALC_FOUND_ROWS variants retain MySQL's
// iterator. A simple LIMIT/OFFSET never needs to materialize its output rows.
class VectorizedLimitIterator final : public RowIterator, public BatchReadable {
 public:
  VectorizedLimitIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, ha_rows limit, ha_rows offset,
                          ha_rows *skipped, const Prealloced_array<TABLE *, 4> &tables, size_t budget);
  bool Init() override;
  int Read() override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
  bool SupportsBatchRead() const override { return m_batch != nullptr; }
  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override {
    return m_batch ? m_batch->Dictionary(field) : nullptr;
  }
  void SetNullRowFlag(bool value) override { m_source->SetNullRowFlag(value); }
  void UnlockRow() override { m_source->UnlockRow(); }
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }

 private:
  inline int Error() {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid LIMIT could not preserve its batch input");
    return 1;
  }
  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;
  unique_ptr_destroy_only<RowIterator> m_source;
  BatchReadable *m_batch{nullptr};
  ha_rows m_limit, m_offset, m_seen{0}, *m_skipped;
  std::pmr::vector<Field *> m_fields;
  std::pmr::vector<ColumnChunk> m_discard;
  bool m_eof{false};
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
