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

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "storage/rapid_engine/resource_management/res_mgmt.h"

#include "sql/iterators/row_iterator.h"
#include "sql/pack_rows.h"
#include "sql_string.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"

class Filesort;
class Field;
class THD;

namespace ShannonBase {
namespace Executor {
// Only used for DISTINCT: equal keys may choose any input representative.
// Four-bit MSD partitions need 32 size_t counters per digit, independent of
// row count. Callers charge this scratch to the operator's memory reservation.
inline constexpr size_t kRadixMaxKeyBytes = 16;
inline constexpr size_t kRadixShortKeyMaxBytes = sizeof(uint64_t) + sizeof(unsigned char);
inline constexpr size_t kRadixMinRows = 512;
inline constexpr size_t kRadixWideKeyMinRows = 4096;
inline constexpr size_t kRadixDigitBits = 4;
inline constexpr size_t kRadixDigitsPerByte = CHAR_BIT / kRadixDigitBits;
inline constexpr size_t kRadixBucketCount = size_t{1} << kRadixDigitBits;

// Each digit retains cumulative bucket ends and the next write positions.
inline constexpr size_t kRadixCountersPerDigit = kRadixBucketCount + kRadixBucketCount;
inline constexpr size_t kRadixSmallPartitionRows = 64;
static_assert(CHAR_BIT % kRadixDigitBits == 0);

inline bool UseRadixForDistinct(size_t rows, size_t width) {
  // NULL-marked BIGINT keys fit in nine bytes. Longer composite/DECIMAL keys
  // need larger runs to amortize prefix passes, especially on skewed input.
  return width > 0 && width <= kRadixMaxKeyBytes &&
         rows >= (width <= kRadixShortKeyMaxBytes ? kRadixMinRows : kRadixWideKeyMinRows);
}

inline size_t FixedKeySortScratchSize(size_t width) { return width * kRadixDigitsPerByte * kRadixCountersPerDigit; }

inline void RadixDistinctKeys(uint32_t *order, size_t rows, const unsigned char *keys, size_t width, size_t digit,
                              size_t *scratch) {
  while (rows > kRadixSmallPartitionRows && digit < width * kRadixDigitsPerByte) {
    size_t *end = scratch + digit * kRadixCountersPerDigit;
    size_t *next = end + kRadixBucketCount;
    std::fill(end, end + kRadixBucketCount, 0);

    const auto bucket = [=](uint32_t row) {
      const size_t shift = (kRadixDigitsPerByte - 1 - digit % kRadixDigitsPerByte) * kRadixDigitBits;
      return (keys[size_t{row} * width + digit / kRadixDigitsPerByte] >> shift) & (kRadixBucketCount - 1);
    };

    for (size_t i = 0; i < rows; ++i) ++end[bucket(order[i])];

    size_t sum{0}, occupied{0};
    for (size_t b = 0; b < kRadixBucketCount; ++b) {
      next[b] = sum;
      occupied += end[b] != 0;
      sum += end[b];
      end[b] = sum;
    }
    // Skip common prefixes without another recursion level or any swaps.
    if (occupied == 1) {
      ++digit;
      continue;
    }

    for (size_t b = 0; b < kRadixBucketCount; ++b) {
      while (next[b] < end[b]) {
        const size_t target = bucket(order[next[b]]);
        if (target == b)
          ++next[b];
        else
          std::swap(order[next[b]], order[next[target]++]);
      }
    }

    size_t start{0};
    for (size_t b = 0; b < kRadixBucketCount; ++b) {
      RadixDistinctKeys(order + start, end[b] - start, keys, width, digit + 1, scratch);
      start = end[b];
    }
    return;
  }

  if (digit < width * kRadixDigitsPerByte) {
    std::sort(order, order + rows, [=](uint32_t a, uint32_t b) {
      return memcmp(keys + size_t{a} * width, keys + size_t{b} * width, width) < 0;
    });
  }
}

inline void SortFixedDistinctKeys(uint32_t *order, size_t rows, const unsigned char *keys, size_t width,
                                  size_t *scratch) {
  assert(width > 0 && width <= kRadixMaxKeyBytes);
  // Avoid radix passes for ordered, reverse-ordered, or constant input. This
  // also protects workloads where comparison sort already does little work.
  bool ascending = true, descending = true;
  for (size_t i = 1; i < rows && (ascending || descending); ++i) {
    const int c = memcmp(keys + size_t{order[i - 1]} * width, keys + size_t{order[i]} * width, width);
    ascending &= c <= 0;
    descending &= c >= 0;
  }

  if (ascending) return;

  if (descending) {
    std::reverse(order, order + rows);
    return;
  }
  RadixDistinctKeys(order, rows, keys, width, 0, scratch);
}

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
class VectorizedSortIterator final : public RowIterator, public BatchReadable {
  // Declared first so the reservation outlives all retained buffers.
  ResMgmt::MemoryBudget::Reservation m_memory_reservation;
  ResMgmt::BoundedMemoryResource m_sort_memory;
  ResMgmt::BoundedMemoryResource m_aux_memory;

 public:
  /// A key part wider than this leaves the sort to MySQL's filesort.
  static constexpr size_t kMaxKeyPartBytes = 1024;
  /// A LIMIT up to this many rows is kept in a bounded heap.
  static constexpr size_t kMaxTopNRows = 1 << 20;
  /// Rows per batch read from a batch-capable child.

  /// Whether this operator can run the sort; false keeps SortingIterator.
  /// Decided when the plan is built, so EXPLAIN shows what runs.
  static bool CanVectorize(const Filesort *filesort, bool unwrap_rollup, table_map tables_to_get_rowid_for);
  static bool CanVectorizeBatches(const Filesort *filesort);

