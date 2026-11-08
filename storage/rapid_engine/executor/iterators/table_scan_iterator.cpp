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

   The fundmental code for imcs.  It's based on mysql executor iterators.
*/
/** The table scan iterator class for IMCS. All specific iterators are all inherited
 * from this.
 * vectorized table scan iterator impl for rapid engine.
 *
 * Vectorized, not parallelized: the whole executor is single-threaded, and
 * ColumnChunk is deliberately single-consumer (see iterator.h).
 */

#include "storage/rapid_engine/executor/iterators/table_scan_iterator.h"

#include <algorithm>

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/debug_sync.h"
#include "sql/sql_class.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/varlen0data.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"

namespace ShannonBase {
namespace Executor {
namespace {
// Latency band of the batch-size controller: growing below kFastBatchMs keeps the
// doubled batch under kSlowBatchMs, so the controller cannot oscillate.
constexpr double kSlowBatchMs = 50.0;
constexpr double kFastBatchMs = 10.0;
constexpr size_t kAdaptEvery = 8;  // batches per decision
constexpr double kEwmaAlpha = 0.3;
constexpr double kRowsCap = 10000.0;

// A batch of whole kernel blocks leaves no ragged tail block per fetch.
inline size_t AlignBatchRows(size_t rows) {
  if (rows >= SHANNON_BATCH_NUM) return rows / SHANNON_BATCH_NUM * SHANNON_BATCH_NUM;
  return (rows + 3) & ~size_t(3);
}
}  // namespace

VectorizedTableScanIterator::VectorizedTableScanIterator(THD *thd, TABLE *mtable, double expected_rows,
                                                         ha_rows *examined_rows,
                                                         std::unique_ptr<Imcs::Predicate> predicate,
                                                         const std::vector<uint32_t> &projection, ha_rows limit,
                                                         ha_rows offset, bool use_storage_index, int index_no,
                                                         bool reverse)
    : TableRowIterator(thd, mtable),
      m_pushed_predicate{std::move(predicate)},
      m_projected_columns(projection),
      m_limit{limit},
      m_offset{offset},
      m_use_storage_index{use_storage_index},
      m_index_no{index_no},
      m_reverse{reverse},
      m_examined_rows{examined_rows},
      m_batch_size{0},
      m_opt_batch_size{0},
      m_curr_batch_size{0},
      m_curr_row_in_batch{0},
      m_batch_exhausted{true},
      m_eof_reached{false},
      m_fields_cached{false},
      m_cardinality(expected_rows) {
  m_opt_batch_size = CalculateOptimalBatchSize(expected_rows);
  // Chunks are allocated once, for this many rows: a larger request does not grow
  // them, it loses rows (see ColumnChunkRecv::on_row).
  m_chunk_capacity = std::max(m_opt_batch_size, static_cast<size_t>(ShannonBase::SHANNON_ROWS_IN_CHUNK));
  m_max_batch_size = std::min(m_chunk_capacity, m_opt_batch_size * 2);
  m_min_batch_size = std::min(m_opt_batch_size, std::max<size_t>(SHANNON_BATCH_NUM, SHANNON_VECTOR_WIDTH));
  ResetBatchRamp();

  m_metrics.reset();
}

VectorizedTableScanIterator::~VectorizedTableScanIterator() {
  if (table()->file != nullptr) table()->file->ha_index_or_rnd_end();
}

size_t VectorizedTableScanIterator::CalculateOptimalBatchSize(double expected_rows) {
  const size_t base_size = std::max<size_t>(SHANNON_VECTOR_WIDTH, 128);
  size_t l3_cache_size{8 * SHANNON_MB};
#if defined(CACHE_L3_BYTES)
  l3_cache_size = CACHE_L3_BYTES;
#endif

  size_t row_size = EstimateRowSize();
  size_t cache_optimal = l3_cache_size / (row_size * 4);

  // An unknown estimate used to skip the cap and pick up to 131072 rows, while a
  // known one never exceeded 10000: treat "unknown" as "large".
  const size_t rows_based =
      static_cast<size_t>(expected_rows > 0 ? std::min(expected_rows / 10.0, kRowsCap) : kRowsCap);

  size_t candidate = std::min(cache_optimal, rows_based);
  candidate = std::clamp(candidate, base_size, static_cast<size_t>(131072));
  return AlignBatchRows(candidate);
}

size_t VectorizedTableScanIterator::EstimateRowSize() const {
  // Size the batch by the chunk cell width (4 bytes for a dictionary-coded string),
  // not pack_length(), which overstates a VARCHAR(255) utf8mb4 column ~250x.
  size_t total_size = 0;
  for (uint idx = 0; idx < table()->s->fields; idx++) {
    Field *field = *(table()->field + idx);
    const bool explicitly_projected =
        std::find(m_projected_columns.begin(), m_projected_columns.end(), idx) != m_projected_columns.end();
    if ((bitmap_is_set(table()->read_set, idx) || explicitly_projected) && !field->is_flag_set(NOT_SECONDARY_FLAG))
      total_size += std::max<size_t>(1, static_cast<size_t>(Utils::Util::normalized_length(field)));
  }
  return total_size > 0 ? total_size : 64;
}

bool VectorizedTableScanIterator::Init() {
  m_cardinality.Reset();
  DBUG_EXECUTE_IF("rapid_iterator_bad_alloc", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid iterator allocation failure");
    return true;
  });
  // An ordered scan reads off the ART index instead of in rowid order, and is
  // positioned at whichever end the direction calls for. Everything below is
  // the same either way: the batch it produces is the same shape.
  if (m_index_no >= 0) {
    if (table()->file->ha_index_init(m_index_no, /*sorted=*/true)) return true;
  } else if (table()->file->ha_rnd_init(true)) {
    return true;
  }

