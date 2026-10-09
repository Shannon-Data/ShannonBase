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
#include <limits>
#include <numeric>
#include <string_view>

#include "my_dbug.h"
#include "sql/field.h"
#include "sql/filesort.h"
#include "sql/item.h"
#include "sql/sort_param.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"
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
      m_memory_reservation(ResMgmt::reserve_query_memory(thd, memory_budget)),
      m_sort_memory(m_memory_reservation.bytes() / 2),
      m_aux_memory(m_memory_reservation.bytes() - m_memory_reservation.bytes() / 2),
      m_filesort(filesort),
      m_source(std::move(source)),
      m_tables(std::move(tables)),
      m_memory_budget(m_memory_reservation.bytes()),
      m_examined_rows(examined_rows),
      m_keys(&m_sort_memory),
      m_payload(&m_sort_memory),
      m_payload_offsets(&m_sort_memory),
      m_order(&m_sort_memory) {}

VectorizedSortIterator::~VectorizedSortIterator() = default;

void VectorizedSortIterator::ResetState() {
  decltype(m_runs)(&m_aux_memory).swap(m_runs);
  m_spool.reset();
  m_manifest.reset();
  m_spool_run_count = m_spool_bytes = m_live_spill_bytes = 0;
  decltype(m_merge_heap)(&m_aux_memory).swap(m_merge_heap);
  m_last_run = SIZE_MAX;
  std::pmr::vector<uchar>(&m_sort_memory).swap(m_keys);
  std::pmr::vector<uchar>(&m_sort_memory).swap(m_payload);
  std::pmr::vector<size_t>(1, 0, &m_sort_memory).swap(m_payload_offsets);
  std::pmr::vector<uint32_t>(&m_sort_memory).swap(m_order);
  decltype(m_topn_payload)(&m_sort_memory).swap(m_topn_payload);
  decltype(m_chunks)(&m_aux_memory).swap(m_chunks);
  decltype(m_payload_fields)(&m_aux_memory).swap(m_payload_fields);
  decltype(m_batch_dictionaries)(&m_aux_memory).swap(m_batch_dictionaries);
  decltype(m_dictionary_decode_scratch)(&m_aux_memory).swap(m_dictionary_decode_scratch);
  decltype(m_row_image)(&m_aux_memory).swap(m_row_image);
  decltype(m_block_keys)(&m_aux_memory).swap(m_block_keys);
  m_row_buffer.set(static_cast<char *>(nullptr), 0, &my_charset_bin);
  decltype(m_row_storage)(&m_aux_memory).swap(m_row_storage);
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

bool VectorizedSortIterator::CanUseTopN() const {
  if (m_limit == HA_POS_ERROR || m_limit > kMaxTopNRows) return false;
  if (m_limit == 0) return true;
  if (m_tables.has_blob_column()) return false;
  const size_t row_upper_bound =
      m_batch_source != nullptr ? m_row_width : pack_rows::ComputeRowSizeUpperBound(m_tables);
  const size_t fixed = 2 * m_key_width + 2 * sizeof(std::string) + 3 * sizeof(uint32_t) + 64;
  if (row_upper_bound > std::numeric_limits<size_t>::max() / 2 - fixed) return false;
  const size_t slot_upper_bound = fixed + 2 * row_upper_bound;
  return static_cast<size_t>(m_limit) <= (m_memory_budget / 2) / slot_upper_bound;
}

// Sorts the buffered rows' indices by key; ties keep input order.
void VectorizedSortIterator::SortBuffered() {
  const size_t rows = m_payload_offsets.size() - 1;
  const size_t width = m_key_width;
  const uchar *keys = m_keys.data();
  m_order.resize(rows);
  std::iota(m_order.begin(), m_order.end(), 0u);
  std::sort(m_order.begin(), m_order.end(), [keys, width](uint32_t a, uint32_t b) {
    const int c = memcmp(keys + size_t{a} * width, keys + size_t{b} * width, width);
    return c < 0 || (c == 0 && a < b);
  });
}

// Batch input: a single-table child that can hand over column chunks, and
// every column the query reads has a fixed-width chunk image. String payloads
// remain dictionary codes until the sort key or output Field needs a value.
bool VectorizedSortIterator::SetupBatch() {
  DBUG_EXECUTE_IF("rapid_sort_aux_bad_alloc", { throw std::bad_alloc(); });
  m_batch_source = nullptr;
  if (m_tables.tables().size() != 1) return false;
  auto *batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (batch == nullptr || !batch->SupportsBatchRead()) return false;
  TABLE *table = m_tables.tables()[0].table;
  if (table->is_nullable()) return false;

  m_payload_fields.clear();
  m_batch_dictionaries.clear();
  m_batch_dictionaries.resize(table->s->fields);
  m_row_width = 0;
  for (const pack_rows::Column &column : m_tables.tables()[0].columns) {
    Field *field = column.field;
    if (field->is_flag_set(NOT_SECONDARY_FLAG) || Utils::IsOffPageField(field) || field->is_flag_set(BLOB_FLAG))
      return false;
    size_t image_width = field->pack_length();
    if (Utils::Util::is_string(field->type())) {
      auto dictionary = batch->Dictionary(field);
      if (dictionary == nullptr || field->field_index() >= m_batch_dictionaries.size()) return false;
      m_batch_dictionaries[field->field_index()] = std::move(dictionary);
      image_width = sizeof(uint32_t);
    }
    m_payload_fields.push_back({field, m_row_width, image_width});
    m_row_width += 1 + image_width;
  }
  for (const KeyPart &kp : m_key_parts) {
    if (!bitmap_is_set(table->read_set, kp.field->field_index())) return false;
    if (Utils::Util::is_string(kp.field->type()) && (kp.field->field_index() >= m_batch_dictionaries.size() ||
                                                     m_batch_dictionaries[kp.field->field_index()] == nullptr))
      return false;
  }

  if (m_key_width > SIZE_MAX - m_row_width || m_key_width + m_row_width == 0) return false;
  m_batch_capacity = std::min(kBatchRows, (m_memory_budget / 4) / (m_key_width + m_row_width));
  if (m_batch_capacity == 0) return false;
  m_chunks.clear();
  m_chunks.reserve(table->s->fields);
  for (uint i = 0; i < table->s->fields; ++i) {
    Field *field = table->field[i];
    if (bitmap_is_set(table->read_set, i) && !field->is_flag_set(NOT_SECONDARY_FLAG))
      m_chunks.emplace_back(field, m_batch_capacity, &m_aux_memory);
    else
      m_chunks.emplace_back(nullptr, 0);
  }
  for (const PayloadField &pf : m_payload_fields)
    if (Utils::Util::is_string(pf.field->type()) && m_chunks[pf.field->field_index()].width() != sizeof(uint32_t))
      return false;
  m_batch_source = batch;
  return true;
}

bool VectorizedSortIterator::RestoreDictionaryField(Field *field, uint32_t code) {
  if (field == nullptr || field->field_index() >= m_batch_dictionaries.size()) return true;
  const auto &dictionary = m_batch_dictionaries[field->field_index()];
  if (dictionary == nullptr) return true;
  std::string_view value = dictionary->get_view(code);
  if (value.data() == nullptr) {
    // Compressed entries need a decode buffer; an existing empty entry has a
    // non-null data pointer, so it stays on the zero-copy path.
    m_dictionary_decode_scratch.resize(field->field_length + 1);
    const auto length = dictionary->get(code, m_dictionary_decode_scratch.data(), m_dictionary_decode_scratch.size());
    if (!length.has_value()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not decode a dictionary value");
      return true;
    }
    value = std::string_view(m_dictionary_decode_scratch.data(), *length);
  }
  field->set_notnull();
  if (field->real_type() == MYSQL_TYPE_VARCHAR && value.size() <= field->field_length) {
    // This value was stored in the same column before Rapid encoded it. The
    // charset and field width have already been checked, so recreate the
    // Field_varstring record image directly instead of converting it again.
    auto *varstring = down_cast<Field_varstring *>(field);
    uchar *to = varstring->field_ptr();
    const uint32 length_bytes = varstring->get_length_bytes();
    if (length_bytes == 1)
      to[0] = static_cast<uchar>(value.size());
    else
      int2store(to, static_cast<uint16>(value.size()));
    memcpy(to + length_bytes, value.data(), value.size());
    return false;
  }
  Utils::ColumnMapGuard write_guard(field->table, Utils::ColumnMapGuard::TYPE::WRITE);
  field->store(value.data(), value.size(), field->charset());
  return false;
}

bool VectorizedSortIterator::EncodeDictionaryVarstringKey(const KeyPart &key_part, uint32_t code, uchar *to) {
  Field *field = key_part.field;
  if (field == nullptr || field->field_index() >= m_batch_dictionaries.size()) return true;
  const auto &dictionary = m_batch_dictionaries[field->field_index()];
  if (dictionary == nullptr) return true;
  std::string_view decoded = dictionary->get_view(code);
  if (decoded.data() == nullptr) {
    m_dictionary_decode_scratch.resize(field->field_length + 1);
    const auto decoded_length =
        dictionary->get(code, m_dictionary_decode_scratch.data(), m_dictionary_decode_scratch.size());
    if (!decoded_length.has_value()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not decode a dictionary key");
      return true;
    }
    decoded = std::string_view(m_dictionary_decode_scratch.data(), *decoded_length);
  }
  const CHARSET_INFO *charset = field->charset();
  const auto *value = pointer_cast<const uchar *>(decoded.data());
  const size_t prefix_length = my_charpos(charset, value, value + decoded.size(), field->char_length());
  const size_t value_length = std::min(decoded.size(), prefix_length);
  const int flags = charset->pad_attribute == NO_PAD ? 0 : MY_STRXFRM_PAD_TO_MAXLEN;
  const size_t written =
      charset->coll->strnxfrm(charset, to, key_part.width, field->char_length(), value, value_length, flags);
  if (written < key_part.width) memset(to + written, 0, key_part.width - written);
  return false;
}

// Keys of block row `row` from the column chunks. Integers, DATE and YEAR are
// encoded directly (big-endian, sign bit flipped); DECIMAL's record image is
// already byte-comparable; anything else goes through Field::make_sort_key.
bool VectorizedSortIterator::EncodeBatchKeys(size_t rows) {
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
            if (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING) {
              uint32_t code;
              memcpy(&code, from, sizeof(code));
              if (EncodeDictionaryVarstringKey(kp, code, to)) return true;
              break;
            }
            if (Utils::Util::is_string(type)) {
              uint32_t code;
              memcpy(&code, from, sizeof(code));
              if (RestoreDictionaryField(kp.field, code)) return true;
            } else {
              kp.field->set_notnull();
              memcpy(kp.field->field_ptr(), from, kp.field->pack_length());
            }
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
  return false;
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
  const int err = m_batch_source->ReadBatch(m_chunks, m_batch_capacity, rows);
  if (err == HA_ERR_END_OF_FILE)
    m_batch_eof = true;
  else if (err != 0)
    return -1;
  if (rows == 0) return 0;
  if (EncodeBatchKeys(rows)) return -1;
  if (m_examined_rows != nullptr) *m_examined_rows += rows;
  return static_cast<long>(rows);
}

// Payload of block row `row`: the packed row on the row path, a fixed image of
// [NULL byte][record bytes] per read column on the batch path.
const uchar *VectorizedSortIterator::BlockPayload(size_t row, size_t *length) {
  if (m_batch_source == nullptr) {
    if (pack_rows::ComputeRowSizeUpperBound(m_tables) > m_memory_budget / 4) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort row exceeds its input memory budget");
      return nullptr;
    }
    PrepareBudgetedString(&m_row_buffer, &m_row_storage, pack_rows::ComputeRowSizeUpperBound(m_tables));
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

bool VectorizedSortIterator::LoadRow(const uchar *row) {
  if (m_batch_source == nullptr) {
    pack_rows::LoadIntoTableBuffers(m_tables, row);
    return false;
  }

  for (const PayloadField &pf : m_payload_fields) {
    const uchar *from = row + pf.offset;
    if (from[0] != 0) {
      pf.field->set_null();
      continue;
    }

    if (Utils::Util::is_string(pf.field->type())) {
      uint32_t code;
      memcpy(&code, from + 1, sizeof(code));
      if (RestoreDictionaryField(pf.field, code)) return true;
    } else {
      pf.field->set_notnull();
      memcpy(pf.field->field_ptr(), from + 1, pf.width);
    }
  }
  return false;
}

bool VectorizedSortIterator::Sink() {
  size_t total = 0;
  for (;;) {
    const long rows = FetchBlock();
    if (rows < 0) return true;
    if (rows == 0) break;
    total += rows;
    for (long row = 0; row < rows; ++row) {
      if (m_payload_offsets.size() > UINT32_MAX && SpillRun()) return true;
      size_t length = 0;
      const uchar *payload = BlockPayload(row, &length);
      if (!payload) return true;
      const uchar *key = m_block_keys.data() + row * m_key_width;
      // Reserve every replacement buffer before changing any logical row.
      // If the bounded allocator refuses growth, the existing run remains
      // complete and can be spilled before retrying this same input row.
      auto reserve_row = [&]() -> bool {
        auto grow = [](auto &buffer, size_t required) {
          if (required <= buffer.capacity()) return;
          size_t capacity = buffer.capacity();
          if (capacity <= SIZE_MAX / 2) capacity *= 2;
          buffer.reserve(std::max(required, capacity));
        };
        if (m_keys.size() > SIZE_MAX - m_key_width || m_payload.size() > SIZE_MAX - length ||
            m_payload_offsets.size() == SIZE_MAX)
          return false;
        try {
          grow(m_keys, m_keys.size() + m_key_width);
          grow(m_payload, m_payload.size() + length);
          grow(m_payload_offsets, m_payload_offsets.size() + 1);
          grow(m_order, m_payload_offsets.size());
        } catch (const std::bad_alloc &) {
          return false;
        }
        return true;
      };
      if (!reserve_row()) {
        if (m_payload_offsets.size() <= 1) {
          my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort row exceeds its memory budget");
          return true;
        }
        if (SpillRun()) return true;
        if (!reserve_row()) {
          my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort row exceeds its memory budget");
          return true;
        }
      }
      m_keys.insert(m_keys.end(), key, key + m_key_width);
      m_payload.insert(m_payload.end(), payload, payload + length);
      m_payload_offsets.push_back(m_payload.size());
    }
  }
  RapidMonitor::rapid_counter_vectorized_sort_rows(total);
  if (m_spool_run_count != 0) return (m_payload_offsets.size() > 1 && SpillRun()) || StartMerge();
  SortBuffered();
  return false;
}

// Keeps the m_limit best rows in a max-heap of slots; a row that cannot enter
// is never copied.
bool VectorizedSortIterator::SinkTopN() {
  const size_t limit = static_cast<size_t>(m_limit);
  const size_t width = m_key_width;
  std::pmr::vector<uint32_t> heap(&m_sort_memory);
  auto slot_key = [this, width](uint32_t slot) { return m_keys.data() + size_t{slot} * width; };
  auto worse = [&](uint32_t a, uint32_t b) {
    const int c = memcmp(slot_key(a), slot_key(b), width);
    return c < 0 || (c == 0 && a < b);
  };

  size_t total{0};
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

      size_t length{0};
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

VectorizedSortIterator::SpillStream VectorizedSortIterator::OpenSpill(const char *prefix) {
  struct Owner {
    ResMgmt::BoundedMemoryResource::Reservation file_memory;
    std::array<char, 1024> buffer{};
    FILE *file{nullptr};
    Owner(ResMgmt::BoundedMemoryResource *memory, const char *name)
        : file_memory(memory, sizeof(FILE) + 1024), file(Utils::Util::create_spill_file(name)) {
      if (file && setvbuf(file, buffer.data(), _IOFBF, buffer.size())) {
        Utils::Util::close_spill_file(file);
        file = nullptr;
      }
    }
    ~Owner() { Utils::Util::close_spill_file(file); }
  };
  auto owner =
      std::allocate_shared<Owner>(std::pmr::polymorphic_allocator<Owner>(&m_aux_memory), &m_aux_memory, prefix);
  if (!owner->file) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not create a spill file");
    return {};
  }
  return SpillStream(owner, owner->file);
}

bool VectorizedSortIterator::WriteRunRecord(Run *run, const uchar *key, const uchar *payload, size_t length) {
  DBUG_EXECUTE_IF("rapid_sort_spill_write_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Injected Rapid sort spill write failure");
    return true;
  });
  if (thd()->killed) {
    thd()->send_kill_message();
    return true;
  }
  // Leave room for input buffers, vector/string growth and stdio buffers.
  const size_t head_limit = m_memory_budget / (4 * kMaxOpenRuns);
  if (length > UINT32_MAX || m_key_width > head_limit || length > head_limit - m_key_width) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort row exceeds the bounded merge memory budget");
    return true;
  }
  const uint64_t bytes = m_key_width + sizeof(uint32_t) + length;
  const uint64_t limit = shannon_rpd_engine_cfg.sort_spill_size_max;
  if (bytes > limit || m_live_spill_bytes > limit - bytes) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort exceeded rapid_sort_spill_size_max");
    return true;
  }
  const uint32_t packed_length = static_cast<uint32_t>(length);
  if (fwrite(key, 1, m_key_width, run->file.get()) != m_key_width ||
      fwrite(&packed_length, sizeof(packed_length), 1, run->file.get()) != 1 ||
      (length && fwrite(payload, 1, length, run->file.get()) != length)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not write its spill file");
    return true;
  }
  run->bytes += bytes;
  m_live_spill_bytes += bytes;
  ++run->remaining;
  return false;
}

