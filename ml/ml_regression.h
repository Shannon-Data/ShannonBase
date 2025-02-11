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
#ifndef __SHANNONBASE_ML_REGRESSION_H__
#define __SHANNONBASE_ML_REGRESSION_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

namespace LightGBM {
class DatasetLoader;
class Dataset;
class Boosting;
class ObjectiveFunction;
class Metric;
class Config;
}  // namespace LightGBM

class Json_wrapper;
namespace ShannonBase {
namespace ML {

using Traing_data_t = std::vector<std::vector<double>>;
class ML_regression : public ML_algorithm {
 public:
  ML_regression();
  virtual ~ML_regression() override;
  int train() override;
  int predict() override;
  int load(std::string &model_content) override;
  int load_from_file(std::string &modle_file_full_path, std::string &model_handle_name) override;
  int unload(std::string &model_handle_name) override;
  int import(std::string &model_handle_name, std::string &user_name, std::string &content) override;
  double score(std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
               std::string &metric_str) override;
  int explain_row() override;
  int explain_table() override;
  int predict_row() override;
  int predict_table() override;
  ML_TASK_TYPE type() override;

  void set_schema(std::string &schema_name) { m_sch_name = schema_name; }
  std::string get_schema() const { return m_sch_name; }
  void set_table(std::string &table_name) { m_table_name = table_name; }
  std::string get_table() const { return m_table_name; }
  void set_target(std::string &target_name) { m_target_name = target_name; }
  std::string get_target() const { return m_target_name; }
  void set_handle_name(std::string &handle_name) { m_handler_name = handle_name; }
  std::string get_handle_name() const { return m_handler_name; }
  void set_options(Json_wrapper &options) { m_options = options; }
  Json_wrapper &get_options() { return m_options; }

  static const std::vector<std::string> metrics;

 private:
  std::string m_sch_name;
  std::string m_table_name;
  std::string m_target_name;
  std::string m_handler_name;
  Json_wrapper m_options;

  void *m_handler{nullptr};
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_REGRESSION_H__