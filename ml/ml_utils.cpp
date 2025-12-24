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

#include <fstream>
#include <iostream>
#include <sstream>

#include "include/my_inttypes.h"

#include "decimal.h"
#include "sql-common/json_dom.h"
#include "sql/binlog.h"
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "ml_algorithm.h"
#include "storage/rapid_engine/include/rapid_loaded_table.h"  //loaded table.

namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string_view, ML_TASK_TYPE_T, std::less<>> OPT_TASKS_MAP = {
    {"", ML_TASK_TYPE_T::UNKNOWN},
    {"CLASSIFICATION", ML_TASK_TYPE_T::CLASSIFICATION},
    {"REGRESSION", ML_TASK_TYPE_T::REGRESSION},
    {"FORECASTING", ML_TASK_TYPE_T::FORECASTING},
    {"ANOMALY_DETECTION", ML_TASK_TYPE_T::ANOMALY_DETECTION},
    {"RECOMMENDATION", ML_TASK_TYPE_T::RECOMMENDATION}};

std::map<ML_TASK_TYPE_T, std::string> TASK_NAMES_MAP = {
    {ML_TASK_TYPE_T::CLASSIFICATION, "CLASSIFICATION"},
    {ML_TASK_TYPE_T::REGRESSION, "REGRESSION"},
    {ML_TASK_TYPE_T::FORECASTING, "FORECASTING"},
    {ML_TASK_TYPE_T::ANOMALY_DETECTION, "ANOMALY_DETECTION"},
    {ML_TASK_TYPE_T::RECOMMENDATION, "RECOMMENDATION"}};

std::map<std::string, MODEL_PREDICTION_EXP_T, std::less<>> MODEL_EXPLAINERS_MAP = {
    {"MODEL_PERMUTATION_IMPORTANCE", MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE},
    {"MODEL_SHAP", MODEL_PREDICTION_EXP_T::MODEL_SHAP},
    {"MODEL_FAST_SHAP", MODEL_PREDICTION_EXP_T::MODEL_FAST_SHAP},
    {"MODEL_PARTIAL_DEPENDENCE", MODEL_PREDICTION_EXP_T::MODEL_PARTIAL_DEPENDENCE},
    {"PREDICT_PARTIAL_DEPENDENCE", MODEL_PREDICTION_EXP_T::PREDICT_PERMUTATION_IMPORTANCE},
    {"PREDICT_SHAP", MODEL_PREDICTION_EXP_T::PREDICT_SHAP}};

std::map<MODEL_STATUS_T, std::string> MODEL_STATUS_MAP = {
    {MODEL_STATUS_T::CREATING, "CREATEING"},
    {MODEL_STATUS_T::READY, "READY"},
    {MODEL_STATUS_T::ERROR, "ERROR"}};

std::map<MODEL_FORMAT_T, std::string> MODEL_FORMATS_MAP = {
    {MODEL_FORMAT_T::VER_1, "HWMLv1.0"},
    {MODEL_FORMAT_T::ONNX, "ONNX"}};

std::map<MODEL_QUALITY_T, std::string> MODEL_QUALITIES_MAP = {
    {MODEL_QUALITY_T::LOW, "LOW"},
    {MODEL_QUALITY_T::HIGH, "HIGH"}};