  if (m_pushed_predicate) down_cast<ha_rapid *>(table()->file)->set_predicate(std::move(m_pushed_predicate));
  down_cast<ha_rapid *>(table()->file)->set_projection(m_projected_columns);
  down_cast<ha_rapid *>(table()->file)->set_scan_limit(m_limit, m_offset);
  down_cast<ha_rapid *>(table()->file)->set_storage_index(m_use_storage_index);

  // Allocate row buffer for batch processing. to store data in mysql format in column format.
  CacheActiveFields();

  if (PreallocateColumnChunks()) return true;

  m_curr_batch_size = 0;
  m_curr_row_in_batch = 0;
  m_batch_exhausted = true;
  m_eof_reached = false;
  ResetBatchRamp();
  m_metrics.reset();
  m_operator_stats = VectorizedOperatorStats{};

  m_lookahead_count = 0;
  m_lookahead_start = 0;
  for (auto &c : m_lookahead_chunks) c.clear();

  m_imcu_snapshot.clear();
  m_imcu_snapshot_valid = false;

  m_share = ShannonBase::shannon_loaded_tables->get(table()->s->db.str, table()->s->table_name.str);
  if (!m_share) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid table metadata disappeared before scan initialization");
    return true;
  }
  auto table_id = m_share->m_tableid;
  m_rpd_table = Imcs::Imcs::instance()->get_rpd_table(table_id);
  if (!m_rpd_table) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid in-memory table disappeared before scan initialization");
    return true;
  }
  return false;
}

void VectorizedTableScanIterator::CacheActiveFields() {
  if (m_fields_cached) return;

  m_active_fields.clear();
  m_field_indices.clear();

  for (uint ind = 0; ind < table()->s->fields; ind++) {
    Field *field = *(table()->field + ind);
    const bool explicitly_projected =
        std::find(m_projected_columns.begin(), m_projected_columns.end(), ind) != m_projected_columns.end();
    if ((bitmap_is_set(table()->read_set, ind) || explicitly_projected) && !field->is_flag_set(NOT_SECONDARY_FLAG)) {
      m_active_fields.push_back(field);
      m_field_indices.push_back(ind);
    }
  }

  m_fields_cached = true;
}

Imcs::Imcu *VectorizedTableScanIterator::LocateImcuForRow(row_id_t global_row_id) {
  const auto find_in_snapshot = [&]() -> Imcs::Imcu * {
    // Table IMCUs are ordered by their non-overlapping global row ranges.
    // Find the last start <= row id instead of scanning every IMCU for every
    // off-page value materialized by the row iterator.
    auto it = std::upper_bound(
        m_imcu_snapshot.begin(), m_imcu_snapshot.end(), global_row_id,
        [](row_id_t row, const std::shared_ptr<Imcs::Imcu> &imcu) { return imcu && row < imcu->get_start_row(); });
    while (it != m_imcu_snapshot.begin()) {
      --it;
      if (!*it) continue;
      const auto start = (*it)->get_start_row();
      const auto cap = (*it)->get_capacity();
      return global_row_id >= start && global_row_id - start < cap ? it->get() : nullptr;
    }
    return nullptr;
  };

  if (m_imcu_snapshot_valid) {
    if (Imcs::Imcu *found = find_in_snapshot()) return found;
  }

  // Miss (or first call): refresh once. Concurrent population can append IMCUs
  // after Init(), so a stale snapshot must not be treated as authoritative.
  if (m_rpd_table == nullptr) return nullptr;
  m_imcu_snapshot = m_rpd_table->get_imcus();
  m_imcu_snapshot_valid = true;
  return find_in_snapshot();
}

bool VectorizedTableScanIterator::PreallocateColumnChunks() {
  if (m_col_chunks.size() != static_cast<size_t>(table()->s->fields)) {
    m_col_chunks.assign(table()->s->fields, ShannonBase::Executor::ColumnChunk(nullptr, 0));
  }

  // Sized from the optimum, not from the (ramping) current batch size.
  const auto initial_capacity = m_chunk_capacity;

  uint valid_fields = 0;
  for (uint ind = 0; ind < table()->s->fields; ind++) {
    Field *field = table()->field[ind];
    const bool explicitly_projected =
        std::find(m_projected_columns.begin(), m_projected_columns.end(), ind) != m_projected_columns.end();
    if ((bitmap_is_set(table()->read_set, ind) || explicitly_projected) && !field->is_flag_set(NOT_SECONDARY_FLAG)) {
      m_col_chunks[ind] = ShannonBase::Executor::ColumnChunk(field, initial_capacity);
      // A refused buffer cannot be told apart from an unprojected field later.
      if (m_col_chunks[ind].failed()) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid table scan auxiliary memory exhausted");
        return true;
      }
      valid_fields++;
    }
  }

  if (valid_fields == 0 && !m_active_fields.empty()) {
    for (const auto &field : m_active_fields) {
      auto idx = field->field_index();
      if (idx < m_col_chunks.size()) {
        m_col_chunks[idx] = ShannonBase::Executor::ColumnChunk(field, initial_capacity);
        if (m_col_chunks[idx].failed()) {
          my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid table scan auxiliary memory exhausted");
          return true;
        }
      }
    }
  }
  return false;
}

double VectorizedTableScanIterator::UpdatePerformanceMetrics(
    std::chrono::high_resolution_clock::time_point start_time) {
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  double read_time = duration.count() / 1000.0;  // in ms.

  m_metrics.avg_batch_time =
      (m_metrics.avg_batch_time * (m_metrics.total_batches - 1) + read_time) / m_metrics.total_batches;
  m_metrics.total_read_time += read_time;
  m_metrics.last_batch_time = end_time;
  return read_time;
}

