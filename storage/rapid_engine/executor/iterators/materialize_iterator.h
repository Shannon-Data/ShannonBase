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
#ifndef __SHANNONBASE_MATERIALIZE_ITERATOR_H__
#define __SHANNONBASE_MATERIALIZE_ITERATOR_H__
#include "storage/rapid_engine/executor/iterators/iterator.h"
class AccessPath;

namespace ShannonBase {
namespace Executor {
// SQL still owns deduplication, invalidation and LIMIT. Drain its table while
// Init owns the build phase, then serve only Rapid-owned columns to consumers.
class VectorizedMaterializeIterator final : public RowIterator, public BatchReadable {
 public:
  static bool CanVectorize(const AccessPath *path);
  VectorizedMaterializeIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source, TABLE *table, size_t budget,
                                ha_rows limit = HA_POS_ERROR);
  bool Init() override;
  int Read() override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
  void SetNullRowFlag(bool value) override { value ? m_table->set_null_row() : m_table->reset_null_row(); }
  void UnlockRow() override {}
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }
  std::unique_ptr<Morsel::ParallelSource> CreateParallelSource(ResMgmt::BoundedMemoryResource *memory,
                                                               const std::pmr::vector<Field *> &fields) override;
  bool SupportsBatchRead() const override { return true; }

 private:
  inline int Error(const char *message) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), message);
    return 1;
  }
  bool WriteBlock();
  bool InitImpl();
  int EnsureBlock();
  bool Restore(const std::pmr::vector<ColumnChunk> &chunks, size_t row);

  ResMgmt::MemoryBudget::Reservation m_reservation;
  ResMgmt::BoundedMemoryResource m_memory;
  unique_ptr_destroy_only<RowIterator> m_source;
  TABLE *m_table{nullptr};
  std::pmr::vector<ColumnChunk> m_columns, m_tail;
  std::pmr::vector<ColumnChunk> m_input;
  std::pmr::vector<uint32_t> m_input_selection{&m_memory};
  std::pmr::vector<uchar> m_encoded;
  std::shared_ptr<FILE> m_file;

  size_t m_rows{0}, m_position{0}, m_total{0}, m_emitted{0}, m_max_rows{0};
  size_t m_tail_start{0}, m_tail_count{0}, m_batch_capacity{0};
  uint64_t m_spill_bytes{0};
  ha_rows m_limit{HA_POS_ERROR};
  query_id_t m_query_id{0};
  bool m_ready{false};
};

namespace Morsel {
/**
 * Serves a resident, immutable block of columns to parallel workers.
 *
 * The block is filled once by the coordinator and never mutated afterwards, so
 * workers only copy row ranges out of it. Ordinals are the serial row positions,
 * which is what a consumer merges on when tasks finish out of order.
 *
 * Two consumers need this shape: an operator that materializes its input
 * (derived tables) and a nested loop join that must rescan its inner side once
 * per outer row. `Replay()` is what makes the second one possible.
 */
class MaterializedSource final : public ParallelSource, public ReplayableParallelSource {
 public:
  MaterializedSource(ResMgmt::BoundedMemoryResource *memory, const std::pmr::vector<ColumnChunk> &rows,
                     size_t row_count, const std::pmr::vector<Field *> &fields)
      : m_rows(rows), m_fields(fields, memory), m_queue(memory), m_row_count(row_count) {
    m_queue.Add(0, row_count);
    m_max_ordinal = row_count ? row_count - 1 : 0;
  }

  size_t tasks() const override { return m_queue.size(); }
  uint64_t max_ordinal() const override { return m_max_ordinal; }
  size_t replay_row_count() const override { return m_row_count; }

  // Any thread may copy any range out of the block. Chunks must be bound to the
  // same fields, in the same order, this source was created with.
  bool Replay(size_t from, size_t count, std::pmr::vector<ColumnChunk> &chunks) override {
    if (count == 0) return true;
    if (chunks.size() != m_fields.size()) return false;
    for (size_t col = 0; col < m_fields.size(); ++col) {
      const ColumnChunk *column = Find(m_fields[col]);
      if (column == nullptr || !chunks[col].append_range_from(*column, from, count)) return false;
    }
    return true;
  }

  bool ExecuteImpl(THD *, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                   const Consumer &consume) override;

 private:
  const ColumnChunk *Find(const Field *field) const {
    for (const auto &candidate : m_rows)
      if (candidate.valid() && candidate.source_field() == field) return &candidate;
    return nullptr;
  }

  const std::pmr::vector<ColumnChunk> &m_rows;
  std::pmr::vector<Field *> m_fields;
  ParallelQueue m_queue;
  size_t m_row_count{0};
  uint64_t m_max_ordinal{0};
};
}  // namespace Morsel
}  // namespace Executor
}  // namespace ShannonBase
#endif
