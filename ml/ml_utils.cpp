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

   The fundmental code for ML.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/

#include "ml_utils.h"

#include "include/my_inttypes.h"
#include "sql/sql_class.h"
#include "sql/current_thd.h"
#include "sql/derror.h" //ER_TH
//#include "sql/mysqld.h"
#include "sql/current_thd.h"
#include "sql/sql_base.h"
#include "sql/table.h"
#include "sql/handler.h"

namespace ShannonBase {
namespace ML {

int Utils::open_table_by_name(std::string schema_name, std::string table_name, thr_lock_type lk_mode, TABLE** table_ptr) {
  THD* thd = current_thd;
  Open_table_context table_ref_context(thd, 0);
  thr_lock_type lock_mode (lk_mode);

  //the definition of this table, ref: `ml_train.sql`
  Table_ref table_ref(schema_name.c_str(), schema_name.length(),
                  table_name.c_str(), table_name.length(), table_name.c_str(), TL_WRITE);
  MDL_REQUEST_INIT(&table_ref.mdl_request, MDL_key::TABLE,
                  schema_name.c_str(),  table_name.c_str(),
                  (lock_mode > TL_READ) ? MDL_SHARED_WRITE : MDL_SHARED_READ,
                  MDL_TRANSACTION);

  if (open_table(thd, &table_ref, &table_ref_context) || !table_ref.table->file) {
    return HA_ERR_GENERIC;
  }
  *table_ptr = table_ref.table;

  return 0;
}

handler* Utils::get_secondary_handler(TABLE* source_table_ptr) {
  // The defined secondary engine must be the name of a valid storage engine.
  if (!source_table_ptr) return nullptr;

  THD* thd = current_thd;
  plugin_ref plugin =
      ha_resolve_by_name(thd, &source_table_ptr->s->secondary_engine, false);
  if ((plugin == nullptr) || !plugin_is_ready(source_table_ptr->s->secondary_engine,
                                              MYSQL_STORAGE_ENGINE_PLUGIN)) {
    push_warning_printf(
        thd, Sql_condition::SL_WARNING, ER_UNKNOWN_STORAGE_ENGINE,
        ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE), source_table_ptr->s->secondary_engine.str);
    return nullptr;
  }

  // The engine must support being used as a secondary engine.
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (!(hton->flags & HTON_IS_SECONDARY_ENGINE)) {
    my_error(ER_SECONDARY_ENGINE, MYF(0),
             "Unsupported secondary storage engine");
    return nullptr;
  }

  // Get handler to the secondary engine into which the table will be loaded.
  const bool is_partitioned = source_table_ptr->s->m_part_info != nullptr;
  return get_new_handler(source_table_ptr->s, is_partitioned, thd->mem_root, hton);
}

int Utils::close_table(TABLE* table[[maybe_unused]]) {
  return 0;
}
} //ns:ml
} //ns:shannonbase    