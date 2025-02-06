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

#include "ml_algorithm.h"
#include "sql/binlog.h"
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"

namespace ShannonBase {
namespace ML {

// open table by name. return table ptr, otherwise return nullptr.
TABLE *Utils::open_table_by_name(std::string schema_name, std::string table_name, thr_lock_type lk_mode) {
  THD *thd = current_thd;
  Open_table_context table_ref_context(thd, MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK);

  Table_ref table_ref(schema_name.c_str(), table_name.c_str(), lk_mode);
  table_ref.open_strategy = Table_ref::OPEN_IF_EXISTS;

  if (open_table(thd, &table_ref, &table_ref_context) || !table_ref.table->file) {
    return nullptr;
  }

  auto table_ptr = table_ref.table;
  if (!table_ptr->next_number_field)  // in case.
    table_ptr->next_number_field = table_ptr->found_next_number_field;

  return table_ptr;
}

int Utils::close_table(TABLE *table [[maybe_unused]]) {
  assert(table);
  // it will close in close_thread_tables(). so here do nothing.
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

// do ML train jobs.
BoosterHandle Utils::ML_train(std::string &task_mode, uint data_type, const void *training_data, uint n_data,
                              uint n_feature, uint label_data_type, const void *label_data,
                              std::string &model_content) {
  std::string parameters = "max_bin=254 ";
  DatasetHandle train_dataset_handler{nullptr};
  //clang-format off
  auto ret = LGBM_DatasetCreateFromMat(training_data,       // feature data ptr
                                       data_type,           // type of sampe data
                                       n_data,              // # of sample data
                                       n_feature,           // # of features
                                       1,                   // 1 row-first, 0 column-first.
                                       parameters.c_str(),  // params.
                                       nullptr,             // ref.
                                       &train_dataset_handler);

  // to set label data
  ret = LGBM_DatasetSetField(train_dataset_handler, "label", label_data, n_data, label_data_type);
  //clang-format on
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
  auto model_content_buff = std::make_unique<char[]>(out_len);
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, 0, bufflen, &out_len, model_content_buff.get());
  if (ret == -1) return nullptr;

  if (out_len > bufflen) {
    bufflen = out_len;
    model_content_buff.reset(new char[bufflen]);
    ret = LGBM_BoosterSaveModelToString(booster,
                                        0,                              // start iter idx
                                        -1,                             // end inter idx
                                        C_API_FEATURE_IMPORTANCE_GAIN,  // feature_importance_type
                                        bufflen,                        // buff len
                                        &out_len,                       // out len
                                        model_content_buff.get());
  }

  ret = LGBM_DatasetFree(train_dataset_handler);
  ret = LGBM_BoosterFree(booster);
  model_content.assign(model_content_buff.get(), bufflen);
  return booster;
}

int Utils::store_model_catalog(std::string &mode_type, std::string &oper_type, const Json_wrapper *options,
                               std::string &user_name, std::string &handler_name, std::string &model_content,
                               std::string &target_name, std::string &source_name) {
  THD *thd = current_thd;
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;
  std::string cat_table_name = "MODEL_CATALOG";

  auto cat_table_ptr = Utils::open_table_by_name(catalog_schema_name, cat_table_name, TL_WRITE);
  if (!cat_table_ptr) {
    std::ostringstream err;
    err << catalog_schema_name.c_str() << "." << cat_table_name.c_str() << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Here, why we set MODEL_CATALOG IS NOT REPLCIATED, this called by SELECT sys.ML_TRAIN()
  // it's a select statement. And in ML_TRAIN function, if the train process is proceeded successfull,
  // then stores the meta info of trainned model. If we dont rpl the meta to replica to set.
  // if binlog statement format is stmt, call statement will record, and replay on replica.
  // therefore, we do not need to record the meta info of trainned model to replica. it will be
  // replayed thee call statement on replica.
  auto org_no_replicate = cat_table_ptr->no_replicate;
  if (thd->variables.binlog_format == BINLOG_FORMAT_STMT) {
    cat_table_ptr->no_replicate = true;
  } else {
    // To write the binlog statement.
    thd->binlog_write_table_map(cat_table_ptr, true, true);
  }

  cat_table_ptr->file->ha_external_lock(thd, F_WRLCK | F_RDLCK);
  cat_table_ptr->use_all_columns();

  Field *field_ptr{nullptr};
  cat_table_ptr->file->ha_index_init(cat_table_ptr->s->next_number_index, true);
  cat_table_ptr->file->ha_index_last(cat_table_ptr->record[0]);
  int64_t next_id = (*(cat_table_ptr->field))->val_int() + 1;

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_ID)];
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(handler_name.c_str(), handler_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  field_ptr->store(model_content.data(), model_content.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OWNER)];
  field_ptr->set_notnull();
  field_ptr->store(user_name.c_str(), user_name.length(), &my_charset_utf8mb4_general_ci);

