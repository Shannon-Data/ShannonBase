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

#include <map>
#include <sstream>
#include <thread>
#include <utility>  // std::pair

#include "include/my_base.h"  //key_range
#include "include/ut0dbg.h"   //ut_a
#include "sql/field.h"        //field
#include "sql/table.h"        //TABLE

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/log_commons.h"  //shannon_pop_buff
#include "storage/rapid_engine/populate/log_populate.h"

#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction
#include "storage/rapid_engine/utils/utils.h"      //Blob

#include "storage/rapid_engine/imcs/cu.h"         //CU
#include "storage/rapid_engine/imcs/imcs.h"       //IMCS
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
  if (m_inited.load(std::memory_order_acquire)) return end();
  reset_index_runtime_state(true);
  clear_start_range();
  clear_end_range();
  m_scan_state.reset();
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::init() {
  if (m_inited.load(std::memory_order_acquire)) return ShannonBase::SHANNON_SUCCESS;

  m_scan_context = std::make_unique<Rapid_scan_context>();
  m_scan_context->m_thd = current_thd;
  m_scan_context->m_extra_info.m_keynr = m_active_index;

  m_scan_context->m_trx = ShannonBase::Transaction::get_or_create_trx(current_thd);
  if (m_scan_context->m_trx == nullptr) return HA_ERR_GENERIC;
  if (m_scan_context->m_trx->begin() != ShannonBase::SHANNON_SUCCESS) return HA_ERR_GENERIC;

  m_scan_context->m_extra_info.m_trxid = m_scan_context->m_trx->get_id();
  m_scan_context->m_extra_info.m_scn = TransactionCoordinator::instance().get_current_scn();

  ::ReadView *read_view = m_scan_context->m_trx->acquire_snapshot();
  if (m_scan_context->m_trx->isolation_level() > ShannonBase::Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED &&
      read_view == nullptr) {
    return HA_ERR_GENERIC;
  }

  /*
    HeatWave-style query-demand synchronization. The ordering is a correctness
    requirement, not merely an optimization:

      1. acquire the exact primary InnoDB ReadView;
      2. capture the table's current propagation watermark;
      3. trigger propagation and wait only for that watermark;
      4. start the Rapid scan.

    Capturing the watermark before ReadView would allow a primary transaction
    to commit between the two operations: the new ReadView could then see that
    transaction while Rapid had not been required to materialize it.

    We deliberately do not recapture the watermark while waiting. DML that is
    enqueued after this point cannot become newly visible in the already-fixed
    ReadView, so chasing later changes would only starve analytical queries
    under continuous OLTP traffic.
  */
  RpdTable *barrier_table = m_rpd_table != nullptr ? m_rpd_table : m_src_rpd_table;
  if (barrier_table == nullptr) return HA_ERR_GENERIC;

  const table_id_t table_id = barrier_table->meta().table_id;
  const auto barrier = ShannonBase::Populate::Populator::request_table_barrier(table_id);
  if (barrier.state == ShannonBase::Populate::TablePropagationState::BROKEN) {
    my_error(ER_SECONDARY_ENGINE, MYF(0),
             "Rapid table has failed DML propagation and must be reloaded before secondary-engine reads");
    return HA_ERR_GENERIC;
  }

  if (barrier.needs_wait()) {
    for (;;) {
      if (current_thd != nullptr && current_thd->killed) return HA_ERR_GENERIC;

      const auto wait_result = ShannonBase::Populate::Populator::wait_table_applied_for(
          table_id, barrier.required_change_id, ShannonBase::Populate::QUERY_PROPAGATION_WAIT_SLICE_MS);

      if (wait_result == ShannonBase::Populate::TablePropagationWaitResult::APPLIED) break;

      if (wait_result == ShannonBase::Populate::TablePropagationWaitResult::BROKEN) {
        my_error(ER_SECONDARY_ENGINE, MYF(0),
                 "Rapid table DML propagation failed while waiting for the query watermark; reload required");
        return HA_ERR_GENERIC;
      }

      if (wait_result == ShannonBase::Populate::TablePropagationWaitResult::GONE) {
        my_error(ER_SECONDARY_ENGINE, MYF(0), "Rapid table was unloaded while waiting for DML propagation");
        return HA_ERR_GENERIC;
      }

      // PENDING after the timed condition-variable wait is not an error. It is
      // only a chance to observe query cancellation or Populator shutdown.
      if (!ShannonBase::Populate::Populator::active()) {
        my_error(ER_SECONDARY_ENGINE, MYF(0),
                 "Rapid change propagation stopped before the query watermark was applied");
        return HA_ERR_GENERIC;
      }
    }
  }

  switch_scan_imcus(m_rpd_table);

  m_scan_context->m_schema_name = const_cast<char *>(m_data_source->s->db.str);
  m_scan_context->m_table_name = const_cast<char *>(m_data_source->s->table_name.str);
  m_scan_context->limit = 0;
  m_scan_context->rows_returned = 0;

  init_col_chunks();

  m_scan_state.reset();
  m_rows_skipped = 0;
  m_rows_returned = 0;
  m_last_returned_rowid = INVALID_ROW_ID;

  m_projection_columns.clear();
  m_proj_cols_cache.clear();
  m_proj_cols_dirty = true;
  m_scan_predicates.clear();
  m_use_storage_index = true;
  m_scan_limit = HA_POS_ERROR;
  m_scan_offset = 0;

  m_inited.store(true, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::end() {
  if (!m_inited.load(std::memory_order_acquire)) {
    reset_index_runtime_state(true);
    clear_start_range();
    clear_end_range();
    return ShannonBase::SHANNON_SUCCESS;
  }

  reset_index_runtime_state(true);
  clear_start_range();
  clear_end_range();

  for (auto &imcu : m_scan_imcus) {
    if (imcu) imcu->release_reader();
  }
  m_scan_imcus.clear();

  m_scan_state.reset();
  m_rows_skipped = 0;
  m_rows_returned = 0;
  m_last_returned_rowid = INVALID_ROW_ID;

  m_inited.store(false, std::memory_order_release);
  return ShannonBase::SHANNON_SUCCESS;
}

void RapidCursor::reset_scan() {
  // Rewind scan position only — keep the transaction, snapshot, and IMCU readers alive.
  m_scan_state.reset();
  m_rows_skipped = 0;
  m_rows_returned = 0;
  m_last_returned_rowid = INVALID_ROW_ID;

  // Re-init column chunks so the vectorized scan sees fresh buffers.
  init_col_chunks();

  if (m_scan_context) {
    m_scan_context->limit = 0;
    m_scan_context->rows_returned = 0;
  }
}

void RapidCursor::switch_scan_imcus(RpdTable *new_table) {
  for (auto &imcu : m_scan_imcus) {
    if (imcu) imcu->release_reader();
  }
  m_scan_imcus.clear();

  m_rpd_table = new_table;
  if (!m_rpd_table) return;

  // Acquire readers on a consistent snapshot.  A concurrent compact may swap
  // an IMCU between get_imcus() and acquire; retry with a fresh snapshot.
  for (;;) {
    std::vector<std::shared_ptr<Imcu>> snapshot = m_rpd_table->get_imcus();

    bool all_acquired = true;
    size_t acquired = 0;
    for (auto &imcu : snapshot) {
      if (!imcu) continue;
      if (!imcu->try_acquire_reader()) {
        all_acquired = false;
        break;
      }
      ++acquired;
    }

    if (all_acquired) {
      m_scan_imcus = std::move(snapshot);
      return;
    }

    // Release the readers acquired so far and retry with a fresh snapshot.
    size_t to_release = acquired;
    for (auto &imcu : snapshot) {
      if (!imcu) continue;
      if (to_release == 0) break;
      imcu->release_reader();
      --to_release;
    }
    std::this_thread::yield();
  }
}

void RapidCursor::active_table(RpdTable *rpd_table) {
  const bool had_active_index = (m_active_index != MAX_KEY);
  reset_index_runtime_state(false);
  switch_scan_imcus(rpd_table);
  m_scan_state.reset();

  if (had_active_index) (void)bind_active_index_iterator();
}

void RapidCursor::clear_start_range() {
  m_start_range_length = 0;
  m_start_range_keypart_map = 0;
  m_start_range_flag = ha_rkey_function{};
  m_has_start_range = false;
}

void RapidCursor::set_start_range(const key_range *start_range) {
  clear_start_range();
  if (!start_range || start_range->key == nullptr || start_range->length == 0) return;

  m_start_range_length = start_range->length;
  m_start_range_keypart_map = static_cast<uint64_t>(start_range->keypart_map);
  m_start_range_flag = start_range->flag;
  m_has_start_range = true;
}

void RapidCursor::clear_end_range() {
  m_end_range_key.clear();
  m_end_range_length = 0;
  m_end_range_storage_length = 0;
  m_end_range_keypart_map = 0;
  m_end_range_flag = ha_rkey_function{};
  m_has_end_range = false;
  m_end_range_key_complete = false;
  m_end_key.clear();
}

void RapidCursor::set_end_range(const key_range *end_range) {
  clear_end_range();
  if (!end_range || end_range->length == 0 || end_range->key == nullptr) return;

  uint copy_length = 0;
  if (m_rpd_table != nullptr && m_active_index != MAX_KEY) {
    const auto *index_desc = m_rpd_table->get_art_index_descriptor(static_cast<uint>(m_active_index));
    if (index_desc != nullptr && index_desc->supports_ordered_access() &&
        Index::RapidKeyCodec::RequiredSearchImageLength(*index_desc, end_range->key, end_range->length, &copy_length)) {
      // Deep-copy exactly the bytes MySQL's comparator may inspect. In
      // particular, do not blindly copy a full store_length for a compact
      // final VARCHAR/BLOB boundary: Field::key_cmp() reads only its 2-byte
      // length prefix plus the actual payload.
      m_end_range_key_complete = true;
    }
  }

  if (!m_end_range_key_complete || copy_length == 0) {
    // The range cannot be represented safely by the compiled descriptor. Keep
    // the logical metadata so index_read() can fail closed; do not guess how
    // many bytes exist beyond key_range::length.
    copy_length = end_range->length;
  }

  m_end_range_key.assign(end_range->key, end_range->key + copy_length);
  m_end_range_length = end_range->length;
  m_end_range_storage_length = copy_length;
  m_end_range_keypart_map = static_cast<uint64_t>(end_range->keypart_map);
  m_end_range_flag = end_range->flag;
  m_has_end_range = true;
}

void RapidCursor::reset_index_runtime_state(bool clear_active_index) {
  m_index_iter.reset();
  m_key.clear();
  m_end_key.clear();
  for (auto &chunk : m_col_chunks) chunk.clear();
  m_batch_row_ids.clear();
  m_scan_state.batch_size = 0;
  m_scan_state.row_in_batch = 0;
  m_scan_state.key_rowid = 0;
  if (clear_active_index) m_active_index = MAX_KEY;
}

bool RapidCursor::bind_active_index_iterator() {
  m_index_iter.reset();
  if (!m_rpd_table || m_active_index == MAX_KEY) return false;

  const uint keynr = static_cast<uint>(m_active_index);
  const auto *index_desc = m_rpd_table->get_art_index_descriptor(keynr);
  auto *index = m_rpd_table->get_index(keynr);
  if (index_desc == nullptr || index == nullptr || !index->initialized()) return false;

  m_index_iter.reset(new Index::Art_Iterator(index->impl()));
  return true;
}

void RapidCursor::init_col_chunks() {
  const size_t cap =
      std::max(static_cast<size_t>(SHANNON_BATCH_NUM), static_cast<size_t>(ShannonBase::SHANNON_ROWS_IN_CHUNK));
  const uint nfields = m_data_source->s->fields;

  if (m_col_chunks.size() != nfields) {  // Schema changed or first call: full rebuild.
    m_col_chunks.clear();
    m_col_chunks.reserve(nfields);
    for (uint ind = 0; ind < nfields; ++ind) {
      Field *fld = m_data_source->field[ind];
      const bool explicitly_projected =
          std::find(m_projection_columns.begin(), m_projection_columns.end(), ind) != m_projection_columns.end();
      const bool active = (bitmap_is_set(m_data_source->read_set, ind) || explicitly_projected) &&
                          !fld->is_flag_set(NOT_SECONDARY_FLAG);
      m_col_chunks.emplace_back(active ? fld : nullptr, active ? cap : 0);
    }
  } else {
    for (uint ind = 0; ind < nfields; ++ind) {
      Field *fld = m_data_source->field[ind];
      const bool explicitly_projected =
          std::find(m_projection_columns.begin(), m_projection_columns.end(), ind) != m_projection_columns.end();
      const bool active = (bitmap_is_set(m_data_source->read_set, ind) || explicitly_projected) &&
                          !fld->is_flag_set(NOT_SECONDARY_FLAG);
      m_col_chunks[ind].reset(active ? fld : nullptr, active ? cap : 0);
    }
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
    const bool secondary = !fld->is_flag_set(NOT_SECONDARY_FLAG);
    const bool in_read_set = bitmap_is_set(m_data_source->read_set, idx) && secondary;
    const bool in_explicit =
        std::find(m_projection_columns.begin(), m_projection_columns.end(), idx) != m_projection_columns.end() &&
        secondary;
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

    if (Utils::Util::is_string(fld->type()) || Utils::Util::is_varlen(fld->type())) {
      // String / BLOB path (mirrors ProcessStringField)
      if (fld->real_type() == MYSQL_TYPE_ENUM || fld->real_type() == MYSQL_TYPE_SET) {
        fld->pack(const_cast<uchar *>(fld->data_ptr()), chunk.data(row_idx), fld->pack_length());
      } else {
        Utils::ColumnMapGuard guard(fld->table, Utils::ColumnMapGuard::TYPE::WRITE);
        // BLOB / TEXT must go through VarlenPool — never dictionary-encoded.
        if (Utils::Util::is_varlen(fld->type())) {
          auto [data_ptr, data_len] = resolve_blob_from_chunk(col_idx, row_idx);
          if (data_ptr && data_len > 0 && data_len != UNIV_SQL_NULL) {
            // Data from InnoDB is already in binary format; use base-class
            // store to bypass type-specific parsing (e.g. JSON text parse).
            Utils::Util::store_blob_data(fld, reinterpret_cast<const char *>(data_ptr), data_len);
          } else {
            fld->reset();
          }
        } else {
          auto dict = m_rpd_table->meta().fields[col_idx].dictionary;
          if (dict) {
            auto str_id = *reinterpret_cast<const uint32 *>(chunk.data(row_idx));
            const auto &str_val = dict->get(str_id);
            fld->store(str_val.c_str(), str_val.size(), fld->charset());
          } else {
            // Non-dictionary-encoded string: data is stored inline in the
            // chunk.  Copy it directly to the Field.
            fld->store(reinterpret_cast<const char *>(chunk.data(row_idx)), chunk.width(), fld->charset());
          }
        }
      }
    } else {
      // Fixed-width CUs store the bytes copied from TABLE::record[] verbatim.
      // Field::pack() converts a record image to MySQL's packed/transfer image;
      // using it in the reverse direction corrupts values whose packed format is
      // not byte-identical to the record format (notably high-bit BIGINT UNSIGNED).
      const size_t record_len = fld->pack_length();
      if (chunk.width() < record_len) return HA_ERR_GENERIC;
      std::memcpy(const_cast<uchar *>(fld->data_ptr()), chunk.data(row_idx), record_len);
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

std::pair<const uchar *, size_t> RapidCursor::resolve_blob_from_chunk(uint32_t col_idx, size_t row_in_batch) {
  const auto &chunk = m_col_chunks[col_idx];
  VarlenDataPool::VarlenReference ref{};
  std::memcpy(&ref, chunk.data(row_in_batch), std::min(sizeof(ref), chunk.width()));

  if (ref.is_inline()) {
    // The slot must be at least one VarlenReference large and the recorded
    // inline length must actually fit in the slot's payload area, otherwise a
    // corrupt/truncated ref could make the caller read past the chunk buffer.
    if (chunk.width() < sizeof(ref)) return {nullptr, 0};
    const size_t available = chunk.width() - sizeof(ref);
    if (ref.length > available) return {nullptr, 0};
    const uchar *inline_data = chunk.data(row_in_batch) + sizeof(ref);
    return {inline_data, ref.length};
  }

  if (row_in_batch >= m_batch_row_ids.size()) return {nullptr, 0};
  const row_id_t global_row_id = m_batch_row_ids[row_in_batch];

  for (const auto &imcu : m_scan_imcus) {
    if (!imcu) continue;
    const auto start = imcu->get_start_row();
    const auto cap = imcu->get_capacity();
    if (global_row_id < start || global_row_id >= start + cap) continue;

    auto *cu = imcu->get_cu(col_idx);
    if (!cu) return {nullptr, 0};

    // Copy the payload out of the pool under the pool lock.  The returned
    // pointer aliases m_blob_scratch, which stays valid for the duration of
    // populate_row_from_chunks() (it is never retained past this call).
    auto *pool = cu->get_varlen_pool();
    if (pool) {
      m_blob_scratch.resize(ref.length);
      size_t copied = 0;
      if (!pool->copy_data(ref, m_blob_scratch.data(), m_blob_scratch.size(), copied) || copied != ref.length) {
        return {nullptr, 0};
      }
      return {m_blob_scratch.data(), copied};
    }
    // Fallback for CU without a varlen pool (should not happen for
    // pool references, but handle gracefully).
    return {nullptr, 0};
  }

  return {nullptr, 0};
}

int RapidCursor::next(uchar *buf) {
  // assert(m_inited.load(std::memory_order_acquire));
  if (!m_inited.load(std::memory_order_acquire)) init();

  // Refill the column-chunk batch whenever the current one is exhausted.
  if (m_scan_state.is_exhausted()) {
    if (m_scan_state.exhausted.load(std::memory_order_acquire)) return HA_ERR_END_OF_FILE;

    for (auto &chunk : m_col_chunks) chunk.clear();

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
      ha_rows rows_received() const { return skipped; }
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
row_id_t RapidCursor::position(const unsigned char *) {
  // handler::position() may be called even when the primary-key columns are
  // absent from TABLE::read_set (Duplicate Weedout for SELECT COUNT(*) is one
  // example). Reconstructing the row id from the record is therefore unsafe
  // and also mutates the active index iterator. Every successful row and
  // batch read already records the physical row id, so use that stable value.
  return m_last_returned_rowid;
}

int RapidCursor::rnd_pos(uchar *buff, uchar *pos) {
  row_id_t rowid{0};
  std::memcpy(&rowid, pos, sizeof(row_id_t));

  size_t rows_per_imcu = m_rpd_table->meta().rows_per_imcu;
  for (auto &chunk : m_col_chunks) chunk.clear();
  m_batch_row_ids.clear();

  const auto &proj = projection_columns();
  const size_t imcu_idx = rowid / rows_per_imcu;
  if (imcu_idx >= m_scan_imcus.size()) return HA_ERR_KEY_NOT_FOUND;
  auto imcu = m_scan_imcus[imcu_idx];
  if (!imcu) return HA_ERR_KEY_NOT_FOUND;

  std::vector<uint32_t> offsets = {static_cast<uint32_t>(rowid % rows_per_imcu)};
  size_t start_cnt = 0;
  ColumnChunkRecv receiver{this, proj, m_col_chunks, m_batch_row_ids, start_cnt};
  auto collector = [&](row_id_t rid, const std::vector<const uchar *> &row_data) { receiver.on_row(rid, row_data); };
  imcu->scan_rows_vectorized(m_scan_context.get(), offsets, m_scan_predicates, proj, collector);

  if (m_batch_row_ids.empty()) return HA_ERR_KEY_NOT_FOUND;

  m_scan_state.commit_batch(1);
  int status = populate_row_from_chunks(0);
  if (status) return HA_ERR_GENERIC;
  m_last_returned_rowid = m_batch_row_ids[0];
  m_scan_state.advance_row();
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::locate(row_id_t start_row_id) {
  m_scan_state.seek(start_row_id, m_rpd_table->meta().rows_per_imcu);
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::index_init(uint keynr, bool sorted) {
  (void)sorted;
  if (!m_rpd_table) return HA_ERR_WRONG_INDEX;

  const auto *index_desc = m_rpd_table->get_art_index_descriptor(keynr);
  auto *index = m_rpd_table->get_index(keynr);
  if (index_desc == nullptr || index == nullptr || !index->initialized()) {
    std::ostringstream oss;
    oss << m_data_source->s->db.str << "." << m_data_source->s->table_name.str << " index not found";
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), oss.str().c_str());
    return HA_ERR_KEY_NOT_FOUND;
  }

  m_active_index = static_cast<int8_t>(keynr);
  int ret = init();
  if (ret != ShannonBase::SHANNON_SUCCESS) {
    m_active_index = MAX_KEY;
    return ret;
  }
  if (m_scan_context) m_scan_context->m_extra_info.m_keynr = m_active_index;

  reset_index_runtime_state(false);
  if (!bind_active_index_iterator()) {
    (void)end();
    return HA_ERR_KEY_NOT_FOUND;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidCursor::index_end() { return end(); }

int RapidCursor::materialize_index_candidate(row_id_t rowid, bool emit_row) {
  if (rowid == INVALID_ROW_ID || !m_rpd_table || !m_scan_context) return HA_ERR_KEY_NOT_FOUND;

  const size_t rows_per_imcu = m_rpd_table->meta().rows_per_imcu;
  const size_t imcu_idx = static_cast<size_t>(rowid / rows_per_imcu);
  if (imcu_idx >= m_scan_imcus.size()) return HA_ERR_KEY_NOT_FOUND;
  auto imcu = m_scan_imcus[imcu_idx];
  if (!imcu) return HA_ERR_KEY_NOT_FOUND;

  for (auto &chunk : m_col_chunks) chunk.clear();
  m_batch_row_ids.clear();
  const auto &proj = projection_columns();
  std::vector<uint32_t> offsets = {static_cast<uint32_t>(rowid % rows_per_imcu)};
  size_t start_cnt = 0;
  ColumnChunkRecv receiver{this, proj, m_col_chunks, m_batch_row_ids, start_cnt};
  auto collector = [&](row_id_t rid, const std::vector<const uchar *> &row_data) { receiver.on_row(rid, row_data); };
  imcu->scan_rows_vectorized(m_scan_context.get(), offsets, m_scan_predicates, proj, collector);

  // ART entries intentionally outlive row versions/deletes. An invisible
  // candidate is therefore not equivalent to "key not present"; the caller
  // must advance to the next physical ART candidate.
  if (m_batch_row_ids.empty()) return HA_ERR_KEY_NOT_FOUND;
  if (!emit_row) return ShannonBase::SHANNON_SUCCESS;

  m_scan_state.commit_batch(m_batch_row_ids.size());
  int status = populate_row_from_chunks(0);
  if (status) return HA_ERR_GENERIC;
  m_last_returned_rowid = m_batch_row_ids[0];
  m_scan_state.advance_row();
  return ShannonBase::SHANNON_SUCCESS;
}

namespace {

bool MysqlKeyPartMapLength(const ArtIndexDescriptor &index_desc, uint64_t keypart_map, uint *key_len) {
  if (key_len == nullptr || keypart_map == 0) return false;

  uint length = 0;
  size_t part_no = 0;
  while (keypart_map != 0) {
    // MySQL only accepts prefix keypart maps for index_read_map().
    if ((keypart_map & 1ULL) == 0 || part_no >= index_desc.parts.size()) return false;
    length += index_desc.parts[part_no].store_length;
    keypart_map >>= 1;
    ++part_no;
  }

  *key_len = length;
  return length != 0;
}

}  // namespace

// index read.
int RapidCursor::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag, bool navigation) {
  ut_a(m_active_index != MAX_KEY);
  if (key_len == 0 && key != nullptr) return HA_ERR_WRONG_COMMAND;

  const auto *index_desc =
      m_rpd_table == nullptr ? nullptr : m_rpd_table->get_art_index_descriptor(static_cast<uint>(m_active_index));
  if (index_desc == nullptr) return HA_ERR_WRONG_INDEX;

  const bool ordered = index_desc->supports_ordered_access();
  const bool physical_first = find_flag == HA_READ_KEY_OR_NEXT && key == nullptr && key_len == 0;
  if (!ordered && find_flag != HA_READ_KEY_EXACT && !physical_first) {
    return HA_ERR_WRONG_COMMAND;
  }
  if (!ordered && (m_has_start_range || m_has_end_range)) return HA_ERR_WRONG_COMMAND;

  // index_read_map() expands keypart_map to complete store_length bytes. Keep
  // that expanded key_len as the backing-image size, but preserve the original
  // key_range::length as the logical MySQL comparison boundary.
  uint logical_key_len = key_len;
  if (m_has_start_range) {
    uint mapped_key_len = 0;
    const bool valid_start = m_start_range_flag == find_flag && m_start_range_length <= index_desc->key_length &&
                             MysqlKeyPartMapLength(*index_desc, m_start_range_keypart_map, &mapped_key_len) &&
                             m_start_range_length <= mapped_key_len && mapped_key_len == key_len;
    if (valid_start) logical_key_len = m_start_range_length;
    clear_start_range();
    if (!valid_start) return HA_ERR_WRONG_COMMAND;
  }

  m_key.clear();
  if (key_len != 0 && !Index::RapidKeyCodec::EncodeSearchKey(*index_desc, key, logical_key_len, key_len, &m_key)) {
    return HA_ERR_WRONG_COMMAND;
  }

  const uchar *start_ptr = m_key.empty() ? nullptr : m_key.data();
  const uint start_len = static_cast<uint>(m_key.size());

  const uchar *end_ptr = nullptr;
  uint end_len = 0;
  bool end_incl = false;
  m_end_key.clear();
  if (m_has_end_range && m_end_range_length > 0) {
    uint compare_image_len = 0;
    if (!m_end_range_key_complete ||
        !Index::RapidKeyCodec::RequiredSearchImageLength(*index_desc, m_end_range_key.data(), m_end_range_length,
                                                         &compare_image_len) ||
        compare_image_len > m_end_range_storage_length || compare_image_len > m_end_range_key.size() ||
        !Index::RapidKeyCodec::EncodeSearchKey(*index_desc, m_end_range_key.data(), m_end_range_length,
                                               m_end_range_storage_length, &m_end_key)) {
      return HA_ERR_WRONG_COMMAND;
    }
    end_ptr = m_end_key.data();
    end_len = static_cast<uint>(m_end_key.size());
    // MySQL end-range flags map to inclusivity as follows:
    //   HA_READ_BEFORE_KEY  -> strict less-than  (< X): key == X is out of range
    //   HA_READ_AFTER_KEY   -> less-or-equal     (<= X): key == X is in range
    //   HA_READ_KEY_EXACT   -> single key match  (== X)
    end_incl = (m_end_range_flag != HA_READ_BEFORE_KEY);
  }

  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  bool reverse = false;
  switch (find_flag) {
    case HA_READ_KEY_EXACT: {  // (=), including a multipart-prefix equality range.
      if (!start_ptr) return HA_ERR_WRONG_COMMAND;
      m_index_iter->init_scan(start_ptr, start_len, true, start_ptr, start_len, true);
    } break;

    case HA_READ_KEY_OR_NEXT: {  // (>=), nullptr means index_first().
      m_index_iter->init_scan(start_ptr, start_len, true, end_ptr, end_len, end_incl);
    } break;

    case HA_READ_AFTER_KEY: {  // (>). For a multipart prefix, skip the whole prefix group.
      if (!start_ptr) return HA_ERR_WRONG_COMMAND;
      m_index_iter->init_scan(start_ptr, start_len, false, end_ptr, end_len, end_incl);
    } break;

    case HA_READ_KEY_OR_PREV: {  // (<=), native ART seek_le().
      if (!start_ptr) return HA_ERR_WRONG_COMMAND;
      m_index_iter->init_reverse_scan(nullptr, 0, false, start_ptr, start_len, true);
      reverse = true;
    } break;

    case HA_READ_BEFORE_KEY: {  // (<), nullptr means index_last().
      m_index_iter->init_reverse_scan(nullptr, 0, false, start_ptr, start_len, false);
      reverse = true;
    } break;

    case HA_READ_PREFIX_LAST: {
      if (!start_ptr) return HA_ERR_WRONG_COMMAND;
      // [prefix, prefix] plus reverse positioning lands directly on the
      // rightmost physical key/value belonging to the prefix equivalence class.
      m_index_iter->init_reverse_scan(start_ptr, start_len, true, start_ptr, start_len, true);
      reverse = true;
    } break;

    default:
      return HA_ERR_WRONG_COMMAND;
  }

  const uchar *result_key = nullptr;
  uint32_t result_key_len = 0;
  row_id_t rowid = INVALID_ROW_ID;

  for (;;) {
    const bool found = reverse ? m_index_iter->prev(&result_key, &result_key_len, &rowid)
                               : m_index_iter->next(&result_key, &result_key_len, &rowid);
    if (!found) return HA_ERR_KEY_NOT_FOUND;

    // ART entries intentionally outlive some row versions/deletes.  Keep the
    // physical cursor on the candidate actually examined and skip candidates
    // that are not visible in the bound InnoDB ReadView.
    const int status = materialize_index_candidate(rowid, !navigation);
    if (status == HA_ERR_KEY_NOT_FOUND) continue;
    if (status != ShannonBase::SHANNON_SUCCESS) return status;
    if (navigation) m_scan_state.key_rowid = rowid;
    return ShannonBase::SHANNON_SUCCESS;
  }
}

int RapidCursor::index_next(uchar * /*buf*/) {
  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  const uchar *key = nullptr;
  uint32_t key_len = 0;
  row_id_t rowid = INVALID_ROW_ID;
  while (m_index_iter->next(&key, &key_len, &rowid)) {
    const int status = materialize_index_candidate(rowid, true);
    if (status == HA_ERR_KEY_NOT_FOUND) continue;
    if (status != ShannonBase::SHANNON_SUCCESS) return status;
    m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);
    return ShannonBase::SHANNON_SUCCESS;
  }
  return HA_ERR_END_OF_FILE;
}

int RapidCursor::index_prev(uchar * /*buf*/) {
  if (!m_index_iter) return HA_ERR_INTERNAL_ERROR;

  const auto *index_desc =
      m_rpd_table == nullptr ? nullptr : m_rpd_table->get_art_index_descriptor(static_cast<uint>(m_active_index));
  if (index_desc == nullptr || !index_desc->supports_ordered_access()) return HA_ERR_WRONG_COMMAND;

  const uchar *key = nullptr;
  uint32_t key_len = 0;
  row_id_t rowid = INVALID_ROW_ID;
  while (m_index_iter->prev(&key, &key_len, &rowid)) {
    const int status = materialize_index_candidate(rowid, true);
    if (status == HA_ERR_KEY_NOT_FOUND) continue;
    if (status != ShannonBase::SHANNON_SUCCESS) return status;
    m_total_rows_scanned.fetch_add(1, std::memory_order_relaxed);
    return ShannonBase::SHANNON_SUCCESS;
  }
  return HA_ERR_END_OF_FILE;
}

row_id_t RapidCursor::find(uchar *buf) {
  // Not implemented.  Returning INVALID_ROW_ID (rather than a valid-looking 0)
  // keeps an unsupported operation from masquerading as a successful lookup.
  (void)buf;
  return INVALID_ROW_ID;
}

template <typename Reciever>
size_t RapidCursor::scan_batch_internal(size_t batch_size, const std::vector<uint32_t> &projection_cols,
                                        Reciever &recv) {
  auto &st = m_scan_state;
  size_t remaining = batch_size;

  recv.on_batch_begin();

  while (st.curr_imcu_idx < m_scan_imcus.size() && remaining > 0 && recv.accept_more()) {
    auto imcu = m_scan_imcus[st.curr_imcu_idx];
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
    size_t rows_before = recv.rows_received();
    size_t rows_examined = imcu->scan(m_scan_context.get(), st.curr_imcu_offset, remaining, m_scan_predicates,
                                      projection_cols, collector_func);
    size_t rows_returned = recv.rows_received() - rows_before;

    // Quota tracking uses rows actually returned (callback → on_row).
    remaining -= std::min(remaining, rows_returned);
    // Row-index accounting uses rows examined (all rows the scan touched).
    st.curr_row_idx.fetch_add(rows_examined, std::memory_order_release);

    size_t imcu_rows = imcu->get_row_count();
    if (st.curr_imcu_offset + rows_examined >= imcu_rows) {
      st.curr_imcu_idx++;
      st.curr_imcu_offset = 0;
    } else {
      st.curr_imcu_offset += rows_examined;
      if (remaining == 0) break;  // If we got what we needed, break
    }
  }

  if (st.curr_imcu_idx >= m_scan_imcus.size()) {
    st.exhausted.store(true, std::memory_order_release);
  }

  recv.on_batch_end();
  return batch_size - remaining;
}

void ColumnChunkRecv::on_row(row_id_t rowid, const std::vector<const uchar *> &row_data) {
  for (size_t idx = 0; idx < projection_cols.size(); ++idx) {
    auto col_idx = projection_cols[idx];
    if (col_idx >= chunks.size()) continue;
    auto &chunk = chunks[col_idx];
    auto normal_len = cursor->table()->meta().fields[col_idx].normalized_length;

    if (!chunk.add(row_data[idx], normal_len, row_data[idx] == nullptr)) return;  // chunk full
  }
  row_ids.push_back(rowid);
  ++read_cnt;
}
}  // namespace Imcs
}  // namespace ShannonBase
