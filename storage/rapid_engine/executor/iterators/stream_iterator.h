/*
   Copyright (c) 2014, 2023, Oracle and/or its affiliates.

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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_STREAM_ITERATOR_H__
#define __SHANNONBASE_STREAM_ITERATOR_H__
#include "sql/iterators/composite_iterators.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"

namespace ShannonBase {
namespace Executor {
class VectorizedStreamIterator final : public RowIterator, public BatchReadable {
 public:
  static bool CanVectorize(const Temp_table_param *param, TABLE *table, bool provide_rowid);
  VectorizedStreamIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, Temp_table_param *param, TABLE *table,
                           bool provide_rowid, JOIN *join, int ref_slice,
                           const Prealloced_array<TABLE *, 4> &input_tables, size_t budget);
  bool Init() override;
  int Read() override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool SupportsBatchRead() const override { return m_batch != nullptr; }
  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
  void SetNullRowFlag(bool value) override { m_native.SetNullRowFlag(value); }
  void UnlockRow() override { m_source->UnlockRow(); }
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }

 private:
  struct Binding {
    Field *source;
    Field *target;
    size_t index;
    std::shared_ptr<Compress::Dictionary> dictionary;
  };
  int Error();
  bool PushbackBatchTailImpl(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total);
  bool LoadTail();
  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;
  unique_ptr_destroy_only<RowIterator> m_source;
  StreamingIterator m_native;
  Temp_table_param *m_param;
  TABLE *m_table;
  JOIN *m_join;
  int m_output_slice, m_input_slice{-1};
  bool m_provide_rowid;
  BatchReadable *m_batch{nullptr};
  std::pmr::vector<Field *> m_input_fields;
  std::pmr::vector<ColumnChunk> m_input, m_tail;
  std::pmr::vector<Binding> m_bindings;
  std::pmr::vector<char> m_decode;
  size_t m_tail_start{0}, m_tail_count{0};
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
