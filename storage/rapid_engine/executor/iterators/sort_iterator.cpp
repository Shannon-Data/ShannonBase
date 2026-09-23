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
#include "storage/rapid_engine/executor/iterators/sort_iterator.h"

#include <algorithm>
#include <cstring>
#include <numeric>

#include "my_dbug.h"
#include "sql/field.h"
#include "sql/filesort.h"
#include "sql/item.h"
#include "sql/sort_param.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Executor {
namespace {
// Bytes Field::make_sort_key writes for this column, or 0 when it cannot key it.
size_t KeyPartWidth(const Field *field) {
  switch (field->real_type()) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VECTOR:
      return 0;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return field->pack_length();
    default:
      break;
  }
  if (field->is_flag_set(BLOB_FLAG) || field->is_array()) return 0;
  if (!is_temporal_type(field->type()) && field->result_type() == STRING_RESULT) {
    const CHARSET_INFO *cs = field->charset();
    return cs->coll->strnxfrmlen(cs, field->field_length);
  }
  return field->pack_length();
}

// First up to eight key bytes as a big-endian integer, zero padded.
inline uint64_t LoadKeyPrefix(const uchar *key, size_t width) {
  uint64_t v = 0;
  const size_t n = std::min<size_t>(width, 8);
  for (size_t i = 0; i < n; ++i) v = (v << 8) | key[i];
  return v << (8 * (8 - n));
}
}  // namespace

bool VectorizedSortIterator::CanVectorize(const Filesort *filesort, bool unwrap_rollup,
                                          table_map tables_to_get_rowid_for) {
  // Lets a test run the same query on MySQL's filesort for comparison.
  DBUG_EXECUTE_IF("rapid_disable_vectorized_sort", { return false; });
  if (filesort == nullptr || unwrap_rollup || tables_to_get_rowid_for != 0) return false;
  if (filesort->m_remove_duplicates || filesort->m_force_sort_rowids) return false;
  if (filesort->sort_order_length() == 0 || filesort->tables.empty()) return false;
  for (uint i = 0; i < filesort->sort_order_length(); ++i) {
    const Item *item = filesort->sortorder[i].item;
    if (item == nullptr) return false;
    item = const_cast<Item *>(item)->real_item();
    if (item->type() != Item::FIELD_ITEM) return false;
    const Field *field = down_cast<const Item_field *>(item)->field;
    if (field == nullptr || field->table == nullptr ||
        std::find(filesort->tables.begin(), filesort->tables.end(), field->table) == filesort->tables.end())
      return false;
    const size_t width = KeyPartWidth(field);
    if (width == 0 || width > kMaxKeyPartBytes) return false;
  }
  return true;
}

VectorizedSortIterator::VectorizedSortIterator(THD *thd, Filesort *filesort,
                                               unique_ptr_destroy_only<RowIterator> source,
                                               pack_rows::TableCollection tables, size_t memory_budget,
                                               ha_rows *examined_rows)
    : RowIterator(thd),
      m_filesort(filesort),
      m_source(std::move(source)),
      m_tables(std::move(tables)),
      m_memory_budget(memory_budget),
      m_examined_rows(examined_rows) {
  for (uint i = 0; i < filesort->sort_order_length(); ++i) {
    const st_sort_field &sf = filesort->sortorder[i];
    KeyPart kp;
    kp.field = down_cast<Item_field *>(sf.item->real_item())->field;
    kp.width = KeyPartWidth(kp.field);
    kp.reverse = sf.reverse;
    kp.maybe_null = kp.field->is_nullable() || kp.field->table->is_nullable();
    m_key_width += kp.width + (kp.maybe_null ? 1 : 0);
    m_key_parts.push_back(kp);
  }
}

VectorizedSortIterator::~VectorizedSortIterator() { ResetState(); }

void VectorizedSortIterator::ResetState() {
  for (Run &run : m_runs) Utils::Util::close_spill_file(run.file);
  m_runs.clear();
  m_merge_heap.clear();
  m_last_run = SIZE_MAX;
  m_keys.clear();
  m_payload.clear();
  m_payload_offsets.assign(1, 0);
  m_order.clear();
  m_topn_payload.clear();
  m_next = 0;
}

