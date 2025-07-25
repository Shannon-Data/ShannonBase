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
#include "storage/rapid_engine/imcs/data_table.h"

#include <sstream>

#include "include/ut0dbg.h"  //ut_a
#include "sql/field.h"       //field
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/imcs/chunk.h"  //CHUNK
#include "storage/rapid_engine/imcs/cu.h"     //CU
#include "storage/rapid_engine/imcs/imcs.h"   //IMCS
#include "storage/rapid_engine/imcs/table.h"  //RapidTable
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"  //sys_pop_buff
#include "storage/rapid_engine/trx/readview.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction
#include "storage/rapid_engine/utils/utils.h"      //Blob

namespace ShannonBase {
namespace Imcs {
ShannonBase::Utils::SimpleRatioAdjuster DataTable::m_adaptive_ratio(0.3);

DataTable::DataTable(TABLE *source, RapidTable *rpd) : m_initialized{false}, m_data_source(source), m_rapid_table(rpd) {
  ut_a(m_data_source && m_rapid_table);
  m_rowid.store(0);
}

DataTable::~DataTable() {
  if (m_context && m_context->m_trx) {
    m_context->m_trx->release_snapshot();
    m_context->m_trx->commit();
  }
}

int DataTable::open() {
  m_rowid.store(0);
  return ShannonBase::SHANNON_SUCCESS;
}

int DataTable::close() {
  m_rowid.store(0);
  return ShannonBase::SHANNON_SUCCESS;
}

int DataTable::init() {
  assert(!m_initialized.load());
  if (!m_initialized.load()) {
    m_initialized.store(true);
    m_rowid.store(0);

    m_context = std::make_unique<Rapid_load_context>();
    m_context->m_thd = current_thd;
    m_context->m_extra_info.m_keynr = m_active_index;

    m_context->m_trx = ShannonBase::Transaction::get_or_create_trx(current_thd);
    m_context->m_trx->set_read_only(true);
    if (!m_context->m_trx->is_active())
      m_context->m_trx->begin(ShannonBase::Transaction::get_rpd_isolation_level(current_thd));

    m_context->m_trx->acquire_snapshot();

    m_context->m_schema_name = const_cast<char *>(m_data_source->s->db.str);
    m_context->m_table_name = const_cast<char *>(m_data_source->s->table_name.str);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

DataTable::FETCH_STATUS DataTable::fetch_next_row(ulong current_chunk, ulong offset_in_chunk, Field *source_fld) {
  auto rpd_field = m_rapid_table->get_field(source_fld->field_name);
  ut_a(rpd_field);

  auto normalized_length = rpd_field->normalized_pack_length();
  DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "secondary_engine_rapid_next_error");
    return DataTable::FETCH_STATUS::FETCH_ERROR;
  });

  // to check version link to check its old value.
  auto current_chunk_ptr = rpd_field->chunk(current_chunk);
  auto current_data_ptr = current_chunk_ptr->base() + offset_in_chunk * normalized_length;
  if ((uintptr_t(current_data_ptr) & (CACHE_LINE_SIZE - 1)) == 0)
    SHANNON_PREFETCH_R(current_data_ptr + PREFETCH_AHEAD * CACHE_LINE_SIZE);

  auto data_len = normalized_length;
  auto data_ptr = ensure_buffer_size(data_len);
  std::memcpy(data_ptr, current_data_ptr, data_len);

  uint8 status{0};
  auto versioned_ptr =
      current_chunk_ptr->header()->m_smu->build_prev_vers(m_context.get(), offset_in_chunk, data_ptr, data_len, status);
  if (!versioned_ptr &&
      (status &
       static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_ROLLBACKED))) {  // means rollbacked rows.
    return DataTable::FETCH_STATUS::FETCH_NEXT_ROW;
  }
  if (status &
      static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_NULL)) {  // the original value is null.
    ut_a(data_len == UNIV_SQL_NULL);
    source_fld->set_null();
    return DataTable::FETCH_STATUS::FETCH_CONTINUE;
  }
  if (status & static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_DELETED)) {
    return DataTable::FETCH_STATUS::FETCH_NEXT_ROW;
  }

  source_fld->set_notnull();
  ut_a(source_fld->type() == rpd_field->header()->m_source_fld->type());
  if (Utils::Util::is_string(source_fld->type()) || Utils::Util::is_blob(source_fld->type())) {
    if (source_fld->real_type() == MYSQL_TYPE_ENUM) {
      source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), data_ptr, source_fld->pack_length());
    } else {
      auto str_id = *reinterpret_cast<uint32 *>(data_ptr);
      auto str_ptr = rpd_field->header()->m_local_dict->get(str_id);
      source_fld->store(str_ptr.c_str(), strlen(str_ptr.c_str()), source_fld->charset());
    }
  } else {
    source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), data_ptr, normalized_length);
  }
  return DataTable::FETCH_STATUS::FETCH_OK;
}