void VectorizedTableScanIterator::ResetBatchRamp() {
  m_in_ramp = m_min_batch_size < m_opt_batch_size;
  m_batch_size = m_in_ramp ? m_min_batch_size : m_opt_batch_size;
  m_batch_ms_ewma = 0.0;
  m_adapt_samples = 0;
}

void VectorizedTableScanIterator::AdaptBatchSize(double batch_ms) {
  // Slow start: double until the optimum, so a parent that stops early never pays
  // for a full batch up front.
  if (m_in_ramp) {
    m_batch_size = std::min(AlignBatchRows(m_batch_size * 2), m_opt_batch_size);
    if (m_batch_size >= m_opt_batch_size) m_in_ramp = false;
    return;
  }

  // Windowed latency: a cumulative mean could not react once a few hundred batches
  // had accumulated, and averaged in batches of the previous size after a resize.
  m_batch_ms_ewma = m_adapt_samples == 0 ? batch_ms : (1.0 - kEwmaAlpha) * m_batch_ms_ewma + kEwmaAlpha * batch_ms;
  if (++m_adapt_samples < kAdaptEvery) return;
  m_adapt_samples = 0;

  size_t next = m_batch_size;
  if (m_batch_ms_ewma > kSlowBatchMs) {
    next = std::max(AlignBatchRows(m_batch_size / 2), m_min_batch_size);  // bounds kill latency
  } else if (m_batch_ms_ewma < kFastBatchMs) {
    next = std::min(AlignBatchRows(m_batch_size * 2), m_max_batch_size);
  }
  if (next != m_batch_size) {
    m_batch_size = next;
    m_batch_ms_ewma = 0.0;  // the next window must describe the new size
  }
}

bool VectorizedTableScanIterator::ProcessStringField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk,
                                                     size_t rowid) {
  // ENUM / SET are stored as raw ordinals / bitmasks (never dictionary-encoded).
  if (field->real_type() == MYSQL_TYPE_ENUM || field->real_type() == MYSQL_TYPE_SET) {
    field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), field->pack_length());
    return true;
  }

  auto fld_idx = field->field_index();
  if (Utils::IsOffPageField(field)) {
    Imcs::VarlenDataPool::VarlenReference ref{};
    std::memcpy(&ref, col_chunk.data(rowid), std::min(sizeof(ref), col_chunk.width()));

    if (ref.length == 0) {
      // Valid empty value — store as empty, not as NULL/default.
      Utils::Util::store_blob_data(field, "", 0);
      return true;
    }

    if (ref.is_inline()) {
      if (col_chunk.width() < sizeof(ref)) {
        field->reset();  // Slot too small to even hold the reference.
      } else {
        const size_t available = col_chunk.width() - sizeof(ref);
        if (ref.length > available) {
          field->reset();  // Corrupt/truncated inline length — do not over-read.
        } else {
          const uchar *inline_data = col_chunk.data(rowid) + sizeof(ref);
          Utils::Util::store_blob_data(field, reinterpret_cast<const char *>(inline_data), ref.length);
        }
      }
      return true;
    }

    if (rowid >= m_batch_row_ids.size()) {
      field->reset();  // Missing row-to-IMCU mapping — error.
      return true;
    }
    row_id_t global_row_id = m_batch_row_ids[rowid];

    Imcs::Imcu *imcu = LocateImcuForRow(global_row_id);
    auto *cu = imcu ? imcu->get_cu(fld_idx) : nullptr;
    if (!cu) {
      field->reset();
      return true;
    }

    // Resolve the reference carried by the batch itself.  Using
    // resolve_data(local_row_id) would jump to the current physical slot and
    // return the post-UPDATE value even when the batch contains an older
    // ReadView-visible VarlenReference.
    auto data_guard = cu->resolve_data(ref);
    const uchar *data = data_guard.get();
    if (data && ref.length > 0 && ref.length != UNIV_SQL_NULL) {
      Utils::Util::store_blob_data(field, reinterpret_cast<const char *>(data), ref.length);
    } else {
      field->reset();
    }
    return true;
  }

  // Dictionary-encoded VARCHAR path.
  auto dict = m_rpd_table->meta().fields[fld_idx].dictionary;
  if (dict) {
    auto *data_ptr = reinterpret_cast<const char *>(col_chunk.data(rowid));
    auto str_id = *reinterpret_cast<uint32 *>(const_cast<char *>(data_ptr));
    m_str_buf.resize(field->field_length + 1);
    const auto len = dict->get(str_id, m_str_buf.data(), m_str_buf.size());
    // A failed decode is not an empty value. Storing "" here handed the query a
    // wrong answer with nothing to show for it.
    if (!len.has_value()) return false;
    field->store(m_str_buf.data(), *len, field->charset());
    return true;
  }

  // Non-dictionary VARCHAR fallback — raw inline data.
  field->store(reinterpret_cast<const char *>(col_chunk.data(rowid)), col_chunk.width(), field->charset());
  return true;
}

