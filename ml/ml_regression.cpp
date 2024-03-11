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
#include "sql-common/json_dom.h" //Json_wrapper.
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"

// clang-format off
#include <LightGBM/c_api.h>
// clang-format on
namespace ShannonBase {
namespace ML {

ML_regression::ML_regression(std::string sch_name, std::string table_name,
                             std::string target_name, std::string handler_name,
                             Json_wrapper* options)
                                                  : m_sch_name(sch_name),
                                                  m_table_name(table_name),
                                                  m_target_name(target_name),
                                                  m_handler_name(handler_name),
                                                  m_options(options) {
}

ML_regression::~ML_regression() {
}

int ML_regression::train() {
  THD* thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
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
  auto feature_num =  table_ref.table->file->stats.records;
  auto sample_num = table_ref.table->s->fields;
  Traing_data_t train_data((size_t)feature_num, std::vector<double>(sample_num));

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
  //table full scan to train the model. the cols means `sample data` and row menas `feature number`
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
  tb_handler->ha_rnd_end();
  tb_handler->ha_external_lock(thd, F_UNLCK);
  tb_handler->ha_close();

  feature_num = index;
  //TODO: using the data to train the model by the specific ML libs.
  DatasetHandle train_dataset_handler;
  std::string parameters = "";
  auto ret = LGBM_DatasetCreateFromMat(reinterpret_cast<const void*>(train_data.data()), C_API_DTYPE_FLOAT64,
                            sample_num, feature_num, 1, parameters.c_str(), nullptr, &train_dataset_handler);
  if (ret == -1)
    return HA_ERR_GENERIC;

  std::string mode_params = "objective=regression";
  BoosterHandle booster;
  ret = LGBM_BoosterCreate(train_dataset_handler, mode_params.c_str(), &booster);
  if (ret == -1)
    return HA_ERR_GENERIC;

  int finished;
  for (auto iter = 0; iter < 100; ++iter) {
    ret = LGBM_BoosterUpdateOneIter(booster, &finished);
    if (ret == -1)
      return HA_ERR_GENERIC;
    if (finished) break;
  }
  int64_t out_len;
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, m_model_content_length, &out_len, nullptr);
  if (ret ==-1)
    return HA_ERR_GENERIC;
  if (m_model_content_length < out_len)
    m_model_content_length = out_len;
  m_model_buffer.reset(new char[m_model_content_length+1]);
  LGBM_BoosterDumpModel(booster, 0, -1, 0, m_model_content_length, &out_len, m_model_buffer.get());

  //the definition of this table, ref: `ml_train.sql`
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;
  std::string cat_table_name = "MODEL_CATALOG";
  Table_ref cat_table_ref(catalog_schema_name.c_str(), catalog_schema_name.length(),
                  cat_table_name.c_str(), cat_table_name.length(), cat_table_name.c_str(), TL_WRITE);
  Open_table_context cat_table_ctx(thd, 0);
  MDL_REQUEST_INIT(&cat_table_ref.mdl_request, MDL_key::TABLE,
                  catalog_schema_name.c_str(),
                  cat_table_name.c_str(), MDL_SHARED_WRITE, MDL_TRANSACTION);
  if (open_table(thd, &cat_table_ref, &cat_table_ctx) || !cat_table_ref.table->file) {
    return HA_ERR_GENERIC;
  }
  cat_table_ref.table->file->ha_external_lock(thd, F_WRLCK | F_RDLCK);
  cat_table_ref.table->use_all_columns();

  cat_table_ref.table->file->ha_index_init(0, true);
  cat_table_ref.table->file->ha_index_last(cat_table_ref.table->record[0]);
  int64_t next_id  = (*(cat_table_ref.table->field))->val_int() + 1;
  cat_table_ref.table->file->ha_index_end();
  Field* field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_ID)];
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(m_handler_name.c_str(), m_handler_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  field_ptr->store(m_model_buffer.get(), m_model_content_length, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_OWNER)];
  field_ptr->set_notnull();
  field_ptr->store(user_name.c_str(), user_name.length(), &my_charset_utf8mb4_general_ci);

  std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const my_timeval tm = {static_cast<int64_t>(timestamp), 0};
  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::BUILD_TIMESTAMP)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::TARGET_COLUMN_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(m_target_name.c_str(), m_target_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::TRAIN_TABLE_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(m_table_name.c_str(), m_table_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(m_model_content_length);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_TYPE)];
  field_ptr->set_notnull();
  field_ptr->store("regression", 10, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::TASK)];
  field_ptr->set_notnull();
  field_ptr->store("train", 5, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::COLUMN_NAMES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci); //columns

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_EXPLANATION)];
  field_ptr->set_notnull();
  field_ptr->store(1.0);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::LAST_ACCESSED)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  down_cast<Field_json*>(field_ptr)->store_json(m_options);

  field_ptr = cat_table_ref.table->field[static_cast<int>(FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);
  //???binlog???
  ret = cat_table_ref.table->file->ha_write_row(cat_table_ref.table->record[0]);
  cat_table_ref.table->file->ha_external_lock(thd, F_UNLCK);
  return ret;
}

int ML_regression::predict() {
  return 0;
}

int ML_regression::load(std::string model_handle_name, std::string user_name) {
  return 0;
}

int ML_regression::unload(std::string model_handle_name) {
  return 0;
}
int ML_regression::import() {
  return 0;
}

double ML_regression::score() {
  return 0;
}

int ML_regression::explain_row() {
  return 0;
}

int ML_regression::explain_table() {
  return 0;
}

int ML_regression::predict_row() {
  return 0;
}

int ML_regression::predict_table() {
  return 0;
}

} //ML
} //shannonbase