boost::asio::awaitable<int> DataTable::next_async(uchar *buf) {
  // In optimization phase. we should not choice rapid to scan, when pop threading
  // is running to repop the data to rapid.
  // ut_a(ShannonBase::Populate::sys_pop_buff.size() == 0);
  // make all ptr in m_field_ptrs to move forward one step(one row).
  assert(m_initialized.load());

  auto field_read_pool = ShannonBase::Imcs::Imcs::pool();
  auto executor = co_await boost::asio::this_coro::executor;
  const size_t n_fields = m_data_source->s->fields;
  const size_t max_batch_sz = m_adaptive_ratio.getMaxBatchSize();

start:
  if (m_rowid >= m_rapid_table->m_fields.begin()->second->prows()) co_return HA_ERR_END_OF_FILE;

  const auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
  const auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;

  for (size_t i = 0; i < n_fields; i += max_batch_sz) {
    Utils::ColumnMapGuard guard(m_data_source);
    size_t end = std::min(i + max_batch_sz, n_fields);

    auto counter = std::make_shared<std::atomic<size_t>>(end - i);
    auto batch_promise = std::make_shared<ShannonBase::Utils::shared_promise<DataTable::FETCH_STATUS>>();
    auto promise_set = std::make_shared<std::atomic<bool>>(false);

    for (size_t sub_idx = i; sub_idx < end; ++sub_idx) {
      auto source_fld = *(m_data_source->field + sub_idx);

      // Skip fields not in read_set or marked as NOT_SECONDARY_FLAG
      if (!bitmap_is_set(m_data_source->read_set, sub_idx) || source_fld->is_flag_set(NOT_SECONDARY_FLAG)) {
        if (counter->fetch_sub(1) == 1 && !promise_set->exchange(true)) {
          batch_promise->set_value(DataTable::FETCH_STATUS::FETCH_OK);
        }
        continue;
      }

      boost::asio::co_spawn(
          *field_read_pool,
          [this, sub_idx, max_batch_sz, current_chunk, offset_in_chunk, source_fld, counter, batch_promise,
           promise_set]() mutable -> boost::asio::awaitable<void> {
            DeferGuard defer([counter, promise_set, batch_promise]() {
              if (counter->fetch_sub(1) == 1 && !promise_set->exchange(true)) {
                // If last coroutine finishes, and no one has set promise
                batch_promise->set_value(DataTable::FETCH_STATUS::FETCH_OK);
              }
            });

            try {
              auto res = fetch_next_row(current_chunk, offset_in_chunk, source_fld);

              if (res == DataTable::FETCH_STATUS::FETCH_NEXT_ROW || res == DataTable::FETCH_STATUS::FETCH_ERROR) {
                if (!promise_set->exchange(true)) {
                  batch_promise->set_value(res);
                }
                co_return;
              }
            } catch (...) {
              if (!promise_set->exchange(true)) {
                batch_promise->set_value(DataTable::FETCH_STATUS::FETCH_ERROR);
              }
              co_return;
            }
            co_return;
          },
          boost::asio::detached);
    }

    // Wait for this batch to complete
    auto batch_result = co_await batch_promise->get_awaitable(executor);
    if (batch_result == DataTable::FETCH_STATUS::FETCH_ERROR) {
      co_return HA_ERR_GENERIC;
    } else if (batch_result == DataTable::FETCH_STATUS::FETCH_NEXT_ROW) {
      m_rowid.fetch_add(1);
      goto start;
    }
  }

  m_rowid.fetch_add(1);
  co_return ShannonBase::SHANNON_SUCCESS;
}

