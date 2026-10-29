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
#ifndef __SHANNONBASE_RECORD_ITERATOR_H__
#define __SHANNONBASE_RECORD_ITERATOR_H__
#include "sql/pack_rows.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"
class AccessPath;

namespace ShannonBase {
namespace Executor {
std::shared_ptr<Compress::Dictionary> RecordDictionary(const Field *field);
bool CanBatchRecords(TABLE *table);
bool SelectVectorizedLookup(THD *thd, AccessPath *path);

// The record boundary required by SQL lookup iterators. The retained resume
// image is independent of TABLE buffers overwritten by a batch consumer.
class VectorizedRecordIterator : public RowIterator, public BatchReadable {
 public:
  VectorizedRecordIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                           const Prealloced_array<TABLE *, 4> &tables, size_t budget, double estimate,
                           const char *operator_name,
                           ResMgmt::QueryMemoryOperator kind = ResMgmt::QueryMemoryOperator::kBatchTransport);
  bool Init() override;
  int Read() override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override;
  void SetNullRowFlag(bool value) override { m_source->SetNullRowFlag(value); }
  void UnlockRow() override {
    if (!m_replayed_row) m_source->UnlockRow();
  }
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }
  const VectorizedOperatorStats &GetOperatorStats() const { return m_stats; }

 private:
  struct Encoding {
    Field *field{nullptr};
    std::shared_ptr<Compress::Dictionary> dictionary;
  };

  /// Rows per batch read; the null-flag buffer is reserved for this size.
  static constexpr size_t kMaxBatchRows = 1024;

  const Encoding *FindEncoding(const Field *field) const;
  bool PushbackBatchTailImpl(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total);
  int ReadLive();
  bool RestoreResume();
  bool SaveResume();
  uint64_t NullTables() const;
  bool RestoreRow(const std::pmr::vector<ColumnChunk> &chunks, size_t row, uint64_t null_tables);
  bool AppendRow(std::pmr::vector<ColumnChunk> &chunks);
  bool AppendTail(std::pmr::vector<ColumnChunk> &chunks, size_t row);

  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;
  unique_ptr_destroy_only<RowIterator> m_source;
  const char *m_operator_name{nullptr};
  pack_rows::TableCollection m_tables;
  std::pmr::vector<Encoding> m_encodings;
  std::pmr::vector<ColumnChunk> m_tail;
  std::pmr::vector<uint64_t> m_tail_nulls;
  std::pmr::vector<uint64_t> m_batch_nulls;
  std::pmr::vector<char> m_resume_storage;
  std::pmr::vector<char> m_decode_storage;
  String m_resume;
  size_t m_tail_start{0}, m_tail_count{0};
  bool m_have_resume{false}, m_replayed_row{false};
  VectorizedOperatorStats m_stats;
  CardinalityObservation m_cardinality;
};

class VectorizedRefIterator final : public VectorizedRecordIterator {
 public:
  using VectorizedRecordIterator::VectorizedRecordIterator;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