// NULL sorts first ascending and last descending: the marker byte is 0 for
// NULL and is inverted with the rest of the part.
void VectorizedSortIterator::EncodeKey(uchar *to) const {
  for (const KeyPart &kp : m_key_parts) {
    uchar *const start = to;
    const bool is_null = kp.field->is_null();
    if (kp.maybe_null) *to++ = is_null ? 0 : 1;
    if (is_null) {
      memset(to, 0, kp.width);
    } else {
      const size_t written = kp.field->make_sort_key(to, kp.width);
      if (written < kp.width) memset(to + written, 0, kp.width - written);
    }
    to += kp.width;
    if (kp.reverse)
      for (uchar *p = start; p < to; ++p) *p = static_cast<uchar>(~*p);
  }
}

size_t VectorizedSortIterator::BufferedBytes() const {
  return m_keys.size() + m_payload.size() + m_payload_offsets.size() * sizeof(size_t);
}

// Sorts the buffered rows' indices by key; ties keep input order.
void VectorizedSortIterator::SortBuffered() {
  const size_t rows = m_payload_offsets.size() - 1;
  const size_t width = m_key_width;
  const uchar *keys = m_keys.data();
  m_order.resize(rows);
  if (width <= 8) {
    std::vector<std::pair<uint64_t, uint32_t>> prefixed(rows);
    for (size_t i = 0; i < rows; ++i) prefixed[i] = {LoadKeyPrefix(keys + i * width, width), static_cast<uint32_t>(i)};
    std::sort(prefixed.begin(), prefixed.end());
    for (size_t i = 0; i < rows; ++i) m_order[i] = prefixed[i].second;
    return;
  }
  std::iota(m_order.begin(), m_order.end(), 0u);
  std::sort(m_order.begin(), m_order.end(), [keys, width](uint32_t a, uint32_t b) {
    const int c = memcmp(keys + size_t{a} * width, keys + size_t{b} * width, width);
    return c < 0 || (c == 0 && a < b);
  });
}

// Batch input: a single-table child that can hand over column chunks, and
// every column the query reads is fixed-width, so its chunk bytes are the
// Field's record image.
bool VectorizedSortIterator::SetupBatch() {
  m_batch_source = nullptr;
  if (m_tables.tables().size() != 1) return false;
  auto *batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (batch == nullptr || !batch->SupportsBatchRead()) return false;
  TABLE *table = m_tables.tables()[0].table;
  if (table->is_nullable()) return false;

  m_payload_fields.clear();
  m_row_width = 0;
  for (const pack_rows::Column &column : m_tables.tables()[0].columns) {
    Field *field = column.field;
    if (field->is_flag_set(NOT_SECONDARY_FLAG) || Utils::Util::is_string(field->type()) ||
        Utils::IsOffPageField(field) || field->is_flag_set(BLOB_FLAG))
      return false;
    m_payload_fields.push_back({field, m_row_width, field->pack_length()});
    m_row_width += 1 + field->pack_length();
  }
  for (const KeyPart &kp : m_key_parts)
    if (!bitmap_is_set(table->read_set, kp.field->field_index())) return false;

  m_chunks.clear();
  m_chunks.reserve(table->s->fields);
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (bitmap_is_set(table->read_set, i) && !field->is_flag_set(NOT_SECONDARY_FLAG))
      m_chunks.emplace_back(field, kBatchRows);
    else
      m_chunks.emplace_back(nullptr, 0);
  }
  m_batch_source = batch;
  return true;
}

