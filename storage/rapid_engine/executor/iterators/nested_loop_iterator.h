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
#ifndef __SHANNONBASE_NESTED_LOOP_ITERATOR_H__
#define __SHANNONBASE_NESTED_LOOP_ITERATOR_H__

#include "sql/iterators/composite_iterators.h"
#include "storage/rapid_engine/executor/iterators/record_iterator.h"

class AccessPath;
namespace ShannonBase {
namespace Executor {
bool SelectVectorizedNestedLoop(THD *thd, AccessPath *path, bool batch_consumer);

namespace detail {

class BatchNestedLoop final : public RowIterator, public BatchReadable {
 public:
  BatchNestedLoop(THD *thd, unique_ptr_destroy_only<RowIterator> outer, unique_ptr_destroy_only<RowIterator> inner,
                  JoinType type, bool pfs, const Prealloced_array<TABLE *, 4> &tables,
                  const Prealloced_array<TABLE *, 4> &outer_tables, const Prealloced_array<TABLE *, 4> &inner_tables,
                  size_t budget);
  bool Init() override;
  bool SupportsBatchRead() const override;
  bool PreservesBatchNullRows() const override;
  int Read() override;
  void SetNullRowFlag(bool value) override;
  void UnlockRow() override;
  void EndPSIBatchModeIfStarted() override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) override;
  bool BatchNullRow(TABLE *table, size_t row) const override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;

 private:
  int ReadBatchImpl(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows);
  bool Prepare(const pack_rows::TableCollection &tables, std::pmr::vector<ColumnChunk> &chunks);
  int Fetch(BatchReadable *source, std::pmr::vector<ColumnChunk> &chunks, std::pmr::vector<uint32_t> &selection,
            std::pmr::vector<uint64_t> &nulls, size_t &rows, bool &eof);
  int FetchInnerRows();
  bool BindOuter();
  bool Emit(std::pmr::vector<ColumnChunk> &output, bool complement);
  void FinishProbe();

  unique_ptr_destroy_only<RowIterator> m_outer, m_inner;
  bool m_skip_outer_init{false};
  NestedLoopIterator m_native;
  JoinType m_type;
  bool m_pfs;
  pack_rows::TableCollection m_tables, m_outer_tables, m_inner_tables;
  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;
  std::pmr::vector<ColumnChunk> m_outer_chunks, m_inner_chunks;
  struct OutputColumn {
    size_t target, source;
    bool inner;
  };
  std::pmr::vector<OutputColumn> m_output_columns;
  std::pmr::vector<uint32_t> m_outer_selection, m_inner_selection;
  std::pmr::vector<uint64_t> m_outer_nulls, m_inner_nulls, m_output_nulls;
  std::pmr::vector<char> m_decode;
  BatchReadable *m_outer_batch{nullptr}, *m_inner_batch{nullptr}, *m_inner_active{nullptr};
  size_t m_outer_pos{0}, m_outer_rows{0}, m_inner_pos{0}, m_inner_rows{0}, m_capacity{0};
  bool m_enabled{false}, m_outer_eof{false}, m_inner_eof{false}, m_probe{false}, m_found{false};
};
}  // namespace detail

// Batch-driven join; the record adapter materializes only at scalar consumers.
class VectorizedNestedLoopIterator final : public VectorizedRecordIterator {
 public:
  static bool CanVectorize(AccessPath *path);
  VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                               unique_ptr_destroy_only<RowIterator> inner, JoinType type, bool pfs_batch_mode,
                               const Prealloced_array<TABLE *, 4> &tables,
                               const Prealloced_array<TABLE *, 4> &outer_tables,
                               const Prealloced_array<TABLE *, 4> &inner_tables, size_t budget, double estimate);
  VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                               unique_ptr_destroy_only<RowIterator> inner, JoinType type, bool pfs_batch_mode,
                               const Prealloced_array<TABLE *, 4> &tables, size_t budget, double estimate);
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
