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
#include "storage/rapid_engine/executor/iterators/record_iterator.h"
#include <utility>
#include "scope_guard.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/sql_executor.h"
#include "sql/sql_opt_exec_shared.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/optimizer/utils.h"

namespace ShannonBase {
namespace Executor {
namespace detail {
BorrowedRows::BorrowedRows(THD *thd, RowIterator *source, bool *skip_init)
    : RowIterator(thd), m_source(source), m_skip_init(skip_init) {}

bool BorrowedRows::Init() {
  if (m_skip_init && std::exchange(*m_skip_init, false)) return false;
  return m_source->Init();
}

void BorrowedRows::SetNullRowFlag(bool value) { m_source->SetNullRowFlag(value); }

void BorrowedRows::UnlockRow() { m_source->UnlockRow(); }

void BorrowedRows::StartPSIBatchMode() { m_source->StartPSIBatchMode(); }

void BorrowedRows::EndPSIBatchModeIfStarted() { m_source->EndPSIBatchModeIfStarted(); }

int BorrowedRows::Read() { return m_source->Read(); }
}  // namespace detail

std::shared_ptr<Compress::Dictionary> RecordDictionary(const Field *field) {
  if (!field || !Utils::Util::is_string(field->type()) || field->real_type() == MYSQL_TYPE_ENUM ||
      field->real_type() == MYSQL_TYPE_SET)
    return {};

  auto *table = Optimizer::Utils::rpd_lookup_func()(field->table);
  if (!table || table->type() == Imcs::RpdTable::TYPE::PARTTABLE || field->field_index() >= table->meta().fields.size())
    return {};

  return table->meta().fields[field->field_index()].dictionary;
}

bool CanBatchRecords(TABLE *table) {
  if (table == nullptr || table->s == nullptr || table->read_set == nullptr) return false;

  for (uint i = 0; i < table->s->fields; ++i) {
    if (!bitmap_is_set(table->read_set, i)) continue;

    Field *field = table->field[i];
    if (field == nullptr || Utils::IsOffPageField(field)) return false;
    if (Utils::Util::is_string(field->type()) && field->real_type() != MYSQL_TYPE_ENUM &&
        field->real_type() != MYSQL_TYPE_SET) {
      if (!RecordDictionary(field)) return false;
    } else if (Utils::Util::normalized_length(field) != field->pack_length())
      return false;
  }

  return true;
}

bool SelectVectorizedLookup(THD *thd, AccessPath *path) {
  if (thd == nullptr || path == nullptr) return false;

  TABLE *table{nullptr};
  switch (path->type) {
    case AccessPath::REF:
      table = path->ref().table;
      break;
    case AccessPath::REF_OR_NULL:
      table = path->ref_or_null().table;
      break;
    case AccessPath::EQ_REF:
      table = path->eq_ref().table;
      break;
    default:
      return false;
  }

  if (!CanBatchRecords(table)) return false;

  DBUG_EXECUTE_IF("rapid_disable_vectorized_ref", { return false; });
  DBUG_EXECUTE_IF("rapid_force_vectorized_ref", { return true; });

  double columns = 0, strings = 0, width = 0, record_width = table->s->null_bytes;
  for (uint i = 0; i < table->s->fields; ++i) {
    if (!bitmap_is_set(table->read_set, i)) continue;

    const Field *field = table->field[i];
    ++columns;
    width += Utils::Util::normalized_length(field);
    record_width += field->pack_length();
    strings += RecordDictionary(field) != nullptr;
  }

  using Optimizer::RapidCostConstants;
  return Optimizer::RpdCostEstimator::NestedLoopBatchEstimator::estimate(
             path->num_output_rows(), columns, strings, width, record_width,
             std::min<size_t>(Utils::Util::hash_join_memory_budget(thd), 2 * 1024 * 1024),
             RapidCostConstants::kHashBuildPerRow, RapidCostConstants::kVectorDecompCostPerCell,
             RapidCostConstants::kHashLookupPerRow, RapidCostConstants::kArtRangeSetupCost,
             RapidCostConstants::kHashBuildCopyPerByte)
      .choose_batches();
}

namespace {
int BatchError() {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid record transport could not preserve its batch input");
  return 1;
}
}  // namespace
VectorizedRecordIterator::VectorizedRecordIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                   const Prealloced_array<TABLE *, 4> &tables, size_t budget,
                                                   double estimate, const char *operator_name,
                                                   ResMgmt::QueryMemoryOperator kind)
    : RowIterator(thd),
      m_reservation(ResMgmt::reserve_query_memory(thd, kind, budget)),
      m_memory(m_reservation.bytes()),
      m_source(std::move(source)),
      m_scalar_chunks(&m_memory),
      m_operator_name(operator_name),
      m_tables(tables, false, 0, ~table_map{0}),
      m_encodings(&m_memory),
      m_tail(&m_memory),
      m_tail_nulls(&m_memory),
      m_batch_nulls(&m_memory),
      m_resume_storage(&m_memory),
      m_decode_storage(&m_memory),
      m_cardinality(estimate) {}

bool VectorizedRecordIterator::Init() {
  if (!m_reservation) return BatchError();
  m_cardinality.Reset();
  m_stats = {};
  m_tail_start = m_tail_count = 0;
  m_tail.clear();
  m_tail_nulls.clear();
  m_batch_nulls.clear();
  m_have_resume = m_replayed_row = false;
  m_can_encode_rows = false;
  m_encodings.clear();
  size_t decode_width{0};
  size_t column_count{0}, row_bytes{sizeof(uint64_t)};
  for (const auto &entry : m_tables.tables()) column_count += entry.table->s->fields;

  if (!TryReserve(m_encodings, column_count)) return BatchError();

  for (const auto &entry : m_tables.tables()) {
    TABLE *table = entry.table;
    for (uint i = 0; i < table->s->fields; ++i) {
      if (!bitmap_is_set(table->read_set, i) && !bitmap_is_set(&table->read_set_internal, i)) continue;
      Field *field = table->field[i];
      m_encodings.push_back({field, RecordDictionary(field)});
      decode_width = std::max(decode_width, static_cast<size_t>(field->pack_length()));
      row_bytes += Utils::Util::normalized_length(field) + 1;
    }
  }

  if (!TryReserve(m_decode_storage, decode_width)) return BatchError();
  if (m_decode_storage.size() < decode_width) m_decode_storage.resize(decode_width);
  if (!PrepareBudgetedString(&m_resume, &m_resume_storage, pack_rows::ComputeRowSizeUpperBound(m_tables)))
    return BatchError();

  m_batch_capacity = SelectBatchRows(row_bytes, m_memory.limit() - m_memory.used(), 2);
  if (m_batch_capacity == 0 || !TryReserve(m_batch_nulls, m_batch_capacity)) return BatchError();
  DBUG_EXECUTE_IF("rapid_nlj_aux_bad_alloc", { return BatchError(); });

  if (m_source->Init()) return true;

  m_batch_source = dynamic_cast<BatchReadable *>(m_source->real_iterator());
  if (m_batch_source && !m_batch_source->SupportsBatchRead()) m_batch_source = nullptr;

  m_can_encode_rows = true;
  for (const auto &entry : m_tables.tables()) m_can_encode_rows = m_can_encode_rows && CanBatchRecords(entry.table);
  if (m_batch_source) {
    // Projection/limit wrappers can expose an immutable dictionary for a temp
    // Field which is not itself owned by an IMCS table.
    for (auto &encoding : m_encodings) {
      auto dictionary = m_batch_source->Dictionary(encoding.field);
      if (dictionary) encoding.dictionary = std::move(dictionary);
    }
  }
  return false;
}

bool VectorizedRecordIterator::SupportsBatchRead() const { return m_batch_source || m_can_encode_rows; }

const VectorizedRecordIterator::Encoding *VectorizedRecordIterator::FindEncoding(const Field *field) const {
  for (const auto &encoding : m_encodings) {
    if (encoding.field == field) return &encoding;
  }

  return nullptr;
}

std::shared_ptr<Compress::Dictionary> VectorizedRecordIterator::Dictionary(const Field *field) const {
  const auto *encoding = FindEncoding(field);
  return encoding ? encoding->dictionary : nullptr;
}

bool VectorizedRecordIterator::SaveResume() {
  m_resume.length(0);
  if (pack_rows::StoreFromTableBuffers(m_tables, &m_resume)) return true;

  m_have_resume = true;
  return false;
}

bool VectorizedRecordIterator::RestoreResume() {
  if (!m_have_resume) return false;

  pack_rows::LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_resume.ptr()));
  m_have_resume = false;
  return false;
}