// clang-format on

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
std::string Utils::read_file(std::string &file_path) {
  if (check_valid_path(file_path.c_str(), file_path.length())) {
    return "";
  }

  std::ifstream file(file_path.c_str(), std::ios::binary);
  if (!file) {
    return "";
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
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
      auto buffer = std::unique_ptr<char[]>(new char[length + 10]);
      char *ptr = buffer.get() + length;

      my_decimal m;
      std::string decimal_str;
      if (options.get_decimal_data(&m)) return true; /* purecov: inspected */
      decimal2string(&m, ptr, &length);
      decimal_str = ptr;
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
        if (option_key.length()) option_value[option_key];
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

int Utils::check_table_available(std::string &sch_tb_name) {
  std::string schema_name, table_name;
  std::ostringstream err;

  auto pos = sch_tb_name.find('.');
  if (pos != std::string::npos) {
    schema_name = sch_tb_name.substr(0, pos);
    table_name = sch_tb_name.substr(pos + 1);
  } else
    return HA_ERR_GENERIC;

  auto share = ShannonBase::shannon_loaded_tables->get(schema_name.c_str(), table_name.c_str());
  if (!share) {
    err << sch_tb_name << " NOT loaded into rapid engine for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  return 0;
}

// open table by name. return table ptr, otherwise return nullptr.
TABLE *Utils::open_table_by_name(const std::string &schema_name, const std::string &table_name, thr_lock_type) {
  THD *thd = current_thd;
  /**
   * due to in function, `select xxxx`, when the statment executed, it enter lock table mode
   * but there's not even a opened table, so that, here we try to open a table, it failed before
   * exiting the lock table mode. such as executing `selecct ml_predict_row(xxx) int xx;`
   */
  Table_ref table_list;
  table_list.db = schema_name.c_str();
  table_list.db_length = schema_name.length();
  table_list.table_name = table_name.c_str();
  table_list.table_name_length = table_name.length();
  table_list.alias = table_name.c_str();
  table_list.set_lock({TL_READ, THR_DEFAULT});
  MDL_REQUEST_INIT(&table_list.mdl_request,
                   MDL_key::TABLE,       // namespace
                   schema_name.c_str(),  // db
                   table_name.c_str(),   // name
                   MDL_SHARED_READ,      // type
                   MDL_TRANSACTION);     // duration

  Table_ref *table_list_ptr = &table_list;
  uint counter{0};
  uint flags = MYSQL_OPEN_GET_NEW_TABLE | MYSQL_OPEN_IGNORE_FLUSH;
  if (open_tables(thd, &table_list_ptr, &counter, flags)) return nullptr;
  return table_list.table;
}

int Utils::close_table(TABLE *table [[maybe_unused]]) {
  assert(table);
  // The table will be closed by `close_thread_tables()` in the caller.
  // No manual cleanup is required here.
  // close_thread_table(current_thd, &table);
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
    my_error(ER_ML_FAIL, MYF(0), "Unsupported secondary storage engine");
    return nullptr;
  }

  // Get handler to the secondary engine into which the table will be loaded.
  const bool is_partitioned = source_table_ptr->s->m_part_info != nullptr;
  return get_new_handler(source_table_ptr->s, is_partitioned, thd->mem_root, hton);
}

int Utils::get_txt2num_dict(Json_wrapper &model_meta, txt2numeric_map_t &txt2num_dict) {
  MYSQL_LEX_CSTRING lex_key;
  lex_key.str = ML_KEYWORDS::txt2num_dict;
  lex_key.length = strlen(lex_key.str);
  auto result = model_meta.lookup(lex_key);
  assert(!result.empty());
  if (result.type() != enum_json_type::J_OBJECT) return HA_ERR_GENERIC;

  OPTION_VALUE_T dict_opt;
  std::string strkey;
  if (Utils::parse_json(result, dict_opt, strkey, 0)) return HA_ERR_GENERIC;
  for (auto &it : dict_opt) {
    auto name = it.first;
    std::set<std::string> val_set;
    for (auto val_str : it.second) {
      val_set.insert(val_str);
    }
    txt2num_dict.insert({name, val_set});
  }
  return 0;
}
// return the # of rows read from table successfully. otherwise, 0.
// a new param, check_condition needed? std::function<void(double)> ? to check the data must
// satisfy the the condition.
int Utils::read_data(TABLE *table, std::vector<double> &train_data, std::vector<std::string> &features_name,
                     std::string &label_name, std::vector<float> &label_data, int &n_class,
                     txt2numeric_map_t &txt2numeric_dict) {
  THD *thd = current_thd;
  auto n_read{0u};

  txt2numeric_map_t txt2numeric;
  // read the training data from target table.
  for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
    Field *field_ptr = *(table->field + field_id);
    if (field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    txt2numeric[field_ptr->field_name];

    if (likely(!strcmp(field_ptr->field_name, label_name.c_str()))) continue;
    features_name.push_back(field_ptr->field_name);
  }

  const dd::Table *table_obj{nullptr};
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (!table_obj && table->s->table_category != TABLE_UNKNOWN_CATEGORY) {
    if (thd->dd_client()->acquire(table->s->db.str, table->s->table_name.str, &table_obj)) {
      return n_read;
    }
  }

  // must read from secondary engine.
  unique_ptr_destroy_only<handler> sec_tb_handler(Utils::get_secondary_handler(table));
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);
  sec_tb_handler->ha_open(table, table->s->normalized_path.str, O_RDONLY, HA_OPEN_IGNORE_IF_LOCKED, table_obj);
  if (sec_tb_handler && sec_tb_handler->ha_external_lock(thd, F_RDLCK)) {
    sec_tb_handler->ha_close();
    return n_read;
  }

  if (sec_tb_handler->ha_rnd_init(true)) {
    sec_tb_handler->ha_external_lock(thd, F_UNLCK);
    sec_tb_handler->ha_close();
    return n_read;
  }

  std::map<float, int> n_classes;
  while (sec_tb_handler->ha_rnd_next(table->record[0]) != HA_ERR_END_OF_FILE) {
    for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
      Field *field_ptr = *(table->field + field_id);
      if (field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

      auto data_val{0.0};
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
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING: {  // convert txt string to numeric
          String buf;
          buf.set_charset(field_ptr->charset());
          field_ptr->val_str(&buf);
          txt2numeric[field_ptr->field_name].insert(buf.c_ptr_safe());
          assert(txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()) != txt2numeric[field_ptr->field_name].end());
          data_val = std::distance(txt2numeric[field_ptr->field_name].begin(),
                                   txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()));
          txt2numeric_dict[field_ptr->field_name].insert(buf.c_ptr_safe());
        } break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME: {
          data_val = field_ptr->val_real();
        } break;
        default:
          assert(false);
          break;
      }

      if (likely(strcmp(field_ptr->field_name, label_name.c_str()))) {
        train_data.push_back(data_val);
      } else {  // is label data.
        if (n_classes.find((float)data_val) == n_classes.end()) n_classes[(float)data_val] = n_classes.size();
        label_data.push_back(n_classes[(float)data_val]);
      }
    }  // for
    n_read++;
  }  // while

  if (old_map) tmp_restore_column_map(table->read_set, old_map);

  sec_tb_handler->ha_rnd_end();
  sec_tb_handler->ha_external_lock(thd, F_UNLCK);
  // to close the secondary engine table.
  sec_tb_handler->ha_close();

  n_class = n_classes.size();
  return n_read;
}