bool VectorizedSortIterator::WriteDescriptor(FILE *manifest, const Run &run) {
  const uint64_t descriptor[] = {run.offset, run.bytes, run.remaining};
  const uint64_t limit = shannon_rpd_engine_cfg.sort_spill_size_max;
  if (sizeof(descriptor) > limit || m_live_spill_bytes > limit - sizeof(descriptor) ||
      fwrite(descriptor, sizeof(descriptor), 1, manifest) != 1) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not write its run manifest within the spill limit");
    return true;
  }
  m_live_spill_bytes += sizeof(descriptor);
  return false;
}

bool VectorizedSortIterator::ReadDescriptor(FILE *manifest, const SpillStream &file, Run *run) {
  uint64_t descriptor[3];
  if (fread(descriptor, sizeof(descriptor), 1, manifest) != 1 || descriptor[1] > UINT64_MAX - descriptor[0] ||
      descriptor[2] > SIZE_MAX)
    return true;
  run->file = file;
  run->offset = run->read_offset = descriptor[0];
  run->bytes = descriptor[1];
  run->end_offset = descriptor[0] + descriptor[1];
  run->remaining = static_cast<size_t>(descriptor[2]);
  return false;
}

bool VectorizedSortIterator::SpillRun() {
  SortBuffered();
  if (!m_spool) m_spool = OpenSpill("rapid_sort");
  if (!m_manifest) m_manifest = OpenSpill("rapid_sort_manifest");
  if (!m_spool || !m_manifest) return true;
  Run run(&m_aux_memory);
  run.file = m_spool;
  const auto offset = ftello(m_spool.get());
  if (offset < 0) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not locate its spill write position");
    return true;
  }
  run.offset = static_cast<uint64_t>(offset);
  for (const uint32_t idx : m_order) {
    if (WriteRunRecord(&run, m_keys.data() + size_t{idx} * m_key_width, m_payload.data() + m_payload_offsets[idx],
                       m_payload_offsets[idx + 1] - m_payload_offsets[idx]))
      return true;
  }
  if (WriteDescriptor(m_manifest.get(), run)) return true;
  ++m_spool_run_count;
  m_spool_bytes = m_live_spill_bytes;
  RapidMonitor::rapid_counter_vectorized_sort_spill_rows(m_order.size());
  // clear() retained capacity and triggered another spill immediately.
  std::pmr::vector<uchar>(&m_sort_memory).swap(m_keys);
  std::pmr::vector<uchar>(&m_sort_memory).swap(m_payload);
  std::pmr::vector<size_t>(1, 0, &m_sort_memory).swap(m_payload_offsets);
  std::pmr::vector<uint32_t>(&m_sort_memory).swap(m_order);
  return false;
}

