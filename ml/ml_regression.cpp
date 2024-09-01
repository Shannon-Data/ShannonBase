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

#include "include/my_inttypes.h"
#include "include/thr_lock.h"     //TL_READ
#include "sql-common/json_dom.h"  //Json_wrapper.
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "ml_utils.h"                                       //ml utils
#include "storage/innobase/include/ut0dbg.h"                //for ut_a
#include "storage/rapid_engine/include/rapid_status.h"      //loaded table.

#include "LightGBM/c_api.h"
// clang-format off
// clang-format on
namespace ShannonBase {

namespace ML {
int ML_regression::train() {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine.";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  TABLE *source_table_ptr{nullptr};
  if (Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ, &source_table_ptr)) return HA_ERR_GENERIC;

  auto n_sample = source_table_ptr->file->stats.records;
  auto n_feature = source_table_ptr->s->fields;

  std::vector<const char *> feature_name_vec(n_feature);
  for (auto idx = 0u; idx < n_feature; idx++) {
    auto field_ptr = *(source_table_ptr->s->field + idx);
    feature_name_vec.emplace_back(field_ptr->field_name);
  }

  unique_ptr_destroy_only<handler> tb_handler(Utils::get_secondary_handler(source_table_ptr));
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  my_bitmap_map *old_map = tmp_use_all_columns(source_table_ptr, source_table_ptr->read_set);
  tb_handler->ha_open(source_table_ptr, source_table_ptr->s->table_name.str, O_RDONLY, HA_OPEN_IGNORE_IF_LOCKED,
                      source_table_ptr->s->tmp_table_def);
  if (tb_handler && tb_handler->ha_external_lock(thd, F_RDLCK)) return HA_ERR_GENERIC;
  if (tb_handler->ha_rnd_init(true)) {
    return HA_ERR_GENERIC;
  }

  // table full scan to train the model. the cols means `sample data` and row menas `feature number`
  ha_rows r_index = 0;
  Traing_data_t train_data(n_sample, std::vector<double>(n_feature));
  while (tb_handler->ha_rnd_next(source_table_ptr->record[0]) == 0) {
    for (auto field_id = 0u; field_id < source_table_ptr->s->fields; field_id++) {
      Field *field_ptr = *(source_table_ptr->field + field_id);

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
        default:
          break;
      }
      train_data[r_index][field_id] = data_val;
    }
    r_index++;
  }
  ut_a(n_sample == r_index);

  if (old_map) tmp_restore_column_map(source_table_ptr->read_set, old_map);
  tb_handler->ha_rnd_end();
  tb_handler->ha_external_lock(thd, F_UNLCK);
  tb_handler->ha_close();
  Utils::close_table(source_table_ptr);

  std::string parameters = "max_bin=254 ";
  DatasetHandle train_dataset_handler{nullptr};
  auto ret = LGBM_DatasetCreateFromMat(reinterpret_cast<const void *>(train_data.data()), C_API_DTYPE_FLOAT64, n_sample,
                                       n_feature, 1, parameters.c_str(), nullptr, &train_dataset_handler);
  int num_data{0}, feat_nums{0};
  ret = LGBM_DatasetSetFeatureNames(train_dataset_handler, (const char **)feature_name_vec.data(), n_feature);
  ret = LGBM_DatasetGetNumData(train_dataset_handler, &num_data);
  ret = LGBM_DatasetGetNumFeature(train_dataset_handler, &feat_nums);

  if (ret == -1) return HA_ERR_GENERIC;

  std::string mode_params = "task=train objective=regression num_leaves=31 verbose=0";
  BoosterHandle booster;
  ret = LGBM_BoosterCreate(train_dataset_handler, mode_params.c_str(), &booster);
  ret = LGBM_BoosterAddValidData(booster, train_dataset_handler);
  if (ret == -1) return HA_ERR_GENERIC;

  int finished{0};
  for (auto iter = 0; iter < 100; ++iter) {
    ret = LGBM_BoosterUpdateOneIter(booster, &finished);
    if (ret == -1) return HA_ERR_GENERIC;
    if (finished) break;
  }

  int64_t bufflen(1024), out_len{0};
  std::unique_ptr<char[]> model_buffer = std::make_unique<char[]>(out_len);
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, bufflen, &out_len, model_buffer.get());
  if (ret == -1) return HA_ERR_GENERIC;

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
  // the definition of this table, ref: `ml_train.sql`
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
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);  // columns

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_EXPLANATION)];
  field_ptr->set_notnull();
  field_ptr->store(1.0);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::LAST_ACCESSED)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  down_cast<Field_json *>(field_ptr)->store_json(m_options);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);
  //???binlog??? pop thread is running ????
  ret = cat_tale_ptr->file->ha_write_row(cat_tale_ptr->record[0]);
  cat_tale_ptr->file->ha_external_lock(thd, F_UNLCK);

  m_handler = booster;
  return ret;
}

int ML_regression::predict() { return 0; }

int ML_regression::load(std::string &model_content) {
  // the definition of this table, ref: `ml_train.sql`
  BoosterHandle bt_handler;
  int out_num_iterations;
  if (LGBM_BoosterLoadModelFromString(model_content.c_str(), &out_num_iterations, &bt_handler) == -1)
    return HA_ERR_GENERIC;

  m_handler = bt_handler;
  return 0;
}

int ML_regression::load_from_file(std::string modle_file_full_path, std::string model_handle_name) {
  // to update the `MODEL_CATALOG.MODEL_OBJECT`
  if (check_valid_path(modle_file_full_path.c_str(), modle_file_full_path.length()) || !model_handle_name.length())
    return HA_ERR_GENERIC;

  return 0;
}

int ML_regression::unload(std::string model_handle_name) {
  if (!model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

  if (m_handler) {
    BoosterHandle bt_handler = m_handler;
    LGBM_BoosterFree(bt_handler);
    m_handler = nullptr;
  }
  return 0;
}

int ML_regression::import(std::string model_handle_name, std::string user_name, std::string &content) {
  THD *thd = current_thd;

  BoosterHandle bt_handler;
  int out_num_iterations;
  if (LGBM_BoosterLoadModelFromString(content.c_str(), &out_num_iterations, &bt_handler) == -1) return HA_ERR_GENERIC;

  m_handler = bt_handler;

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
  field_ptr->store(model_handle_name.c_str(), model_handle_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  field_ptr->store(content.c_str(), content.size(), &my_charset_utf8mb4_general_ci);

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
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TRAIN_TABLE_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(content.size());

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_TYPE)];
  field_ptr->set_notnull();
  field_ptr->store("regression", 10, &my_charset_utf8mb4_general_ci);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TASK)];
  field_ptr->set_notnull();
  field_ptr->store("import", 6, &my_charset_utf8mb4_general_ci);

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
  down_cast<Field_json *>(field_ptr)->store_json(m_options);

  field_ptr = cat_tale_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);
  //???binlog???
  auto ret = cat_tale_ptr->file->ha_write_row(cat_tale_ptr->record[0]);
  cat_tale_ptr->file->ha_external_lock(thd, F_UNLCK);

  return ret;
}

double ML_regression::score() { return 0; }

int ML_regression::explain_row() { return 0; }

int ML_regression::explain_table() { return 0; }

int ML_regression::predict_row() { return 0; }

int ML_regression::predict_table() { return 0; }

ML_TASK_TYPE ML_regression::type() { return ML_TASK_TYPE::REGRESSION; }

}  // namespace ML
}  // namespace ShannonBase