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
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/imcs/chunk.h"  //CHUNK
#include "storage/rapid_engine/imcs/cu.h"     //CU
#include "storage/rapid_engine/imcs/imcs.h"   //IMCS
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"  //sys_pop_buff
#include "storage/rapid_engine/trx/transaction.h"    //Transaction
#include "storage/rapid_engine/utils/utils.h"        //Blob

namespace ShannonBase {
namespace Imcs {

DataTable::DataTable(TABLE *source_table) : m_initialized{false}, m_data_source(source_table) {
  ut_a(m_data_source);

  std::string key_part, key;
  // key_part << m_data_source->s->db.str << ":" << m_data_source->s->table_name.str << ":";
  thread_local std::string key_buffer;
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
  if (m_context) {
    m_context->m_trx->release_snapshot();
    m_context->m_trx->commit();
  }
}

int DataTable::open() {
  m_rowid.store(0);
  return 0;
}

int DataTable::close() {
  m_rowid.store(0);
  return 0;
}

int DataTable::init() {
  assert(!m_initialized.load());
  if (!m_initialized.load()) {
    m_initialized.store(true);
    m_rowid.store(0);

    m_context = std::make_unique<Rapid_load_context>();
    m_context->m_thd = current_thd;

    m_context->m_trx = ShannonBase::Transaction::get_or_create_trx(current_thd);
    m_context->m_trx->set_read_only(true);
    if (!m_context->m_trx->is_active())
      m_context->m_trx->begin(ShannonBase::Transaction::get_rpd_isolation_level(current_thd));

    if (!m_context->m_trx->has_snapshot()) m_context->m_trx->acquire_snapshot();

    m_context->m_schema_name = const_cast<char *>(m_data_source->s->db.str);
    m_context->m_table_name = const_cast<char *>(m_data_source->s->table_name.str);

#ifndef NDEBUG
    auto first_num = m_field_cus[0]->prows();
    for (auto &item : m_field_cus) {
      ut_a(first_num == item->prows());
    }
#endif
  }
  return 0;
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
    auto cu = m_field_cus[idx];
    auto normalized_length = cu->normalized_pack_length();
    auto is_text_value = Utils::Util::is_string(cu->header()->m_source_fld->type()) ||
                         Utils::Util::is_blob(cu->header()->m_source_fld->type());
    DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
      return HA_ERR_GENERIC;
    });

    // to check version link to check its old value.
    uchar *data_ptr{nullptr};
    auto is_deleted = cu->chunk(current_chunk)->is_deleted(m_context.get(), offset_in_chunk);
    auto is_null = cu->chunk(current_chunk)->is_null(m_context.get(), offset_in_chunk);
    if (is_deleted) {
      auto smu = cu->chunk(current_chunk)->get_header()->m_smu.get();
      data_ptr = smu->build_prev_vers(m_context.get(), offset_in_chunk);
      if (!data_ptr && !is_null) {
        m_rowid.fetch_add(1);
        goto start;
      }
    } else {
      data_ptr = cu->chunk(current_chunk)->base() + offset_in_chunk * normalized_length;
      if ((uintptr_t(data_ptr) & (CACHE_LINE_SIZE - 1)) == 0)
        SHANNON_PREFETCH_R(data_ptr + PREFETCH_AHEAD * CACHE_LINE_SIZE);
    }

    auto source_fld = *(m_data_source->field + cu->header()->m_source_fld->field_index());
    auto old_map = tmp_use_all_columns(m_data_source, m_data_source->write_set);
    if (is_null) {
      source_fld->set_null();
      if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
      continue;
    }
    source_fld->set_notnull();

    if (is_text_value) {
      uint32 str_id = *reinterpret_cast<uint32 *>(data_ptr);
      auto str_ptr = cu->header()->m_local_dict->get(str_id);
      auto len =
          (Utils::Util::is_blob(cu->header()->m_type) || Utils::Util::is_varstring(cu->header()->m_source_fld->type()))
              ? str_ptr.length()
              : cu->pack_length();
      source_fld->store(str_ptr.c_str(), len, source_fld->charset());
    } else
      source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), data_ptr, normalized_length);

    if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
  }

  m_rowid.fetch_add(1);
  return 0;
}

int DataTable::end() {
  m_context->m_trx->release_snapshot();
  m_context->m_trx->commit();

  m_rowid.store(0);
  m_initialized.store(false);
  return 0;
}

row_id_t DataTable::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}

}  // namespace Imcs
}  // namespace ShannonBase
