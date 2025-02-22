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
  OPTION_VALUE_T opt_values;
  std::string keystr;
  Utils::parse_json(m_options, opt_values, keystr, 0);
  assert(opt_values.size());
  m_task_type_str = opt_values["task"].size() ? opt_values["task"][0] : "classification";
  std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  build_task(m_task_type_str);
}

void Auto_ML::build_task(std::string task_str) {
  if (!task_str.length()) return;

  auto option_obj = m_options.clone_dom();
  switch (OPT_TASKS_MAP[task_str]) {
    // if we have already had an instance of cf task, then do nothing, using the old instance.
    case ML_TASK_TYPE_T::CLASSIFICATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::CLASSIFICATION)
        m_ml_task.reset(new ML_classification());

      down_cast<ML_classification *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_classification *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_classification *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::REGRESSION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::REGRESSION) m_ml_task.reset(new ML_regression());

      down_cast<ML_regression *>(m_ml_task.get())->set_schema(m_schema_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_table(m_table_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_target(m_target_name);
      down_cast<ML_regression *>(m_ml_task.get())->set_options(m_options);
      down_cast<ML_regression *>(m_ml_task.get())->set_handle_name(m_handler);
      break;
    case ML_TASK_TYPE_T::FORECASTING:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::FORECASTING)
        m_ml_task.reset(new ML_forecasting());
      break;
    case ML_TASK_TYPE_T::ANOMALY_DETECTION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::ANOMALY_DETECTION)
        m_ml_task.reset(new ML_anomaly_detection());
      break;
    case ML_TASK_TYPE_T::RECOMMENDATION:
      if (m_ml_task == nullptr || m_ml_task->type() != ML_TASK_TYPE_T::RECOMMENDATION)
        m_ml_task.reset(new ML_recommendation());
      break;
    default:
      break;
  }
  return;
}

int Auto_ML::precheck_and_process_meta_info(std::string &model_hanle_name, std::string &model_content,
                                            bool should_loaded) {
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

  if (Utils::read_model_content(model_hanle_name, m_options)) return HA_ERR_GENERIC;
  auto dom_ptr = m_options.clone_dom();
  if (!dom_ptr) return HA_ERR_GENERIC;

  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr{nullptr};
  value_dom_ptr = json_obj->get("task");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    m_task_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(m_task_type_str.begin(), m_task_type_str.end(), m_task_type_str.begin(), ::toupper);
  }

  if (Utils::read_model_object_content(model_hanle_name, model_content)) return HA_ERR_GENERIC;

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

int Auto_ML::load(String *model_handler_name) {
  // in load, the schem_name means user name.
  assert(model_handler_name);
  m_handler = model_handler_name->c_ptr_safe();

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, false)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->load(model_content_str) : HA_ERR_GENERIC;
}

int Auto_ML::unload(String *model_handler_name) {
  // in unload, the schem_name means user name.
  assert(model_handler_name);
  m_handler = model_handler_name->c_ptr_safe();

  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, true)) return HA_ERR_GENERIC;

  return m_ml_task ? m_ml_task->unload(m_handler) : HA_ERR_GENERIC;
}

double Auto_ML::score(String *sch_table_name, String *target_column_name, String *model_handle_name, String *metric,
                      Json_wrapper options) {
  assert(sch_table_name && target_column_name && model_handle_name);

  std::string model_handler_name_str(model_handle_name->c_ptr_safe());
  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_content_str, true)) return 0;

  std::string sch_table_name_str(sch_table_name->c_ptr_safe());
  std::string target_column_name_str(target_column_name->c_ptr_safe());
  std::string metric_str(metric->c_ptr_safe());
  return m_ml_task
             ? m_ml_task->score(sch_table_name_str, target_column_name_str, model_handler_name_str, metric_str, options)
             : 0;
}

int Auto_ML::predict_row(Json_wrapper &input, String *model_handler_name, Json_wrapper options, Json_wrapper &result) {
  assert(model_handler_name);
  std::string model_handler_name_str(model_handler_name->c_ptr_safe());
  std::string model_content_str;
  if (precheck_and_process_meta_info(model_handler_name_str, model_content_str, true)) return 0;

  // start to check validity of input options.
  std::ostringstream err;
  int ret{0};
  if (!options.empty()) {
    OPTION_VALUE_T opt_values;
    std::string keystr;
    Utils::parse_json(options, opt_values, keystr, 0);

    auto opt_value = opt_values["remove_seen"];
    assert(opt_value.size() == 1);
    if (opt_value[0] != "true" && opt_value[0] != "false") {
      err << "wrong remove_seen value in optin you specified: " << opt_value[0];
      ret = HA_ERR_GENERIC;
      goto error;
    }
    opt_value = opt_values["additional_details"];
    assert(opt_value.size() == 1);
    if (opt_value[0] != "true" && opt_value[0] != "false") {
      err << "wrong additional_details value in optin you specified: " << opt_value[0];
      ret = HA_ERR_GENERIC;
      goto error;
    }
    opt_value = opt_values["recommend"];  // for recommendation option.
    if (opt_value.size() && (opt_value[0] != "ratings" && opt_value[0] != "items" && opt_value[0] != "users" &&
                             opt_value[0] != "users_to_items" && opt_value[0] != "items_to_users" &&
                             opt_value[0] != "items_to_items" && opt_value[0] != "users_to_users")) {
      err << "wrong additional_details value in optin you specified: " << opt_value[0];
      ret = HA_ERR_GENERIC;
      goto error;
    }
  }
  ret = m_ml_task ? m_ml_task->predict_row(input, model_handler_name_str, options, result) : HA_ERR_GENERIC;
error:
  if (ret) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
  }
  return ret;
}

int Auto_ML::import(Json_wrapper &model_object, Json_wrapper &model_metadata, String *model_handler_name) {
  std::string handler_name_str(model_handler_name->ptr());

  if (m_ml_task) return m_ml_task->import(model_object, model_metadata, handler_name_str);

  return 0;
}

int Auto_ML::explain(String *sch_tb_name, String *target_column_name, String *model_handler_name,
                     Json_wrapper exp_options) {
  assert(sch_tb_name && target_column_name && model_handler_name);
  m_options = exp_options;

  m_handler = model_handler_name->c_ptr_safe();
  std::string model_content_str;
  if (precheck_and_process_meta_info(m_handler, model_content_str, true)) return HA_ERR_GENERIC;

  std::string sch_tb_name_str(sch_tb_name->c_ptr_safe());
  std::string target_column_name_str(target_column_name->c_ptr_safe());
  std::string modle_handle_name_str(model_handler_name->c_ptr_safe());
  return m_ml_task ? m_ml_task->explain(sch_tb_name_str, target_column_name_str, modle_handle_name_str, exp_options)
                   : HA_ERR_GENERIC;
}

}  // namespace ML
}  // namespace ShannonBase