Json_object *Utils::build_up_model_metadata(
    std::string &task, std::string &target_column_name, std::string &tain_table_name,
    std::vector<std::string> &featurs_name, Json_object *model_explanation, std::string &notes, std::string &format,
    std::string &status, std::string &model_quality, double training_time, std::string &algorithm_name,
    double training_score, size_t n_rows, size_t n_columns, size_t n_selected_rows, size_t n_selected_columns,
    std::string &optimization_metric, std::vector<std::string> &selected_column_names, double contamination,
    Json_wrapper *train_options, std::string &training_params, Json_object *onnx_inputs_info,
    Json_object *onnx_outputs_info, Json_object *training_drift_metric, size_t chunks,
    txt2numeric_map_t &txt2num_dict) {
  auto now = std::chrono::system_clock::now();
  auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  Json_object *model_obj = new (std::nothrow) Json_object();
  if (model_obj == nullptr) return nullptr;

  if (training_params.empty()) training_params = "{}";

  model_obj->add_clone(ML_KEYWORDS::options, train_options->clone_dom().get());
  model_obj->add_clone(ML_KEYWORDS::training_params, new (std::nothrow) Json_string(training_params));
  model_obj->add_clone(ML_KEYWORDS::onnx_inputs_info, onnx_inputs_info);
  model_obj->add_clone(ML_KEYWORDS::onnx_outputs_info, onnx_outputs_info);
  model_obj->add_clone(ML_KEYWORDS::training_drift_metric, training_drift_metric);

  model_obj->add_clone(ML_KEYWORDS::task, new (std::nothrow) Json_string(task));
  model_obj->add_clone(ML_KEYWORDS::build_timestamp, new (std::nothrow) Json_double(now_seconds));
  model_obj->add_clone(ML_KEYWORDS::target_column_name, new (std::nothrow) Json_string(target_column_name));
  model_obj->add_clone(ML_KEYWORDS::train_table_name, new (std::nothrow) Json_string(tain_table_name));

  Json_array *column_names_arr = new (std::nothrow) Json_array();
  for (auto &feature_name : featurs_name) {
    column_names_arr->append_clone(new (std::nothrow) Json_string(feature_name));
  }
  model_obj->add_clone(ML_KEYWORDS::column_names, column_names_arr);

  model_obj->add_clone(ML_KEYWORDS::model_explanation, model_explanation);

  model_obj->add_clone(ML_KEYWORDS::notes, new (std::nothrow) Json_string(notes));
  model_obj->add_clone(ML_KEYWORDS::format, new (std::nothrow) Json_string(format));
  model_obj->add_clone(ML_KEYWORDS::status, new (std::nothrow) Json_string(status));

  model_obj->add_clone(ML_KEYWORDS::model_quality, new (std::nothrow) Json_string(model_quality));
  model_obj->add_clone(ML_KEYWORDS::training_time, new (std::nothrow) Json_double(training_time));
  model_obj->add_clone(ML_KEYWORDS::algorithm_name, new (std::nothrow) Json_string(algorithm_name));
  model_obj->add_clone(ML_KEYWORDS::training_score, new (std::nothrow) Json_double(training_score));
  model_obj->add_clone(ML_KEYWORDS::n_rows, new (std::nothrow) Json_double(n_rows));
  model_obj->add_clone(ML_KEYWORDS::n_columns, new (std::nothrow) Json_double(n_columns));
  model_obj->add_clone(ML_KEYWORDS::n_selected_rows, new (std::nothrow) Json_double(n_selected_rows));
  model_obj->add_clone(ML_KEYWORDS::n_selected_columns, new (std::nothrow) Json_double(n_selected_columns));
  model_obj->add_clone(ML_KEYWORDS::optimization_metric, new (std::nothrow) Json_string(optimization_metric));

  Json_array *selected_column_names_arr = new (std::nothrow) Json_array();
  for (auto &sel_col_name : selected_column_names) {
    selected_column_names_arr->append_clone(new (std::nothrow) Json_string(sel_col_name));
  }
  model_obj->add_clone(ML_KEYWORDS::selected_column_names, selected_column_names_arr);

  model_obj->add_clone(ML_KEYWORDS::contamination, new (std::nothrow) Json_double(contamination));

  model_obj->add_clone(ML_KEYWORDS::chunks, new (std::nothrow) Json_double(chunks));

  if (txt2num_dict.size()) {
    Json_object *txt2num_dict_obj = new (std::nothrow) Json_object();
    for (const auto &iter : txt2num_dict) {
      const std::string &key = iter.first;
      const std::set<std::string> &value = iter.second;
      Json_array *value_arr = new (std::nothrow) Json_array();
      for (const auto &val : value) {
        value_arr->append_clone(new (std::nothrow) Json_string(val));
      }
      txt2num_dict_obj->add_clone(key.c_str(), value_arr);
    }
    model_obj->add_clone(ML_KEYWORDS::txt2num_dict, txt2num_dict_obj);
  }
  return model_obj;
}

