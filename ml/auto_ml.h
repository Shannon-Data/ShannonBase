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
#ifndef __SHANNONBASE_AUTO_ML_H__
#define __SHANNONBASE_AUTO_ML_H__

#include <map>
#include <memory>
#include <string>

#include "include/my_inttypes.h"
#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

namespace ShannonBase {
namespace ML {

class Auto_ML {
 public:
  Auto_ML(std::string schema, std::string table_name, std::string target_name, Json_wrapper *options,
          std::string handler);
  Auto_ML() = default;
  virtual ~Auto_ML();

  // do ML training.
  int train();
  // load the trainned model into Rapid.
  int load(String *model_handler_name, String *model_user);
  // unload the loaded mode from Rapid.
  int unload(String *model_handler_name, Json_wrapper *model_meta);
  // import the model from another.
  int import(String *model_handler_name, String *user_name, Json_wrapper *model_meta, String *model_content);
  // get the type of job.
  ML_TASK_TYPE type();

 private:
  // get the json array value.
  std::string get_array_string(Json_array *array);
  // init task job map, get an instance by type.
  void init_task_map();
  // build the a ML task, such as regress, classification, etc.
  void build_task(std::string task_str);

 private:
  // the source schema name.
  std::string m_schema_name;
  // the source table name.
  std::string m_table_name;
  // the label column name.
  std::string m_target_name;
  // the options in JSON format.
  Json_wrapper *m_options;
  // name of the model content.
  std::string m_handler;

  // the followings are parsed from m_options.
  //  {'classification'|'regression'|'forecasting'|'anomaly_detection'|'recommendation'}|NULL
  std::string m_task_type_str;
  std::map<std::string, ML_TASK_TYPE> m_opt_task_map;
  //'column'
  std::string m_opt_datetime_index;
  // JSON_ARRAY('column'[,'column'] ...), to string.
  std::string m_opt_endogenous_variables;
  // JSON_ARRAY('column'[,'column'] ...)
  std::string m_opt_exogenous_variables;
  // JSON_ARRAY('model'[,'model'] ...)
  std::string m_opt_model_list;
  // JSON_ARRAY('model'[,'model'] ...)
  std::string m_opt_exclude_model_list;
  //'metric'
  std::string m_opt_optimization_metric;
  // JSON_ARRAY('column'[,'column'] ...)
  std::string m_opt_include_column_list;
  // JSON_ARRAY('column'[,'column'] ...)
  std::string m_opt_exclude_column_list;
  //'contamination factor', 0< xx < 0.5, default 0.1
  double m_opt_contamination{0.1f};
  // 'users_column'
  std::string m_opt_users;
  //'items_column'
  std::string m_opt_item;
  //'notes_text'
  std::string m_opt_notes;
  // {'explicit'|'implicit'}
  std::string m_opt_feedback{"explicit"};
  // 'threshold'
  double m_opt_feedback_threshold{1.0f};

  std::unique_ptr<ML_algorithm> m_ml_task{nullptr};
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTO_ML_H__