  VectorizedSortIterator(THD *thd, Filesort *filesort, unique_ptr_destroy_only<RowIterator> source,
                         pack_rows::TableCollection tables, size_t memory_budget, ha_rows *examined_rows);
  ~VectorizedSortIterator() override;

  bool Init() override;
  int Read() override;
  bool SupportsBatchRead() const override { return m_batch_source != nullptr; }
  std::shared_ptr<Compress::Dictionary> Dictionary(const Field *field) const override;
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override;
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from, size_t total) override;
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
  using SpillStream = std::shared_ptr<FILE>;
  struct Run {
    explicit Run(std::pmr::memory_resource *memory = std::pmr::get_default_resource()) : key(memory), payload(memory) {}
    SpillStream file;
    uint64_t offset{0};
    uint64_t bytes{0};
    size_t remaining{0};
    uint64_t read_offset{0};
    uint64_t end_offset{0};
    std::array<uchar, 1024> read_buffer{};
    size_t read_pos{0}, read_size{0};
    std::pmr::vector<uchar> key;
    std::pmr::string payload;
  };

  struct PayloadField {
    Field *field{nullptr};
    size_t offset{0};  // of its NULL byte in the fixed row image
    size_t width{0};
    size_t chunk_index{0};
  };

  void EncodeKey(uchar *to) const;
  bool InitImpl();
  int ReadBatchImpl(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows);
  bool SetupBatch();
  bool EncodeBatchKeys(size_t rows);
  long FetchBlock();
  const uchar *BlockPayload(size_t row, size_t *length);
  bool LoadRow(const uchar *row);
  size_t ChunkIndex(const Field *field) const;
  int NextPayload(const uchar **row);
  bool LoadTailRow();
  bool RestoreDictionaryField(Field *field, uint32_t code);
  bool EncodeDictionaryVarstringKey(const KeyPart &key_part, uint32_t code, uchar *to);
  bool Sink();
  bool SinkTopN();
  bool CanUseTopN() const;
  bool SortBuffered();
  bool SpillRun();
  bool AdvanceRun(Run *run);
  bool MergePass();
  SpillStream OpenSpill(const char *prefix);
  bool ReadRunBytes(Run *run, void *data, size_t length);
  bool WriteDescriptor(FILE *manifest, const Run &run);
  bool ReadDescriptor(FILE *manifest, const SpillStream &file, Run *run);
  bool WriteRunRecord(Run *run, const uchar *key, const uchar *payload, size_t length);
  static constexpr size_t kMaxOpenRuns = 8;
  size_t MergeFanIn() const { return std::clamp(m_memory_budget / (32 * 1024), size_t{2}, kMaxOpenRuns); }
  bool StartMerge();
  int ReadMerged(const uchar **row);
  bool ResetState();

  Filesort *m_filesort;
  unique_ptr_destroy_only<RowIterator> m_source;
  pack_rows::TableCollection m_tables;
  const size_t m_memory_budget;
  ha_rows *m_examined_rows;

  std::pmr::vector<KeyPart> m_key_parts{&m_aux_memory};
  size_t m_key_width{0};
  ha_rows m_limit{HA_POS_ERROR};
  ha_rows m_emitted{0};
  // Exact equality key retained across batches and spill-run boundaries.
  std::pmr::vector<uchar> m_distinct_key{&m_aux_memory};
  bool m_have_distinct_key{false};
  size_t m_radix_runs{0}, m_comparison_runs{0};

  // Buffered rows: m_keys holds m_key_width bytes per row, m_payload the
  // packed rows back to back, m_payload_offsets[i]..[i+1] the i-th row.
  std::pmr::vector<uchar> m_keys;
  std::pmr::vector<uchar> m_payload;
  std::pmr::vector<size_t> m_payload_offsets;
  std::pmr::vector<uint32_t> m_order;  // row indices in sorted order
  size_t m_next{0};

  // Top-N: one packed payload per heap slot.
  std::pmr::vector<std::pmr::string> m_topn_payload{&m_sort_memory};

  // Run descriptors are streamed from disk, not retained once per input run.
  SpillStream m_spool, m_manifest;
  uint64_t m_spool_run_count{0};
  uint64_t m_spool_bytes{0};
  uint64_t m_live_spill_bytes{0};
  std::pmr::vector<Run> m_runs{&m_aux_memory};
  std::pmr::vector<size_t> m_merge_heap{&m_aux_memory};
  size_t m_last_run{SIZE_MAX};  // run whose record is loaded; advanced on the next Read()

  // Batch path: chunks indexed by field_index, the read columns' fixed row
  // image layout, and the keys of the block just read.
  BatchReadable *m_batch_source{nullptr};
  bool m_batch_eof{false};
  size_t m_batch_capacity{1};
  std::pmr::vector<ColumnChunk> m_chunks{&m_aux_memory};
  std::pmr::vector<uint32_t> m_input_selection{&m_aux_memory};
  std::pmr::vector<PayloadField> m_payload_fields{&m_aux_memory};
  std::pmr::vector<std::shared_ptr<Compress::Dictionary>> m_batch_dictionaries{&m_aux_memory};
  std::pmr::string m_dictionary_decode_scratch{&m_aux_memory};
  size_t m_row_width{0};
  std::pmr::vector<uchar> m_row_image{&m_aux_memory};
  std::pmr::vector<uchar> m_block_keys{&m_aux_memory};

  std::pmr::vector<char> m_row_storage{&m_aux_memory};
  String m_row_buffer;
  std::pmr::vector<ColumnChunk> m_output_tail{&m_aux_memory};
  std::pmr::vector<const PayloadField *> m_output_columns{&m_aux_memory};
  size_t m_output_tail_start{0}, m_output_tail_count{0};
};
}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_SORT_ITERATOR_H__
