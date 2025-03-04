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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/imcs.h"

#include <threads.h>
#include <sstream>
#include <string>

#include "include/decimal.h"
#include "include/my_dbug.h"                  //DBUG_EXECUTE_IF
#include "storage/innobase/include/univ.i"    //UNIV_SQL_NULL
#include "storage/innobase/include/ut0dbg.h"  //ut_ad
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/populate.h"
#include "storage/rapid_engine/utils/utils.h"  //Utils

namespace ShannonBase {
namespace Imcs {

Imcs *Imcs::m_instance{nullptr};
std::once_flag Imcs::one;

thread_local Imcs *current_imcs_instance = Imcs::instance();

int Imcs::initialize() {
  m_inited.store(1);
  return 0;
}

int Imcs::deinitialize() {
  m_inited.store(0);
  return 0;
}

int Imcs::create_table_mem(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  std::string keypart, key;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");

  for (auto index = 0u; index < source->s->fields; index++) {
    auto fld = *(source->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    key.append(keypart).append(fld->field_name);
    m_cus.emplace(key, std::make_unique<Cu>(fld));
    key.clear();
  }

  /* in secondary load phase, the table not loaded into imcs. therefore, it can be seen
     by any transactions. If this table has been loaded into imcs. A new data such as
     insert/update/delete will associated with a SMU items to trace its visibility. Therefore
     the following DB_TRX_ID cu no need to build.
  key.clear();
  std::unique_ptr<Mock_field_trxid> trx_fld = std::make_unique<Mock_field_trxid>();
  trx_fld.get()->table = const_cast<TABLE *>(source);
  key.append(keypart).append(SHANNON_DB_TRX_ID);
  m_cus.emplace(key, std::make_unique<Cu>(trx_fld.get()));
  */

  return 0;
}

Cu *Imcs::at(size_t index) {
  if (index >= m_cus.size()) return m_cus.end()->second.get();

  auto it = m_cus.begin();
  std::advance(it, index);
  return (it->second).get();
}

Cu *Imcs::get_cu(std::string_view key) {
  std::string key_str(key);

  if (m_cus.find(key_str) == m_cus.end()) return nullptr;
  return m_cus[key_str].get();
}

int Imcs::load_table(const Rapid_load_context *context, const TABLE *source) {
  auto m_thd = context->m_thd;

  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  if (source->file->inited == handler::NONE && source->file->ha_rnd_init(true)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), source->s->db.str, source->s->table_name.str);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  if (create_table_mem(context, source)) return HA_ERR_GENERIC;

  std::string key_part, key;
  key_part.append(source->s->db.str).append(":").append(source->s->table_name.str).append(":");
  while ((tmp = source->file->ha_rnd_next(source->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    key.clear();
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), source->s->db.str, source->s->table_name.str);
      source->file->ha_rnd_end();
      return HA_ERR_GENERIC;
    });

    /**
     * for VARCHAR type Data in field->ptr is stored as: 1 or 2 bytes length-prefix-header  (from
     * Field_varstring::length_bytes) data. the here we dont use var_xxx to get data, rather getting
     * directly, due to we dont care what real it is. ref to: field.cc:6703
     */
    for (auto index = 0u; index < source->s->fields; index++) {
      auto fld = *(source->field + index);
      if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

      key.append(key_part).append(fld->field_name);
      auto extra_offset = Utils::Util::is_varstring(fld->type()) ? (fld->field_length > 256 ? 2 : 1) : 0;
      auto data_ptr = fld->is_null() ? nullptr : fld->field_ptr() + extra_offset;
      auto data_len = fld->is_null()
                          ? UNIV_SQL_NULL
                          : (Utils::Util::is_varstring(fld->type())
                                 ? ((extra_offset > 1) ? *(uint16 *)fld->field_ptr() : *(uchar *)fld->field_ptr())
                                 : fld->pack_length());
      if (Utils::Util::is_blob(fld->type())) {
        data_ptr = const_cast<uchar *>(fld->data_ptr());
        data_len = fld->data_length(0);
      }

      if (!m_cus[key]->write_row(context, data_ptr, data_len)) {
        std::string errmsg;
        errmsg.append("load data from ")
            .append(source->s->db.str)
            .append(".")
            .append(source->s->table_name.str)
            .append(" to imcs failed.");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
        source->file->ha_rnd_end();
        return HA_ERR_GENERIC;
      }
      key.clear();
    }

    m_thd->inc_sent_row_count(1);
    if (tmp == HA_ERR_RECORD_DELETED && !m_thd->killed) continue;
  }
  // end of load the data from innodb to imcs.
  source->file->ha_rnd_end();
  return 0;
}

int Imcs::unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                       bool error_if_not_loaded) {
  /** the key format: "db_name:table_name:field_name", all the ghost columns also should be
   *  removed*/
  // TODO: need to wait all wrk threads finish.
  std::string keypart;
  auto found{false};
  keypart.append(db_name).append(":").append(table_name).append(":");
  for (auto it = m_cus.begin(); it != m_cus.end();) {
    if (it->first.find(keypart) != std::string::npos) {
      it = m_cus.erase(it);
      found = true;
    } else
      ++it;

    if (error_if_not_loaded && !found) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), db_name, table_name);
      return HA_ERR_GENERIC;
    }
  }

  return 0;
}

int Imcs::insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf) {
  ut_a(context && buf);

  return 0;
}

int Imcs::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);
  if (!m_cus.size()) return 0;

  std::string keypart;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  for (auto it = m_cus.begin(); it != m_cus.end();) {
    if (UNIV_UNLIKELY(!it->second || it->first.find(keypart) == std::string::npos)) {
      ++it;
      continue;
    }

    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }
  return 0;
}

int Imcs::delete_rows(const Rapid_load_context *context, std::vector<row_id_t> &rowids) {
  ut_a(context);

  if (!m_cus.size()) return 0;

  std::string keypart;
  keypart.append(context->m_schema_name).append(":").append(context->m_table_name).append(":");
  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : m_cus) {
      if (cu.first.find(keypart) == std::string::npos) continue;

      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return 0;
  }

  for (auto &rowid : rowids) {
    for (auto &cu : m_cus) {
      if (cu.first.find(keypart) == std::string::npos) continue;

      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
  }
  return 0;
}

int Imcs::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                     const uchar *new_field_data, size_t nlen) {
  ut_a(context);

  ut_a(m_cus[field_key]);
  auto ret = m_cus[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return 0;
}

int Imcs::update_row(const Rapid_load_context *context, row_id_t row_id,
                     std::map<std::string, std::unique_ptr<uchar[]>> &upd_recs) {
  ut_a(context);

  for (auto &rec : upd_recs) {
    if (m_cus.find(rec.first) == m_cus.end()) continue;

    auto pack_length = m_cus[rec.first]->normalized_pack_length();
    if (!rec.second)  // null value.
      pack_length = UNIV_SQL_NULL;
    if (!m_cus[rec.first]->update_row(context, row_id, rec.second.get(), pack_length)) return HA_ERR_GENERIC;
  }

  return 0;
}

}  // namespace Imcs
}  // namespace ShannonBase