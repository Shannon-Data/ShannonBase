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
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"  //sys_pop_buff
#include "storage/rapid_engine/trx/readview.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction
#include "storage/rapid_engine/utils/utils.h"      //Blob

namespace ShannonBase {
namespace Imcs {

DataTable::DataTable(TABLE *source_table) : m_initialized{false}, m_data_source(source_table) {
  ut_a(m_data_source);

  std::string key_part, key;
  std::string key_buffer;
  key_buffer.reserve(256);
  for (auto index = 0u; index < m_data_source->s->fields; index++) {
    key_buffer.clear();
    auto fld = *(m_data_source->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    key_buffer.append(m_data_source->s->db.str)
        .append(":")
        .append(m_data_source->s->table_name.str)
        .append(":")
        .append(fld->field_name);
    m_field_cus.emplace_back(Imcs::instance()->get_cu(key_buffer));
  }

  key_buffer.clear();
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

int DataTable::next(uchar *buf) {
  // In optimization phase. we should not choice rapid to scan, when pop threading
  // is running to repop the data to rapid.
  // ut_a(ShannonBase::Populate::sys_pop_buff.size() == 0);
  // make all ptr in m_field_ptrs to move forward one step(one row).
start:
  assert(m_initialized.load());

  if (m_rowid >= m_field_cus[0]->prows()) return HA_ERR_END_OF_FILE;

  auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;

  for (auto idx = 0u; idx < m_field_cus.size(); idx++) {
    ut_a(m_field_cus[idx]);

    auto normalized_length = m_field_cus[idx]->normalized_pack_length();
    DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "secondary_engine_rapid_next_error");
      return HA_ERR_GENERIC;
    });

    auto source_fld = *(m_data_source->field + m_field_cus[idx]->header()->m_source_fld->field_index());
    auto old_map = tmp_use_all_columns(m_data_source, m_data_source->write_set);

    // to check version link to check its old value.
    auto current_chunk_ptr = m_field_cus[idx]->chunk(current_chunk);
    auto current_data_ptr = current_chunk_ptr->base() + offset_in_chunk * normalized_length;
    if ((uintptr_t(current_data_ptr) & (CACHE_LINE_SIZE - 1)) == 0)
      SHANNON_PREFETCH_R(current_data_ptr + PREFETCH_AHEAD * CACHE_LINE_SIZE);

    auto data_len = normalized_length;
    auto data_ptr = std::make_unique<uchar[]>(data_len);
    std::memcpy(data_ptr.get(), current_data_ptr, data_len);

    uint8 status{0};
    auto versioned_ptr [[maybe_unused]] = current_chunk_ptr->header()->m_smu->build_prev_vers(
        m_context.get(), offset_in_chunk, data_ptr.get(), data_len, status);
    if (status &
        static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_NULL)) {  // the original value is null.
      source_fld->set_null();
      if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
      continue;
    }
    if (status & static_cast<uint8>(ShannonBase::ReadView::RECONSTRUCTED_STATUS::STAT_DELETED)) {
      m_rowid.fetch_add(1);
      if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
      goto start;
    }

    source_fld->set_notnull();
    if (Utils::Util::is_string(m_field_cus[idx]->header()->m_source_fld->type()) ||
        Utils::Util::is_blob(m_field_cus[idx]->header()->m_source_fld->type())) {
      uint32 str_id = *reinterpret_cast<uint32 *>(data_ptr.get());
      auto str_ptr = m_field_cus[idx]->header()->m_local_dict->get(str_id);
      source_fld->store(str_ptr.c_str(), strlen(str_ptr.c_str()), source_fld->charset());
    } else {
      source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), data_ptr.get(), normalized_length);
    }
    if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
  }

  m_rowid.fetch_add(1);
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

  auto imcs_instace = Imcs::Imcs::instance();
  std::string keypart;
  keypart.append(m_data_source->s->db.str).append(":").append(m_data_source->s->table_name.str).append(":");

  auto keykeypart = keypart;
  keykeypart.append(m_data_source->s->key_info[keynr].name).append(":");

  auto index = imcs_instace->get_index(keykeypart);
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