  std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  const my_timeval tm = {static_cast<int64_t>(timestamp), 0};
  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::BUILD_TIMESTAMP)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TARGET_COLUMN_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(target_name.c_str(), target_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TRAIN_TABLE_NAME)];
  field_ptr->set_notnull();
  field_ptr->store(source_name.c_str(), source_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(model_content.length());

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_TYPE)];
  field_ptr->set_notnull();
  field_ptr->store(mode_type.c_str(), mode_type.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::TASK)];
  field_ptr->set_notnull();
  field_ptr->store(oper_type.c_str(), oper_type.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::COLUMN_NAMES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);  // columns

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_EXPLANATION)];
  field_ptr->set_notnull();
  field_ptr->store(1.0);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::LAST_ACCESSED)];
  field_ptr->set_notnull();
  field_ptr->store_timestamp(&tm);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  assert(options);
  down_cast<Field_json *>(field_ptr)->store_json(options);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::NOTES)];
  field_ptr->set_notnull();
  field_ptr->store(" ", 1, &my_charset_utf8mb4_general_ci);

  auto ret = cat_table_ptr->file->ha_write_row(cat_table_ptr->record[0]);
  cat_table_ptr->file->ha_index_end();
  cat_table_ptr->file->ha_external_lock(thd, F_UNLCK);

  // restore the no_replicate.
  if (thd->variables.binlog_format == BINLOG_FORMAT_STMT) {
    cat_table_ptr->no_replicate = org_no_replicate;
  }

  Utils::close_table(cat_table_ptr);
  return ret;
}

int Utils::read_model_content(std::string model_user_name, std::string model_handle_name, Json_wrapper *options,
                              std::string &model_content_str) {
  std::string model_schema_name = "ML_SCHEMA_" + model_user_name;
  auto cat_table_ptr = Utils::open_table_by_name(model_schema_name, "MODEL_CATALOG", TL_READ);
  if (!cat_table_ptr) return HA_ERR_GENERIC;

  // get the model content from model catalog table by model handle name. table
  my_bitmap_map *old_map = tmp_use_all_columns(cat_table_ptr, cat_table_ptr->read_set);
  if (cat_table_ptr->file->ha_external_lock(current_thd, F_RDLCK)) {
    return HA_ERR_GENERIC;
  }

  if (cat_table_ptr->file->ha_rnd_init(true)) {
    cat_table_ptr->file->ha_rnd_end();
    cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);
    return HA_ERR_GENERIC;
  }

  while (cat_table_ptr->file->ha_rnd_next(cat_table_ptr->record[0]) == 0) {
    // handle_name.
    String handle_name, model_content;
    auto field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE));
    handle_name.set_charset(field_ptr->charset());
    field_ptr->val_str(&handle_name);

    // user name.
    field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OWNER));
    String owner_name;
    handle_name.set_charset(field_ptr->charset());
    field_ptr->val_str(&owner_name);
    assert(!strcmp(model_user_name.c_str(), owner_name.c_ptr_safe()));

    if (likely(strcmp(handle_name.c_ptr_safe(), model_handle_name.c_str())))
      continue;
    else {
      // model object.[trainned model content]
      field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT));
      model_content.set_charset(field_ptr->charset());
      field_ptr->val_str(&model_content);
      model_content_str.assign(model_content.c_ptr_safe(), model_content.length());

      field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA));
      assert(field_ptr->type() == MYSQL_TYPE_JSON);
      down_cast<Field_json *>(field_ptr)->val_json(options);
      break;
    }
  }  // while

  if (old_map) tmp_restore_column_map(cat_table_ptr->read_set, old_map);
  cat_table_ptr->file->ha_rnd_end();
  cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);

  Utils::close_table(cat_table_ptr);

  return 0;
}

}  // namespace ML
}  // namespace ShannonBase