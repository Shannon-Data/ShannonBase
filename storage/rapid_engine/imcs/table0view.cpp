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
#include "storage/rapid_engine/imcs/table0view.h"

#include <sstream>
#include <utility>  // std::pair

#include "include/my_base.h"  //key_range
#include "include/ut0dbg.h"   //ut_a
#include "sql/field.h"        //field
#include "sql/table.h"        //TABLE

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/log_commons.h"  //shannon_pop_buff

#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction
#include "storage/rapid_engine/utils/utils.h"      //Blob

#include "storage/rapid_engine/imcs/cu.h"    //CU
#include "storage/rapid_engine/imcs/imcs.h"  //IMCS
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/predicate.h"  //predicate
#include "storage/rapid_engine/imcs/table.h"      //RapidTable

namespace ShannonBase {
namespace Imcs {
RapidCursor::RapidCursor(TABLE *source, RpdTable *rpd)
    : m_inited{false}, m_data_source(source), m_rpd_table(rpd), m_src_rpd_table(rpd) {
  // if m_rapid_table is null, means we will get its real imp by part_id when it used.
  ut_a(m_data_source);
  m_scan_state.reset();
}

int RapidCursor::open() {
  m_scan_state.reset();
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::close() {
  m_scan_state.reset();
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::init() {
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
  m_scan_context->limit = 0;
  m_scan_context->rows_returned = 0;

  // Build columnar chunk buffers based on the current read_set
  init_col_chunks();

  m_scan_state.reset();
  m_rows_skipped = 0;
  m_rows_returned = 0;
  m_last_returned_rowid = INVALID_ROW_ID;

  m_inited.store(true, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::end() {
  m_scan_context->m_trx->release_snapshot();
  m_scan_context->m_trx->commit();

  m_scan_state.reset();
  m_rows_skipped = 0;
  m_rows_returned = 0;
  m_last_returned_rowid = INVALID_ROW_ID;

  m_inited.store(false, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

void RapidCursor::init_col_chunks() {
  m_col_chunks.clear();
  m_col_chunks.reserve(m_data_source->s->fields);

  const size_t cap = std::max(static_cast<size_t>(SHANNON_BATCH_NUM),
                              static_cast<size_t>(((ShannonBase::SHANNON_ROWS_IN_CHUNK + 7) / 8) + 1));

  for (uint ind = 0; ind < m_data_source->s->fields; ++ind) {
    Field *fld = m_data_source->field[ind];
    const bool active = bitmap_is_set(m_data_source->read_set, ind) && !fld->is_flag_set(NOT_SECONDARY_FLAG);
    m_col_chunks.emplace_back(active ? fld : nullptr, active ? cap : 0);
  }

  m_batch_row_ids.reserve(SHANNON_BATCH_NUM);

  // Bind CursorState to the (now stable) vector addresses.
  m_scan_state.bind(&m_col_chunks, &m_batch_row_ids);

  // Invalidate projection cache — read_set may have changed.
  m_proj_cols_dirty = true;
}

std::vector<uint32_t> RapidCursor::projection_columns() const {
  if (!m_proj_cols_dirty) return m_proj_cols_cache;

  m_proj_cols_cache.clear();
  m_proj_cols_cache.reserve(m_data_source->s->fields);

  for (uint32_t idx = 0; idx < m_data_source->s->fields; ++idx) {
    Field *fld = m_data_source->field[idx];
    const bool in_read_set = bitmap_is_set(m_data_source->read_set, idx) && !fld->is_flag_set(NOT_SECONDARY_FLAG);
    const bool in_explicit =
        std::find(m_projection_columns.begin(), m_projection_columns.end(), idx) != m_projection_columns.end();
    if (in_read_set || in_explicit) m_proj_cols_cache.push_back(idx);
  }

  m_proj_cols_dirty = false;
  return m_proj_cols_cache;
}

int RapidCursor::populate_row_from_chunks(size_t row_idx) {
  for (uint32_t col_idx = 0; col_idx < m_data_source->s->fields; ++col_idx) {
    Field *fld = m_data_source->field[col_idx];
    if (!bitmap_is_set(m_data_source->read_set, col_idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    const auto &chunk = m_col_chunks[col_idx];
    if (!chunk.valid()) continue;

    if (chunk.nullable(row_idx)) {
      fld->set_null();
      continue;
    }
    fld->set_notnull();

    if (Utils::Util::is_string(fld->type()) || Utils::Util::is_blob(fld->type())) {
      // String / BLOB path (mirrors ProcessStringField)
      if (fld->real_type() == MYSQL_TYPE_ENUM) {
        fld->pack(const_cast<uchar *>(fld->data_ptr()), chunk.data(row_idx), fld->pack_length());
      } else {
        Utils::ColumnMapGuard guard(fld->table, Utils::ColumnMapGuard::TYPE::WRITE);
        auto str_id = *reinterpret_cast<const uint32 *>(chunk.data(row_idx));
        auto dict = m_rpd_table->meta().fields[col_idx].dictionary;
        if (dict) {
          const auto &str_val = dict->get(str_id);
          fld->store(str_val.c_str(), str_val.size(), fld->charset());
        }
      }
    } else {
      fld->pack(const_cast<uchar *>(fld->data_ptr()), chunk.data(row_idx), chunk.width());
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::next(uchar *buf) {
  assert(m_inited.load(std::memory_order_acquire));
  if (!m_inited.load(std::memory_order_acquire)) init();

  // Refill the column-chunk batch whenever the current one is exhausted.
  if (m_scan_state.is_exhausted()) {
    if (m_scan_state.exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;

    size_t read_cnt = 0;
    int result = next(SHANNON_BATCH_NUM, m_col_chunks, read_cnt);
    if (result != ShannonBase::SHANNON_SUCCESS) return result;

    m_scan_state.commit_batch(read_cnt);
    m_batch_fetch_count.fetch_add(1, std::memory_order_relaxed);
  }

  // Populate MySQL field buffers from the columnar chunk for the current row.
  int status = populate_row_from_chunks(m_scan_state.row_in_batch);
  if (status) return HA_ERR_GENERIC;

  m_last_returned_rowid = m_batch_row_ids[m_scan_state.row_in_batch];

  m_scan_state.advance_row();
  m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);
  return ShannonBase::SHANNON_SUCCESS;
}

boost::asio::awaitable<int> RapidCursor::next_async(uchar *buf) {
  assert(m_inited.load(std::memory_order_acquire));
  if (!m_inited.load(std::memory_order_acquire)) init();

  auto executor = co_await boost::asio::this_coro::executor;

  // Refill the column-chunk batch asynchronously whenever the current one is exhausted.
  while (m_scan_state.is_exhausted()) {
    if (m_scan_state.exhausted.load(std::memory_order_acquire)) co_return HA_ERR_END_OF_FILE;

    // Yield to the executor so other coroutines can run while we refill.
    co_await boost::asio::post(executor, boost::asio::use_awaitable);

    size_t read_cnt = 0;
    int result = next(SHANNON_BATCH_NUM, m_col_chunks, read_cnt);
    if (result == HA_ERR_END_OF_FILE) co_return HA_ERR_END_OF_FILE;
    if (result != ShannonBase::SHANNON_SUCCESS) co_return result;
    if (read_cnt == 0) continue;  // all filtered; retry

    m_scan_state.commit_batch(read_cnt);
    m_batch_fetch_count.fetch_add(1, std::memory_order_relaxed);
    break;
  }

  if (m_scan_state.is_exhausted()) co_return HA_ERR_END_OF_FILE;

  int status = populate_row_from_chunks(m_scan_state.row_in_batch);
  if (status) co_return HA_ERR_GENERIC;

  m_last_returned_rowid = m_batch_row_ids[m_scan_state.row_in_batch];
  m_scan_state.advance_row();
  m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);
  co_return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::next(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &col_chunks,
                      size_t &read_cnt) {
  read_cnt = 0;
  // Fast-path: source already drained.
  if (m_scan_state.exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;

  // Phase 1: OFFSET — scan-and-discard rows until the skip quota is met
  //
  // Runs at most once per cursor lifetime because m_rows_skipped is monotonic.
  // An empty projection is passed so IMCU scan avoids materialising any column
  // data for skipped rows; predicates still apply, so only qualifying rows
  // count toward the offset.
  if (m_scan_offset > 0 && m_rows_skipped < m_scan_offset) {
    const ha_rows to_skip = m_scan_offset - m_rows_skipped;

    // Lightweight receiver: counts qualifying rows, stores nothing.
    struct SkipRecv : RecieverBase {
      ha_rows &skipped;
      explicit SkipRecv(ha_rows &s) : skipped(s) {}
      void on_row(row_id_t, const std::vector<const uchar *> &) noexcept { ++skipped; }
    } skip_recv{m_rows_skipped};

    const std::vector<uint32_t> empty_proj;  // no column data needed while skipping
    scan_batch_internal(static_cast<size_t>(to_skip), empty_proj, skip_recv);

    if (m_rows_skipped < m_scan_offset) return HA_ERR_END_OF_FILE;
    if (m_scan_state.exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;
  }

  // Phase 2: LIMIT — early-exit and per-batch cap
  //
  // m_rows_returned tracks how many rows have been handed to the caller across
  // ALL prior calls to this function (both the row-by-row path via next(uchar*)
  // and the direct vectorised path used by VectorizedTableScanIterator).
  if (m_scan_limit != HA_POS_ERROR) {
    if (m_rows_returned >= m_scan_limit) return HA_ERR_END_OF_FILE;

    // Cap the batch so we never return more than the remaining quota in one go.
    const ha_rows remaining_quota = m_scan_limit - m_rows_returned;
    if (static_cast<ha_rows>(batch_size) > remaining_quota) batch_size = static_cast<size_t>(remaining_quota);
  }

  // Phase 3: columnar batch fetch
  const std::vector<uint32_t> projection_cols = projection_columns();

#ifndef NDEBUG
  for (uint32_t col_id : projection_cols) {
    assert(col_id < col_chunks.size() && col_chunks[col_id].valid());
  }
#endif

  ColumnChunkRecv receiver{this, projection_cols, col_chunks, m_batch_row_ids, read_cnt};
  scan_batch_internal(batch_size, projection_cols, receiver);
  if (read_cnt > 0) {
    m_rows_returned += static_cast<ha_rows>(read_cnt);
    return ShannonBase::SHANNON_SUCCESS;
  }
  return HA_ERR_END_OF_FILE;
}

// Random access (position / rnd_pos)
row_id_t RapidCursor::position(const unsigned char *record) {
  // get the rowid of current row `record`.
  /* Copy primary key as the row reference */
  // KEY *key_info = table->key_info + table_share->primary_key;
  // the first key is always primary key.
  if (m_active_index == MAX_KEY) {
    index_init(0, false);
  }

  auto key_info = m_data_source->s->key_info + m_active_index;
  auto ref = std::make_unique<uchar[]>(key_info->key_length);
  key_copy(ref.get(), (uchar *)record, key_info, key_info->key_length);

  auto key_len = m_rpd_table->meta().keys[0].key_length;
  index_read(ref.get(), ref.get(), key_len, HA_READ_KEY_EXACT);
  index_end();
  return m_scan_state.curr_row_idx;
}

int RapidCursor::rnd_pos(uchar *buff, uchar *pos) {
  // position(m_scan_state.curr_imcu_idx);
  auto ret = next(buff);
  return (ret) ? ret : ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::position(row_id_t start_row_id) {
  // TODO: using index to fast locating the location  by `start_row_id`.
  m_scan_state.seek(start_row_id, m_rpd_table->meta().rows_per_imcu);
  return ShannonBase::SHANNON_SUCCESS;
}

void RapidCursor::encode_key_parts(uchar *encoded_key, const uchar *original_key, uint key_len, KEY *key) {
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
        auto val = Utils::Util::get_field_numeric<double>(fld, original_key + offset, nullptr,
                                                          m_data_source->s->db_low_byte_first);
        Index::Encoder<double>::Encode(val, encoding);
        std::memcpy(encoded_key + offset, encoding, key_part_info.length);
      } break;
      case MYSQL_TYPE_LONG: {
        ut_a(key_part_info.length == sizeof(int32_t));
        uchar encoding[4] = {0};
        auto val = Utils::Util::get_field_numeric<int32_t>(fld, original_key + offset, nullptr,
                                                           m_data_source->s->db_low_byte_first);
        Index::Encoder<int32_t>::Encode(val, encoding);
        std::memcpy(encoded_key + offset, encoding, key_part_info.length);
      } break;
      default:
        break;
    }
    offset += fld->pack_length();
    if (offset >= key_len) break;
  }
}

int RapidCursor::index_init(uint keynr, bool sorted) {
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

int RapidCursor::index_end() {
  m_active_index = MAX_KEY;
  return end();
}

// index read.
int RapidCursor::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) {
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
    m_scan_state.curr_row_idx.store(value);
    position(value);
    auto ret = next(buf);
    return ret ? ret : ShannonBase::SHANNON_SUCCESS;
  }
  return HA_ERR_KEY_NOT_FOUND;
}

int RapidCursor::index_next(uchar *buf) {
  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  const uchar *result_key{nullptr};
  uint32_t result_key_len{0};
  row_id_t value{std::numeric_limits<row_id_t>::max()};

  if (m_index_iter->next(&result_key, &result_key_len, &value)) {
    position(value);
    auto ret = next(buf);
    return ret ? ret : ShannonBase::SHANNON_SUCCESS;
  }
  return HA_ERR_END_OF_FILE;
}

int RapidCursor::index_prev(uchar * /*buf*/) { return HA_ERR_WRONG_COMMAND; }

row_id_t RapidCursor::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}

template <typename Reciever>
size_t RapidCursor::scan_batch_internal(size_t batch_size, const std::vector<uint32_t> &projection_cols,
                                        Reciever &recv) {
  auto &st = m_scan_state;
  size_t remaining = batch_size;

  recv.on_batch_begin();

  while (st.curr_imcu_idx < m_rpd_table->meta().total_imcus && remaining > 0 && recv.accept_more()) {
    Imcu *imcu = m_rpd_table->locate_imcu(st.curr_imcu_idx);
    if (!imcu) {
      st.curr_imcu_idx++;
      st.curr_imcu_offset = 0;
      continue;
    }

    // Storage Index pruning
    if (m_use_storage_index && !m_scan_predicates.empty()) {
      std::lock_guard<std::mutex> lock(m_predicate_mutex);
      if (imcu->can_skip_imcu(m_scan_predicates)) {
        st.curr_imcu_idx++;
        st.curr_imcu_offset = 0;
        continue;
      }
    }

    auto collector_func = [&](row_id_t rowid, const std::vector<const uchar *> &row_data) {
      recv.on_row(rowid, row_data);
    };
    size_t scanned = imcu->scan_range(m_scan_context.get(), st.curr_imcu_offset, remaining, m_scan_predicates,
                                      projection_cols, collector_func);

    remaining -= scanned;
    st.curr_row_idx.fetch_add(scanned, std::memory_order_release);

    size_t imcu_rows = imcu->get_row_count();
    if (st.curr_imcu_offset + scanned >= imcu_rows) {
      st.curr_imcu_idx++;
      st.curr_imcu_offset = 0;
    } else {
      st.curr_imcu_offset += scanned;
      if (remaining == 0) break;  // If we got what we needed, break
    }
  }

  if (st.curr_imcu_idx >= m_rpd_table->meta().total_imcus) {
    st.exhausted.store(true, std::memory_order_release);
  }

  recv.on_batch_end();
  return batch_size - remaining;
}

void ColumnChunkRecv::on_row(row_id_t, const std::vector<const uchar *> &row_data) {
  for (size_t idx = 0; idx < projection_cols.size(); ++idx) {
    auto col_idx = projection_cols[idx];
    auto &chunk = chunks[col_idx];
    auto normal_len = cursor->table()->meta().fields[col_idx].normalized_length;
    chunk.add(row_data[idx], normal_len, row_data[idx] == nullptr);
  }
  ++read_cnt;
}
}  // namespace Imcs
}  // namespace ShannonBase