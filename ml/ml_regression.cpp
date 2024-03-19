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

#include "ml_utils.h"

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
  m_handler  = std::make_unique<ML_handler>();
}

ML_regression::~ML_regression() {
}

int ML_regression::train() {
  THD* thd = current_thd;
  std::string user_name(thd->security_context()->user().str);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine.";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  TABLE* source_table_ptr{nullptr};
  if (Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ, &source_table_ptr))
    return HA_ERR_GENERIC;

  auto feature_num = source_table_ptr->file->stats.records;
  auto sample_num = source_table_ptr->s->fields;
  Traing_data_t train_data((size_t)feature_num, std::vector<double>(sample_num));

  unique_ptr_destroy_only<handler> tb_handler(Utils::get_secondary_handler(source_table_ptr));
  /* Read the traning data into train_data vector from rapid engine. here,
  we use training data as lablels too */
  my_bitmap_map *old_map =
  tmp_use_all_columns(source_table_ptr, source_table_ptr->read_set);
  tb_handler->ha_open(source_table_ptr, source_table_ptr->s->table_name.str, O_RDONLY,
                      HA_OPEN_IGNORE_IF_LOCKED, source_table_ptr->s->tmp_table_def);
  if (tb_handler && tb_handler->ha_external_lock(thd, F_RDLCK))
    return HA_ERR_GENERIC;
  if (tb_handler->ha_rnd_init(true)) {
    return HA_ERR_GENERIC;
  }
  //table full scan to train the model. the cols means `sample data` and row menas `feature number`
  auto index = 0;
  while (tb_handler->ha_rnd_next(source_table_ptr->record[0]) == 0) {
    for (auto field_id = 0u; field_id < source_table_ptr->s->fields; field_id++) {
      Field* field_ptr = *(source_table_ptr->field + field_id);
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
  if (old_map) tmp_restore_column_map(source_table_ptr->read_set, old_map);
  tb_handler->ha_rnd_end();
  tb_handler->ha_external_lock(thd, F_UNLCK);
  tb_handler->ha_close();
  Utils::close_table(source_table_ptr);

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

  int64_t bufflen(1024), out_len{0};
  std::unique_ptr<char[]> model_buffer = std::make_unique<char[]>(out_len);
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, bufflen, &out_len, model_buffer.get());
  if (ret ==-1)
    return HA_ERR_GENERIC;

  if (out_len > bufflen) {
    bufflen = out_len;
    model_buffer.reset(new char[bufflen]);
    ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, bufflen, &out_len, model_buffer.get());
  }

  LGBM_DatasetFree(train_dataset_handler);
  LGBM_BoosterFree(booster);
  //the definition of this table, ref: `ml_train.sql`
  TABLE* cat_tale_ptr{nullptr};
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;
  std::string cat_table_name = "MODEL_CATALOG";
  if (Utils::open_table_by_name(catalog_schema_name, cat_table_name, TL_WRITE, &cat_tale_ptr))
    return HA_ERR_GENERIC;

  cat_tale_ptr->file->ha_external_lock(thd, F_WRLCK | F_RDLCK);
  cat_tale_ptr->use_all_columns();

  cat_tale_ptr->file->ha_index_init(0, true);
  cat_tale_ptr->file->ha_index_last(cat_tale_ptr->record[0]);
  int64_t next_id  = (*(cat_tale_ptr->field))->val_int() + 1;
  cat_tale_ptr->file->ha_index_end();
  Field* field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_ID)];
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(m_handler_name.c_str(), m_handler_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  field_ptr->store(model_buffer.get(), out_len, &my_charset_utf8mb4_general_ci);

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
  field_ptr->store(m_target_name.c_str(), m_target_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TRAIN_TABLE_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(m_table_name.c_str(), m_table_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(bufflen);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_TYPE)];
  field_ptr->set_notnull();
  field_ptr->store("regression", 10, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TASK)];
  field_ptr->set_notnull();
  field_ptr->store("train", 5, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::COLUMN_NAMES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci); //columns

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_EXPLANATION)];
  field_ptr->set_notnull();
  field_ptr->store(1.0);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::LAST_ACCESSED)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  down_cast<Field_json*>(field_ptr)->store_json(m_options);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);
  //???binlog???
  ret = cat_tale_ptr->file->ha_write_row(cat_tale_ptr->record[0]);
  cat_tale_ptr->file->ha_external_lock(thd, F_UNLCK);

  m_handler->set(booster);
  return ret;
}

int ML_regression::predict() {
  return 0;
}

int ML_regression::load(std::string model_handle_name, std::string user_name) {
  //the definition of this table, ref: `ml_train.sql`
  THD* thd = current_thd;
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;

  std::string cat_table_name = "MODEL_CATALOG";
  TABLE* cat_table_ptr{nullptr};
  if (Utils::open_table_by_name(catalog_schema_name, cat_table_name, TL_WRITE, &cat_table_ptr))
    return HA_ERR_GENERIC;

  cat_table_ptr->file->ha_external_lock(thd, F_WRLCK | F_RDLCK);
  cat_table_ptr->use_all_columns();
  cat_table_ptr->file->ha_rnd_init(true);

  String modle_handle_name_str, model_user_name_str, model_content;
  modle_handle_name_str.set_charset(&my_charset_utf8mb4_general_ci);
  model_user_name_str.set_charset(&my_charset_utf8mb4_general_ci);
  model_content.set_charset(&my_charset_utf8mb4_general_ci);
  Field* field_ptr {nullptr};
  while (cat_table_ptr->file->ha_rnd_next(cat_table_ptr->record[0]) == 0) { //to get the data.
    field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
    field_ptr->val_str(&modle_handle_name_str);

    field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OWNER)];
    field_ptr->val_str(&model_user_name_str);
    if (!strcmp(modle_handle_name_str.c_ptr(), model_handle_name.c_str()) &&
        !strcmp(model_user_name_str.c_ptr(), user_name.c_str())) { //
        field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
        field_ptr->val_str(&model_content);
        break;
    }
  }
  cat_table_ptr->file->ha_rnd_end();
  cat_table_ptr->file->ha_external_lock(thd, F_UNLCK);
  Utils::close_table(cat_table_ptr);

  BoosterHandle bt_handler;
  int out_num_iterations;
  if (LGBM_BoosterLoadModelFromString(model_content.c_ptr(), &out_num_iterations, &bt_handler) == -1)
    return HA_ERR_GENERIC;

  m_handler->set(bt_handler);
  return 0;
}

int  ML_regression::load_from_file (std::string modle_file_full_path, std::string model_handle_name) {
  //to update the `MODEL_CATALOG.MODEL_OBJECT`
  if (check_valid_path(modle_file_full_path.c_str(), modle_file_full_path.length()))
    return HA_ERR_GENERIC;

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

ML_TASK_TYPE ML_regression::type() {
  return ML_TASK_TYPE::REGRESSION;
}

} //ML
} //shannonbase