// Keys of block row `row` from the column chunks. Integers, DATE and YEAR are
// encoded directly (big-endian, sign bit flipped); DECIMAL's record image is
// already byte-comparable; anything else goes through Field::make_sort_key.
void VectorizedSortIterator::EncodeBatchKeys(size_t rows) {
  const size_t width = m_key_width;
  m_block_keys.resize(rows * width);
  size_t offset = 0;
  for (const KeyPart &kp : m_key_parts) {
    const ColumnChunk &chunk = m_chunks[kp.field->field_index()];
    const enum_field_types type = kp.field->real_type();
    const bool is_unsigned = kp.field->is_flag_set(UNSIGNED_FLAG);
    for (size_t r = 0; r < rows; ++r) {
      uchar *to = m_block_keys.data() + r * width + offset;
      uchar *const start = to;
      const bool is_null = chunk.nullable_fast(r);
      if (kp.maybe_null) *to++ = is_null ? 0 : 1;
      if (is_null) {
        memset(to, 0, kp.width);
      } else {
        const uchar *from = chunk.data_fast(r);
        switch (type) {
          case MYSQL_TYPE_TINY:
          case MYSQL_TYPE_SHORT:
          case MYSQL_TYPE_INT24:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
            for (size_t i = 0; i < kp.width; ++i) to[i] = from[kp.width - 1 - i];
            if (!is_unsigned) to[0] ^= 0x80;
            break;
          case MYSQL_TYPE_NEWDATE:
          case MYSQL_TYPE_YEAR:
            for (size_t i = 0; i < kp.width; ++i) to[i] = from[kp.width - 1 - i];
            break;
          case MYSQL_TYPE_NEWDECIMAL:
            memcpy(to, from, kp.width);
            break;
          default: {
            kp.field->set_notnull();
            memcpy(kp.field->field_ptr(), from, kp.field->pack_length());
            const size_t written = kp.field->make_sort_key(to, kp.width);
            if (written < kp.width) memset(to + written, 0, kp.width - written);
          }
        }
      }
      if (kp.reverse)
        for (uchar *p = start; p < to + kp.width; ++p) *p = static_cast<uchar>(~*p);
    }
    offset += kp.width + (kp.maybe_null ? 1 : 0);
  }
}

// Reads the next block: one row on the row path, one batch on the batch path.
// Its keys land in m_block_keys. Returns the row count, 0 at the end, -1 on error.
long VectorizedSortIterator::FetchBlock() {
  if (thd()->killed) {
    thd()->send_kill_message();
    return -1;
  }
  if (m_batch_source == nullptr) {
    const int err = m_source->Read();
    if (err == -1) return 0;
    if (err != 0) return -1;
    m_block_keys.resize(m_key_width);
    EncodeKey(m_block_keys.data());
    if (m_examined_rows != nullptr) ++*m_examined_rows;
    return 1;
  }
  if (m_batch_eof) return 0;
  for (ColumnChunk &chunk : m_chunks) chunk.clear();
  size_t rows = 0;
  const int err = m_batch_source->ReadBatch(m_chunks, kBatchRows, rows);
  if (err == HA_ERR_END_OF_FILE)
    m_batch_eof = true;
  else if (err != 0)
    return -1;
  if (rows == 0) return 0;
  EncodeBatchKeys(rows);
  if (m_examined_rows != nullptr) *m_examined_rows += rows;
  return static_cast<long>(rows);
}

// Payload of block row `row`: the packed row on the row path, a fixed image of
// [NULL byte][record bytes] per read column on the batch path.
const uchar *VectorizedSortIterator::BlockPayload(size_t row, size_t *length) {
  if (m_batch_source == nullptr) {
    m_row_buffer.length(0);
    if (pack_rows::StoreFromTableBuffers(m_tables, &m_row_buffer)) return nullptr;
    *length = m_row_buffer.length();
    return pointer_cast<const uchar *>(m_row_buffer.ptr());
  }
  m_row_image.resize(m_row_width);
  for (const PayloadField &pf : m_payload_fields) {
    const ColumnChunk &chunk = m_chunks[pf.field->field_index()];
    uchar *to = m_row_image.data() + pf.offset;
    to[0] = chunk.nullable_fast(row) ? 1 : 0;
    if (to[0] == 0) memcpy(to + 1, chunk.data_fast(row), pf.width);
  }
  *length = m_row_width;
  return m_row_image.data();
}

