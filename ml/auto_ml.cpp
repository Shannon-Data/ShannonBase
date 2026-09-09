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
#include "auto_ml.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/my_base.h"
#include "include/my_bitmap.h"
#include "include/mysqld_error.h"
#include "include/sql_string.h"  //String
#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/innobase/include/ut0dbg.h"  //for ut_a

#include "sql-common/json_error_handler.h"

#include "ml_algorithm.h"
#include "ml_anomaly_detection.h"
#include "ml_classification.h"
#include "ml_forecasting.h"
#include "ml_recommendation.h"
#include "ml_regression.h"
#include "ml_topic_modeling.h"
#include "ml_utils.h"

namespace ShannonBase {
namespace ML {
std::mutex models_mutex;
std::map<std::string, std::string> Loaded_models;

Auto_ML::Auto_ML(std::string schema, std::string table_name, std::string target_name, Json_wrapper options,
                 std::string handler)
    : m_schema_name(schema),
      m_table_name(table_name),
      m_target_name(target_name),
      m_options(options),
      m_handler(handler) {
  init_task_map();
}

Auto_ML::~Auto_ML() {}

std::string Auto_ML::get_array_string(Json_array *array) {
  std::string ret_val;
  if (!array) return ret_val;

  for (auto id = 0u; id < array->size(); id++) {
    Json_dom *it = (*array)[id];
    if (it && it->json_type() == enum_json_type::J_STRING) {
      ret_val += down_cast<Json_string *>(it)->value() + ",";
    }
  }
  return ret_val;
}

void Auto_ML::init_task_map() {
  OPTION_VALUE_T opt_values;
  std::string keystr;
  Utils::parse_json(m_options, opt_values, keystr, 0);

  // An empty option object is legal (the task then defaults to classification),
  // so look the key up instead of indexing: operator[] would insert an empty
  // entry for a key the caller never passed.
  auto task_it = opt_values.find(ML_KEYWORDS::task);
  m_task_type_str =
      (task_it != opt_values.end() && !task_it->second.empty()) ? task_it->second[0] : ML_KEYWORDS::classification;
  std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  build_task(m_task_type_str);
}

void Auto_ML::build_task(std::string_view task_str) {
  if (!task_str.length()) return;

  // OPT_TASKS_MAP is a process-wide map shared by every session. operator[]
  // would insert a node for an unrecognized task -- value-initializing the
  // mapped enum to CLASSIFICATION, so a typo silently trains a classifier --
  // and it would do so without holding any lock. Look the task up instead.
  auto task_it = OPT_TASKS_MAP.find(task_str);
  const ML_TASK_TYPE_T task_type = (task_it != OPT_TASKS_MAP.end()) ? task_it->second : ML_TASK_TYPE_T::UNKNOWN;

  switch (task_type) {
    case ML_TASK_TYPE_T::CLASSIFICATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::CLASSIFICATION)
        m_ml_task = std::make_unique<ML_classification>();

      down_cast<ML_classification *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_classification *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::REGRESSION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::REGRESSION)
        m_ml_task = std::make_unique<ML_regression>();

      down_cast<ML_regression *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_regression *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::FORECASTING:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::FORECASTING)
        m_ml_task = std::make_unique<ML_forecasting>();