bool VectorizedSortIterator::ReadRunBytes(Run *run, void *data, size_t length) {
  auto *out = static_cast<uchar *>(data);
  while (length != 0) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return true;
    }
    if (run->read_pos == run->read_size) {
      if (run->read_offset >= run->end_offset) return true;
      const size_t bytes =
          static_cast<size_t>(std::min<uint64_t>(run->read_buffer.size(), run->end_offset - run->read_offset));
      // Each head has its own read-ahead buffer and offset. pread avoids
      // thrashing a shared stdio buffer as the merge switches between runs.
      if (my_pread(fileno(run->file.get()), run->read_buffer.data(), bytes, run->read_offset, MYF(0)) != bytes)
        return true;
      run->read_offset += bytes;
      run->read_pos = 0;
      run->read_size = bytes;
    }
    const size_t bytes = std::min(length, run->read_size - run->read_pos);
    std::memcpy(out, run->read_buffer.data() + run->read_pos, bytes);
    out += bytes;
    length -= bytes;
    run->read_pos += bytes;
  }
  return false;
}

bool VectorizedSortIterator::AdvanceRun(Run *run) {
  if (run->remaining == 0) {
    run->file.reset();
    run->key.clear();
    std::pmr::string(&m_aux_memory).swap(run->payload);
    return false;
  }
  uint32_t length = 0;
  run->key.resize(m_key_width);
  if (ReadRunBytes(run, run->key.data(), m_key_width) || ReadRunBytes(run, &length, sizeof(length))) return true;
  const size_t head_limit = m_memory_budget / (4 * kMaxOpenRuns);
  if (m_key_width > head_limit || length > head_limit - m_key_width) return true;
  run->payload.resize(length);
  if (ReadRunBytes(run, run->payload.data(), length)) return true;
  --run->remaining;
  return false;
}

