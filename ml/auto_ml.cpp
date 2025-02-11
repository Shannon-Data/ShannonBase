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

#include "include/my_base.h"
#include "include/my_bitmap.h"
#include "include/mysqld_error.h"
#include "include/sql_string.h"  //String
#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/table.h"

#include "sql-common/json_error_handler.h"

#include "ml_algorithm.h"
#include "ml_anomaly_detection.h"
#include "ml_classification.h"
#include "ml_forecasting.h"
#include "ml_recommendation.h"
#include "ml_regression.h"
#include "ml_utils.h"

namespace ShannonBase {
namespace ML {

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
  auto dom_ptr = m_options.clone_dom();
  if (!dom_ptr) return;
  // parse the options.
  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr{nullptr};
  value_dom_ptr = json_obj->get("task");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_task_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  }

  value_dom_ptr = json_obj->get("datetime_index");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_datetime_index = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("endogenous_variables");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_endogenous_variables = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("exogenous_variables");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_exogenous_variables = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("model_list");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_model_list = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("exclude_model_list");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_exclude_model_list = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("optimization_metric");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_optimization_metric = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("include_column_list");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_include_column_list = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("exclude_column_list");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
    auto var_arrary = down_cast<Json_array *>(value_dom_ptr);
    m_opt_exclude_column_list = get_array_string(var_arrary);
  }
  value_dom_ptr = json_obj->get("contamination");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_DOUBLE) {
    m_opt_contamination = down_cast<Json_double *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("users");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_users = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("items");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_item = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("notes");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_notes = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("feedback");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_opt_feedback = down_cast<Json_string *>(value_dom_ptr)->value();
  }
  value_dom_ptr = json_obj->get("feedback_threshold");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_DOUBLE) {
    m_opt_feedback_threshold = down_cast<Json_double *>(value_dom_ptr)->value();
  }

  build_task(m_task_type_str);
}

void Auto_ML::build_task(std::string task_str) {
  if (!task_str.length()) return;

  auto option_obj = m_options.clone_dom();
  switch (opt_task_map[task_str]) {
    // if we have already had an instance of cf task, then do nothing, using the old instance.
    case ML_TASK_TYPE::CLASSIFICATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::CLASSIFICATION)
        m_ml_task.reset(new ML_classification());

      down_cast<ML_classification *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_classification *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE::REGRESSION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::REGRESSION) m_ml_task.reset(new ML_regression());

      down_cast<ML_regression *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_regression *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE::FORECASTING:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::FORECASTING) m_ml_task.reset(new ML_forecasting());
      break;
    case ML_TASK_TYPE::ANOMALY_DETECTION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::ANOMALY_DETECTION)
        m_ml_task.reset(new ML_anomaly_detection());
      break;
    case ML_TASK_TYPE::RECOMMENDATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::RECOMMENDATION)
        m_ml_task.reset(new ML_recommendation());
      break;
    default:
      break;
  }
  return;
}

int Auto_ML::precheck_and_process_meta_info(std::string &model_hanle_name, std::string &model_user_name,
                                            std::string &model_content, bool should_loaded) {
  if (should_loaded && (Loaded_models.find(model_hanle_name) == Loaded_models.end())) {
    // should been loaded, but not loaded.
    std::ostringstream err;
    err << model_hanle_name << " has not been loaded";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  } else if (!should_loaded && (Loaded_models.find(model_hanle_name) != Loaded_models.end())) {
    // should not been loaded, but loaded.
    std::ostringstream err;
    err << model_hanle_name << " has been loaded";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  if (Utils::read_model_content(model_user_name, model_hanle_name, m_options)) return HA_ERR_GENERIC;
  auto dom_ptr = m_options.clone_dom();
  if (!dom_ptr) return HA_ERR_GENERIC;

  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr{nullptr};
  value_dom_ptr = json_obj->get("task");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_task_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  }

  if (Utils::read_model_object_content(model_user_name, model_hanle_name, model_content)) return HA_ERR_GENERIC;

  if (m_task_type_str.length()) {
    init_task_map();
    build_task(m_task_type_str);
  }

  return 0;
}

int Auto_ML::train() {
  auto ret = m_ml_task ? m_ml_task->train() : 1;
  return ret;
}

int Auto_ML::load(String *model_handler_name, String *model_user) {
  // in load, the schem_name means user name.
  assert(model_handler_name && model_user);
  m_handler = model_handler_name->c_ptr_safe();
  std::string model_user_str(model_user->c_ptr_safe());

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_user_str, model_content_str, false)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->load(model_content_str) : HA_ERR_GENERIC;
}

int Auto_ML::unload(String *model_handler_name, String *model_user) {
  // in unload, the schem_name means user name.
  assert(model_handler_name && model_user);
  m_handler = model_handler_name->c_ptr_safe();
  std::string model_user_str(model_user->c_ptr_safe());

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_user_str, model_content_str, true)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->unload(m_handler) : HA_ERR_GENERIC;
}

double Auto_ML::score(String *sch_table_name, String *taget_column_name, String *model_handle_name,
                      String *model_usr_name, String *metric, Json_wrapper *options) {
  assert(sch_table_name && taget_column_name && model_handle_name && model_usr_name);

  std::string model_handler_name_str(model_handle_name->c_ptr_safe());
  std::string model_user_str(model_usr_name->c_ptr_safe());

  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_user_str, model_content_str, true)) return 0;

  assert(metric && options);
  std::string sch_table_name_str(sch_table_name->c_ptr_safe());
  std::string taget_column_name_str(taget_column_name->c_ptr_safe());
  std::string metric_str(metric->c_ptr_safe());
  return m_ml_task ? m_ml_task->score(sch_table_name_str, taget_column_name_str, model_handler_name_str, metric_str)
                   : 0;
}

int Auto_ML::import(String *model_handler_name, String *user_name, Json_wrapper *model_meta, String *model_content) {
  assert(model_handler_name && user_name && model_meta && model_content);

  std::string model_user_name_str(user_name->ptr());
  std::string handler_name_str(model_handler_name->ptr());
  std::string model_content_str(model_content->ptr());

  if (m_ml_task) return m_ml_task->import(model_user_name_str, handler_name_str, model_content_str);

  return 0;
}

}  // namespace ML
}  // namespace ShannonBase