      down_cast<ML_forecasting *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_forecasting *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_forecasting *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_forecasting *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_forecasting *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::LOG_ANOMALY_DETECTION:
      if (m_ml_task == nullptr || (m_ml_task->type() != ML_TASK_TYPE_T::ANOMALY_DETECTION &&
                                   m_ml_task->type() != ML_TASK_TYPE_T::LOG_ANOMALY_DETECTION))
        m_ml_task = std::make_unique<ML_anomaly_detection>();

      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_handle_name(m_handler);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_is_logad(true);
      break;
    case ML_TASK_TYPE_T::ANOMALY_DETECTION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::ANOMALY_DETECTION)
        m_ml_task = std::make_unique<ML_anomaly_detection>();

      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_anomaly_detection *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::RECOMMENDATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::RECOMMENDATION)
        m_ml_task = std::make_unique<ML_recommendation>();

      down_cast<ML_recommendation *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_recommendation *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_recommendation *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_recommendation *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_recommendation *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::TOPIC_MODELING:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::TOPIC_MODELING)
        m_ml_task = std::make_unique<ML_topic_modeling>();

      down_cast<ML_topic_modeling *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_topic_modeling *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_topic_modeling *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_topic_modeling *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_topic_modeling *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    default: {
      // No task object is built, so every entry point returns HA_ERR_GENERIC.
      // Raise a diagnostic here, otherwise the statement fails with no error set.
      m_ml_task.reset();
      THD *thd = current_thd;
      if (thd && !thd->is_error()) {
        std::ostringstream err;
        err << "unsupported ML task: " << task_str;
        my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      }
    } break;
  }
  return;
}

int Auto_ML::precheck_and_process_meta_info(std::string &model_handle_name, std::string &model_content,
                                            bool should_loaded) {
  if (model_handle_name.length() == 0) return HA_ERR_GENERIC;

  {
    std::lock_guard<std::mutex> lock(models_mutex);
    if (should_loaded && (Loaded_models.find(model_handle_name) == Loaded_models.end())) {
      // should been loaded, but not loaded.
      std::ostringstream err;
      err << model_handle_name << " has not been loaded";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    } else if (!should_loaded && (Loaded_models.find(model_handle_name) != Loaded_models.end())) {
      // should not been loaded, but loaded.
      std::ostringstream err;
      err << model_handle_name << " has been loaded";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }  // lock_guard released here

  if (Utils::read_model_content(model_handle_name, m_options)) return HA_ERR_GENERIC;
  auto dom_ptr = m_options.clone_dom();
  if (!dom_ptr) return HA_ERR_GENERIC;

  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr{nullptr};
  value_dom_ptr = json_obj->get(ML_KEYWORDS::task);
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_task_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  }

  if (Utils::read_model_object_content(model_handle_name, model_content)) return HA_ERR_GENERIC;

  // m_task_type_str was just read from the model metadata's top-level "task"
  // key above. Do not re-derive it through init_task_map(): that re-parses the
  // whole metadata document flattened, where a nested "task" key can shadow the
  // real one, and it would build the task object a second time.
  if (m_task_type_str.length()) build_task(m_task_type_str);

  return 0;
}

int Auto_ML::train(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  std::string sch_tb_name{m_schema_name};
  sch_tb_name.append(".");
  sch_tb_name.append(m_table_name);
  if (Utils::check_table_available(sch_tb_name)) return HA_ERR_GENERIC;

  auto ret = m_ml_task ? m_ml_task->train(thd, model_object, model_metadata) : HA_ERR_GENERIC;
  return ret;
}

int Auto_ML::load(THD *thd, String *model_handler_name) {
  ut_a(model_handler_name);
  m_handler = model_handler_name->c_ptr_safe();

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, false)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->load(thd, model_content_str) : HA_ERR_GENERIC;
}

int Auto_ML::unload(THD *thd, String *model_handler_name) {
  ut_a(model_handler_name);
  m_handler = model_handler_name->c_ptr_safe();

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, true)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->unload(thd, m_handler) : HA_ERR_GENERIC;
}

double Auto_ML::score(THD *thd, String *sch_table_name, String *target_column_name, String *model_handle_name,
                      String *metric, Json_wrapper options) {
  ut_a(sch_table_name && target_column_name && model_handle_name);

  std::string sch_tb_name_str(sch_table_name->c_ptr_safe());
  if (Utils::check_table_available(sch_tb_name_str)) return 0;

  std::string model_handler_name_str(model_handle_name->c_ptr_safe());
  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_content_str, true)) return 0;

  std::string target_column_name_str(target_column_name->c_ptr_safe());
  std::string metric_str(metric->c_ptr_safe());
  return m_ml_task ? m_ml_task->score(thd, sch_tb_name_str, target_column_name_str, model_handler_name_str, metric_str,
                                      options)
                   : 0;
}