uint64_t VectorizedRecordIterator::NullTables() const {
  uint64_t flags{0};
  size_t index{0};
  for (const auto &table : m_tables.tables()) {
    if (table.table->has_null_row()) flags |= uint64_t{1} << index;
    ++index;
  }
  return flags;
}

bool VectorizedRecordIterator::BatchNullRow(TABLE *table, size_t row) const {
  if (row >= m_batch_nulls.size()) return false;

  size_t index{0};
  for (const auto &entry : m_tables.tables()) {
    if (entry.table == table) return (m_batch_nulls[row] & (uint64_t{1} << index)) != 0;
    ++index;
  }
  return false;
}

int VectorizedRecordIterator::ReadLive() {
  m_replayed_row = false;
  const int result = m_source->Read();
  if (result == 0) {
    ++m_stats.rows_in;
    ++m_stats.rows_out;
  } else if (result == -1)
    m_cardinality.Finish(thd(), m_operator_name, m_stats.rows_out);

  return result;
}

bool VectorizedRecordIterator::AppendRow(std::pmr::vector<ColumnChunk> &chunks) {
  for (auto &chunk : chunks) {
    if (!chunk.valid()) continue;

    Field *field = chunk.source_field();
    const auto *encoding = FindEncoding(field);
    if (!encoding || chunk.width() != Utils::Util::normalized_length(field)) return true;
    if (field->is_null()) {
      if (!chunk.add(nullptr, 0, true)) return true;
      continue;
    }

    if (encoding->dictionary) {
      String view;
      int64 id{-1};
      if (field->type() == MYSQL_TYPE_STRING)
        id = encoding->dictionary->find_id(
            std::string_view(reinterpret_cast<const char *>(field->field_ptr()), field->pack_length()));

      if (id < 0) {
        const String *text = field->val_str(&view);
        if (!text) return true;
        id = encoding->dictionary->find_id(std::string_view(text->ptr(), text->length()));
      }
      if (id < 0 || static_cast<uint64_t>(id) > UINT32_MAX) return true;

      const uint32 code = static_cast<uint32>(id);
      if (!chunk.add(reinterpret_cast<const uchar *>(&code), sizeof(code), false)) return true;
    } else if (!chunk.add(field->field_ptr(), field->pack_length(), false))
      return true;

    m_stats.bytes_copied += chunk.width();
  }
  return false;
}

