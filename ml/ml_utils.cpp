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
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "ml_algorithm.h"

namespace ShannonBase {
namespace ML {
int Utils::open_table_by_name(std::string schema_name, std::string table_name, thr_lock_type lk_mode,
                              TABLE **table_ptr) {
  THD *thd = current_thd;
  Open_table_context table_ref_context(thd, 0);
  thr_lock_type lock_mode(lk_mode);

  // the definition of this table, ref: `ml_train.sql`
  Table_ref table_ref(schema_name.c_str(), schema_name.length(), table_name.c_str(), table_name.length(),
                      table_name.c_str(), TL_WRITE);
  MDL_REQUEST_INIT(&table_ref.mdl_request, MDL_key::TABLE, schema_name.c_str(), table_name.c_str(),
                   (lock_mode > TL_READ) ? MDL_SHARED_WRITE : MDL_SHARED_READ, MDL_TRANSACTION);

  if (open_table(thd, &table_ref, &table_ref_context) || !table_ref.table->file) {
    return HA_ERR_GENERIC;
  }
  *table_ptr = table_ref.table;

  return 0;
}

handler *Utils::get_primary_handler(TABLE *source_table_ptr) {
  // The defined secondary engine must be the name of a valid storage engine.
  if (!source_table_ptr) return nullptr;

  return source_table_ptr->file;
}

handler *Utils::get_secondary_handler(TABLE *source_table_ptr) {
  // The defined secondary engine must be the name of a valid storage engine.
  if (!source_table_ptr) return nullptr;

  THD *thd = current_thd;
  plugin_ref plugin = ha_resolve_by_name(thd, &source_table_ptr->s->secondary_engine, false);
  if ((plugin == nullptr) || !plugin_is_ready(source_table_ptr->s->secondary_engine, MYSQL_STORAGE_ENGINE_PLUGIN)) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNKNOWN_STORAGE_ENGINE,
                        ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE), source_table_ptr->s->secondary_engine.str);
    return nullptr;
  }

  // The engine must support being used as a secondary engine.
  handlerton *hton = plugin_data<handlerton *>(plugin);
  if (!(hton->flags & HTON_IS_SECONDARY_ENGINE)) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), "Unsupported secondary storage engine");
    return nullptr;
  }

  // Get handler to the secondary engine into which the table will be loaded.
  const bool is_partitioned = source_table_ptr->s->m_part_info != nullptr;
  return get_new_handler(source_table_ptr->s, is_partitioned, thd->mem_root, hton);
}

int Utils::close_table(TABLE *table [[maybe_unused]]) { return 0; }

BoosterHandle Utils::ML_train(std::string &task_mode, const void *training_data, uint n_data, uint data_type,
                              const char **features, uint n_feature, std::string &model_content) {
  std::string parameters = "max_bin=254 ";
  DatasetHandle train_dataset_handler{nullptr};
  auto ret = LGBM_DatasetCreateFromMat(training_data, data_type, n_data, n_feature, 1, parameters.c_str(), nullptr,
                                       &train_dataset_handler);
  int num_data{0}, feat_nums{0};
  ret = LGBM_DatasetSetFeatureNames(train_dataset_handler, features, n_feature);
  ret = LGBM_DatasetGetNumData(train_dataset_handler, &num_data);
  ret = LGBM_DatasetGetNumFeature(train_dataset_handler, &feat_nums);

  if (ret == -1) return nullptr;

  BoosterHandle booster;
  ret = LGBM_BoosterCreate(train_dataset_handler, task_mode.c_str(), &booster);
  ret = LGBM_BoosterAddValidData(booster, train_dataset_handler);
  if (ret == -1) return nullptr;

  int finished{0};
  for (auto iter = 0; iter < 100; ++iter) {
    ret = LGBM_BoosterUpdateOneIter(booster, &finished);
    if (ret == -1) return nullptr;
    if (finished) break;
  }

  int64_t bufflen(1024), out_len{0};
  std::unique_ptr<char[]> model_buffer = std::make_unique<char[]>(out_len);
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, bufflen, &out_len, model_buffer.get());
  if (ret == -1) return nullptr;

  if (out_len > bufflen) {
    bufflen = out_len;
    model_buffer.reset(new char[bufflen + 1]);
    ret = LGBM_BoosterSaveModelToString(booster,
                                        0,                              // start iter idx
                                        -1,                             // end inter idx
                                        C_API_FEATURE_IMPORTANCE_GAIN,  // feature_importance_type
                                        bufflen,                        // buff len
                                        &out_len,                       // out len
                                        model_buffer.get());
  }

  ret = LGBM_DatasetFree(train_dataset_handler);
  ret = LGBM_BoosterFree(booster);

  model_content.clear();
  model_content.assign(model_buffer.get(), bufflen);
  return booster;
}

int Utils::store_model_catalog(std::string &mode_type, std::string &oper_type, const Json_wrapper *options,
                               std::string &user_name, std::string &handler_name, std::string &model_content,
                               std::string &target_name, std::string &source_name) {
  THD *thd = current_thd;
  TABLE *cat_tale_ptr{nullptr};
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;
  std::string cat_table_name = "MODEL_CATALOG";
  if (Utils::open_table_by_name(catalog_schema_name, cat_table_name, TL_WRITE, &cat_tale_ptr)) return HA_ERR_GENERIC;

  cat_tale_ptr->file->ha_external_lock(thd, F_WRLCK | F_RDLCK);
  cat_tale_ptr->use_all_columns();

  cat_tale_ptr->file->ha_index_init(0, true);
  cat_tale_ptr->file->ha_index_last(cat_tale_ptr->record[0]);
  int64_t next_id = (*(cat_tale_ptr->field))->val_int() + 1;
  cat_tale_ptr->file->ha_index_end();
  Field *field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_ID)];
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(handler_name.c_str(), handler_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  field_ptr->store(model_content.c_str(), model_content.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OWNER)];
  field_ptr->set_notnull();
  field_ptr->store(user_name.c_str(), user_name.length(), &my_charset_utf8mb4_general_ci);

  std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const my_timeval tm = {static_cast<int64_t>(timestamp), 0};
  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::BUILD_TIMESTAMP)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TARGET_COLUMN_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(target_name.c_str(), target_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TRAIN_TABLE_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(source_name.c_str(), source_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(model_content.length());

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_TYPE)];
  field_ptr->set_notnull();
  field_ptr->store(mode_type.c_str(), mode_type.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TASK)];
  field_ptr->set_notnull();
  field_ptr->store(oper_type.c_str(), oper_type.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::COLUMN_NAMES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);  // columns

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_EXPLANATION)];
  field_ptr->set_notnull();
  field_ptr->store(1.0);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::LAST_ACCESSED)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  assert(options);
  down_cast<Field_json *>(field_ptr)->store_json(options);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);
  //???binlog??? pop thread is running ????
  auto ret = cat_tale_ptr->file->ha_write_row(cat_tale_ptr->record[0]);
  cat_tale_ptr->file->ha_external_lock(thd, F_UNLCK);

  return ret;
}

}  // namespace ML
}  // namespace ShannonBase