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
/** The table scan iterator class for IMCS. All specific iterators are all inherited
 * from this.
 * vectorized/parallelized table scan iterator impl for rapid engine. In
 */
#include "storage/rapid_engine/executor/iterators/table_scan_iterator.h"

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/sql_class.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Executor {
VectorizedTableScanIterator::VectorizedTableScanIterator(THD *thd, TABLE *table, double expected_rows,
                                                         ha_rows *examined_rows,
                                                         std::unique_ptr<Imcs::Predicate> predicate,
                                                         const std::vector<uint32_t> &projection, ha_rows limit,
                                                         ha_rows offset, bool use_storage_index)
    : TableRowIterator(thd, table),
      m_table{table},
      m_pushed_predicate{std::move(predicate)},
      m_projected_columns(projection),
      m_limit{limit},
      m_offset{offset},
      m_use_storage_index{use_storage_index},
      m_batch_size{0},
      m_opt_batch_size{0},
      m_curr_batch_size{0},
      m_curr_row_in_batch{0},
      m_batch_exhausted{true},
      m_eof_reached{false},
      m_fields_cached{false} {
  m_opt_batch_size = CalculateOptimalBatchSize(expected_rows);
  m_batch_size = m_opt_batch_size;

  m_metrics.reset();
  auto share = shannon_loaded_tables->get(m_table->s->db.str, m_table->s->table_name.str);
  auto table_id = share ? share->m_tableid : 0;
  m_cursor.reset(new ShannonBase::Imcs::RapidCursor(table, Imcs::Imcs::instance()->get_rpd_table(table_id)));
}

size_t VectorizedTableScanIterator::EstimateRowSize() const {
  size_t total_size = 0;
  for (uint idx = 0; idx < m_table->s->fields; idx++) {
    Field *field = *(m_table->field + idx);
    if (bitmap_is_set(m_table->read_set, idx) && !field->is_flag_set(NOT_SECONDARY_FLAG))
      total_size += field->pack_length();
  }
  return total_size > 0 ? total_size : 64;
}

size_t VectorizedTableScanIterator::CalculateOptimalBatchSize(double expected_rows) {
  const size_t base_size = std::max<size_t>(SHANNON_VECTOR_WIDTH, 128);
  size_t l3_cache_size{8 * 1024 * 1024};
#if defined(CACHE_L3_SIZE)
  l3_cache_size = CACHE_L3_SIZE;
#endif

  size_t row_size = EstimateRowSize();
  size_t cache_optimal = l3_cache_size / (row_size * 4);

  size_t rows_based = 0;
  if (expected_rows > 0) rows_based = static_cast<size_t>(std::min(expected_rows / 10.0, 10000.0));

  size_t candidate = rows_based ? std::min(cache_optimal, rows_based) : cache_optimal;
  candidate = std::clamp(candidate, base_size, static_cast<size_t>(131072));
  candidate = (candidate + 3) & ~size_t(3);  // align to 4
  return candidate;
}

void VectorizedTableScanIterator::CacheActiveFields() {
  if (m_fields_cached) return;

  m_active_fields.clear();
  m_field_indices.clear();

  for (uint ind = 0; ind < m_table->s->fields; ind++) {
    Field *field = *(m_table->field + ind);
    if (bitmap_is_set(m_table->read_set, ind) && !field->is_flag_set(NOT_SECONDARY_FLAG)) {
      m_active_fields.push_back(field);
      m_field_indices.push_back(ind);
    }
  }

  m_fields_cached = true;
}

void VectorizedTableScanIterator::PreallocateColumnChunks() {
  m_col_chunks.clear();
  m_col_chunks.reserve(m_table->s->fields);

  for (uint ind = 0; ind < m_table->s->fields; ind++) {
    Field *field = *(m_table->field + ind);
    if (!bitmap_is_set(m_table->read_set, ind) || field->is_flag_set(NOT_SECONDARY_FLAG)) {
      m_col_chunks.emplace_back(nullptr, 0);  // place-holder.
    } else {                                  // using larger memory to reduce `allocate/new`
      auto initial_capacity =
          std::max(m_batch_size, static_cast<size_t>(((ShannonBase::SHANNON_ROWS_IN_CHUNK + 7) / 8) + 1));
      m_col_chunks.emplace_back(field, initial_capacity);
    }
  }
}

void VectorizedTableScanIterator::UpdatePerformanceMetrics(std::chrono::high_resolution_clock::time_point start_time) {
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  double read_time = duration.count() / 1000.0;  // in ms.

  m_metrics.avg_batch_time =
      (m_metrics.avg_batch_time * (m_metrics.total_batches - 1) + read_time) / m_metrics.total_batches;
  m_metrics.total_read_time += read_time;
  m_metrics.last_batch_time = end_time;
}

void VectorizedTableScanIterator::AdaptBatchSize() {
  if (m_metrics.total_batches % 100 == 0 && m_metrics.total_batches > 0) {
    double current_avg = m_metrics.avg_batch_time;

    if (current_avg > 50.0) {  // over 50 ms. reduce the batch size.
      m_batch_size = std::max(m_batch_size / 2, static_cast<size_t>(SHANNON_VECTOR_WIDTH));
    } else if (current_avg < 10.0 && m_batch_size < m_opt_batch_size * 2) {
      // less than 10 ms, raise up the batch size.
      m_batch_size = std::min(m_batch_size * 2, m_opt_batch_size * 4);
    }
  }
}

int VectorizedTableScanIterator::PopulateCurrentRow() {
  size_t rowid = m_curr_row_in_batch;

  for (size_t i = 0; i < m_active_fields.size(); ++i) {
    Field *field = m_active_fields[i];
    uint field_idx = m_field_indices[i];
    assert(field->is_flag_set(NOT_SECONDARY_FLAG) == false);

    if (m_col_chunks[field_idx].nullable(rowid)) {
      field->set_null();
    } else {
      field->set_notnull();
      ProcessFieldData(field, m_col_chunks[field_idx], rowid);
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

bool VectorizedTableScanIterator::Init() {
  // Initialize similar to ha_rapid::rnd_init()
  if (m_pushed_predicate) m_cursor->set_scan_predicates(std::move(m_pushed_predicate));
  if (!m_projected_columns.empty()) m_cursor->set_projection_columns(m_projected_columns);
  if (m_limit != HA_POS_ERROR) m_cursor->set_scan_limit(m_limit, m_offset);
  if (m_use_storage_index) m_cursor->enable_storage_index();

  if (m_cursor->init()) return true;

  // Allocate row buffer for batch processing. to store data in mysql format in column format.
  CacheActiveFields();

  PreallocateColumnChunks();

  m_curr_batch_size = 0;
  m_curr_row_in_batch = 0;
  m_batch_exhausted = true;
  m_eof_reached = false;
  m_metrics.reset();
  return false;
}

int VectorizedTableScanIterator::Read() {
  int result{ShannonBase::SHANNON_SUCCESS};

  if (m_batch_exhausted && !m_eof_reached) {
    result = ReadNextBatch();
    if (result) {
      if (result == HA_ERR_END_OF_FILE && m_curr_batch_size == 0) return HandleError(HA_ERR_END_OF_FILE);
      if (result != HA_ERR_END_OF_FILE) return HandleError(result);
    }
  }

  if (m_curr_row_in_batch >= m_curr_batch_size) {
    if (m_eof_reached) {
      return HandleError(HA_ERR_END_OF_FILE);
    } else {  // read the next batch.
      m_batch_exhausted = true;
      return Read();
    }
  }

  // fill up the data to table->field
  result = PopulateCurrentRow();
  if (result) return HandleError(result);

  // move to the next row.
  m_curr_row_in_batch++;
  m_metrics.total_rows++;

  if (m_curr_row_in_batch >= m_curr_batch_size) {
    m_batch_exhausted = true;
    if (!m_eof_reached) m_curr_row_in_batch = 0;
  }

  return result;
}

int VectorizedTableScanIterator::ReadNextBatch() {
  auto batch_start = std::chrono::high_resolution_clock::now();

  ClearBatchData();

  size_t read_cnt = 0;
  int result = m_cursor->next(m_batch_size, m_col_chunks, read_cnt);

  if (result != 0) {
    if (result == HA_ERR_END_OF_FILE) {
      m_eof_reached = true;
      if (read_cnt) {
        m_batch_exhausted = false;
        m_metrics.total_batches++;
        UpdatePerformanceMetrics(batch_start);
      }
      m_curr_batch_size = read_cnt;
      m_curr_row_in_batch = 0;
      return HA_ERR_END_OF_FILE;
    }

    if (++m_metrics.error_count > 10) return HA_ERR_GENERIC;

    if (result == HA_ERR_RECORD_DELETED && !thd()->killed) return ReadNextBatch();
    return result;
  }

  if (read_cnt == 0) {  // no data read, therefore set to EOF.
    m_eof_reached = true;
    return HA_ERR_END_OF_FILE;
  }

  m_curr_batch_size = read_cnt;
  m_curr_row_in_batch = 0;
  m_batch_exhausted = false;
  m_metrics.total_batches++;

  UpdatePerformanceMetrics(batch_start);

  AdaptBatchSize();

  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace Executor
}  // namespace ShannonBase