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
#include "ml_regression.h"

#include <string>

#include "include/thr_lock.h" //TL_READ
#include "include/my_inttypes.h"
#include "sql/derror.h" //ER_TH
#include "sql/mysqld.h"
#include "sql/current_thd.h"
#include "sql/sql_base.h"
#include "sql/table.h"
#include "sql/handler.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"

// clang-format off
// clang-format on
namespace ShannonBase {
namespace ML {

ML_regression::ML_regression(std::string sch_name, std::string table_name,
                             std::string target_name) : m_sch_name(sch_name),
                                                        m_table_name(table_name),
                                                        m_target_name(target_name) {
}

ML_regression::~ML_regression() {
}

int ML_regression::train() {

  THD* thd = current_thd;
  Open_table_context table_ctx(thd, 0);
  thr_lock_type lock_mode (TL_READ);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine.";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  Table_ref table_ref(m_sch_name.c_str(), strlen(m_sch_name.c_str()),
                   m_table_name.c_str(), strlen( m_table_name.c_str()),
                   m_table_name.c_str(), lock_mode);

  MDL_REQUEST_INIT(&table_ref.mdl_request, MDL_key::TABLE,
                  m_sch_name.c_str(),  m_table_name.c_str(),
                  (lock_mode > TL_READ) ? MDL_SHARED_WRITE : MDL_SHARED_READ,
                  MDL_TRANSACTION);
  if (open_table(thd, &table_ref, &table_ctx)) {
    return HA_ERR_GENERIC;
  }
  auto num_data =  table_ref.table->file->stats.records;
  auto cols = table_ref.table->s->fields;
  Traing_data_t train_data((size_t)num_data, std::vector<double>(cols));

  // The defined secondary engine must be the name of a valid storage engine.
  plugin_ref plugin =
      ha_resolve_by_name(thd, &table_ref.table->s->secondary_engine, false);
  if ((plugin == nullptr) || !plugin_is_ready(table_ref.table->s->secondary_engine,
                                              MYSQL_STORAGE_ENGINE_PLUGIN)) {
    push_warning_printf(
        thd, Sql_condition::SL_WARNING, ER_UNKNOWN_STORAGE_ENGINE,
        ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE), table_ref.table->s->secondary_engine.str);
    return false;
  }

  // The engine must support being used as a secondary engine.
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (!(hton->flags & HTON_IS_SECONDARY_ENGINE)) {
    my_error(ER_SECONDARY_ENGINE, MYF(0),
             "Unsupported secondary storage engine");
    return true;
  }

  // Get handler to the secondary engine into which the table will be loaded.
  const bool is_partitioned = table_ref.table->s->m_part_info != nullptr;
  unique_ptr_destroy_only<handler> tb_handler(
      get_new_handler(table_ref.table->s, is_partitioned, thd->mem_root, hton));

  /* Read the traning data into train_data vector from rapid engine. here,
  we use training data as lablels too */
  my_bitmap_map *old_map =
  tmp_use_all_columns(table_ref.table, table_ref.table->read_set);
  tb_handler->ha_open(table_ref.table, table_ref.table->s->table_name.str, O_RDONLY,
                      HA_OPEN_IGNORE_IF_LOCKED, table_ref.table->s->tmp_table_def);
  if (tb_handler && tb_handler->ha_external_lock(thd, F_RDLCK))
    return HA_ERR_GENERIC;
  if (tb_handler->ha_rnd_init(true)) {
    return HA_ERR_GENERIC;
  }
  //table full scan to train the model.
  auto index = 0;
  while (tb_handler->ha_rnd_next(table_ref.table->record[0]) == 0) {
    for (auto field_id = 0u; field_id < table_ref.table->s->fields; field_id++) {
      Field* field_ptr = *(table_ref.table->field + field_id);
        double data_val{0.0};
        switch (field_ptr->type()) {
          case MYSQL_TYPE_INT24:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE: {
            data_val = field_ptr->val_real();
          } break;
          case MYSQL_TYPE_DECIMAL:
          case MYSQL_TYPE_NEWDECIMAL: {
            my_decimal dval;
            field_ptr->val_decimal(&dval);
            my_decimal2double(10, &dval, &data_val);
          } break;
          default: break;
        }
        train_data[index][field_id] = data_val;
    }
    index++;
  }
  if (old_map) tmp_restore_column_map(table_ref.table->read_set, old_map);
  assert((int)num_data == index);
  tb_handler->ha_rnd_end();
  tb_handler->ha_external_lock(thd, F_UNLCK);
  tb_handler->ha_close();
  //TODO: using the data to train the model by the specific ML libs.
  return 0;
}

int ML_regression::predict() {
  return 0;
}

} //ML
} //shannonbase