bool VectorizedSortIterator::MergePass() {
  auto output = OpenSpill("rapid_sort_merge");
  auto manifest = OpenSpill("rapid_sort_manifest");
  if (!output || !manifest || fflush(m_spool.get()) || fflush(m_manifest.get()) || fseek(m_manifest.get(), 0, SEEK_SET))
    return true;
  const uint64_t before = m_live_spill_bytes;
  uint64_t next_runs = 0;
  for (uint64_t first = 0; first < m_spool_run_count; first += MergeFanIn()) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(MergeFanIn(), m_spool_run_count - first));
    std::pmr::vector<Run> inputs(&m_aux_memory);
    inputs.reserve(count);
    for (size_t i = 0; i < count; ++i) inputs.emplace_back(&m_aux_memory);
    for (auto &input : inputs) {
      if (ReadDescriptor(m_manifest.get(), m_spool, &input) || AdvanceRun(&input)) return true;
    }
    Run merged(&m_aux_memory);
    merged.file = output;
    const auto offset = ftello(output.get());
    if (offset < 0) return true;
    merged.offset = static_cast<uint64_t>(offset);
    for (;;) {
      size_t winner = count;
      for (size_t i = 0; i < count; ++i) {
        if (!inputs[i].key.empty() &&
            (winner == count || std::memcmp(inputs[i].key.data(), inputs[winner].key.data(), m_key_width) < 0))
          winner = i;
      }
      if (winner == count) break;
      Run &input = inputs[winner];
      if (WriteRunRecord(&merged, input.key.data(), pointer_cast<const uchar *>(input.payload.data()),
                         input.payload.size()) ||
          AdvanceRun(&input))
        return true;
    }
    if (WriteDescriptor(manifest.get(), merged)) return true;
    ++next_runs;
  }
  const uint64_t next_bytes = m_live_spill_bytes - before;
  // Release the previous pass only after every output run and descriptor was
  // written. At most four anonymous files and eight merge heads are live.
  m_spool = std::move(output);
  m_manifest = std::move(manifest);
  m_live_spill_bytes -= m_spool_bytes;
  m_spool_bytes = next_bytes;
  m_spool_run_count = next_runs;
  return false;
}