// IMPORTANT NOTIC: IF YOU CHANGE THE CODE HERE, YOU SHOULD CHANGE THE PARTITIAL TABLE `DataTable::next_batch`
// CORRESPONDINGLY.
int DataTable::next(uchar *buf) {
  // In optimization phase. we should not choice rapid to scan, when pop threading
  // is running to repop the data to rapid.
  // ut_a(ShannonBase::Populate::sys_pop_buff.size() == 0);
  // make all ptr in m_field_ptrs to move forward one step(one row).
start:
  assert(m_initialized.load());

  if (m_rowid >= m_rapid_table->m_fields.begin()->second->prows()) return HA_ERR_END_OF_FILE;

  auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;

  Utils::ColumnMapGuard guard(m_data_source);
  for (auto ind = 0u; ind < m_data_source->s->fields; ind++) {
    auto source_fld = *(m_data_source->field + ind);
    if (!bitmap_is_set(m_data_source->read_set, ind) || source_fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    auto res = fetch_next_row(current_chunk, offset_in_chunk, source_fld);
    if (res == DataTable::FETCH_STATUS::FETCH_NEXT_ROW) {
      m_rowid.fetch_add(1);
      goto start;
    } else if (res == DataTable::FETCH_STATUS::FETCH_CONTINUE) {
      continue;
    } else if (res == DataTable::FETCH_STATUS::FETCH_ERROR) {
      return HA_ERR_GENERIC;
    }
  }

  m_rowid.fetch_add(1);
  return ShannonBase::SHANNON_SUCCESS;
}

/** TODO:
 * First perform visibility checks in a row-wise manner, then process column data.
for exam:：
std::vector<rowid_t> visible_rows;
std::vector<RowVersionInfo> row_versions;

while (visible_rows.size() < batch_size) {
    // Perform visibility checks in a unified manner (read versioned_ptr only once).
    auto status = ...;
    if (status is visible) {
        visible_rows.push_back(m_rowid);
        row_versions.push_back(...);
    }
    m_rowid++;
}

// Then, for each column, batch process the visible_rows.
for (col : cols) {
    for (i = 0; i < visible_rows.size(); ++i) {
        auto rid = visible_rows[i];
        // directly decode the column data corresponding to each rid and write it into col_chunk[i].
    }
}
 */
// IMPORTANT NOTIC: IF YOU CHANGE THE CODE HERE, YOU SHOULD CHANGE THE PARTITIAL TABLE `DataTable::next`
// CORRESPONDINGLY.
int DataTable::next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data, size_t &read_cnt) {
start:
  assert(m_initialized.load());
  while (read_cnt < batch_size) {
    if (m_rowid >= m_rapid_table->m_fields.begin()->second->prows()) return HA_ERR_END_OF_FILE;

    auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
    auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;

    for (auto ind = 0u; ind < m_data_source->s->fields; ind++) {
      auto source_fld = *(m_data_source->field + ind);
      if (!bitmap_is_set(m_data_source->read_set, ind) || source_fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

      auto rpd_field = m_rapid_table->get_field(source_fld->field_name);
      ut_a(rpd_field);

      auto &col_chunk = data[ind];
      auto normalized_length = rpd_field->normalized_pack_length();
      DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "secondary_engine_rapid_next_error");
        return HA_ERR_GENERIC;
      });

      // to check version link to check its old value.
      auto current_chunk_ptr = rpd_field->chunk(current_chunk);
      auto current_data_ptr = current_chunk_ptr->base() + offset_in_chunk * normalized_length;
      if ((uintptr_t(current_data_ptr) & (CACHE_LINE_SIZE - 1)) == 0)
        SHANNON_PREFETCH_R(current_data_ptr + PREFETCH_AHEAD * CACHE_LINE_SIZE);

      auto data_len = normalized_length;
      auto data_ptr = ensure_buffer_size(data_len);
      std::memcpy(data_ptr, current_data_ptr, data_len);

      uint8 status{0};
      auto versioned_ptr = current_chunk_ptr->header()->m_smu->build_prev_vers(m_context.get(), offset_in_chunk,
                                                                               data_ptr, data_len, status);
      if (!versioned_ptr &&
          (status & static_cast<uint8>(
                        ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_ROLLBACKED))) {  // means rollbacked rows.
        m_rowid.fetch_add(1);
        goto start;
      }
      if (status &
          static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_NULL)) {  // the original value is null.
        ut_a(data_len == UNIV_SQL_NULL);
        // source_fld->set_null();
        col_chunk.set_null(read_cnt);
        read_cnt++;
        continue;
      }
      if (status & static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_DELETED)) {
        m_rowid.fetch_add(1);
        goto start;
      }

      // source_fld->set_notnull();
      ut_a(source_fld->type() == rpd_field->header()->m_source_fld->type());
      if (Utils::Util::is_string(source_fld->type()) || Utils::Util::is_blob(source_fld->type())) {
        if (source_fld->real_type() == MYSQL_TYPE_ENUM) {
          col_chunk.add(data_ptr, source_fld->pack_length(), false);
        } else {
          uint32 str_id = *reinterpret_cast<uint32 *>(data_ptr);
          auto str_ptr = rpd_field->header()->m_local_dict->get(str_id);
          col_chunk.add((uchar *)str_ptr.c_str(), strlen(str_ptr.c_str()), false);
        }
      } else {
        col_chunk.add(data_ptr, normalized_length, false);
      }
    }

    m_rowid.fetch_add(1);
    read_cnt++;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int DataTable::end() {
  m_context->m_trx->release_snapshot();
  m_context->m_trx->commit();

  m_rowid.store(0);
  m_initialized.store(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int DataTable::index_init(uint keynr, bool sorted) {
  init();
  m_active_index = keynr;
  auto keyname = m_data_source->s->key_info[keynr].name;
  auto index = m_rapid_table->m_indexes[keyname].get();
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

int DataTable::index_end() {
  m_active_index = MAX_KEY;
  return end();
}

// index read.
int DataTable::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) {
  int err{HA_ERR_KEY_NOT_FOUND};

  ut_a(m_active_index != MAX_KEY);
  auto key_info = m_data_source->s->key_info + m_active_index;
  auto offset{0u};

  m_key = std::make_unique<uchar[]>(key_len);
  std::memcpy(m_key.get(), key, key_len);

  if (key) {
    for (auto part = 0u; part < actual_key_parts(key_info); part++) {
      auto key_part_info = key_info->key_part + part;
      offset += (key_part_info->null_bit) ? 1 : 0;
      auto type = key_part_info->field->type();
      if (type == MYSQL_TYPE_DOUBLE || type == MYSQL_TYPE_FLOAT || type == MYSQL_TYPE_DECIMAL ||
          type == MYSQL_TYPE_NEWDECIMAL) {
        uchar encoding[8] = {0};
        double val = Utils::Util::get_field_numeric<double>(key_part_info->field, key + offset, nullptr);
        Utils::Encoder<double>::EncodeFloat(val, encoding);
        std::memcpy(m_key.get() + offset, encoding, key_part_info->length);
      }
      offset += key_part_info->length;
      if (offset > key_len) break;
    }
  }

  switch (find_flag) {
    case HA_READ_KEY_EXACT: {
      if (!m_index_iter->initialized()) m_index_iter->init_scan(m_key.get(), key_len, true, m_key.get(), key_len, true);
    } break;
    case HA_READ_KEY_OR_NEXT:
      if (!m_index_iter->initialized()) m_index_iter->init_scan(m_key.get(), key_len, true, nullptr, 0, false);
      break;
    case HA_READ_KEY_OR_PREV:
      m_index_iter->init_scan(m_key.get(), key_len, true, nullptr, 0, true);
      break;
    case HA_READ_AFTER_KEY:
      if (!m_index_iter->initialized()) m_index_iter->init_scan(m_key.get(), key_len, false, nullptr, 0, false);
      break;
    case HA_READ_BEFORE_KEY:
      if (!m_index_iter->initialized()) m_index_iter->init_scan(nullptr, 0, true, m_key.get(), key_len, false);
      break;
    default:
      return HA_ERR_WRONG_COMMAND;
  }

  const uchar *keykey{nullptr};
  uint32_t keykey_len{0};
  row_id_t value{std::numeric_limits<size_t>::max()};
  if (m_index_iter->next(keykey, &keykey_len, (void *)&value)) {
    m_rowid.store(value);
    auto ret = next(buf);
    if (ret) {
      return ret;
    }
    err = ShannonBase::SHANNON_SUCCESS;
  }

  return err;
}

int DataTable::index_next(uchar *buf) {
  const uchar *keykey{nullptr};
  uint32_t keykey_len{0};
  row_id_t value{std::numeric_limits<size_t>::max()};
  int err{HA_ERR_END_OF_FILE};

  if (m_index_iter->next(keykey, &keykey_len, (void *)&value)) {
    m_rowid.store(value);
    auto ret = next(buf);
    if (ret) {
      return ret;
    }
    err = ShannonBase::SHANNON_SUCCESS;
  }

  return err;
}

row_id_t DataTable::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}

}  // namespace Imcs
}  // namespace ShannonBase