int VectorizedTableScanIterator::PopulateCurrentRow() {
  const size_t rowid = m_curr_row_in_batch;

  // Cache local references to avoid repeated member access.
  auto &chunks = m_col_chunks;
  auto &fields = m_active_fields;
  auto &indices = m_field_indices;
  const size_t n_fields = fields.size();

  for (size_t i = 0; i < n_fields; ++i) {
    Field *field = fields[i];
    const uint field_idx = indices[i];
    ut_a(field->is_flag_set(NOT_SECONDARY_FLAG) == false);

    // Use nullable_fast() — cached raw pointer + bitwise ops, no bounds check.
    if (chunks[field_idx].nullable_fast(rowid)) {
      field->set_null();
    } else {
      field->set_notnull();
      if (!ProcessFieldData(field, chunks[field_idx], rowid)) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid could not decode a column value");
        return HA_ERR_GENERIC;
      }
      // Count the bytes crossing the column-batch/MySQL-row adapter. For
      // dictionary/varlen values this is the encoded batch width, not decoded
      // payload bytes; decoded payload accounting belongs to the storage layer.
      m_operator_stats.bytes_copied += chunks[field_idx].width();
    }
  }

  ++m_operator_stats.row_materializations;

  return ShannonBase::SHANNON_SUCCESS;
}

int VectorizedTableScanIterator::Read() {
  int result{ShannonBase::SHANNON_SUCCESS};

  if (m_batch_exhausted && !m_eof_reached) {
    result = ReadNextBatch();
    // EOF never carries rows: every cursor entry point zeroes its counter before
    // it can return HA_ERR_END_OF_FILE.
    if (result) {
      if (result == HA_ERR_END_OF_FILE) m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
      return HandleError(result);
    }
  }

  if (m_curr_row_in_batch >= m_curr_batch_size) {
    if (m_eof_reached) {
      m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
      return HandleError(HA_ERR_END_OF_FILE);
    } else {  // read the next batch.
      m_batch_exhausted = true;
      return Read();
    }
  }

  // fill up the data to table->field
  Utils::ColumnMapGuard guard(table(), Utils::ColumnMapGuard::TYPE::WRITE);
  result = PopulateCurrentRow();
  if (result) return HandleError(result);

  // Update cursor position for filesort / ORDER BY so rnd_pos() later
  // can re-read the correct row.
  if (m_curr_row_in_batch < m_batch_row_ids.size()) {
    down_cast<ha_rapid *>(table()->file)->set_last_returned_rowid(m_batch_row_ids[m_curr_row_in_batch]);
  }

  // move to the next row.
  m_curr_row_in_batch++;
  m_metrics.total_rows++;
  ++m_operator_stats.rows_in;
  ++m_operator_stats.rows_out;
  if (m_curr_row_in_batch >= m_curr_batch_size) {
    m_batch_exhausted = true;
    if (!m_eof_reached) m_curr_row_in_batch = 0;
  }

  return result;
}

int VectorizedTableScanIterator::ReadNextBatch() {
  // A row deleted under the scan makes the batch read come back empty-handed;
  // retry a bounded number of times. The budget is per call and only
  // HA_ERR_RECORD_DELETED spends it: it used to accumulate every error over the
  // whole scan, so a long query died on its eleventh transient hiccup, and
  // every non-retryable error was reported as HA_ERR_GENERIC once ten had gone
  // by. Retrying in a loop also keeps the stack flat.
  constexpr size_t kMaxRetries = 10;
  auto *file = down_cast<ha_rapid *>(table()->file);

  for (size_t attempt = 0;; ++attempt) {
    auto batch_start = std::chrono::high_resolution_clock::now();

    ClearBatchData();

    size_t read_cnt = 0;
    const int result = m_index_no >= 0 ? file->index_next_batch(m_batch_size, m_col_chunks, read_cnt, m_reverse)
                                       : file->rnd_next_batch(m_batch_size, m_col_chunks, read_cnt);
    if (result != 0) {
      if (result == HA_ERR_END_OF_FILE) {
        m_eof_reached = true;
        m_curr_batch_size = 0;
        m_curr_row_in_batch = 0;
        return HA_ERR_END_OF_FILE;
      }

      m_metrics.error_count++;
      if (result == HA_ERR_RECORD_DELETED && !thd()->killed && attempt < kMaxRetries) continue;
      return result;
    }

    if (read_cnt == 0) {  // no data read, therefore set to EOF.
      m_eof_reached = true;
      // Leave no stale batch behind: Read() would re-serve its first row.
      m_curr_batch_size = 0;
      m_curr_row_in_batch = 0;
      return HA_ERR_END_OF_FILE;
    }

    m_batch_row_ids = file->last_batch_row_ids();
    m_curr_batch_size = read_cnt;
    m_curr_row_in_batch = 0;
    m_batch_exhausted = false;
    m_metrics.total_batches++;
    if (m_examined_rows != nullptr) *m_examined_rows += read_cnt;

    AdaptBatchSize(UpdatePerformanceMetrics(batch_start));

    return ShannonBase::SHANNON_SUCCESS;
  }
}

std::shared_ptr<Compress::Dictionary> VectorizedTableScanIterator::Dictionary(const Field *field) const {
  if (field == nullptr || field->table != table() || m_rpd_table == nullptr || !Utils::Util::is_string(field->type()) ||
      Utils::IsOffPageField(field))
    return nullptr;
  const uint index = field->field_index();
  const auto &fields = m_rpd_table->meta().fields;
  if (index >= fields.size() || fields[index].normalized_length != sizeof(uint32_t)) return nullptr;
  return fields[index].dictionary;
}

