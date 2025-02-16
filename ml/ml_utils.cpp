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

#include "decimal.h"
#include "sql-common/json_dom.h"
#include "sql/binlog.h"
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

std::map<std::string, ML_TASK_TYPE_T> OPT_TASKS_MAP = {{"", ML_TASK_TYPE_T::UNKNOWN},
                                                       {"CLASSIFICATION", ML_TASK_TYPE_T::CLASSIFICATION},
                                                       {"REGRESSION", ML_TASK_TYPE_T::REGRESSION},
                                                       {"FORECASTING", ML_TASK_TYPE_T::FORECASTING},
                                                       {"ANOMALY_DETECTION", ML_TASK_TYPE_T::ANOMALY_DETECTION},
                                                       {"RECOMMENDATION", ML_TASK_TYPE_T::RECOMMENDATION}};

std::map<ML_TASK_TYPE_T, std::string> TASK_NAMES_MAP = {{ML_TASK_TYPE_T::CLASSIFICATION, "CLASSIFICATION"},
                                                        {ML_TASK_TYPE_T::REGRESSION, "REGRESSION"},
                                                        {ML_TASK_TYPE_T::FORECASTING, "FORECASTING"},
                                                        {ML_TASK_TYPE_T::ANOMALY_DETECTION, "ANOMALY_DETECTION"},
                                                        {ML_TASK_TYPE_T::RECOMMENDATION, "RECOMMENDATION"}};

std::map<std::string, MODEL_PREDICTION_EXP_T> MODEL_EXPLAINERS_MAP = {
    {"MODEL_PERMUTATION_IMPORTANCE", MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE},
    {"MODEL_SHAP", MODEL_PREDICTION_EXP_T::MODEL_SHAP},
    {"MODEL_FAST_SHAP", MODEL_PREDICTION_EXP_T::MODEL_FAST_SHAP},
    {"MODEL_PARTIAL_DEPENDENCE", MODEL_PREDICTION_EXP_T::MODEL_PARTIAL_DEPENDENCE},
    {"PREDICT_PARTIAL_DEPENDENCE", MODEL_PREDICTION_EXP_T::PREDICT_PERMUTATION_IMPORTANCE},
    {"PREDICT_SHAP", MODEL_PREDICTION_EXP_T::PREDICT_SHAP}};

std::map<MODEL_STATUS_T, std::string> MODEL_STATUS_MAP = {
    {MODEL_STATUS_T::CREATING, "CREATEING"}, {MODEL_STATUS_T::READY, "READY"}, {MODEL_STATUS_T::ERROR, "ERROR"}};

std::map<MODEL_FORMAT_T, std::string> MODEL_FORMATS_MAP = {{MODEL_FORMAT_T::VER_1, "HWMLv1.0"},
                                                           {MODEL_FORMAT_T::ONNX, "ONNX"}};

std::map<MODEL_QUALITY_T, std::string> MODEL_QUALITIES_MAP = {{MODEL_QUALITY_T::LOW, "LOW"},
                                                              {MODEL_QUALITY_T::HIGH, "HIGH"}};

int Utils::splitString(const std::string &str, char delimiter, std::vector<std::string> &result) {
  std::stringstream ss(str);
  std::string item;

  while (std::getline(ss, item, delimiter)) {
    // to erase the space chars.
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);
    result.push_back(item);
  }

  return 0;
}

