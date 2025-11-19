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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/

/**DataTable to mock a table hehaviors. We can use a DataTable to open the IMCS
 * with sepecific table information. After the Cu belongs to this table were found
 * , we can use this DataTable object to read/write, etc., just like a normal innodb
 * table.
 */
#include "storage/rapid_engine/imcs/table0view.h"

#include <sstream>
#include <utility>  // std::pair

#include "include/my_base.h"  //key_range
#include "include/ut0dbg.h"   //ut_a
#include "sql/field.h"        //field
#include "sql/table.h"        //TABLE
#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/imcs/cu.h"    //CU
#include "storage/rapid_engine/imcs/imcs.h"  //IMCS
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/table.h"  //RapidTable
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/log_commons.h"  //sys_pop_buff
#include "storage/rapid_engine/trx/readview.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction
#include "storage/rapid_engine/utils/utils.h"      //Blob

namespace ShannonBase {
namespace Imcs {
ShannonBase::Utils::SimpleRatioAdjuster RpdTableView::m_adaptive_ratio(0.3);
RpdTableView::RpdTableView(TABLE *source, RpdTable *rpd)
    : m_scan_initialized{false}, m_data_source(source), m_rpd_table(rpd), m_source_rpd_table(rpd) {
  // if m_rapid_table is null, means we will get its real imp by part_id when it used.
  ut_a(m_data_source);
  m_current_row_idx.store(0);
}

int RpdTableView::open() {
  m_current_row_idx.store(0);
  return ShannonBase::SHANNON_SUCCESS;
}

int RpdTableView::close() {
  m_current_row_idx.store(0);
  return ShannonBase::SHANNON_SUCCESS;
}

int RpdTableView::init() {
  assert(!m_scan_initialized.load(std::memory_order_acquire));
  if (!m_scan_initialized.load(std::memory_order_acquire)) {
    m_scan_context = std::make_unique<Rapid_scan_context>();
    m_scan_context->m_thd = current_thd;
    m_scan_context->m_extra_info.m_keynr = m_active_index;

    m_scan_context->m_trx = ShannonBase::Transaction::get_or_create_trx(current_thd);
    m_scan_context->m_trx->begin();
    m_scan_context->m_extra_info.m_trxid = m_scan_context->m_trx->get_id();
    m_scan_context->m_extra_info.m_scn = TransactionCoordinator::instance().get_current_scn();

    if (!m_scan_context->m_trx->is_active())
      m_scan_context->m_trx->begin(ShannonBase::Transaction::get_rpd_isolation_level(current_thd));
    m_rpd_table->register_transaction(m_scan_context->m_trx);
    m_scan_context->m_trx->acquire_snapshot();

    m_scan_context->m_schema_name = const_cast<char *>(m_data_source->s->db.str);
    m_scan_context->m_table_name = const_cast<char *>(m_data_source->s->table_name.str);
    // Initialize scan parameters
    m_scan_context->limit = 0;  // No limit for full table scan
    m_scan_context->rows_returned = 0;

    // Reserve buffer cache
    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    // Reserve buffer cache with extra space to reduce reallocation
    m_row_buffer_cache.reserve(SHANNON_BATCH_NUM + 16);

    // Initialize scan position
    m_current_row_idx.store(0, std::memory_order_release);

    m_batch_start = 0;
    m_batch_end = 0;
    m_current_imcu_idx = 0;
    m_current_imcu_offset = 0;
    m_scan_exhausted.store(false, std::memory_order_release);

    m_scan_initialized.store(true, std::memory_order_release);
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int RpdTableView::end() {
  m_scan_context->m_trx->release_snapshot();
  m_scan_context->m_trx->commit();

  m_row_buffer_cache.clear();

  m_current_row_idx.store(0);
  m_using_batch.store(true, std::memory_order_release);

  m_current_imcu_idx = 0;
  m_current_imcu_offset = 0;
  m_batch_start = 0;
  m_batch_end = 0;
  m_scan_exhausted.store(false, std::memory_order_release);

  m_scan_initialized.store(false, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

void RpdTableView::encode_key_parts(uchar *encoded_key, const uchar *original_key, uint key_len, KEY *key) {
  if (!encoded_key || !original_key || !key) return;

  auto offset{0u};
  std::memcpy(encoded_key, original_key, key_len);

  for (auto part = 0u; part < key->user_defined_key_parts; part++) {
    auto &key_part_info = key->key_part[part];
    if (key_part_info.null_bit) offset += 1;
    auto fld = key_part_info.field;

    switch (fld->type()) {
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        uchar encoding[8] = {0};
        auto val = Utils::Util::get_field_numeric<double>(fld, original_key + offset, nullptr);
        Index::Encoder<double>::EncodeData(val, encoding);
        std::memcpy(encoded_key + offset, encoding, key_part_info.length);
      } break;
      case MYSQL_TYPE_LONG: {
        ut_a(key_part_info.length == sizeof(int32_t));
        uchar encoding[4] = {0};
        auto val = Utils::Util::get_field_numeric<int32_t>(fld, original_key + offset, nullptr);
        Index::Encoder<int32_t>::EncodeData(val, encoding);
        std::memcpy(encoded_key + offset, encoding, key_part_info.length);
      } break;
      default:
        break;
    }
    offset += fld->pack_length();
    if (offset >= key_len) break;
  }
}

int RpdTableView::position(row_id_t start_row_id) {
  // Reserve buffer cache with extra space to reduce reallocation
  m_using_batch.store(false, std::memory_order_release);
  m_row_buffer_cache.resize(1);
  m_row_buffer_cache.clear();

  // re-initialize scan position
  m_current_row_idx.store(start_row_id, std::memory_order_release);
  m_batch_start = 0;
  m_batch_end = 0;

  m_current_imcu_idx = start_row_id / m_rpd_table->meta().rows_per_imcu;
  m_current_imcu_offset = start_row_id % m_rpd_table->meta().rows_per_imcu;
  m_scan_exhausted.store(false, std::memory_order_release);

  return ShannonBase::SHANNON_SUCCESS;
}

int RpdTableView::next(uchar *buf) {
  assert(m_scan_initialized.load(std::memory_order_acquire));
  if (!m_scan_initialized.load(std::memory_order_acquire)) init();

  size_t current_idx = m_current_row_idx.load(std::memory_order_acquire);
  if (current_idx >= m_batch_end) {
    if (!fetch_next_batch(m_using_batch.load(std::memory_order_acquire) ? SHANNON_BATCH_NUM : 1)) {
      if (m_scan_exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;
      // Empty batch but not exhausted (all rows filtered), try again
      return next(buf);
    }
    // Reset to beginning of new batch
    current_idx = m_batch_start;
  }

  size_t cache_idx = current_idx - m_batch_start;
  if (cache_idx >= m_row_buffer_cache.size()) {
    // Shouldn't happen, but handle gracefully
    if (m_scan_exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;
    m_current_row_idx.store(m_batch_end, std::memory_order_release);
    return next(buf);
  }

  const RowBuffer *row_buffer = m_row_buffer_cache[cache_idx].get();
  if (!row_buffer) return HA_ERR_GENERIC;
  auto status = row_buffer->copy_to_mysql_fields(m_data_source, &m_rpd_table->meta());
  if (status) return HA_ERR_GENERIC;

  m_current_row_idx.fetch_add(1, std::memory_order_release);

  m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);

  return ShannonBase::SHANNON_SUCCESS;
}

boost::asio::awaitable<int> RpdTableView::next_async(uchar *buf) {
  assert(m_scan_initialized.load(std::memory_order_acquire));

  if (!m_scan_initialized.load(std::memory_order_acquire)) init();
  auto executor = co_await boost::asio::this_coro::executor;

  size_t cache_idx = m_current_row_idx.load(std::memory_order_acquire) - m_batch_start;
  if (m_scan_exhausted.load(std::memory_order_acquire) && cache_idx >= m_row_buffer_cache.size())
    co_return HA_ERR_END_OF_FILE;

  // Check if we need to fetch next batch
  if (cache_idx >= m_row_buffer_cache.size()) {
    if (!fetch_next_batch(m_using_batch.load(std::memory_order_acquire) ? SHANNON_BATCH_NUM : 1))
      co_return HA_ERR_END_OF_FILE;
    cache_idx = 0;
  }

  const RowBuffer *row_buffer = m_row_buffer_cache[cache_idx].get();
  if (!row_buffer) co_return HA_ERR_GENERIC;
  // Async convert to MySQL format (parallel column processing for wide tables)
  auto status =
      co_await row_buffer->copy_to_mysql_fields_async(m_data_source,         // TABLE*
                                                      &m_rpd_table->meta(),  // TableMetadata*
                                                      executor,
                                                      m_adaptive_ratio.getMaxBatchSize()  // optional batch size
      );
  if (status) co_return status;

  m_current_row_idx.fetch_add(1, std::memory_order_release);

  m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);

  co_return ShannonBase::SHANNON_SUCCESS;
}

bool RpdTableView::fetch_next_batch(size_t batch_size /* = SHANNON_BATCH_NUM */) {
  std::lock_guard<std::mutex> lock(m_buffer_mutex);
  if (m_scan_exhausted.load(std::memory_order_acquire)) return false;

  // Clear previous batch
  m_row_buffer_cache.clear();

  // Build projection list (columns to read and Not NOT_SECONDARY_FLAG)
  std::vector<uint32_t> projection_cols;
  projection_cols.reserve(m_data_source->s->fields);
  for (uint32_t idx = 0; idx < m_data_source->s->fields; idx++) {
    Field *field = m_data_source->field[idx];
    if (bitmap_is_set(m_data_source->read_set, idx) && !field->is_flag_set(NOT_SECONDARY_FLAG)) {
      projection_cols.push_back(idx);
    }
  }

  // Build empty predicate list (no filtering for full scan)
  std::vector<std::unique_ptr<Predicate>> predicates;
  size_t total_imcus = m_rpd_table->meta().total_imcus;
  size_t remaining = batch_size;

  // Callback to collect rows with prefetch optimization
  auto callback = [this](row_id_t global_row_id, const std::vector<const uchar *> &row_data) {
    auto row_buffer = std::make_unique<RowBuffer>(row_data.size());
    row_buffer->set_row_id(global_row_id);

    // Prefetch next row's data (if available)
    // Assuming row_data pointers are sequential in memory
    if (row_data.size() > 0 && row_data[0] != nullptr) {
      const uchar *first_col_ptr = row_data[0];
      // Prefetch ahead by PREFETCH_AHEAD cache lines
      if ((uintptr_t(first_col_ptr) & (CACHE_LINE_SIZE - 1)) == 0)
        SHANNON_PREFETCH_R(first_col_ptr + PREFETCH_AHEAD * CACHE_LINE_SIZE);
    }

    // Copy column data (zero-copy mode)
    for (size_t idx = 0; idx < row_data.size(); idx++) {
      if (row_data[idx] == nullptr) {
        row_buffer->set_column_null(idx);
      } else {  // Prefetch next column's data
        if (idx + 1 < row_data.size() && row_data[idx + 1] != nullptr) SHANNON_PREFETCH_R(row_data[idx + 1]);

        // Note: We need to determine the length properly
        // For now, use the field's pack_length as approximation
        Field *field = m_data_source->field[idx];
        size_t length = field->pack_length();
        row_buffer->set_column_zero_copy(idx, row_data[idx], length, field->type());
      }
    }
    m_row_buffer_cache.push_back(std::move(row_buffer));
  };

  while (m_current_imcu_idx < total_imcus && remaining > 0) {
    // Get the specific IMCU
    Imcu *imcu = m_rpd_table->locate_imcu(m_current_imcu_idx);
    if (!imcu) {
      m_current_imcu_idx++;
      continue;
    }
    // Scan range within this IMCU
    size_t scanned = imcu->scan_range(m_scan_context.get(),
                                      m_current_imcu_offset,  // Start from where we left off
                                      remaining,              // How many rows we still need
                                      predicates, projection_cols, callback);
    remaining -= scanned;

    // Check if IMCU is exhausted
    size_t imcu_rows = imcu->get_row_count();
    if (!scanned /*no cannced, go to next*/ || (m_current_imcu_offset + scanned >= imcu_rows)) {
      m_current_imcu_idx++;
      m_current_imcu_offset = 0;
    } else {  // Still within current IMCU
      m_current_imcu_offset += scanned;
      if (remaining == 0) break;  // If we got what we needed, break
    }
  }

  // Check if we've exhausted all IMCUs
  if (m_current_imcu_idx >= total_imcus) m_scan_exhausted.store(true, std::memory_order_release);

  // Update batch boundaries
  m_batch_start = m_current_row_idx.load(std::memory_order_acquire);
  m_batch_end = m_batch_start + m_row_buffer_cache.size();

  // Prefetch first few rows of the batch for faster access
  constexpr size_t PREFETCH_ROWS = 8;
  size_t prefetch_count = std::min(PREFETCH_ROWS, m_row_buffer_cache.size());
  for (size_t i = 0; i < prefetch_count; i++) {
    if (m_row_buffer_cache[i]) {  // Prefetch RowBuffer structure
      SHANNON_PREFETCH_R(m_row_buffer_cache[i].get());

      // Prefetch first column data
      const auto *col_data = m_row_buffer_cache[i]->get_column_data(0);
      if (col_data) SHANNON_PREFETCH_R(col_data);
    }
  }

  m_batch_fetch_count.fetch_add(1, std::memory_order_relaxed);
  return !m_row_buffer_cache.empty();
}

int RpdTableView::next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &col_chunks,
                             size_t &read_cnt) {
  read_cnt = 0;
  if (m_scan_exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;

  // 1.build projection columns.
  std::vector<uint32_t> projection_cols;
  for (uint32_t idx = 0; idx < m_data_source->s->fields; ++idx) {
    Field *fld = m_data_source->field[idx];
    if (bitmap_is_set(m_data_source->read_set, idx) && !fld->is_flag_set(NOT_SECONDARY_FLAG))
      projection_cols.push_back(idx);
  }
  if (projection_cols.empty()) return ShannonBase::SHANNON_SUCCESS;

#ifndef NDEBUG
  // 2. preallocate col_chunks
  if (col_chunks.size() != projection_cols.size()) {
    for (uint32_t col_id : projection_cols) {
      assert(col_chunks[col_id].valid());
    }
  }
#endif

  // 3. the predicates.
  std::vector<std::unique_ptr<Predicate>> predicates;

  // 4. fill up the ColumChunk.
  auto callback = [&](row_id_t global_row_id, const std::vector<const uchar *> &row_data) {
    for (size_t idx = 0; idx < projection_cols.size(); ++idx) {
      auto col_idx = projection_cols[idx];
      auto &col_chunk = col_chunks[col_idx];
      auto data_ptr = row_data[idx];
      auto is_null = (data_ptr == nullptr) ? true : false;
      auto normal_len = m_rpd_table->meta().fields[col_idx].normalized_length;
      col_chunk.add(data_ptr, normal_len, is_null);
    }
    read_cnt++;
  };

  // 5. start batch scanning.
  size_t remaining = batch_size;
  while (m_current_imcu_idx < m_rpd_table->meta().total_imcus && remaining > 0) {
    Imcu *imcu = m_rpd_table->locate_imcu(m_current_imcu_idx);
    if (!imcu) {
      m_current_imcu_idx++;
      continue;
    }

    size_t scanned = imcu->scan_range(m_scan_context.get(), m_current_imcu_offset, remaining,
                                      predicates /* predicates */,  // in future, index push down.
                                      projection_cols, callback);

    remaining -= scanned;
    size_t imcu_rows = imcu->get_row_count();
    if (m_current_imcu_offset + scanned >= imcu_rows) {
      m_current_imcu_idx++;
      m_current_imcu_offset = 0;
    } else {
      m_current_imcu_offset += scanned;
      if (remaining == 0) break;
    }
  }

  if (m_current_imcu_idx >= m_rpd_table->meta().total_imcus) {
    m_scan_exhausted.store(true, std::memory_order_release);
  }

  return read_cnt > 0 ? ShannonBase::SHANNON_SUCCESS : HA_ERR_END_OF_FILE;
}

int RpdTableView::index_init(uint keynr, bool sorted) {
  init();
  m_active_index = keynr;
  auto index = m_rpd_table->get_index(m_data_source->s->key_info[keynr].name);
  if (index == nullptr) {
    std::string err;
    err.append(m_data_source->s->db.str)
        .append(".")
        .append(m_data_source->s->table_name.str)
        .append(" index not found");
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.c_str());
    return HA_ERR_KEY_NOT_FOUND;
  }

  ut_a(index->initialized());
  m_index_iter.reset(new Index::Art_Iterator(index->impl()));

  return ShannonBase::SHANNON_SUCCESS;
}

int RpdTableView::index_end() {
  m_active_index = MAX_KEY;
  return end();
}

// index read.
int RpdTableView::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) {
  ut_a(m_active_index != MAX_KEY);
  if (key_len == 0 && key != nullptr) return HA_ERR_WRONG_COMMAND;

  auto key_info = m_data_source->s->key_info + m_active_index;
  m_key = std::make_unique<uchar[]>(key_len);
  encode_key_parts(m_key.get(), key, key_len, key_info);

  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  switch (find_flag) {
    case HA_READ_KEY_EXACT: {  //  (=)
      m_index_iter->init_scan(m_key.get(), key_len, true, m_key.get(), key_len, true);
    } break;
    case HA_READ_KEY_OR_NEXT: {  //  (>=)
      m_index_iter->init_scan(m_key.get(), key_len, true, nullptr, 0, false);
    } break;
    case HA_READ_KEY_OR_PREV: {  // (<=)
      m_index_iter->init_scan(nullptr, 0, true, m_key.get(), key_len, true);
    } break;
    case HA_READ_AFTER_KEY: {  // (>)
      const uchar *start_key_ptr{nullptr}, *end_key_ptr{nullptr};
      uint start_key_len{0}, end_key_len{0};

      start_key_ptr = m_key.get();
      start_key_len = key_len;

      if (m_end_range) {
        start_key_ptr = nullptr;
        start_key_len = 0;
        auto key_info = m_data_source->s->key_info + m_active_index;
        m_end_key = std::make_unique<uchar[]>(m_end_range->length);
        encode_key_parts(m_end_key.get(), m_end_range->key, m_end_range->length, key_info);
        end_key_ptr = m_end_key.get();
        end_key_len = m_end_range->length;
      }
      m_index_iter->init_scan(start_key_ptr, start_key_len, false, end_key_ptr, end_key_len, true);
    } break;
    case HA_READ_BEFORE_KEY: {  //  (<)
      m_end_range ? m_index_iter->init_scan(nullptr, 0, true, m_key.get(), key_len, false)
                  : m_index_iter->init_scan(m_key.get(), key_len, false, nullptr, 0, true);
    } break;
    default:
      return HA_ERR_WRONG_COMMAND;
  }

  const uchar *result_key{nullptr};
  uint32_t result_key_len{0};
  row_id_t value{std::numeric_limits<row_id_t>::max()};

  if (m_index_iter->next(&result_key, &result_key_len, &value)) {
    m_current_row_idx.store(value);
    // using point select.
    position(m_current_row_idx.load(std::memory_order_acquire));
    auto ret = next(buf);
    return (ret) ? ret : ShannonBase::SHANNON_SUCCESS;
  }

  return HA_ERR_KEY_NOT_FOUND;
}

int RpdTableView::index_next(uchar *buf) {
  const uchar *result_key{nullptr};
  uint32_t result_key_len{0};
  row_id_t value{std::numeric_limits<row_id_t>::max()};
  int err{HA_ERR_END_OF_FILE};

  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  if (m_index_iter->next(&result_key, &result_key_len, &value)) {
    m_current_row_idx.store(value);
    // using point select.
    position(m_current_row_idx.load(std::memory_order_acquire));
    auto ret = next(buf);
    if (ret) return ret;
    err = ShannonBase::SHANNON_SUCCESS;
  }

  return err;
}

int RpdTableView::index_prev(uchar *buf) { return HA_ERR_WRONG_COMMAND; }

row_id_t RpdTableView::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}
}  // namespace Imcs
}  // namespace ShannonBase