bool VectorizedRecordIterator::RestoreRow(const std::pmr::vector<ColumnChunk> &chunks, size_t row, uint64_t flags) {
  size_t index{0};
  for (const auto &table : m_tables.tables()) {
    table.table->set_found_row();
    (flags & (uint64_t{1} << index)) ? table.table->set_null_row() : table.table->reset_null_row();
    ++index;
  }

  for (const auto &chunk : chunks) {
    if (!chunk.valid()) continue;

    Field *field = chunk.source_field();
    if (field->table->has_null_row()) continue;
    if (chunk.nullable_fast(row)) {
      field->set_null();
      continue;
    }

    field->set_notnull();
    const auto *encoding = FindEncoding(field);
    if (!encoding) return true;
    if (encoding->dictionary) {
      uint32 id{0};
      std::memcpy(&id, chunk.data_fast(row), sizeof(id));
      // Init() reserved the widest decode target, so this never allocates.
      m_decode_storage.resize(field->pack_length());
      const auto length = encoding->dictionary->get(id, m_decode_storage.data(), m_decode_storage.size());
      Utils::ColumnMapGuard write_guard(field->table, Utils::ColumnMapGuard::TYPE::WRITE);
      if (!length || field->store(m_decode_storage.data(), *length, field->charset()) != TYPE_OK) return true;
    } else
      std::memcpy(field->field_ptr(), chunk.data_fast(row), field->pack_length());
  }
  return false;
}