bool VectorizedSortIterator::StartMerge() {
  decltype(m_chunks)(&m_aux_memory).swap(m_chunks);
  decltype(m_block_keys)(&m_aux_memory).swap(m_block_keys);
  decltype(m_row_image)(&m_aux_memory).swap(m_row_image);
  m_row_buffer.set(static_cast<char *>(nullptr), 0, &my_charset_bin);
  decltype(m_row_storage)(&m_aux_memory).swap(m_row_storage);
  while (m_spool_run_count > MergeFanIn()) {
    if (MergePass()) {
      if (!thd()->is_error()) my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort merge pass failed");
      return true;
    }
  }
  if (fflush(m_spool.get()) || fflush(m_manifest.get()) || fseek(m_manifest.get(), 0, SEEK_SET)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not prepare its spill files for reading");
    return true;
  }
  m_runs.clear();
  m_runs.reserve(static_cast<size_t>(m_spool_run_count));
  for (size_t i = 0; i < m_spool_run_count; ++i) m_runs.emplace_back(&m_aux_memory);
  for (size_t i = 0; i < m_runs.size(); ++i) {
    if (ReadDescriptor(m_manifest.get(), m_spool, &m_runs[i]) || AdvanceRun(&m_runs[i])) {
      if (!thd()->is_error()) my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not read its spill file");
      return true;
    }
    if (!m_runs[i].key.empty()) m_merge_heap.push_back(i);
  }
  m_manifest.reset();
  m_spool.reset();
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
      if (!thd()->is_error()) my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort could not read its spill file");
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
  if (LoadRow(pointer_cast<const uchar *>(m_runs[top].payload.data()))) return 1;
  m_last_run = top;
  return 0;
}