BoosterHandle Utils::load_trained_model_from_string(std::string &model_content) {
  BoosterHandle handle{nullptr};
  int n_iteration;
  auto ret = LGBM_BoosterLoadModelFromString(model_content.c_str(), &n_iteration, &handle);
  return (ret == 0) ? handle : nullptr;
}

int Utils::read_model_content(std::string &model_handle_name, Json_wrapper &options) {
  std::string model_user_name(current_thd->security_context()->user().str);
  std::string model_schema_name = "ML_SCHEMA_" + model_user_name;
  auto cat_table_ptr = Utils::open_table_by_name(model_schema_name, "MODEL_CATALOG", TL_READ);
  if (!cat_table_ptr) return HA_ERR_GENERIC;
  cat_table_ptr->file->init_table_handle_for_HANDLER();

  // get the model content from model catalog table by model handle name. table
  my_bitmap_map *old_map = tmp_use_all_columns(cat_table_ptr, cat_table_ptr->read_set);
  if (cat_table_ptr->file->ha_external_lock(current_thd, F_RDLCK)) {
    Utils::close_table(cat_table_ptr);
    return HA_ERR_GENERIC;
  }

  if (cat_table_ptr->file->inited == handler::NONE && cat_table_ptr->file->ha_rnd_init(true)) {
    cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);
    Utils::close_table(cat_table_ptr);
    return HA_ERR_GENERIC;
  }

  while (cat_table_ptr->file->ha_rnd_next(cat_table_ptr->record[0]) != HA_ERR_END_OF_FILE) {
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
  model_content.clear();

  std::string model_user_name(current_thd->security_context()->user().str);
  std::string model_schema_name = "ML_SCHEMA_" + model_user_name;
  auto cat_table_ptr = Utils::open_table_by_name(model_schema_name, "MODEL_OBJECT_CATALOG", TL_READ);
  if (!cat_table_ptr) return HA_ERR_GENERIC;
  cat_table_ptr->file->init_table_handle_for_HANDLER();

  // get the model content from model catalog table by model handle name. table
  my_bitmap_map *old_map = tmp_use_all_columns(cat_table_ptr, cat_table_ptr->read_set);
  if (cat_table_ptr->file->ha_external_lock(current_thd, F_RDLCK)) {
    tmp_restore_column_map(cat_table_ptr->read_set, old_map);
    Utils::close_table(cat_table_ptr);
    return HA_ERR_GENERIC;
  }

  if (cat_table_ptr->file->inited == handler::NONE && cat_table_ptr->file->ha_rnd_init(true)) {
    cat_table_ptr->file->ha_external_lock(current_thd, F_UNLCK);
    Utils::close_table(cat_table_ptr);
    return HA_ERR_GENERIC;
  }

  struct Chunk {
    uint32_t chunk_id;
    String data;
  };
  std::vector<Chunk> chunks;

  while (cat_table_ptr->file->ha_rnd_next(cat_table_ptr->record[0]) != HA_ERR_END_OF_FILE) {
    String handle;
    Field *handle_field = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::MODEL_HANDLE)];
    handle_field->val_str(&handle);

    if (handle.c_ptr() && strcmp(handle.c_ptr(), model_handle_name.c_str()) == 0) {
      Field *chunk_id_field = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::CHUNK_ID)];
      uint32_t chunk_id = static_cast<uint32_t>(chunk_id_field->val_int());

      Field *model_obj_field = cat_table_ptr->field[static_cast<int>(MODEL_OBJECT_CATALOG_FIELD_INDEX::MODEL_OBJECT)];
      String model_obj;
      model_obj_field->val_str(&model_obj);

      chunks.push_back({chunk_id, std::move(model_obj)});
    }
  }
  if (old_map) tmp_restore_column_map(cat_table_ptr->read_set, old_map);

  std::sort(chunks.begin(), chunks.end(), [](const Chunk &a, const Chunk &b) { return a.chunk_id < b.chunk_id; });

  model_content.reserve(chunks.size() * 1048576);
  for (const auto &c : chunks) model_content.append(c.data.ptr(), c.data.length());

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
  BoosterHandle booster{nullptr};
  int finished{0};
  int64_t bufflen{1024}, out_len{0};
  std::unique_ptr<char[]> model_content_buff;
  //clang-format off
  auto ret = LGBM_DatasetCreateFromMat(training_data,      // feature data ptr
                                       data_type,          // type of sampe data
                                       n_data,             // # of sample data
                                       n_feature,          // # of features
                                       1,                  // 1 row-first, 0 column-first.
                                       task_mode.c_str(),  // params.
                                       nullptr,            // ref.
                                       &train_dataset_handler);
  // clang-format on
  // to set label data
  if (label_data) {
    ret = LGBM_DatasetSetField(train_dataset_handler, "label", label_data, n_data, label_data_type);
    if (ret) goto cleanup_dataset;
  }

  ret = LGBM_DatasetSetFeatureNames(train_dataset_handler, features_name, n_feature);
  if (ret) goto cleanup_dataset;

  ret = LGBM_BoosterCreate(train_dataset_handler, task_mode.c_str(), &booster);
  if (ret) goto cleanup_dataset;

  ret = LGBM_BoosterAddValidData(booster, train_dataset_handler);
  if (ret) goto cleanup_booster;

  for (auto iter = 0; iter < 100; ++iter) {
    ret = LGBM_BoosterUpdateOneIter(booster, &finished);
    if (ret) goto cleanup_booster;
    if (finished) break;
  }

  /* to dump the model conent to a string in json fomrat. But on till now ver 4.6.0. it can
   * not recreate model from the dumped string. The lightgbm is still on unifying the model
   * format. Therefore, we will use `LGBM_BoosterSaveModelToString` to a string, then wrappe
   * it into a json format. When the unified model format is ready to remove the wrapper by
   * defining `LIGHTGBM_UNIFY_FORMAT`.
   */
  model_content_buff = std::make_unique<char[]>(bufflen);
  memset(model_content_buff.get(), 0x0, bufflen);