bool VectorizedRecordIterator::AppendTail(std::pmr::vector<ColumnChunk> &chunks, size_t row) {
  if (chunks.size() != m_tail.size()) return true;

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].valid() && !chunks[i].append_from(m_tail[i], row)) return true;
  }
  return false;
}

int VectorizedRecordIterator::Read() {
  if (m_tail_count) {
    if (RestoreRow(m_tail, m_tail_start, m_tail_nulls[m_tail_start])) return BatchError();

    ++m_tail_start;
    --m_tail_count;
    m_replayed_row = true;
    return 0;
  }

  if (m_batch_source) {
    if (m_scalar_chunks.empty()) {
      size_t count{0};
      for (const auto &entry : m_tables.tables()) count += entry.table->s->fields;

      if (!TryReserve(m_scalar_chunks, count)) return BatchError();

      for (const auto &entry : m_tables.tables())
        for (uint i = 0; i < entry.table->s->fields; ++i) {
          Field *field = bitmap_is_set(entry.table->read_set, i) ? entry.table->field[i] : nullptr;
          m_scalar_chunks.emplace_back(field, 1, &m_memory);
          if (m_scalar_chunks.back().failed()) return BatchError();
        }
    }

    for (auto &chunk : m_scalar_chunks) chunk.clear();

    size_t rows{0};
    const int result = ReadBatch(m_scalar_chunks, 1, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    if (rows == 0) return -1;
    if (RestoreRow(m_scalar_chunks, 0, m_batch_nulls[0])) return BatchError();
    return 0;
  }

  if (RestoreResume()) return BatchError();

  return ReadLive();
}

int VectorizedRecordIterator::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  if (!SupportsBatchRead()) return BatchError();
  auto report_batches = create_scope_guard([&] {
    if (rows != 0 && strcmp(m_operator_name, "ref") == 0)
      RapidMonitor::rapid_counters.query_vectorized_ref_batch_rows_total.fetch_add(rows, std::memory_order_relaxed);
  });

  if (!capacity) return 0;
  capacity = std::min(capacity, m_batch_capacity);
  m_batch_nulls.clear();
  if (m_tail_count) {
    while (rows < capacity && m_tail_count) {
      if (AppendTail(chunks, m_tail_start)) return BatchError();
      m_batch_nulls.push_back(m_tail_nulls[m_tail_start]);
      ++m_tail_start;
      --m_tail_count;
      ++rows;
    }
    m_replayed_row = true;
    ++m_stats.batches;
    return 0;
  }

  if (m_batch_source) {
    const int result = m_batch_source->ReadBatch(chunks, capacity, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) {
      if (!thd()->is_error() && !thd()->killed) return BatchError();
      return result;
    }

    for (size_t row = 0; row < rows; ++row) {
      uint64_t flags = 0;
      size_t index = 0;
      for (const auto &entry : m_tables.tables()) {
        if (m_batch_source->BatchNullRow(entry.table, row)) flags |= uint64_t{1} << index;
        ++index;
      }
      m_batch_nulls.push_back(flags);
    }
    m_stats.rows_in += rows;
    m_stats.rows_out += rows;
    if (rows) ++m_stats.batches;
    if (result == HA_ERR_END_OF_FILE) m_cardinality.Finish(thd(), m_operator_name, m_stats.rows_out);
    m_replayed_row = true;
    return result;
  }

  if (RestoreResume()) return BatchError();

  while (rows < capacity) {
    const int result = ReadLive();
    if (result != 0) {
      if (result == -1 && rows != 0) {
        ++m_stats.batches;
        m_replayed_row = true;
      }
      return result == -1 ? HA_ERR_END_OF_FILE : result;
    }
    if (AppendRow(chunks)) return BatchError();
    m_batch_nulls.push_back(NullTables());
    ++rows;
  }

  if (SaveResume()) return BatchError();
  ++m_stats.batches;
  m_replayed_row = true;
  return 0;
}

bool VectorizedRecordIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                 size_t total) {
  return PushbackBatchTailImpl(chunks, from, total);
}