bool VectorizedSortIterator::Init() try {
  if (!m_memory_reservation) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid query memory reservation exhausted");
    return true;
  }
  DBUG_EXECUTE_IF("rapid_iterator_bad_alloc", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid iterator allocation failure");
    return true;
  });
  ResetState();
  if (m_key_parts.empty()) {
    for (uint i = 0; i < m_filesort->sort_order_length(); ++i) {
      const st_sort_field &sf = m_filesort->sortorder[i];
      KeyPart kp;
      kp.field = down_cast<Item_field *>(sf.item->real_item())->field;
      kp.width = KeyPartWidth(kp.field);
      kp.reverse = sf.reverse;
      kp.maybe_null = kp.field->is_nullable() || kp.field->table->is_nullable();
      m_key_width += kp.width + (kp.maybe_null ? 1 : 0);
      m_key_parts.push_back(kp);
    }
  }
  // StoreFromTableBuffers() relies on the caller's reservation unless a BLOB is involved.
  const size_t input_bytes = pack_rows::ComputeRowSizeUpperBound(m_tables);
  if (input_bytes > m_memory_budget / 4) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort row exceeds its input memory budget");
    return true;
  }
  PrepareBudgetedString(&m_row_buffer, &m_row_storage, input_bytes);
  if (m_source->Init()) return true;

  m_batch_eof = false;
  SetupBatch();
  m_limit = m_filesort->limit;

  if (CanUseTopN()) return SinkTopN();
  return Sink();
} catch (const std::bad_alloc &) {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort auxiliary memory exhausted");
  return true;
}

int VectorizedSortIterator::Read() try {
  if (!m_runs.empty()) return ReadMerged();
  if (m_next >= m_order.size()) return -1;

  const uint32_t idx = m_order[m_next++];
  const uchar *row = m_topn_payload.empty() ? m_payload.data() + m_payload_offsets[idx]
                                            : pointer_cast<const uchar *>(m_topn_payload[idx].data());
  if (LoadRow(row)) return 1;
  return 0;
} catch (const std::bad_alloc &) {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid sort auxiliary memory exhausted");
  return 1;
}

void VectorizedSortIterator::SetNullRowFlag(bool is_null_row) {
  for (TABLE *table : m_filesort->tables) {
    is_null_row ? table->set_null_row() : table->reset_null_row();
  }
}
}  // namespace Executor
}  // namespace ShannonBase