int VectorizedTableScanIterator::ReadBatch(std::pmr::vector<ColumnChunk> &col_chunks, size_t capacity,
                                           size_t &rows_read) {
  rows_read = 0;

  if (m_lookahead_count == 0 && !m_batch_exhausted && m_curr_row_in_batch < m_curr_batch_size) {
    // Push rows [m_curr_row_in_batch, m_curr_batch_size) into lookahead.
    if (PushbackBatchTail(m_col_chunks, m_curr_row_in_batch, m_curr_batch_size)) return HA_ERR_GENERIC;
    // Mark the internal batch as consumed so Read() won't re-serve them.
    m_curr_row_in_batch = m_curr_batch_size;
    m_batch_exhausted = true;
  }

  // 1. Drain lookahead buffer first.
  if (m_lookahead_count > 0) {
    size_t to_copy = std::min(m_lookahead_count, capacity);
    if (col_chunks.size() != m_lookahead_chunks.size()) return HA_ERR_GENERIC;
    for (size_t ci = 0; ci < col_chunks.size(); ++ci) {
      ColumnChunk &target = col_chunks[ci];
      if (!target.valid()) continue;
      const ColumnChunk &source = m_lookahead_chunks[ci];
      if (!source.valid() || source.table() != target.table() || source.field_index() != target.field_index() ||
          source.size() < m_lookahead_start + to_copy)
        return HA_ERR_GENERIC;

      for (size_t r = 0; r < to_copy; ++r) {
        if (!target.append_from(source, m_lookahead_start + r)) return HA_ERR_GENERIC;
      }
    }
    m_lookahead_start += to_copy;
    m_lookahead_count -= to_copy;
    if (m_lookahead_count == 0) {
      m_lookahead_start = 0;
      // Release lookahead memory when fully consumed.
      for (auto &c : m_lookahead_chunks) c.clear();
    }
    rows_read = to_copy;
    m_operator_stats.rows_in += rows_read;
    m_operator_stats.rows_out += rows_read;
    ++m_operator_stats.batches;
    return 0;
  }

  if (m_eof_reached) {
    m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
    return HA_ERR_END_OF_FILE;
  }

  // RapidCursor addresses chunks directly by Field::field_index().
  if (col_chunks.size() != table()->s->fields) return HA_ERR_GENERIC;
  const auto field_required = [this](uint field_idx) {
    Field *field = table()->field[field_idx];
    return !field->is_flag_set(NOT_SECONDARY_FLAG) &&
           (bitmap_is_set(table()->read_set, field_idx) ||
            std::find(m_projected_columns.begin(), m_projected_columns.end(), field_idx) != m_projected_columns.end());
  };

  for (uint field_idx = 0; field_idx < table()->s->fields; ++field_idx) {
    if (!field_required(field_idx)) continue;
    if (!col_chunks[field_idx].valid() || col_chunks[field_idx].table() != table() ||
        col_chunks[field_idx].field_index() != field_idx)
      return HA_ERR_GENERIC;
  }

  auto *file = down_cast<ha_rapid *>(table()->file);
  int result = m_index_no >= 0 ? file->index_next_batch(capacity, col_chunks, rows_read, m_reverse)
                               : file->rnd_next_batch(capacity, col_chunks, rows_read);

  if (result == HA_ERR_END_OF_FILE) {
    m_eof_reached = true;
    // rows_read may be > 0 for a final partial batch — that is valid.
    if (rows_read > 0) {
      m_operator_stats.rows_in += rows_read;
      m_operator_stats.rows_out += rows_read;
      ++m_operator_stats.batches;
      if (m_examined_rows != nullptr) *m_examined_rows += rows_read;
    }
    m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
    return HA_ERR_END_OF_FILE;
  }

  if (result != 0) return result;

  if (rows_read == 0) {
    m_eof_reached = true;
    m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
    return HA_ERR_END_OF_FILE;
  }

  m_metrics.total_rows += rows_read;
  m_metrics.total_batches++;
  m_operator_stats.rows_in += rows_read;
  m_operator_stats.rows_out += rows_read;
  ++m_operator_stats.batches;
  if (m_examined_rows != nullptr) *m_examined_rows += rows_read;
  return 0;
}

bool VectorizedTableScanIterator::SupportsLateMaterialization() const {
  // Buffered rows already carry a full projection. ART/partition cursors and
  // pushed LIMIT/OFFSET retain their existing cursor contracts.
  return m_index_no < 0 && table()->part_info == nullptr && SupportsRuntimeFilter() && m_lookahead_count == 0 &&
         (m_batch_exhausted || m_curr_row_in_batch >= m_curr_batch_size);
}

std::unique_ptr<Morsel::ParallelSource> VectorizedTableScanIterator::CreateParallelSource(
    ResMgmt::BoundedMemoryResource *memory, const std::pmr::vector<Field *> &fields) {
  // An index scan serves rows in its own order, so it has to hand the scan
  // direction over: only a forward candidate walk can be split into runs.
  if (m_eof_reached) return {};
  if (m_index_no < 0 && (m_operator_stats.rows_in || !SupportsLateMaterialization())) return {};
  return down_cast<ha_rapid *>(table()->file)->create_parallel_source(memory, fields, m_reverse);
}

int VectorizedTableScanIterator::ReadPredicateBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity,
                                                    size_t &rows, const std::pmr::vector<Field *> &fields) {
  rows = 0;
  if (!SupportsLateMaterialization() || chunks.size() != table()->s->fields) return HA_ERR_WRONG_COMMAND;
  if (m_eof_reached) return HA_ERR_END_OF_FILE;
  std::vector<uint32_t> projection;
  for (Field *field : fields) {
    if (field->table != table()) continue;  // Bound outer parameters are not scan columns.
    const uint32_t col = field->field_index();
    if (col >= chunks.size() || !chunks[col].valid() || chunks[col].source_field() != field) return HA_ERR_GENERIC;
    projection.push_back(col);
  }
  const int result = down_cast<ha_rapid *>(table()->file)->rnd_next_projected_batch(capacity, chunks, rows, projection);
  if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
  if (result == HA_ERR_END_OF_FILE) m_eof_reached = true;
  m_operator_stats.rows_in += rows;
  m_operator_stats.rows_out += rows;
  ++m_operator_stats.batches;
  if (m_examined_rows != nullptr) *m_examined_rows += rows;
  RAPID_COUNT(query_late_predicate_rows_total, rows);
  if (m_eof_reached) m_cardinality.Finish(thd(), "scan", m_operator_stats.rows_in);
  if (rows) {
    DEBUG_SYNC(thd(), "rapid_late_predicate_batch");
  }
  return result;
}

