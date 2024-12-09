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

#include "storage/rapid_engine/imcs/chunk.h"   //CHUNK
#include "storage/rapid_engine/imcs/cu.h"      //CU
#include "storage/rapid_engine/imcs/imcs.h"    //IMCS
#include "storage/rapid_engine/utils/utils.h"  //Blob

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"  //sys_pop_buff
#include "storage/rapid_engine/trx/transaction.h"    //Transaction

namespace ShannonBase {
namespace Imcs {

DataTable::DataTable(TABLE *source_table) : m_data_source(source_table) { ut_a(m_data_source); }

DataTable::~DataTable() {}

int DataTable::open() { return 0; }

int DataTable::close() { return 0; }

int DataTable::init() {
  if (!m_initialized.load()) {
    m_initialized.store(true);
    scan_init();
  }

  return 0;
}

void DataTable::scan_init() {
  std::ostringstream key_part, key;
  key_part << m_data_source->s->db.str << ":" << m_data_source->s->table_name.str << ":";
  for (auto index = 0u; index < m_data_source->s->fields; index++) {
    auto fld = *(m_data_source->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    key << key_part.str() << fld->field_name;
    auto key_str = key.str();

    m_field_cus.push_back(Imcs::instance()->get_cu(key_str));
    key.str("");
  }
  key.str("");

  m_rowid.store(0);

  m_context = std::make_unique<Rapid_load_context>();
  key << key_part.str() << SHANNON_DB_TRX_ID;
  m_context->m_trx_id_cu = ShannonBase::Imcs::Imcs::instance()->get_cu(key.str().c_str());
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

int DataTable::next(uchar *buf) {
  // In optimization phase. we should not choice rapid to scan, when pop threading
  // is running to repop the data to rapid.
  // ut_a(ShannonBase::Populate::sys_pop_buff.size() == 0);
// make all ptr in m_field_ptrs to move forward one step(one row).
start_pos:
  if (m_rowid >= m_field_cus[0]->prows()) return HA_ERR_END_OF_FILE;

  auto current_chunk = m_rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = m_rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(m_context->m_trx_id_cu);
  auto trx_id_ptr = m_context->m_trx_id_cu->chunk(current_chunk)->seek(offset_in_chunk);
  // more info for __builtin_prefetch: https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html
  if (trx_id_ptr < m_context->m_trx_id_cu->chunk(current_chunk)->where() &&
      (trx_id_ptr - m_context->m_trx_id_cu->chunk(current_chunk)->base()) % CACHE_LINE_SIZE == 0)
    SHANNON_PREFETCH_R(trx_id_ptr + CACHE_LINE_SIZE);

  Transaction::ID trx_id = mach_read_from_6(trx_id_ptr);

  for (auto idx = 0u; idx < m_field_cus.size(); idx++) {
    auto cu = m_field_cus[idx];
    auto normalized_length = cu->normalized_pack_length();
    auto is_text_value = Utils::Util::is_string(cu->header()->m_source_fld->type()) ||
                         Utils::Util::is_blob(cu->header()->m_source_fld->type());
    DBUG_EXECUTE_IF("secondary_engine_rapid_next_error", {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
      return HA_ERR_GENERIC;
    });

    // prefetch data for cpu to reduce the data cache miss.
    if (trx_id_ptr < m_context->m_trx_id_cu->chunk(current_chunk)->where() &&
        (trx_id_ptr - m_context->m_trx_id_cu->chunk(current_chunk)->base()) % CACHE_LINE_SIZE == 0)
      SHANNON_PREFETCH_R(cu->chunk(current_chunk)->seek(offset_in_chunk) + CACHE_LINE_SIZE);

    // visibility check. if it's not visibile and does not have old version, go to next.
    if (!m_context->m_trx->is_visible(trx_id, m_context->m_table_name) &&
        !cu->chunk(current_chunk)->get_header()->m_smu) {
      m_rowid.fetch_add(1);
      goto start_pos;
    } else {  // travers the version link.
    }

    auto source_fld = *(m_data_source->field + cu->header()->m_source_fld->field_index());

    // TODO: to check version link to check its old value. in fact here always the latest version.
    if (cu->chunk(current_chunk)->is_deleted(m_context.get(), offset_in_chunk)) {
      m_rowid.fetch_add(1);
      goto start_pos;
    }

    auto old_map = tmp_use_all_columns(m_data_source, m_data_source->write_set);
    if (cu->chunk(current_chunk)->is_null(m_context.get(), offset_in_chunk)) {
      source_fld->set_null();
      if (old_map) tmp_restore_column_map(m_data_source->write_set, old_map);
      continue;
    }

    source_fld->set_notnull();
    auto data_ptr = cu->chunk(current_chunk)->base() + offset_in_chunk * normalized_length;
    if (is_text_value) {
      uint32 str_id = *reinterpret_cast<uint32 *>(data_ptr);
      auto str_ptr = reinterpret_cast<char *>(cu->header()->m_local_dict->get(str_id));
      auto len =
          (Utils::Util::is_blob(cu->header()->m_type) || Utils::Util::is_varstring(cu->header()->m_source_fld->type()))
              ? strlen(str_ptr)
              : cu->pack_length();
      source_fld->store(str_ptr, len, source_fld->charset());
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
  return 0;
}

row_id_t DataTable::find(uchar *buf) {
  row_id_t rowid{0u};
  return rowid;
}

}  // namespace Imcs
}  // namespace ShannonBase
