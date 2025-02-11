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
#ifndef __SHANNONBASE_ML_ANOMALY_DECTION_H__
#define __SHANNONBASE_ML_ANOMALY_DECTION_H__

#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {

class ML_anomaly_detection : public ML_algorithm {
 public:
  ML_anomaly_detection();
  virtual ~ML_anomaly_detection() override;
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

  // Metrics for anomaly detection can only be used with the ML_SCORE routine.
  // They cannot be used with the ML_TRAIN routine.
  static const std::vector<std::string> non_thr_topk_metrics;
  static const std::vector<std::string> threshold_metrics;
  static const std::vector<std::string> topk_metrics;

 private:
  // source data schema name.
  std::string m_sch_name;
  // source data table name.
  std::string m_table_name;
  // source labelled column name.
  std::string m_target_name;
  // model handle name.
  std::string m_handler_name;
  // model options JSON format.
  Json_wrapper m_options;

  void *m_handler{nullptr};
};

}  // namespace ML
}  // namespace ShannonBase

#endif  //__SHANNONBASE_ML_ANOMALY_DECTION_H__