int VectorizedTableScanIterator::CompleteProjection(std::pmr::vector<ColumnChunk> &chunks,
                                                    const std::pmr::vector<uint32_t> &selection) {
  return down_cast<ha_rapid *>(table()->file)->complete_batch_projection(chunks, selection);
}

// Compact in scan order, before the parent copies columns into probe storage.
// This does not reduce storage reads; it avoids downstream copies/probes.
int VectorizedTableScanIterator::ReadFilteredBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity,
                                                   size_t &rows_read, void *owner, RuntimeFilter filter) {
  if (!SupportsRuntimeFilter() || filter == nullptr || capacity == 0) return HA_ERR_GENERIC;
  for (;;) {
    const int result = ReadBatch(chunks, capacity, rows_read);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return result;
    const size_t input_rows = rows_read;
    size_t kept = 0;
    for (size_t row = 0; row < input_rows; ++row) {
      const int keep = filter(owner, chunks, row);
      if (keep < 0) return HA_ERR_GENERIC;
      if (!keep) continue;
      if (kept != row) {
        for (auto &chunk : chunks) {
          if (!chunk.valid()) continue;
          const bool is_null = chunk.nullable_fast(row);
          auto *mask = chunk.mutable_null_mask_data();
          const uint8_t bit = uint8_t{1} << (kept & 7);
          if (is_null)
            mask[kept >> 3] |= bit;
          else {
            mask[kept >> 3] &= ~bit;
            std::memmove(chunk.mutable_data(kept), chunk.data_fast(row), chunk.width());
          }
        }
      }
      ++kept;
    }
    for (auto &chunk : chunks)
      if (chunk.valid()) chunk.resize(kept);
    m_operator_stats.rows_out -= input_rows - kept;
    rows_read = kept;
    if (kept != 0 || result == HA_ERR_END_OF_FILE || input_rows == 0) return result;
    // A fully rejected batch is not EOF. Continue to the next physical batch.
  }
}

bool VectorizedTableScanIterator::PushbackBatchTail(const std::pmr::vector<ColumnChunk> &chunks, size_t from_row,
                                                    size_t total_rows) {
  assert(from_row <= total_rows);
  if (from_row < total_rows) m_cardinality.Invalidate();
  // Unlike VectorizedFilterIterator/VectorizedHashJoinIterator, this call site doesn't track
  // bytes_copied for pushback (matches pre-existing behavior).
  return PushbackBatchTailShared(chunks, from_row, total_rows, &m_lookahead_chunks, &m_lookahead_start,
                                 &m_lookahead_count,
                                 /*bytes_copied=*/nullptr);
}
}  // namespace Executor
}  // namespace ShannonBase