#ifdef LIGHTGBM_UNIFY_FORMAT
  ret =
      LGBM_BoosterDumpModel(booster, 0, -1, C_API_FEATURE_IMPORTANCE_GAIN, bufflen, &out_len, model_content_buff.get());
#else
  ret = LGBM_BoosterSaveModelToString(booster, 0, -1, C_API_FEATURE_IMPORTANCE_GAIN, bufflen, &out_len,
                                      model_content_buff.get());
#endif
  if (ret) goto cleanup_booster;
  if (out_len > bufflen) {
    model_content_buff.reset(new char[out_len + 1]);
    memset(model_content_buff.get(), 0x0, bufflen);
    // clang-format off, If using LIGHTGBM_UNIFY_FORMAT, it means the model can be dumped with `ONNX-runtime` format.
#ifdef LIGHTGBM_UNIFY_FORMAT
    LGBM_BoosterDumpModel(booster,
                          0,                              // start iter idx
                          1,                              // end inter idx
                          C_API_FEATURE_IMPORTANCE_GAIN,  // feature_importance_type
                          out_len,                        // buff len
                          &out_len,                       // out len
                          model_content_buff.get());
#else
    LGBM_BoosterSaveModelToString(booster, 0, -1, C_API_FEATURE_IMPORTANCE_GAIN, out_len, &out_len,
                                  model_content_buff.get());
#endif
  }
  model_content.assign(model_content_buff.get(), out_len);

  {  // start to assemble the model object json file. If the model content is not JSON format, therefore, we assemble it
     // to JSON format with `SHANNON_LIGHTGBM_CONTENT`.
    Json_object *model_obj = new (std::nothrow) Json_object();
    if (model_obj == nullptr) return -1;
    model_obj->add_clone(ML_KEYWORDS::SHANNON_LIGHTGBM_CONTENT, new (std::nothrow) Json_string(model_content));
    Json_wrapper model_content_json(model_obj);
    String json_format_content;
    model_content_json.to_string(&json_format_content, false, "ML_train", [] { assert(false); });
    model_content.assign(json_format_content.c_ptr_safe(), json_format_content.length());
  }
  // clang-format on

