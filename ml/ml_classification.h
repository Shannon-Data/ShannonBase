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
#ifndef __SHANNONBASE_ML_CLASSICIFICATION_H__
#define __SHANNONBASE_ML_CLASSICIFICATION_H__

#include <map>
#include <set>
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

class TABLE;
class Json_wrapper;

namespace ShannonBase {
namespace ML {

class ML_classification : public ML_algorithm {
 public:
  ML_classification();
  virtual ~ML_classification() override;
  int train(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata) override;
  int load(THD *thd, std::string &model_content) override;
  int load_from_file(THD *thd, std::string &model_file_full_path, std::string &model_handle_name) override;
  int unload(THD *thd, std::string &model_handle_name) override;
  int import(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata,
             std::string &model_handle_name) override;
  double score(THD *thd, std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
               std::string &metric_str, Json_wrapper &option) override;

  int explain(THD *thd, std::string &sch_tb_name, std::string &target_name, std::string &model_handle_name,
              Json_wrapper &exp_options) override;
  int explain_row(THD *thd) override;
  int explain_table(THD *thd) override;
  int predict_row(THD *thd, Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
                  Json_wrapper &result) override;
  int predict_table(THD *thd, std::string &sch_tb_name, std::string &model_handle_name, std::string &out_sch_tb_name,
                    Json_wrapper &options) override;
  ML_TASK_TYPE_T type() override;

  void set_schema(std::string &schema_name) { m_sch_name = schema_name; }
  std::string get_schema() const { return m_sch_name; }
  void set_table(std::string &table_name) { m_table_name = table_name; }
  std::string get_table() const { return m_table_name; }
  void set_target(std::string &target_name) { m_target_name = target_name; }
  std::string get_target() const { return m_target_name; }
  void set_handle_name(std::string &handle_name) { m_handler_name = handle_name; }
  std::string get_handle_name() const { return m_handler_name; }
  void set_options(Json_wrapper &options) { m_options = options; }
  const Json_wrapper &get_options() const { return m_options; }

  enum class SCORE_METRIC_T {
    ACCURACY = CLASS_METRIC_START,
    BALANCED_ACCURACY,
    F1,
    F1_MACRO,
    F1_MICRO,
    F1_SAMPLES,
    F1_WEIGTHED,
    NEG_LOG_LOSS,
    PRECISION,
    PRECISION_MACRO,
    PRECISION_MICRO,
    PRECISION_SAMPLES,
    PRECISION_WEIGHTED,
    RECALL,
    RECALL_MACRO,
    RECALL_MICRO,
    RECALL_SAMPLES,
    RECALL_WEIGHTED,
    ROC_AUC
  };

  static std::map<std::string, SCORE_METRIC_T> score_metrics;

 private:
  MODEL_PREDICTION_EXP_T parse_option(Json_wrapper &options);

  int get_txt2num_dict(Json_wrapper &input, std::string &key, txt2numeric_map_t &txt2num_dict);

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

#endif  //__SHANNONBASE_ML_CLASSICIFICATION_H__