int Utils::parse_json(Json_wrapper &options, OPTION_VALUE_T &option_value, std::string &key, size_t depth) {
  enum_json_type type = options.type();
  // Treat strings saved in opaque as plain json strings
  // @see val_json_func_field_subselect()
  if (type == enum_json_type::J_OPAQUE && options.field_type() == MYSQL_TYPE_VAR_STRING)
    type = enum_json_type::J_STRING;

  switch (type) {
    case enum_json_type::J_TIME:
    case enum_json_type::J_DATE:
    case enum_json_type::J_DATETIME:
    case enum_json_type::J_TIMESTAMP:
      assert(false);
      break;
    case enum_json_type::J_ARRAY: {
      const size_t array_len = options.length();
      for (uint32 i = 0; i < array_len; ++i) {
        auto opt = options[i];
        if (parse_json(opt, option_value, key, depth)) return true; /* purecov: inspected */
      }
      break;
    }
    case enum_json_type::J_BOOLEAN: {
      options.get_boolean() ? option_value[key].push_back("true") : option_value[key].push_back("false");
    } break;
    case enum_json_type::J_DECIMAL: {
      int length = DECIMAL_MAX_STR_LENGTH + 1;
      auto buffer = std::unique_ptr<char[]>(new char[length + 1]);
      char *ptr = buffer.get() + length;

      my_decimal m;
      std::string decimal_str;
      if (options.get_decimal_data(&m) || decimal2string(&m, ptr, &length)) return true; /* purecov: inspected */
      option_value[key].push_back(decimal_str);
      break;
    }
    case enum_json_type::J_DOUBLE: {
      auto double_value = std::to_string(options.get_double());
      option_value[key].push_back(double_value);
      break;
    }
    case enum_json_type::J_INT: {
      auto int_value = std::to_string(options.get_int());
      option_value[key].push_back(int_value);
      break;
    }
    case enum_json_type::J_NULL:
      option_value[key].push_back("null");
      break;
    case enum_json_type::J_OBJECT: {
      for (const auto &iter : Json_object_wrapper(options)) {
        const MYSQL_LEX_CSTRING &key_lex = iter.first;
        std::string option_key(key_lex.str, key_lex.length);
        option_value[option_key];
        auto iter_value = iter.second;
        if (parse_json(iter_value, option_value, option_key, depth)) return true; /* purecov: inspected */
      }
      break;
    }
    case enum_json_type::J_OPAQUE: {
      assert(false);
      break;
    }
    case enum_json_type::J_STRING: {
      std::string data_str(options.get_data(), options.get_data_length());
      option_value[key].push_back(data_str);
      break;
    }
    case enum_json_type::J_UINT: {
      auto int_value = std::to_string(options.get_uint());
      option_value[key].push_back(int_value);
      break;
    }
    default:
      /* purecov: begin inspected */
      DBUG_PRINT("info", ("JSON wrapper: unexpected type %d", static_cast<int>(options.type())));
      assert(false);
      my_error(ER_INTERNAL_ERROR, MYF(0), "JSON wrapper: unexpected type");
      /* purecov: end inspected */
  }
  return 0;
}

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

Json_object *Utils::build_up_model_metadata(
    std::string &task, std::string &target_column_name, std::string &tain_table_name,
    std::vector<std::string> &featurs_name, Json_object *model_explanation, std::string &notes, std::string &format,
    std::string &status, std::string &model_quality, double training_time, std::string &algorithm_name,
    double training_score, size_t n_rows, size_t n_columns, size_t n_selected_rows, size_t n_selected_columns,
    std::string &optimization_metric, std::vector<std::string> &selected_column_names, double contamination,
    Json_wrapper *train_options, Json_object *training_params, Json_object *onnx_inputs_info,
    Json_object *onnx_outputs_info, Json_object *training_drift_metric, size_t chunks) {
  auto now = std::chrono::system_clock::now();
  auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  Json_object *model_obj = new (std::nothrow) Json_object();
  if (model_obj == nullptr) return nullptr;

  model_obj->add_clone("options", train_options->clone_dom().get());
  model_obj->add_clone("training_params", training_params);
  model_obj->add_clone("onnx_inputs_info", onnx_inputs_info);
  model_obj->add_clone("onnx_outputs_info", onnx_outputs_info);
  model_obj->add_clone("training_drift_metric", training_drift_metric);

  model_obj->add_clone("task", new (std::nothrow) Json_string(task));
  model_obj->add_clone("build_timestamp", new (std::nothrow) Json_double(now_seconds));
  model_obj->add_clone("target_column_name", new (std::nothrow) Json_string(target_column_name));
  model_obj->add_clone("train_table_name", new (std::nothrow) Json_string(tain_table_name));

  Json_array *column_names_arr = new (std::nothrow) Json_array();
  for (auto &feature_name : featurs_name) {
    column_names_arr->append_clone(new (std::nothrow) Json_string(feature_name));
  }
  model_obj->add_clone("column_names", column_names_arr);

  model_obj->add_clone("model_explanation", model_explanation);

  model_obj->add_clone("notes", new (std::nothrow) Json_string(notes));
  model_obj->add_clone("format", new (std::nothrow) Json_string(format));
  model_obj->add_clone("status", new (std::nothrow) Json_string(status));

  model_obj->add_clone("model_quality", new (std::nothrow) Json_string(model_quality));
  model_obj->add_clone("training_time", new (std::nothrow) Json_double(training_time));
  model_obj->add_clone("algorithm_name", new (std::nothrow) Json_string(algorithm_name));
  model_obj->add_clone("training_score", new (std::nothrow) Json_double(training_score));
  model_obj->add_clone("n_rows", new (std::nothrow) Json_double(n_rows));
  model_obj->add_clone("n_columns", new (std::nothrow) Json_double(n_columns));
  model_obj->add_clone("n_selected_rows", new (std::nothrow) Json_double(n_selected_rows));
  model_obj->add_clone("n_selected_columns", new (std::nothrow) Json_double(n_selected_columns));
  model_obj->add_clone("optimization_metric", new (std::nothrow) Json_string(optimization_metric));

  Json_array *selected_column_names_arr = new (std::nothrow) Json_array();
  for (auto &sel_col_name : selected_column_names) {
    selected_column_names_arr->append_clone(new (std::nothrow) Json_string(sel_col_name));
  }
  model_obj->add_clone("selected_column_names", selected_column_names_arr);

  model_obj->add_clone("contamination", new (std::nothrow) Json_double(contamination));

  model_obj->add_clone("chunks", new (std::nothrow) Json_double(chunks));

  return model_obj;
}