void VectorizedSortIterator::LoadRow(const uchar *row) {
  if (m_batch_source == nullptr) {
    pack_rows::LoadIntoTableBuffers(m_tables, row);
    return;
  }
  for (const PayloadField &pf : m_payload_fields) {
    const uchar *from = row + pf.offset;
    if (from[0] != 0) {
      pf.field->set_null();
      continue;
    }
    pf.field->set_notnull();
    memcpy(pf.field->field_ptr(), from + 1, pf.width);
  }
}

bool VectorizedSortIterator::Sink() {
  size_t total = 0;
  for (;;) {
    const long rows = FetchBlock();
    if (rows < 0) return true;
    if (rows == 0) break;
    total += rows;
    for (long r = 0; r < rows; ++r) {
      size_t length = 0;
      const uchar *payload = BlockPayload(r, &length);
      if (payload == nullptr) return true;
      const uchar *key = m_block_keys.data() + r * m_key_width;
      m_keys.insert(m_keys.end(), key, key + m_key_width);
      m_payload.insert(m_payload.end(), payload, payload + length);
      m_payload_offsets.push_back(m_payload.size());
    }
    if (BufferedBytes() > m_memory_budget && SpillRun()) return true;
  }
  RapidMonitor::rapid_counter_vectorized_sort_rows(total);
  if (!m_runs.empty()) return (m_payload_offsets.size() > 1 && SpillRun()) || StartMerge();
  SortBuffered();
  return false;
}

// Keeps the m_limit best rows in a max-heap of slots; a row that cannot enter
// is never copied.
bool VectorizedSortIterator::SinkTopN() {
  const size_t limit = static_cast<size_t>(m_limit);
  const size_t width = m_key_width;
  std::vector<uint32_t> heap;
  auto slot_key = [this, width](uint32_t slot) { return m_keys.data() + size_t{slot} * width; };
  auto worse = [&](uint32_t a, uint32_t b) {
    const int c = memcmp(slot_key(a), slot_key(b), width);
    return c < 0 || (c == 0 && a < b);
  };
  size_t total = 0;
  for (;;) {
    const long rows = FetchBlock();
    if (rows < 0) return true;
    if (rows == 0) break;
    total += rows;
    if (limit == 0) continue;
    for (long r = 0; r < rows; ++r) {
      const uchar *key = m_block_keys.data() + r * width;
      uint32_t slot;
      if (heap.size() < limit) {
        slot = static_cast<uint32_t>(heap.size());
        m_keys.resize(m_keys.size() + width);
        m_topn_payload.emplace_back();
      } else {
        if (memcmp(key, slot_key(heap.front()), width) >= 0) continue;
        std::pop_heap(heap.begin(), heap.end(), worse);
        slot = heap.back();
        heap.pop_back();
      }
      size_t length = 0;
      const uchar *payload = BlockPayload(r, &length);
      if (payload == nullptr) return true;
      memcpy(m_keys.data() + size_t{slot} * width, key, width);
      m_topn_payload[slot].assign(pointer_cast<const char *>(payload), length);
      heap.push_back(slot);
      std::push_heap(heap.begin(), heap.end(), worse);
    }
  }
  RapidMonitor::rapid_counter_vectorized_sort_rows(total);
  std::sort(heap.begin(), heap.end(), worse);
  m_order.assign(heap.begin(), heap.end());
  return false;
}

bool VectorizedSortIterator::SpillRun() {
  SortBuffered();
  Run run;
  run.file = Utils::Util::create_spill_file("rapid_sort");
  if (run.file == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not create a spill file");
    return true;
  }
  for (const uint32_t idx : m_order) {
    const uint32_t length = static_cast<uint32_t>(m_payload_offsets[idx + 1] - m_payload_offsets[idx]);
    if (fwrite(m_keys.data() + size_t{idx} * m_key_width, 1, m_key_width, run.file) != m_key_width ||
        fwrite(&length, sizeof(length), 1, run.file) != 1 ||
        fwrite(m_payload.data() + m_payload_offsets[idx], 1, length, run.file) != length) {
      Utils::Util::close_spill_file(run.file);
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not write its spill file");
      return true;
    }
  }
  run.remaining = m_order.size();
  RapidMonitor::rapid_counter_vectorized_sort_spill_rows(m_order.size());
  m_runs.push_back(std::move(run));
  m_keys.clear();
  m_payload.clear();
  m_payload_offsets.assign(1, 0);
  m_order.clear();
  return false;
}

