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

#include <string>

#include "sql-common/json_error_handler.h"
#include "lightgbm/include/LightGBM/application.h"

#include "ml_algorithm.h"
#include "ml_regression.h"

namespace ShannonBase {
namespace ML {

Auto_ML::Auto_ML(std::string schema, std::string table_name, std::string target_name,
                 Json_wrapper options, std::string handler) : m_schema_name(schema),
                                                              m_table_name(table_name),
                                                              m_options(options),
                                                              m_handler(handler) {
  init_task_map();

  auto dom_ptr = m_options.clone_dom();
  if (dom_ptr) { //parse the options.
    Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());

    auto task_dom = json_obj->get("task");
    if (task_dom && task_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_task = down_cast<Json_string *>(task_dom)->value();
      std::transform(m_opt_task.begin(), m_opt_task.end(), m_opt_task.begin(), ::toupper);
    }
    auto datetime_index_dom = json_obj->get("datetime_index");
    if (datetime_index_dom && datetime_index_dom->json_type() == enum_json_type::J_STRING) {
       m_opt_datetime_index = down_cast<Json_string *>(datetime_index_dom)->value();
    }
    auto endogenous_variables_dom = json_obj->get("endogenous_variables");
    if (endogenous_variables_dom &&
        endogenous_variables_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(endogenous_variables_dom);
      m_opt_endogenous_variables = get_array_string(var_arrary);
    }
    auto exogenous_variables_dom = json_obj->get("exogenous_variables");
    if (exogenous_variables_dom &&
        exogenous_variables_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(exogenous_variables_dom);
      m_opt_exogenous_variables = get_array_string(var_arrary);
    }
    auto model_list_dom = json_obj->get("model_list");
    if (model_list_dom && model_list_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(model_list_dom);
      m_opt_model_list = get_array_string(var_arrary);
    }
    auto exclude_model_list_dom = json_obj->get("exclude_model_list");
    if (exclude_model_list_dom &&
        exclude_model_list_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(exclude_model_list_dom);
      m_opt_exclude_model_list = get_array_string(var_arrary);
    }
    auto optimization_metric_dom = json_obj->get("optimization_metric");
    if (optimization_metric_dom &&
        optimization_metric_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_optimization_metric = down_cast<Json_string *>(optimization_metric_dom)->value();
    }
    auto include_column_list_dom = json_obj->get("include_column_list");
    if (include_column_list_dom &&
        include_column_list_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(include_column_list_dom);
      m_opt_include_column_list = get_array_string(var_arrary);
    }
    auto exclude_column_list_dom = json_obj->get("exclude_column_list");
    if (exclude_column_list_dom &&
        exclude_column_list_dom->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(exclude_column_list_dom);
      m_opt_exclude_column_list = get_array_string(var_arrary);
    }
    auto contamination_dom = json_obj->get("contamination");
    if (contamination_dom && contamination_dom->json_type() == enum_json_type::J_DOUBLE) {
     m_opt_contamination = down_cast<Json_double *>(exclude_column_list_dom)->value();
    }
    auto users_dom =  json_obj->get("users");
    if (users_dom && users_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_users = down_cast<Json_string *>(users_dom)->value();
    }
    auto items_dom =  json_obj->get("items");
    if (items_dom && items_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_item = down_cast<Json_string *>(items_dom)->value();
    }
    auto notes_dom =  json_obj->get("notes");
    if (notes_dom && notes_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_notes = down_cast<Json_string *>(notes_dom)->value();
    }
    auto feedback_dom =  json_obj->get("feedback");
    if (feedback_dom && feedback_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_feedback = down_cast<Json_string *>(feedback_dom)->value();
    }
    auto feedback_threshold_dom =  json_obj->get("feedback_threshold");
    if (feedback_threshold_dom && feedback_dom->json_type() == enum_json_type::J_STRING) {
      m_opt_feedback_threshold = down_cast<Json_double *>(feedback_threshold_dom)->value();
    }
  }
  switch (m_opt_task_map[m_opt_task]) {
    case ML_TASK_TYPE::CLASSIFICATION:
      break;
    case ML_TASK_TYPE::REGRESSION:
      m_ml_task = std::make_unique<ML_regression>(m_schema_name, m_table_name, m_target_name);
      break;
    case ML_TASK_TYPE::FORECASTING:
      break;
    case ML_TASK_TYPE::ANOMALY_DETECTION:
      break;
    case ML_TASK_TYPE::RECOMMENDATION:
      break;
  }
}

Auto_ML::~Auto_ML() {
}
std::string Auto_ML::get_array_string (Json_array* array) {
  std::string ret_val;
  if (!array) return ret_val;

  auto sz = array->size();
  for (auto id =0; id < sz; id++) {
     Json_dom* it = (*array)[id];
     if (it && it->json_type() == enum_json_type::J_STRING) {
        ret_val += down_cast<Json_string*> (it)->value() + ",";
     }
  }
  return ret_val;
}
void Auto_ML::init_task_map() {
  m_opt_task_map.insert({"CLASSIFICATION", ML_TASK_TYPE::CLASSIFICATION});
  m_opt_task_map.insert({"REGRESSION", ML_TASK_TYPE::REGRESSION});
  m_opt_task_map.insert({"FORECASTING", ML_TASK_TYPE::FORECASTING});
  m_opt_task_map.insert({"ANOMALY_DETECTION", ML_TASK_TYPE::ANOMALY_DETECTION});
  m_opt_task_map.insert({"RECOMMENDATION", ML_TASK_TYPE::RECOMMENDATION});
}

int Auto_ML::train() {
  if (m_ml_task)
     return m_ml_task->train();

  return 0;
}


} //ML
} //shannonbase