BoosterHandle Utils::load_trained_model_from_string(std::string &model_content) {
  BoosterHandle handle{nullptr};
  int n_iteration;
  auto ret = LGBM_BoosterLoadModelFromString(model_content.c_str(), &n_iteration, &handle);
  return (ret == 0) ? handle : nullptr;
}

int Utils::store_model_catalog(size_t model_obj_size, const Json_wrapper *model_meta, std::string &handler_name) {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
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
  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_ID)];
  int64_t next_id = field_ptr->val_int() + 1;
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(handler_name.c_str(), handler_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_null();  // in ver 9.0, it's set to null.

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OWNER)];
  field_ptr->set_notnull();
  field_ptr->store(user_name.c_str(), user_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_OBJECT_SIZE)];
  field_ptr->set_notnull();
  field_ptr->store(model_obj_size);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA)];
  field_ptr->set_notnull();
  assert(model_meta);
  down_cast<Field_json *>(field_ptr)->store_json(model_meta);

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

int Utils::store_model_object_catalog(std::string &model_handle_name, Json_wrapper *model_content) {
  assert(model_handle_name.length() && model_content);

  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
  std::string catalog_schema_name = "ML_SCHEMA_" + user_name;
  std::string cat_table_name = "MODEL_OBJECT_CATALOG";

  auto cat_table_ptr = Utils::open_table_by_name(catalog_schema_name, cat_table_name, TL_WRITE);
  if (!cat_table_ptr) {
    std::ostringstream err;
    err << catalog_schema_name.c_str() << "." << cat_table_name.c_str() << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

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
  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::CHUNK_ID)];
  int64_t next_id = field_ptr->val_int() + 1;
  field_ptr->set_notnull();
  field_ptr->store(next_id);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
  field_ptr->set_notnull();
  field_ptr->store(model_handle_name.c_str(), model_handle_name.length(), &my_charset_utf8mb4_general_ci);

  field_ptr = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
  field_ptr->set_notnull();
  assert(model_content);
  down_cast<Field_json *>(field_ptr)->store_json(model_content);

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

int Utils::read_model_content(std::string &model_handle_name, Json_wrapper &options) {
  std::string model_user_name(current_thd->security_context()->user().str);
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

    if (likely(strcmp(handle_name.c_ptr_safe(), model_handle_name.c_str())))  // not this one.
      continue;
    else {
      // model object.[trainned model content]
      field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_METADATA));
      assert(field_ptr->type() == MYSQL_TYPE_JSON);
      down_cast<Field_json *>(field_ptr)->val_json(&options);
      break;
    }
  }  // while

  if (old_map) tmp_restore_column_map(cat_table_ptr->read_set, old_map);
  cat_table_ptr->file->ha_rnd_end();
  cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);
  Utils::close_table(cat_table_ptr);
  return 0;
}