namespace ShannonBase {
namespace Executor {
namespace Morsel {

ParallelScan::ParallelScan(ResMgmt::BoundedMemoryResource *memory, const Rapid_scan_context *snapshot,
                           const std::vector<std::shared_ptr<Imcs::Imcu>> &imcus,
                           const std::vector<std::unique_ptr<Imcs::Predicate>> &predicates,
                           const std::pmr::vector<Field *> &fields, bool storage_index)
    : m_snapshot(snapshot),
      m_imcus(imcus),
      m_predicates(predicates),
      m_fields(fields, memory),
      m_ordinal_bases(memory),
      m_projection_charge(memory, fields.size() * sizeof(uint32_t)),
      m_queue(memory),
      m_storage_index(storage_index) {
  m_projection.reserve(fields.size());
  for (auto *field : fields) m_projection.push_back(field->field_index());
  m_ordinal_bases.reserve(imcus.size());
  uint64_t ordinal = 0;
  for (size_t i = 0; i < imcus.size(); ++i) {
    m_ordinal_bases.push_back(ordinal);
    if (imcus[i]) {
      const size_t rows = imcus[i]->get_row_count();
      m_queue.Add(i, rows);
      ordinal += rows;
    }
  }
  m_max_ordinal = ordinal ? ordinal - 1 : 0;
  // Storage still uses ordinary vectors. Bound their live payload from the
  // actual projection and predicate shape, including old/new MVCC slot overlap
  // and recursive temporary masks, instead of a fixed 256 KiB guess. This
  // reservation is for storage scratch; all pipeline buffers use PMR directly.
  size_t projected_width = 0, max_width = 0, depth = 0;
  for (auto *field : fields) projected_width += Utils::Util::normalized_length(field);
  for (const auto &imcu : imcus) {
    if (!imcu) continue;
    for (size_t col = 0; col < imcu->get_column_count(); ++col) {
      const auto *cu = imcu->get_cu(col);
      if (cu && cu->field()) max_width = std::max<size_t>(max_width, cu->field()->pack_length());
    }
    break;  // Every IMCU shares the table schema.
  }
  const auto visit = [&](const auto &self, const Imcs::Predicate *pred, size_t level) -> void {
    depth = std::max(depth, level);
    if (pred && pred->is_compound())
      for (const auto &child : static_cast<const Imcs::Compound_Predicate *>(pred)->children)
        self(self, child.get(), level + 1);
  };
  for (const auto &predicate : predicates) visit(visit, predicate.get(), 1);
  constexpr size_t scan_rows = kBatchRows;
  m_storage_scratch_bytes =
      2 * (fields.size() * (sizeof(VisibleCell) + 3 * sizeof(void *)) + projected_width + 4 * max_width +
           scan_rows * sizeof(const uchar *) + (depth + 3) * (sizeof(bit_array_t) + (scan_rows + 7) / 8) +
           snapshot->m_schema_name.size() + snapshot->m_table_name.size() + 256);
  DBUG_EXECUTE_IF("rapid_parallel_scan_budget", { m_test_budget_refusal = true; });
}

bool ParallelScan::ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                               const Consumer &consume) {
  constexpr size_t capacity = kBatchRows;
  ResMgmt::BoundedMemoryResource::Reservation scratch(memory, m_storage_scratch_bytes);
  Rapid_scan_context context;
  context.m_thd = worker;
  context.m_trx = m_snapshot->m_trx;
  context.m_extra_info.m_trxid = m_snapshot->m_extra_info.m_trxid;
  context.m_extra_info.m_scn = m_snapshot->m_extra_info.m_scn;
  context.m_schema_name = m_snapshot->m_schema_name;
  context.m_table_name = m_snapshot->m_table_name;
  context.limit = 0;
  std::pmr::vector<ColumnChunk> chunks(memory);
  std::pmr::vector<uint32_t> selection(memory);
  // Scan compacts visible matches into dense chunks. FilterParallelSource may
  // instead pass an explicit selection over its input chunks.
  std::pmr::vector<uint64_t> ordinals(memory);
  chunks.reserve(m_fields.size());
  ordinals.reserve(capacity);
  for (Field *field : m_fields) {
    chunks.emplace_back(field, capacity, memory);
    if (chunks.back().failed()) return true;
  }
  Task parallel;
  while (!cancelled.load(std::memory_order_acquire) && m_queue.Claim(&parallel)) {
    context.rows_returned = 0;
    auto &imcu = m_imcus[parallel.partition];
    if (m_storage_index && !m_predicates.empty() && imcu->can_skip_imcu(m_predicates)) continue;
    for (size_t offset = 0; offset < parallel.count; offset += capacity) {
      if (cancelled.load(std::memory_order_acquire)) return false;
      for (auto &chunk : chunks) chunk.clear();
      ordinals.clear();
      bool failed = false;
      const size_t count = std::min(capacity, parallel.count - offset);
      // scan() takes a physical range length, so selective filters cannot run
      // into a neighbouring parallel. Its mutation lock encloses all slot copies.
      imcu->scan(&context, parallel.begin + offset, count, m_predicates, m_projection,
                 [&](row_id_t id, const std::vector<const uchar *> &values) {
                   if (failed) return;
                   for (size_t col = 0; col < chunks.size(); ++col)
                     if (!chunks[col].add(values[col], values[col] ? chunks[col].width() : 0, !values[col])) {
                       failed = true;
                       return;
                     }
                   ordinals.push_back(m_ordinal_bases[parallel.partition] + id - imcu->get_start_row());
                 });
      // scan() has returned: the shared mutation lock is no longer held.
      // Hashing, joins, expression evaluation and spill I/O run only here.
      if (failed) return true;
      if (!ordinals.empty() && consume(chunks, ordinals.size(), selection, ordinals)) return true;
    }
  }
  return false;
}

ParallelIndexScan::ParallelIndexScan(ResMgmt::BoundedMemoryResource *memory, const Rapid_scan_context *snapshot,
                                     const std::vector<std::shared_ptr<Imcs::Imcu>> &imcus,
                                     const std::vector<std::unique_ptr<Imcs::Predicate>> &predicates,
                                     const std::pmr::vector<Field *> &fields, uint32_t rows_per_imcu,
                                     Imcs::Index::ART *index, size_t row_bound)
    : m_snapshot(snapshot),
      m_imcus(imcus),
      m_predicates(predicates),
      m_fields(fields, memory),
      m_offsets(memory),
      m_runs(memory),
      m_index(index),
      m_row_bound(row_bound),
      m_rows_per_imcu(rows_per_imcu) {
  m_projection.reserve(fields.size());
  for (auto *field : fields) m_projection.push_back(field->field_index());

  // Same storage scratch shape as ParallelScan: predicates and the row gather
  // are the only allocations a worker makes on top of the pipeline buffers.
  size_t projected_width = 0, max_width = 0, depth = 0;
  for (auto *field : fields) projected_width += Utils::Util::normalized_length(field);
  for (const auto &imcu : imcus) {
    if (!imcu) continue;
    for (size_t col = 0; col < imcu->get_column_count(); ++col) {
      const auto *cu = imcu->get_cu(col);
      if (cu && cu->field()) max_width = std::max<size_t>(max_width, cu->field()->pack_length());
    }
    break;  // Every IMCU shares the table schema.
  }
  const auto visit = [&](const auto &self, const Imcs::Predicate *pred, size_t level) -> void {
    depth = std::max(depth, level);
    if (pred && pred->is_compound())
      for (const auto &child : static_cast<const Imcs::Compound_Predicate *>(pred)->children)
        self(self, child.get(), level + 1);
  };
  for (const auto &predicate : predicates) visit(visit, predicate.get(), 1);
  constexpr size_t scan_rows = ParallelSource::kBatchRows;
  m_storage_scratch_bytes =
      2 * (fields.size() * (sizeof(VisibleCell) + 3 * sizeof(void *)) + projected_width + 4 * max_width +
           scan_rows * sizeof(const uchar *) + (depth + 3) * (sizeof(bit_array_t) + (scan_rows + 7) / 8) +
           snapshot->m_schema_name.size() + snapshot->m_table_name.size() + 256);
}

void ParallelIndexScan::Collect() {
  // Candidates arrive in index order; a run is a maximal stretch inside one
  // IMCU, so a worker materializes it with a single vectorised scan.
  std::pmr::vector<Run> runs(m_runs.get_allocator().resource());
  std::pmr::vector<uint32_t> offsets(m_offsets.get_allocator().resource());
  Imcs::Index::Art_Iterator walk(m_index);
  walk.init_scan(nullptr, 0, true, nullptr, 0, false);
  const uchar *key{nullptr};
  uint32_t key_len{0};
  row_id_t rowid{INVALID_ROW_ID};
  size_t current_imcu = std::numeric_limits<size_t>::max();
  uint64_t ordinal{0};
  while (walk.next(&key, &key_len, &rowid)) {
    if (rowid == INVALID_ROW_ID) continue;
    const size_t imcu_idx = static_cast<size_t>(rowid / m_rows_per_imcu);
    if (imcu_idx >= m_imcus.size() || !m_imcus[imcu_idx]) continue;
    if (imcu_idx != current_imcu) {
      current_imcu = imcu_idx;
      runs.push_back({imcu_idx, ordinal, offsets.size(), 0});
    }
    offsets.push_back(static_cast<uint32_t>(rowid % m_rows_per_imcu));
    ++runs.back().count;
    ++ordinal;
    if (ordinal > kMaxCandidates) {
      // An empty run list would look exactly like an empty index: every worker
      // would report success and the rows would be lost.
      m_overflow.store(true, std::memory_order_release);
      return;
    }
  }
  m_offsets = std::move(offsets);
  m_runs = std::move(runs);
  m_max_ordinal.store(ordinal ? ordinal - 1 : 0, std::memory_order_release);
  m_ready_runs.store(m_runs.size(), std::memory_order_release);
}

bool ParallelIndexScan::ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory,
                                    const std::atomic<bool> &cancelled, const Consumer &consume) {
  // The walk cannot be split, and it must not be paid before the caller has
  // committed to a parallel phase: run it here, once.
  std::call_once(m_collect, [&] { Collect(); });
  if (m_overflow.load(std::memory_order_acquire)) return true;
  if (m_runs.empty()) return false;
  constexpr size_t capacity = ParallelSource::kBatchRows;
  ResMgmt::BoundedMemoryResource::Reservation scratch(memory, m_storage_scratch_bytes);
  Rapid_scan_context context;
  context.m_thd = worker;
  context.m_trx = m_snapshot->m_trx;
  context.m_extra_info.m_trxid = m_snapshot->m_extra_info.m_trxid;
  context.m_extra_info.m_scn = m_snapshot->m_extra_info.m_scn;
  context.m_schema_name = m_snapshot->m_schema_name;
  context.m_table_name = m_snapshot->m_table_name;
  context.limit = 0;

  std::pmr::vector<ColumnChunk> chunks(memory);
  std::pmr::vector<uint32_t> selection(memory);
  std::pmr::vector<uint64_t> ordinals(memory);
  std::vector<uint32_t> gather;  // reused: scan_rows_vectorized() takes a vector
  chunks.reserve(m_fields.size());
  ordinals.reserve(capacity);
  gather.reserve(capacity);
  for (Field *field : m_fields) {
    chunks.emplace_back(field, capacity, memory);
    if (chunks.back().failed()) return true;
  }

  while (!cancelled.load(std::memory_order_acquire)) {
    const size_t run_index = m_next_run.fetch_add(1, std::memory_order_relaxed);
    if (run_index >= m_runs.size()) break;
    const Run &run = m_runs[run_index];
    if (run.imcu >= m_imcus.size()) return true;
    const auto &imcu = m_imcus[run.imcu];
    if (!imcu) continue;
    context.rows_returned = 0;

    for (size_t offset = 0; offset < run.count; offset += capacity) {
      if (cancelled.load(std::memory_order_acquire)) return false;
      for (auto &chunk : chunks) chunk.clear();
      ordinals.clear();
      const size_t count = std::min(capacity, run.count - offset);
      // scan_rows_vectorized() reports rows in the order their offsets were
      // given, so a single cursor over the run maps every callback back to the
      // ordinal that candidate had in index order.
      const uint32_t *offsets = m_offsets.data() + run.begin + offset;
      size_t next_candidate = 0;
      bool failed = false;
      gather.assign(offsets, offsets + count);
      imcu->scan_rows_vectorized(&context, gather, m_predicates, m_projection,
                                 [&](row_id_t id, const std::vector<const uchar *> &values) {
                                   if (failed) return;
                                   const uint32_t offset_in_imcu = static_cast<uint32_t>(id % m_rows_per_imcu);
                                   while (next_candidate < count && offsets[next_candidate] != offset_in_imcu)
                                     ++next_candidate;
                                   if (next_candidate == count) {
                                     failed = true;
                                     return;
                                   }
                                   for (size_t col = 0; col < chunks.size(); ++col) {
                                     const uchar *value = values[col];
                                     if (!chunks[col].add(value, value ? chunks[col].width() : 0, value == nullptr)) {
                                       failed = true;
                                       return;
                                     }
                                   }
                                   ordinals.push_back(run.ordinal + offset + next_candidate);
                                   ++next_candidate;
                                 });
      if (failed) return true;
      if (!ordinals.empty() && consume(chunks, ordinals.size(), selection, ordinals)) return true;
    }
  }
  return false;
}
}  // namespace Morsel
}  // namespace Executor
}  // namespace ShannonBase