bool VectorizedRecordIterator::PushbackBatchTailImpl(const std::pmr::vector<ColumnChunk> &chunks, size_t from,
                                                     size_t total) {
  if (from >= total) return false;
  if (total > m_batch_nulls.size()) return BatchError();

  std::pmr::vector<uint64_t> flags(&m_memory);
  if (!TryReserve(flags, total - from + m_tail_count)) return BatchError();
  flags.insert(flags.end(), m_batch_nulls.begin() + from, m_batch_nulls.begin() + total);
  flags.insert(flags.end(), m_tail_nulls.begin() + m_tail_start, m_tail_nulls.begin() + m_tail_start + m_tail_count);
  if (PushbackBatchTailShared(chunks, from, total, &m_tail, &m_tail_start, &m_tail_count, &m_stats.bytes_copied,
                              &m_memory))
    return BatchError();

  m_tail_nulls = std::move(flags);
  m_cardinality.Invalidate();
  return false;
}
namespace detail {
VectorizedLookup::VectorizedLookup(THD *thd, unique_ptr_destroy_only<RowIterator> native, TABLE *table,
                                   Index_lookup *ref, bool reverse, bool unique, bool or_null, ha_rows *examined)
    : RowIterator(thd),
      m_native(std::move(native)),
      m_table(table),
      m_ref(ref),
      m_handler(dynamic_cast<ha_rapid *>(table->file)),
      m_reverse(reverse),
      m_unique(unique),
      m_or_null(or_null),
      m_examined(examined),
      m_reservation(
          ResMgmt::reserve_query_memory(thd, ResMgmt::QueryMemoryOperator::kBatchTransport, unique ? 128 * 1024 : 0)),
      m_memory(m_reservation.bytes()),
      m_key(&m_memory),
      m_cached(&m_memory) {}

bool VectorizedLookup::Init() {
  m_first = true;
  m_done = false;
  m_null_phase = false;

  if (m_or_null) *m_ref->null_ref_key = false;
  if (!m_table->file->inited) m_cache_valid = false;
  if (m_native->Init()) return true;
  return false;
}

bool VectorizedLookup::SupportsBatchRead() const {
  return m_handler && !m_table->file->pushed_cond && !m_table->file->pushed_idx_cond && !m_ref->keypart_hash &&
         !(m_table->key_info[m_ref->key].flags & HA_MULTI_VALUED_KEY);
}

bool VectorizedLookup::PreservesBatchNullRows() const { return true; }

int VectorizedLookup::Read() {
  DBUG_EXECUTE_IF("rapid_require_batch_lookup", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid lookup unexpectedly used scalar Read");
    return 1;
  });
  return m_native->Read();
}

void VectorizedLookup::SetNullRowFlag(bool value) { m_native->SetNullRowFlag(value); }

void VectorizedLookup::UnlockRow() {
  if (!m_unique) m_table->file->unlock_row();
}

void VectorizedLookup::EndPSIBatchModeIfStarted() { m_native->EndPSIBatchModeIfStarted(); }

std::shared_ptr<Compress::Dictionary> VectorizedLookup::Dictionary(const Field *field) const {
  return RecordDictionary(field);
}

bool VectorizedLookup::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) {
  return BatchError();  // The owning record adapter handles lookahead.
}

