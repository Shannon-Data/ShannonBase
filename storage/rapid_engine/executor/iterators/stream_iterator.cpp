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
#include "storage/rapid_engine/executor/iterators/stream_iterator.h"
#include "scope_guard.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/sql_optimizer.h"
#include "sql/temp_table_param.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/executor/iterators/record_iterator.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"

namespace ShannonBase {
namespace Executor {
namespace {
class StreamInput final : public RowIterator {
 public:
  StreamInput(THD *thd, RowIterator *source) : RowIterator(thd), m_source(source) {}
  bool Init() override { return m_source->Init(); }
  int Read() override { return m_source->Read(); }
  void SetNullRowFlag(bool value) override { m_source->SetNullRowFlag(value); }
  void UnlockRow() override { m_source->UnlockRow(); }

 private:
  RowIterator *m_source;
};
bool SameRepresentation(Field *source, Field *target) {
  return source != nullptr && target != nullptr && source != target && source->real_type() == target->real_type() &&
         source->pack_length() == target->pack_length() && source->decimals() == target->decimals() &&
         source->field_length == target->field_length &&
         source->is_flag_set(UNSIGNED_FLAG) == target->is_flag_set(UNSIGNED_FLAG) &&
         source->charset() == target->charset() && !Utils::IsOffPageField(source);
}
}  // namespace

bool VectorizedStreamIterator::CanVectorize(const Temp_table_param *param, TABLE *table, bool provide_rowid) {
  if (param == nullptr || table == nullptr || provide_rowid || !param->copy_fields.empty() ||
      param->items_to_copy == nullptr)
    return false;
  DBUG_EXECUTE_IF("rapid_disable_vectorized_stream", { return false; });
  for (const Func_ptr &copy : *param->items_to_copy) {
    if (copy.func() == nullptr || copy.result_field() == nullptr) return false;
    Item *item = copy.func()->real_item();
    if (item->type() != Item::FIELD_ITEM) return false;
    Field *source = down_cast<Item_field *>(item)->field;
    if (!SameRepresentation(source, copy.result_field()) || source->is_flag_set(NOT_SECONDARY_FLAG)) return false;
    if ((source->is_nullable() || source->table->is_nullable()) && !copy.result_field()->is_nullable()) return false;
    if (Utils::Util::is_string(source->type())) {
      if (!RecordDictionary(source)) return false;
    } else if (Utils::Util::normalized_length(source) != source->pack_length())
      return false;
  }
  for (uint i = 0; i < table->s->fields; ++i) {
    if (!bitmap_is_set(table->read_set, i)) continue;
    bool mapped = false;
    for (const Func_ptr &copy : *param->items_to_copy) {
      if (copy.result_field() != table->field[i]) continue;
      Item *item = copy.func()->real_item();
      if (item->type() != Item::FIELD_ITEM ||
          !SameRepresentation(down_cast<Item_field *>(item)->field, copy.result_field()))
        return false;
      mapped = true;
    }
    if (!mapped) return false;
  }
  return true;
}

VectorizedStreamIterator::VectorizedStreamIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                   Temp_table_param *param, TABLE *table, bool provide_rowid,
                                                   JOIN *join, int ref_slice,
                                                   const Prealloced_array<TABLE *, 4> &input_tables, size_t budget)
    : RowIterator(thd),
      m_reservation(ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kBatchTransport, budget)),
      m_memory(m_reservation.bytes()),
      m_source(std::move(source)),
      m_native(thd, NewIterator<StreamInput>(thd, thd->mem_root, m_source.get()), param, table, provide_rowid, join,
               ref_slice),
      m_param(param),
      m_table(table),
      m_join(join),
      m_output_slice(ref_slice),
      m_provide_rowid(provide_rowid),
      m_input_fields(&m_memory),
      m_input(&m_memory),
      m_tail(&m_memory),
      m_bindings(&m_memory),
      m_decode(&m_memory) {
  for (TABLE *input : input_tables)
    for (uint i = 0; i < input->s->fields; ++i)
      m_input_fields.push_back(bitmap_is_set(input->read_set, i) ? input->field[i] : nullptr);
}

int VectorizedStreamIterator::Error() {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid stream could not preserve its columnar projection");
  return 1;
}