int Utils::read_model_object_content(std::string &model_handle_name, std::string &model_content) {
  std::string model_user_name(current_thd->security_context()->user().str);
  std::string model_schema_name = "ML_SCHEMA_" + model_user_name;
  auto cat_table_ptr = Utils::open_table_by_name(model_schema_name, "MODEL_OBJECT_CATALOG", TL_READ);
  if (!cat_table_ptr) return HA_ERR_GENERIC;

  // get the model content from model object catalog table by model handle name. table
  my_bitmap_map *old_map = tmp_use_all_columns(cat_table_ptr, cat_table_ptr->read_set);
  if (cat_table_ptr->file->ha_external_lock(current_thd, F_WRLCK | F_RDLCK)) {
    return HA_ERR_GENERIC;
  }

  if (cat_table_ptr->file->ha_rnd_init(true)) {
    cat_table_ptr->file->ha_rnd_end();
    cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);
    return HA_ERR_GENERIC;
  }

  while (cat_table_ptr->file->ha_rnd_next(cat_table_ptr->record[0]) == 0) {
    String handle_name;
    auto field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_CATALOG_FIELD_INDEX::MODEL_HANDLE));
    handle_name.set_charset(field_ptr->charset());
    field_ptr->val_str(&handle_name);

    if (likely(strcmp(handle_name.c_ptr_safe(), model_handle_name.c_str())))  // not this one.
      continue;
    else {
      // model object.[trainned model content]
      Json_wrapper json_content;
      field_ptr = *(cat_table_ptr->field + static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::MODEL_OBJECT));
      assert(field_ptr->type() == MYSQL_TYPE_JSON);
      down_cast<Field_json *>(field_ptr)->val_json(&json_content);
#ifdef LIGHTGBM_UNIFY_FORMAT
      String model_str;
      json_content.to_string(&model_str, true, "read_model_object_content", [] { assert(false); });
      model_content = model_str.c_ptr_safe();
#else
      std::string keystr;
      OPTION_VALUE_T option_value;
      Utils::parse_json(json_content, option_value, keystr, 0);
      assert(option_value["SHANNON_LIGHTGBM_CONTENT"].size() == 1);
      model_content = option_value["SHANNON_LIGHTGBM_CONTENT"][0];
#endif
      break;
    }
  }

  if (old_map) tmp_restore_column_map(cat_table_ptr->read_set, old_map);
  cat_table_ptr->file->ha_rnd_end();
  cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);

  Utils::close_table(cat_table_ptr);
  return 0;
}

// do ML train jobs.
int Utils::ML_train(std::string &task_mode, uint data_type, const void *training_data, uint n_data,
                    const char **features_name, uint n_feature, uint label_data_type, const void *label_data,
                    std::string &model_content) {
  DatasetHandle train_dataset_handler{nullptr};
  //clang-format off
  auto ret = LGBM_DatasetCreateFromMat(training_data,      // feature data ptr
                                       data_type,          // type of sampe data
                                       n_data,             // # of sample data
                                       n_feature,          // # of features
                                       1,                  // 1 row-first, 0 column-first.
                                       task_mode.c_str(),  // params.
                                       nullptr,            // ref.
                                       &train_dataset_handler);

  // to set label data
  // clang-format on

  ret = LGBM_DatasetSetField(train_dataset_handler, "label", label_data, n_data, label_data_type);
  if (ret) {
    LGBM_DatasetFree(train_dataset_handler);
    return ret;
  }
  ret = LGBM_DatasetSetFeatureNames(train_dataset_handler, features_name, n_feature);
  if (ret) {
    LGBM_DatasetFree(train_dataset_handler);
    return ret;
  }

  BoosterHandle booster;
  ret = LGBM_BoosterCreate(train_dataset_handler, task_mode.c_str(), &booster);
  if (ret) {
    LGBM_DatasetFree(train_dataset_handler);
    return ret;
  }
  LGBM_BoosterAddValidData(booster, train_dataset_handler);

  int finished{0};
  for (auto iter = 0; iter < 100; ++iter) {
    ret = LGBM_BoosterUpdateOneIter(booster, &finished);
    if (ret) {
      LGBM_DatasetFree(train_dataset_handler);
      LGBM_BoosterFree(booster);
      return ret;
    }
    if (finished) break;
  }

  /* to dump the model conent to a string in json fomrat. But on till now ver 4.6.0. it can
   * not recreate model from the dumped string. The lightgbm is still on unifying the model
   * format. Therefore, we will use `LGBM_BoosterSaveModelToString` to a string, then wrappe
   * it into a json format. When the unified model format is ready to remove the wrapper by
   * defining `LIGHTGBM_UNIFY_FORMAT`.
   */
  int64_t bufflen{1024}, out_len{0};
  auto model_content_buff = std::make_unique<char[]>(bufflen);
  memset(model_content_buff.get(), 0x0, bufflen);

#ifdef LIGHTGBM_UNIFY_FORMAT
  ret =
      LGBM_BoosterDumpModel(booster, 0, -1, C_API_FEATURE_IMPORTANCE_GAIN, bufflen, &out_len, model_content_buff.get());
#else
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, C_API_FEATURE_IMPORTANCE_GAIN, bufflen, &out_len,
                                      model_content_buff.get());