int Auto_ML::predict_row(THD *thd, Json_wrapper &input, String *model_handler_name, Json_wrapper options,
                         Json_wrapper &result) {
  ut_a(model_handler_name);
  std::string model_handler_name_str(model_handler_name->c_ptr_safe());
  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_content_str, true)) return 0;

  if (!options.empty()) {
    OPTION_VALUE_T opt_values;
    std::string keystr;
    Utils::parse_json(options, opt_values, keystr, 0);

    // Validate the boolean-valued options only when they are actually present:
    // parse_json leaves absent keys out of the map, and a missing/empty entry
    // must not be indexed.
    for (const char *bool_opt : {ML_KEYWORDS::remove_seen, ML_KEYWORDS::additional_details}) {
      auto it = opt_values.find(bool_opt);
      if (it != opt_values.end() && !it->second.empty() && it->second[0] != "true" && it->second[0] != "false") {
        std::ostringstream err;
        err << "wrong " << bool_opt << " value in option you specified: " << it->second[0];
        my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
        return HA_ERR_GENERIC;
      }
    }

    auto rec_it = opt_values.find("recommend");
    if (rec_it != opt_values.end() && !rec_it->second.empty()) {
      const std::string &rec = rec_it->second[0];
      if (rec != "ratings" && rec != "items" && rec != "users" && rec != "users_to_items" && rec != "items_to_users" &&
          rec != "items_to_items" && rec != "users_to_users") {
        std::ostringstream err;
        err << "wrong recommend value in option you specified: " << rec;
        my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
        return HA_ERR_GENERIC;
      }
    }
  }

  return m_ml_task ? m_ml_task->predict_row(thd, input, model_handler_name_str, options, result) : HA_ERR_GENERIC;
}

int Auto_ML::predict_table(THD *thd, String *in_sch_tb_name, String *model_handler_name, String *out_sch_tb_name,
                           Json_wrapper &options) {
  std::ostringstream err;
  std::string sch_tb_name_str(in_sch_tb_name->c_ptr_safe());
  if (Utils::check_table_available(sch_tb_name_str)) return HA_ERR_GENERIC;

  std::string model_handler_name_str(model_handler_name->c_ptr_safe());
  std::string out_sch_tb_name_str(out_sch_tb_name->c_ptr_safe());
  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_content_str, true)) return 0;

  auto ret = m_ml_task
                 ? m_ml_task->predict_table(thd, sch_tb_name_str, model_handler_name_str, out_sch_tb_name_str, options)
                 : HA_ERR_GENERIC;
  return ret;
}

int Auto_ML::import(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata, String *model_handler_name) {
  std::string handler_name_str(model_handler_name->c_ptr_safe());

  if (m_ml_task) return m_ml_task->import(thd, model_object, model_metadata, handler_name_str);

  return 0;
}

int Auto_ML::explain(THD *thd, String *sch_tb_name, String *target_column_name, String *model_handler_name,
                     Json_wrapper exp_options) {
  ut_a(sch_tb_name && target_column_name && model_handler_name);
  m_options = exp_options;

  m_handler = model_handler_name->c_ptr_safe();
  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, true)) return HA_ERR_GENERIC;

  std::string sch_tb_name_str(sch_tb_name->c_ptr_safe());
  std::string target_column_name_str(target_column_name->c_ptr_safe());
  std::string model_handle_name_str(model_handler_name->c_ptr_safe());
  return m_ml_task
             ? m_ml_task->explain(thd, sch_tb_name_str, target_column_name_str, model_handle_name_str, exp_options)
             : HA_ERR_GENERIC;
}

int Auto_ML::explain_row(THD *thd, Json_wrapper &exp_row, String *model_handler_name, Json_wrapper &exp_options,
                         Json_wrapper &result) {
  if (!model_handler_name) return HA_ERR_GENERIC;
  m_options = exp_options;

  m_handler = model_handler_name->c_ptr_safe();
  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, true)) return HA_ERR_GENERIC;

  std::string model_handle_name_str(model_handler_name->c_ptr_safe());
  return m_ml_task ? m_ml_task->explain_row(thd, exp_row, model_handle_name_str, exp_options, result) : HA_ERR_GENERIC;
}

