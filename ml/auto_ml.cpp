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

#include "ml_algorithm.h"
#include "ml_regression.h"
#include "ml_recommendation.h"
#include "ml_forecasting.h"
#include "ml_classification.h"
#include "ml_anomaly_detection.h"

namespace ShannonBase {
namespace ML {
Auto_ML::Auto_ML(std::string schema, std::string table_name, std::string target_name,
                 Json_wrapper* options, std::string handler) : m_schema_name(schema),
                                                              m_table_name(table_name),
                                                              m_target_name(target_name),
                                                              m_options(options),
                                                              m_handler(handler) {
}

Auto_ML::~Auto_ML() {
}

ML_TASK_TYPE Auto_ML::type() {
  return (m_ml_task) ? m_ml_task->type() : ML_TASK_TYPE::UNKNOWN;
}

std::string Auto_ML::get_array_string (Json_array* array) {
  std::string ret_val;
  if (!array) return ret_val;

  for (auto id = 0u; id < array->size(); id++) {
     Json_dom* it = (*array)[id];
     if (it && it->json_type() == enum_json_type::J_STRING) {
        ret_val += down_cast<Json_string*> (it)->value() + ",";
     }
  }
  return ret_val;
}

void Auto_ML::init_task_map() {
  m_opt_task_map.insert({"", ML_TASK_TYPE::UNKNOWN});
  m_opt_task_map.insert({"CLASSIFICATION", ML_TASK_TYPE::CLASSIFICATION});
  m_opt_task_map.insert({"REGRESSION", ML_TASK_TYPE::REGRESSION});
  m_opt_task_map.insert({"FORECASTING", ML_TASK_TYPE::FORECASTING});
  m_opt_task_map.insert({"ANOMALY_DETECTION", ML_TASK_TYPE::ANOMALY_DETECTION});
  m_opt_task_map.insert({"RECOMMENDATION", ML_TASK_TYPE::RECOMMENDATION});

  auto dom_ptr = m_options->clone_dom();
  if (dom_ptr) { //parse the options.
    Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
    Json_dom* value_dom_ptr{nullptr};
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
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_endogenous_variables = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("exogenous_variables");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_exogenous_variables = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("model_list");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_model_list = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("exclude_model_list");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_exclude_model_list = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("optimization_metric");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
      m_opt_optimization_metric = down_cast<Json_string *>(value_dom_ptr)->value();
    }
    value_dom_ptr = json_obj->get("include_column_list");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_include_column_list = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("exclude_column_list");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_ARRAY) {
      auto var_arrary =  down_cast<Json_array *>(value_dom_ptr);
      m_opt_exclude_column_list = get_array_string(var_arrary);
    }
    value_dom_ptr = json_obj->get("contamination");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_DOUBLE) {
      m_opt_contamination = down_cast<Json_double *>(value_dom_ptr)->value();
    }
    value_dom_ptr =  json_obj->get("users");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
      m_opt_users = down_cast<Json_string *>(value_dom_ptr)->value();
    }
    value_dom_ptr =  json_obj->get("items");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
      m_opt_item = down_cast<Json_string *>(value_dom_ptr)->value();
    }
    value_dom_ptr =  json_obj->get("notes");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
      m_opt_notes = down_cast<Json_string *>(value_dom_ptr)->value();
    }
    value_dom_ptr =  json_obj->get("feedback");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
      m_opt_feedback = down_cast<Json_string *>(value_dom_ptr)->value();
    }
    value_dom_ptr =  json_obj->get("feedback_threshold");
    if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_DOUBLE) {
      m_opt_feedback_threshold = down_cast<Json_double *>(value_dom_ptr)->value();
    }
  }

  build_task(m_task_type_str);
}

void Auto_ML::build_task (std::string task_str) {
  if (!m_opt_task_map.size()) return;

  switch (m_opt_task_map[task_str]) {
    //if we have already had an instance of cf task, then do nothing, using the old instance.
    case ML_TASK_TYPE::CLASSIFICATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::CLASSIFICATION)
        m_ml_task.reset(new ML_classification());
      break;
    case ML_TASK_TYPE::REGRESSION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::REGRESSION)
        m_ml_task.reset(new ML_regression());

      down_cast<ML_regression*>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_regression*>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_regression*>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_regression*>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_regression*>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE::FORECASTING:
       if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::FORECASTING)
        m_ml_task.reset (new ML_forecasting());
      break;
    case ML_TASK_TYPE::ANOMALY_DETECTION:
       if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::ANOMALY_DETECTION)
        m_ml_task.reset(new ML_anomaly_detection());
      break;
    case ML_TASK_TYPE::RECOMMENDATION:
        if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE::RECOMMENDATION)
        m_ml_task.reset (new ML_recommendation());
      break;
    default: break;
  }
  return ;
}

int Auto_ML::train() {
  if (!m_opt_task_map.size()) {
    init_task_map();
  }

  auto ret = m_ml_task? m_ml_task->train() : 1;
  return 0;
}

int Auto_ML::load() {
  //in load, the schem_name means user name.
  std::string model_user_name = m_schema_name;
  if (m_ml_task)
     return m_ml_task->load(m_handler, model_user_name);
  return 0;
}

int Auto_ML::unload() {
  if (m_ml_task)
     m_ml_task->unload(m_handler);
  m_ml_task.reset(nullptr);
  return 0;
}

int Auto_ML::import() {
  std::string model_user_name = m_schema_name;
  std::string model_content = m_target_name;
  if (m_ml_task)
     return m_ml_task->import(m_handler, model_user_name, model_content);

  return 0;
}

} //ML
} //shannonbase