int VectorizedLookup::ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) {
  rows = 0;
  if (!capacity) return 0;
  if (m_done) return HA_ERR_END_OF_FILE;
  if (thd()->killed) {
    thd()->send_kill_message();
    return 1;
  }
  if (m_first) {
    m_first = false;
    const bool impossible = m_ref->impossible_null_ref();
    m_ref->key_err = impossible || construct_lookup(thd(), m_table, m_ref);
    if (thd()->is_error()) return 1;
    if (m_ref->key_err) {
      m_cache_valid = false;
      if (!m_or_null) {
        m_done = true;
        m_table->set_no_row();
        return HA_ERR_END_OF_FILE;
      }
      m_null_phase = true;
      *m_ref->null_ref_key = true;
    }
    // construct_lookup() can itself produce the NULL range for a nullable
    // outer key. Do not search that same range a second time at equality EOF.
    if (m_or_null) m_null_phase = *m_ref->null_ref_key;
    if (m_unique && m_cache_valid && !m_ref->disable_cache && m_key.size() == m_ref->key_length &&
        std::memcmp(m_key.data(), m_ref->key_buff, m_key.size()) == 0) {
      if (chunks.size() != m_cached.size()) return BatchError();
      for (size_t col = 0; col < chunks.size(); ++col)
        if (chunks[col].valid() && !chunks[col].append_from(m_cached[col], 0)) return BatchError();
      m_done = true;
      rows = 1;
      if (m_examined) ++*m_examined;
      return 0;
    }
    m_cache_valid = false;
    const int error = Start();
    if (error) return Error(error);
  }
  for (;;) {
    const int result = m_handler->index_next_batch(m_unique ? 1 : capacity, chunks, rows, m_reverse);
    if (result != 0 && result != HA_ERR_END_OF_FILE && result != HA_ERR_KEY_NOT_FOUND) return Error(result);
    if (rows) {
      if (m_examined) *m_examined += rows;
      if (m_unique) {
        m_done = true;
        if (!SaveCache(chunks)) {
          // The one-key cache is optional. Keep the already-produced row and
          // use direct columnar probes when its allocation is refused.
          m_cache_valid = false;
          m_cache_disabled = true;
          decltype(m_key)(&m_memory).swap(m_key);
          decltype(m_cached)(&m_memory).swap(m_cached);
        }
      }
      return 0;
    }
    if (m_or_null && !m_null_phase) {
      m_null_phase = true;
      *m_ref->null_ref_key = true;
      const int error = Start();
      if (error) return Error(error);
      continue;
    }
    m_done = true;
    m_table->set_no_row();
    return HA_ERR_END_OF_FILE;
  }
}

int VectorizedLookup::Start() {
  m_handler->set_scan_limit(HA_POS_ERROR, 0);
  return m_handler->index_lookup_batch_start(m_ref->key_buff, m_ref->key_length, m_reverse);
}

int VectorizedLookup::Error(int error) {
  m_table->file->print_error(error, MYF(0));
  return 1;
}

bool VectorizedLookup::SaveCache(const std::pmr::vector<ColumnChunk> &chunks) {
  if (m_ref->disable_cache || m_cache_disabled) return true;
  DBUG_EXECUTE_IF("rapid_batch_lookup_cache_bad_alloc", { return false; });
  if (!TryReserve(m_key, m_ref->key_length)) return false;
  m_key.assign(m_ref->key_buff, m_ref->key_buff + m_ref->key_length);
  if (m_cached.empty()) {
    if (!TryReserve(m_cached, chunks.size())) return false;
    for (const auto &chunk : chunks) {
      m_cached.emplace_back(chunk.valid() ? chunk.source_field() : nullptr, 1, &m_memory);
      if (m_cached.back().failed()) return false;
    }
  }
  if (m_cached.size() != chunks.size()) return false;
  for (size_t col = 0; col < chunks.size(); ++col) {
    m_cached[col].clear();
    if (chunks[col].valid() && !m_cached[col].append_from(chunks[col], 0)) return false;
  }
  m_cache_valid = true;
  return true;
}
}  // namespace detail

unique_ptr_destroy_only<RowIterator> CreateVectorizedLookup(THD *thd, unique_ptr_destroy_only<RowIterator> native,
                                                            AccessPath *path, ha_rows *examined) {
  TABLE *table = nullptr;
  Index_lookup *ref = nullptr;
  bool reverse = false;
  if (path->type == AccessPath::REF) {
    table = path->ref().table;
    ref = path->ref().ref;
    reverse = path->ref().reverse;
  } else if (path->type == AccessPath::EQ_REF) {
    table = path->eq_ref().table;
    ref = path->eq_ref().ref;
  } else {
    table = path->ref_or_null().table;
    ref = path->ref_or_null().ref;
  }
  return NewIterator<detail::VectorizedLookup>(thd, thd->mem_root, std::move(native), table, ref, reverse,
                                               path->type == AccessPath::EQ_REF, path->type == AccessPath::REF_OR_NULL,
                                               examined);
}
}  // namespace Executor
}  // namespace ShannonBase