#endif
  if (ret) {
    LGBM_DatasetFree(train_dataset_handler);
    LGBM_BoosterFree(booster);
    return ret;
  }

  if (out_len > bufflen) {
    model_content_buff.reset(new char[out_len + 1]);
    memset(model_content_buff.get(), 0x0, bufflen);
    // clang-format off
  #ifdef LIGHTGBM_UNIFY_FORMAT
    LGBM_BoosterDumpModel(booster,
                          0,                              // start iter idx
                          1,                             // end inter idx
                          C_API_FEATURE_IMPORTANCE_GAIN,  // feature_importance_type
                          out_len,                        // buff len
                          &out_len,                       // out len
                          model_content_buff.get());
  #else
    LGBM_BoosterSaveModelToString(booster,
                                  0,
                                  -1,
                                  C_API_FEATURE_IMPORTANCE_GAIN,
                                  out_len,
                                  &out_len,
                                  model_content_buff.get());
  #endif
  }
  model_content.assign(model_content_buff.get(), out_len);
  #ifndef LIGHTGBM_UNIFY_FORMAT
    Json_object *model_obj = new (std::nothrow) Json_object();
    if (model_obj == nullptr) return -1;
    model_obj->add_clone("SHANNON_LIGHTGBM_CONTENT", new (std::nothrow) Json_string(model_content));
    Json_wrapper model_content_json(model_obj);
    String json_format_content;
    model_content_json.to_string(&json_format_content, false, "ML_train", [] { assert(false); });
    model_content.assign(json_format_content.c_ptr_safe(), json_format_content.length());
  #endif
  // clang-format on

  LGBM_DatasetFree(train_dataset_handler);
  LGBM_BoosterFree(booster);
  return ret;
}

double Utils::calculate_balanced_accuracy(size_t n_samples, std::vector<double> &predictions,
                                          std::vector<float> &label_data) {
  // calculate the balanced accuracy.
  int TP = 0, TN = 0, FP = 0, FN = 0;
  for (size_t i = 0; i < n_samples; i++) {
    int predicted = (predictions[i] >= 0.5) ? 1 : 0;
    auto actual = (int)label_data[i];

    if (predicted == 1 && actual == 1) TP++;  // true positive
    if (predicted == 1 && actual == 0) FP++;  // false positive
    if (predicted == 0 && actual == 0) TN++;  // true negtive
    if (predicted == 0 && actual == 1) FN++;  // false negtive
  }

  auto sensitivity = (TP + FN) > 0 ? (double)TP / (TP + FN) : 0;
  auto specificity = (TN + FP) > 0 ? (double)TN / (TN + FP) : 0;
  auto balanced_accuracy = (sensitivity + specificity) / 2.0;

  return balanced_accuracy;
}
double Utils::model_score(std::string &model_handle_name, int metric_type, size_t n_samples, size_t n_features,
                          std::vector<double> &testing_data, std::vector<float> &label_data) {
  std::string score_params;
  BoosterHandle handler = Utils::load_trained_model_from_string(Loaded_models[model_handle_name]);
  if (!handler) {
    std::ostringstream err;
    err << model_handle_name << " can not load model from content string";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto data_type = sizeof(double) == 8 ? C_API_DTYPE_FLOAT64 : C_API_DTYPE_FLOAT32;
  std::vector<double> predictions(n_samples);
  int64_t out_len;
  // clang-format off
  auto ret = LGBM_BoosterPredictForMat(handler,               /* model handler */
                            testing_data.data(),   /* test data */
                            data_type,             /* testing data type */
                            n_samples,             /* # of testing data */
                            n_features,            /* # of features of testing data */
                            1,                     /* row-based format */
                            C_API_PREDICT_NORMAL,  /* What should be predicted */
                            0,                     /* Start index of the iteration */ 
                            -1,                    /* # of iteration for prediction, <= 0 no limit*/
                            score_params.c_str(),  /* params */
                            &out_len,              /* Length of output result[out] */
                            predictions.data());   /* Pointer to array with predictions[out] */
  //clang-format on
  if (ret) {
    LGBM_BoosterFree(handler);
    return 0.0f;
  }
  LGBM_BoosterFree(handler);

  // calculate the balanced accuracy.
  double balanced_accuracy{0.0};
  switch (metric_type){
    case 0:  // balanced_accuracy
      balanced_accuracy = Utils::calculate_balanced_accuracy(n_samples, predictions, label_data);
      break;
    case 1: {

    } break;
    default:
     break;
  }
  return balanced_accuracy;
}

}  // namespace ML
}  // namespace ShannonBase