// Loads the run's next record; an exhausted run is left with an empty key.
// True on a read error.
bool VectorizedSortIterator::AdvanceRun(Run *run) {
  if (run->remaining == 0) {
    run->key.clear();
    return false;
  }
  uint32_t length = 0;
  run->key.resize(m_key_width);
  if (fread(run->key.data(), 1, m_key_width, run->file) != m_key_width ||
      fread(&length, sizeof(length), 1, run->file) != 1) {
    run->remaining = 0;
    run->key.clear();
    return true;
  }
  run->payload.resize(length);
  if (length != 0 && fread(run->payload.data(), 1, length, run->file) != length) {
    run->remaining = 0;
    run->key.clear();
    return true;
  }
  --run->remaining;
  return false;
}

bool VectorizedSortIterator::StartMerge() {
  for (size_t i = 0; i < m_runs.size(); ++i) {
    Run &run = m_runs[i];
    if (fflush(run.file) != 0 || fseek(run.file, 0, SEEK_SET) != 0 || AdvanceRun(&run)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not read its spill file");
      return true;
    }
    if (!run.key.empty()) m_merge_heap.push_back(i);
  }
  const size_t width = m_key_width;
  std::make_heap(m_merge_heap.begin(), m_merge_heap.end(), [this, width](size_t a, size_t b) {
    const int c = memcmp(m_runs[a].key.data(), m_runs[b].key.data(), width);
    return c > 0 || (c == 0 && a > b);
  });
  return false;
}

// The emitted run is advanced only on the next call: its payload buffer backs
// the fields just loaded, BLOB pointers included.
int VectorizedSortIterator::ReadMerged() {
  const size_t width = m_key_width;
  auto later = [this, width](size_t a, size_t b) {
    const int c = memcmp(m_runs[a].key.data(), m_runs[b].key.data(), width);
    return c > 0 || (c == 0 && a > b);
  };
  if (m_last_run != SIZE_MAX) {
    Run &run = m_runs[m_last_run];
    if (AdvanceRun(&run)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not read its spill file");
      return 1;
    }
    if (!run.key.empty()) {
      m_merge_heap.push_back(m_last_run);
      std::push_heap(m_merge_heap.begin(), m_merge_heap.end(), later);
    }
    m_last_run = SIZE_MAX;
  }
  if (m_merge_heap.empty()) return -1;
  std::pop_heap(m_merge_heap.begin(), m_merge_heap.end(), later);
  const size_t top = m_merge_heap.back();
  m_merge_heap.pop_back();
  LoadRow(pointer_cast<const uchar *>(m_runs[top].payload.data()));
  m_last_run = top;
  return 0;
}

bool VectorizedSortIterator::Init() {
  ResetState();
  // StoreFromTableBuffers() relies on the caller's reservation unless a BLOB is involved.
  if (m_row_buffer.reserve(pack_rows::ComputeRowSizeUpperBound(m_tables))) return true;
  if (m_source->Init()) return true;
  m_batch_eof = false;
  SetupBatch();
  m_limit = m_filesort->limit;
  if (m_limit != HA_POS_ERROR && m_limit <= kMaxTopNRows) return SinkTopN();
  return Sink();
}

int VectorizedSortIterator::Read() {
  if (!m_runs.empty()) return ReadMerged();
  if (m_next >= m_order.size()) return -1;
  const uint32_t idx = m_order[m_next++];
  const uchar *row = m_topn_payload.empty() ? m_payload.data() + m_payload_offsets[idx]
                                            : pointer_cast<const uchar *>(m_topn_payload[idx].data());
  LoadRow(row);
  return 0;
}

void VectorizedSortIterator::SetNullRowFlag(bool is_null_row) {
  for (TABLE *table : m_filesort->tables) {
    if (is_null_row)
      table->set_null_row();
    else
      table->reset_null_row();
  }
}
}  // namespace Executor
}  // namespace ShannonBase
