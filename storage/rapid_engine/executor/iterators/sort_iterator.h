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
#ifndef __SHANNONBASE_SORT_ITERATOR_H__
#define __SHANNONBASE_SORT_ITERATOR_H__

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sql/iterators/row_iterator.h"
#include "sql/pack_rows.h"
#include "sql_string.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"

class Filesort;
class Field;
class THD;

namespace ShannonBase {
namespace Executor {
/**
 * ORDER BY for Rapid plans, replacing MySQL's SortingIterator.
 *
 * Only the ORDER BY columns are sorted: each input row becomes one fixed-width,
 * byte-comparable key (Field::make_sort_key, bytes inverted for DESC) and a
 * packed payload of the columns the query reads (pack_rows, which follows each
 * table's read_set). Keys are sorted as (key, row index) pairs and the payload
 * is gathered in that order on output. With a LIMIT, a bounded heap keeps only
 * the best rows. Past the memory budget, sorted runs go to disk and are merged.
 */
class VectorizedSortIterator final : public RowIterator {
 public:
  /// A key part wider than this leaves the sort to MySQL's filesort.
  static constexpr size_t kMaxKeyPartBytes = 1024;
  /// A LIMIT up to this many rows is kept in a bounded heap.
  static constexpr size_t kMaxTopNRows = 1 << 20;
  /// Rows per batch read from a batch-capable child.
  static constexpr size_t kBatchRows = 1024;

  /// Whether this operator can run the sort; false keeps SortingIterator.
  /// Decided when the plan is built, so EXPLAIN shows what runs.
  static bool CanVectorize(const Filesort *filesort, bool unwrap_rollup, table_map tables_to_get_rowid_for);

  VectorizedSortIterator(THD *thd, Filesort *filesort, unique_ptr_destroy_only<RowIterator> source,
                         pack_rows::TableCollection tables, size_t memory_budget, ha_rows *examined_rows);
  ~VectorizedSortIterator() override;

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override {}
  void StartPSIBatchMode() override { m_source->StartPSIBatchMode(); }
  void EndPSIBatchModeIfStarted() override { m_source->EndPSIBatchModeIfStarted(); }

 private:
  struct KeyPart {
    Field *field{nullptr};
    size_t width{0};  // bytes of make_sort_key output, NULL marker excluded
    bool reverse{false};
    bool maybe_null{false};
  };

  // One sorted run on disk: records of [key][u32 payload length][payload].
  struct Run {
    FILE *file{nullptr};
    size_t remaining{0};
    std::vector<uchar> key;
    std::string payload;
  };

  struct PayloadField {
    Field *field{nullptr};
    size_t offset{0};  // of its NULL byte in the fixed row image
    size_t width{0};
  };

  void EncodeKey(uchar *to) const;
  bool SetupBatch();
  void EncodeBatchKeys(size_t rows);
  long FetchBlock();
  const uchar *BlockPayload(size_t row, size_t *length);
  void LoadRow(const uchar *row);
  bool Sink();
  bool SinkTopN();
  size_t BufferedBytes() const;
  void SortBuffered();
  bool SpillRun();
  bool AdvanceRun(Run *run);
  bool StartMerge();
  int ReadMerged();
  void ResetState();

  Filesort *m_filesort;
  unique_ptr_destroy_only<RowIterator> m_source;
  pack_rows::TableCollection m_tables;
  const size_t m_memory_budget;
  ha_rows *m_examined_rows;

  std::vector<KeyPart> m_key_parts;
  size_t m_key_width{0};
  ha_rows m_limit{HA_POS_ERROR};

  // Buffered rows: m_keys holds m_key_width bytes per row, m_payload the
  // packed rows back to back, m_payload_offsets[i]..[i+1] the i-th row.
  std::vector<uchar> m_keys;
  std::vector<uchar> m_payload;
  std::vector<size_t> m_payload_offsets;
  std::vector<uint32_t> m_order;  // row indices in sorted order
  size_t m_next{0};

  // Top-N: one packed payload per heap slot.
  std::vector<std::string> m_topn_payload;

  std::vector<Run> m_runs;
  std::vector<size_t> m_merge_heap;
  size_t m_last_run{SIZE_MAX};  // run whose record is loaded; advanced on the next Read()

  // Batch path: chunks indexed by field_index, the read columns' fixed row
  // image layout, and the keys of the block just read.
  BatchReadable *m_batch_source{nullptr};
  bool m_batch_eof{false};
  std::vector<ColumnChunk> m_chunks;
  std::vector<PayloadField> m_payload_fields;
  size_t m_row_width{0};
  std::vector<uchar> m_row_image;
  std::vector<uchar> m_block_keys;

  String m_row_buffer;
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_SORT_ITERATOR_H__