cleanup_booster:
  LGBM_BoosterFree(booster);
cleanup_dataset:
  LGBM_DatasetFree(train_dataset_handler);
  return ret;
}

double Utils::calculate_accuracy(size_t n_sample, std::vector<double> &predictions, std::vector<float> &label_data) {
  // calculate the accuracy.
  int TP = 0, TN = 0;
  for (size_t i = 0; i < n_sample; i++) {
    int predicted = (predictions[i] >= 0.5) ? 1 : 0;
    auto actual = (int)label_data[i];

    if (predicted == 1 && actual == 1) TP++;  // true positive
    if (predicted == 0 && actual == 0) TN++;  // true negtive
  }

  auto accuracy = (TP + TN) * 1.0 / n_sample;
  return accuracy;
}

double Utils::calculate_balanced_accuracy(size_t n_sample, std::vector<double> &predictions,
                                          std::vector<float> &label_data) {
  // calculate the balanced accuracy.
  int TP = 0, TN = 0, FP = 0, FN = 0;
  for (size_t i = 0; i < n_sample; i++) {
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

int Utils::model_predict(int type, std::string &model_handle_name, size_t n_samples, size_t n_features,
                         std::vector<double> &testing_data, std::vector<double> &predictions) {
  assert(type == C_API_PREDICT_NORMAL || type == C_API_PREDICT_RAW_SCORE || type == C_API_PREDICT_LEAF_INDEX ||
         type == C_API_PREDICT_CONTRIB);
  std::string score_params;
  BoosterHandle handler{nullptr};
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    handler = Utils::load_trained_model_from_string(Loaded_models[model_handle_name]);

    if (!handler) {
      std::ostringstream err;
      err << model_handle_name << " can not load model from content string";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  assert(sizeof(double) == 8);
  predictions.resize(n_samples, 0.0);
  int64_t out_len;
  // clang-format off
  auto ret = LGBM_BoosterPredictForMat(handler,    /* model handler */
                            testing_data.data(),   /* test data */
                            C_API_DTYPE_FLOAT64,             /* testing data type */
                            n_samples,             /* # of testing data */
                            n_features,            /* # of features of testing data */
                            1,                     /* row-based format */
                            type,                  /* What should be predicted */
                            0,                     /* Start index of the iteration */ 
                            -1,                    /* # of iteration for prediction, <= 0 no limit*/
                            score_params.c_str(),  /* params */
                            &out_len,              /* Length of output result[out] */
                            predictions.data());   /* Pointer to array with predictions[out] */
  //clang-format on
  if (ret) {
    LGBM_BoosterFree(handler);
    return HA_ERR_GENERIC;
  }

  LGBM_BoosterFree(handler);
  return 0;
}

int Utils::ML_predict_row(int type, std::string &model_handle_name,
                         std::vector<ml_record_type_t> &input_data,
                         txt2numeric_map_t& txt2numeric_dict,
                         std::vector<double> &predictions) {

  assert(type == C_API_PREDICT_NORMAL || type == C_API_PREDICT_RAW_SCORE ||
         type == C_API_PREDICT_LEAF_INDEX || type == C_API_PREDICT_CONTRIB);

  std::string model_content;
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    model_content = Loaded_models[model_handle_name];
    assert(model_content.length());
  }

  auto booster = Utils::load_trained_model_from_string(model_content);
  if (!booster) return HA_ERR_GENERIC;

  auto n_feature = input_data.size();
  int64 num_results;

  // First time, get the result dims.
  const int MAX_TEMP_SIZE = 1024 * 1024;
  std::vector<double> temp_predictions(MAX_TEMP_SIZE);

  std::vector<double> sample_data;
  for (auto &field : input_data) {
    auto value = 0.0;
    if (txt2numeric_dict.find(field.first) == txt2numeric_dict.end()) {
      value = std::stod(field.second);
    } else {
      // Text field, to find mapping value.
      auto txt2num = txt2numeric_dict[field.first];
      if (txt2num.size()) {
        auto it = std::find(txt2num.begin(), txt2num.end(), field.second);
        if (it != txt2num.end()) {
          value = std::distance(txt2num.begin(), it);
        } else { // unknown.
          value = txt2num.size();
        }
      } else {
        value = 0.0;
      }
    }
    sample_data.push_back(value);
  }

  // clang-format off
  // The first time, gets the real result number.
  auto ret = LGBM_BoosterPredictForMat(booster,
                                       sample_data.data(),
                                       C_API_DTYPE_FLOAT64,
                                       1,
                                       n_feature,
                                       1,
                                       type,
                                       0,
                                       -1,
                                       "",
                                       &num_results,
                                       temp_predictions.data());

  if (ret || num_results <= 0 || num_results > MAX_TEMP_SIZE) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  predictions.resize(num_results);

  if (num_results != (int64)temp_predictions.size()) { //re-caculate
    ret = LGBM_BoosterPredictForMat(booster,
                                   sample_data.data(),
                                   C_API_DTYPE_FLOAT64,
                                   1,
                                   n_feature,
                                   1,
                                   type,
                                   0,
                                   -1,
                                   "",
                                   &num_results,
                                   predictions.data());
  } else {
    predictions.assign(temp_predictions.begin(), temp_predictions.begin() + num_results);
  }
  // clang-format on

  LGBM_BoosterFree(booster);
  return ret ? HA_ERR_GENERIC : 0;
}
}  // namespace ML
}  // namespace ShannonBase