int Auto_ML::model_active(THD *thd, String *in_user_name, Json_wrapper &out_model_info) {
  ut_a(thd);
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    if (Loaded_models.empty()) {
      auto empty_array = new (std::nothrow) Json_array();
      if (!empty_array) return HA_ERR_GENERIC;
      out_model_info = Json_wrapper(empty_array);
      return 0;
    }
  }

  std::string scope_str = in_user_name ? std::string(in_user_name->c_ptr_safe()) : "current";
  std::transform(scope_str.begin(), scope_str.end(), scope_str.begin(), ::tolower);

  if (scope_str != "current" && scope_str != "all") {
    my_error(ER_ML_FAIL, MYF(0), "ML_MODEL_ACTIVE: user must be 'current', or NULL");
    return HA_ERR_GENERIC;
  }

  if (scope_str == "all") {
    my_error(ER_ML_FAIL, MYF(0),
             "You cannot active the other users' models, please specify 'current' or NULL as the user scope");
    return HA_ERR_GENERIC;
  }

  Security_context *sctx = thd->security_context();
  const std::string cur_user(sctx->priv_user().str, sctx->priv_user().length);
  // Convention: every user's models live under ML_SCHEMA_{username}
  const std::string cur_ml_schema = "ML_SCHEMA_" + cur_user;

  // Snapshot the loaded models, then read their metadata with the mutex
  // released: read_model_content() opens tables and takes storage-engine
  // locks, which must not happen underneath models_mutex.
  std::vector<std::pair<std::string, size_t>> loaded_snapshot;
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    loaded_snapshot.reserve(Loaded_models.size());
    for (const auto &[handle, serialized] : Loaded_models) loaded_snapshot.emplace_back(handle, serialized.size());
  }  // models_mutex released

  // add_alias()/append_alias() hand ownership to the parent DOM, so everything
  // below is owned by root_array once attached; the unique_ptrs only cover the
  // window before attachment.
  Json_object_ptr detail_obj(new (std::nothrow) Json_object());  // second element of root array
  Json_object_ptr size_obj(new (std::nothrow) Json_object());    // first element
  Json_array_ptr root_array(new (std::nothrow) Json_array());
  if (!detail_obj || !size_obj || !root_array) return HA_ERR_GENERIC;

  ulonglong total_bytes = 0;
  for (auto &[handle, serialized_size] : loaded_snapshot) {
    std::string model_handle = handle;
    Json_wrapper meta_wrap;
    // Never swallow this failure. Opening MODEL_CATALOG in the middle of the
    // enclosing statement can legitimately fail with ER_NEED_REPREPARE, and
    // that error has to reach the server so the statement is re-prepared and
    // retried -- clearing it here would silently return an empty model list.
    if (Utils::read_model_content(model_handle, meta_wrap)) return HA_ERR_GENERIC;

    total_bytes += static_cast<ulonglong>(serialized_size);
    auto meta_dom = meta_wrap.clone_dom();
    if (!meta_dom) continue;
    if (detail_obj->add_alias(handle, std::move(meta_dom))) return HA_ERR_GENERIC;
  }

  auto size_dom = new (std::nothrow) Json_uint(total_bytes);
  if (!size_dom || size_obj->add_alias("total model size(bytes)", size_dom)) {
    my_error(ER_ML_FAIL, MYF(0), "ML_MODEL_ACTIVE: failed to build size object");
    return HA_ERR_GENERIC;
  }

  if (root_array->append_alias(std::move(size_obj)) || root_array->append_alias(std::move(detail_obj)))
    return HA_ERR_GENERIC;
  out_model_info = Json_wrapper(root_array.release());
  return 0;
}
}  // namespace ML
}  // namespace ShannonBase