bool VectorizedStreamIterator::Init() {
  m_tail.clear();
  m_tail_start = m_tail_count = 0;
  m_input.clear();
  m_bindings.clear();
  m_batch = nullptr;
  if (!m_reservation) return Error();
  m_input_slice = m_join->get_ref_item_slice();
  if (m_native.Init()) return true;
  auto *batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (batch == nullptr || !batch->SupportsBatchRead() || !CanVectorize(m_param, m_table, m_provide_rowid)) return false;
  if (!TryReserve(m_input, m_input_fields.size())) return Error();
  for (Field *field : m_input_fields) {
    m_input.emplace_back(field, field ? 1 : 0, &m_memory);
    if (m_input.back().failed()) return Error();
  }
  if (!TryReserve(m_bindings, m_param->items_to_copy->size())) return Error();
  for (const Func_ptr &copy : *m_param->items_to_copy) {
    Field *target = copy.result_field();
    if (target == nullptr || !bitmap_is_set(m_table->read_set, target->field_index())) continue;
    Field *source = down_cast<Item_field *>(copy.func()->real_item())->field;
    const auto found = std::find(m_input_fields.begin(), m_input_fields.end(), source);
    if (found == m_input_fields.end()) return false;
    auto dictionary = batch->Dictionary(source);
    if (Utils::Util::is_string(source->type()) && dictionary == nullptr) return false;
    if (!dictionary && Utils::Util::normalized_length(source) != source->pack_length()) return false;
    m_bindings.push_back({source, target, static_cast<size_t>(found - m_input_fields.begin()), std::move(dictionary)});
  }
  // Reserve the widest decode target so LoadTail never allocates per row.
  size_t decode_width{0};
  for (const auto &binding : m_bindings)
    if (binding.dictionary)
      decode_width = std::max(decode_width, static_cast<size_t>(binding.source->field_length) + 1);
  if (m_decode.capacity() < decode_width && !TryReserve(m_decode, decode_width)) return Error();
  if (m_decode.size() < decode_width) m_decode.resize(decode_width);
  m_batch = batch;
  return false;
}

std::shared_ptr<Compress::Dictionary> VectorizedStreamIterator::Dictionary(const Field *field) const {
  for (const auto &binding : m_bindings)
    if (binding.target == field) return binding.dictionary;
  return {};
}

bool VectorizedStreamIterator::LoadTail() {
  m_table->reset_null_row();
  for (const auto &chunk : m_tail) {
    if (!chunk.valid()) continue;
    Field *field = chunk.source_field();
    if (chunk.nullable_fast(m_tail_start)) {
      field->set_null();
      continue;
    }
    field->set_notnull();
    auto dictionary = Dictionary(field);
    if (dictionary) {
      uint32_t code;
      memcpy(&code, chunk.data_fast(m_tail_start), sizeof(code));
      // Init() reserved the widest decode target, so this never allocates.
      m_decode.resize(field->field_length + 1);
      const auto length = dictionary->get(code, m_decode.data(), m_decode.size());
      if (!length || field->store(m_decode.data(), *length, field->charset()) != TYPE_OK) return true;
    } else
      memcpy(field->field_ptr(), chunk.data_fast(m_tail_start), field->pack_length());
  }
  ++m_tail_start;
  --m_tail_count;
  if (m_output_slice >= 0 && !m_join->ref_items[m_output_slice].is_null()) m_join->set_ref_item_slice(m_output_slice);
  return false;
}

int VectorizedStreamIterator::Read() {
  if (m_tail_count != 0) return LoadTail() ? Error() : 0;
  return m_native.Read();
}

int VectorizedStreamIterator::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  auto report_batches = create_scope_guard([&] {
    if (rows != 0)
      RapidMonitor::rapid_counters.query_vectorized_stream_batch_rows_total.fetch_add(rows, std::memory_order_relaxed);
  });
  if (m_batch == nullptr) return Error();
  capacity = std::min<size_t>(capacity, 1024);
  if (capacity == 0) return 0;
  if (m_tail_count != 0) {
    if (chunks.size() != m_tail.size()) return Error();
    while (rows < capacity && m_tail_count != 0) {
      for (size_t i = 0; i < chunks.size(); ++i)
        if (chunks[i].valid() && !chunks[i].append_from(m_tail[i], m_tail_start)) return Error();
      ++rows;
      ++m_tail_start;
      --m_tail_count;
    }
    return 0;
  }
  m_join->set_ref_item_slice(m_input_slice);
  auto output_slice = create_scope_guard([&] {
    if (m_output_slice >= 0 && !m_join->ref_items[m_output_slice].is_null()) m_join->set_ref_item_slice(m_output_slice);
  });
  for (auto &chunk : m_input) {
    chunk.clear();
    if (chunk.valid() && chunk.capacity() < capacity && !chunk.grow(capacity)) return Error();
  }
  const int result = m_batch->ReadBatch(m_input, capacity, rows);
  if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
  for (auto &chunk : chunks) {
    if (!chunk.valid()) continue;
    const Binding *binding = nullptr;
    for (const auto &candidate : m_bindings)
      if (candidate.target == chunk.source_field()) binding = &candidate;
    if (binding == nullptr) return Error();
    const auto &input = m_input[binding->index];
    if (chunk.width() != input.width()) return Error();
    for (size_t row = 0; row < rows; ++row) {
      const bool is_null = input.nullable_fast(row);
      if (!chunk.add(is_null ? nullptr : input.data_fast(row), is_null ? 0 : input.width(), is_null)) return Error();
    }
  }
  return result;
}

bool VectorizedStreamIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                 size_t total) {
  return PushbackBatchTailImpl(chunks, from, total);
}

bool VectorizedStreamIterator::PushbackBatchTailImpl(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                     size_t total) {
  return PushbackBatchTailShared(chunks, from, total, &m_tail, &m_tail_start, &m_tail_count, nullptr, &m_memory);
}
}  // namespace Executor
}  // namespace ShannonBase
