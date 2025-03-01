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
#include "ml_regression.h"

#include <chrono>
#include <string>

#include "include/my_inttypes.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "ml_info.h"
#include "ml_utils.h"                         //ml utils
#include "storage/innobase/include/ut0dbg.h"  //for ut_a

//#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"
#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

// clang-format off
// clang-format on
namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string, ML_regression::SCORE_METRIC_T> ML_regression::score_metrics = {
  {"NEG_MEAN_ABSOLUTE_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_SQUARED_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_MEAN_SQUARED_LOG_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR},
  {"NEG_MEDIAN_ABSOLUTE_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR},
  {"R2", ML_regression::SCORE_METRIC_T::R2}
};
// clang-format on

ML_regression::ML_regression() {}
ML_regression::~ML_regression() {}

int ML_regression::train() {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) return HA_ERR_GENERIC;

  return 0;
}

int ML_regression::load(std::string &model_content) {
  // the definition of this table, ref: `ml_train.sql`
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_regression::load_from_file(std::string &model_file_full_path, std::string &model_handle_name) {
  if (!model_file_full_path.length() || !model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_regression::unload(std::string &model_handle_name) {
  assert(!Loaded_models.empty());

  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return 0;
}

int ML_regression::import(Json_wrapper &, Json_wrapper &, std::string &) {
  // all logical done in ml_model_import stored procedure.
  assert(false);
}

double ML_regression::score(std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                            std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric_str << " is invalid for regression scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }
  OPTION_VALUE_T option_keys;
  std::string strkey;
  if (Utils::parse_json(option, option_keys, strkey, 0)) return 0.0;

  auto pos = std::strstr(sch_tb_name.c_str(), ".") - sch_tb_name.c_str();
  std::string schema_name(sch_tb_name.c_str(), pos);
  std::string table_name(sch_tb_name.c_str() + pos + 1, sch_tb_name.length() - pos);

  // load the test data from rapid engine.
  auto source_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << sch_tb_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return 0.0;
  }
  std::vector<double> test_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2numeric;
  auto n_sample =
      Utils::read_data(source_table_ptr, test_data, features_name, target_name, label_data, n_class, txt2numeric);
  Utils::close_table(source_table_ptr);
  if (!n_sample) return 0.0;

  // gets the prediction values.
  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;
  double score{0.0};
  switch ((int)ML_regression::score_metrics[metrics[0]]) {
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::R2:
      break;
    default:
      break;
  }
  return score;
}

int ML_regression::explain(std::string &sch_tb_name [[maybe_unused]], std::string &target_column_name [[maybe_unused]],
                           std::string &model_handle_name [[maybe_unused]],
                           Json_wrapper &exp_options [[maybe_unused]]) {
  return 0;
}

int ML_regression::explain_row() { return 0; }

int ML_regression::explain_table() { return 0; }

int ML_regression::predict_row(Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
                               Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;

  std::string keystr;
  OPTION_VALUE_T meta_feature_names, input_values, options;
  if (!option.empty() && Utils::parse_json(option, options, keystr, 0)) return HA_ERR_GENERIC;

  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  if ((!input_data.empty() && Utils::parse_json(input_data, input_values, keystr, 0)) ||
      (!model_meta.empty() && Utils::parse_json(model_meta, meta_feature_names, keystr, 0))) {
    err << "invalid input data or model meta info.";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  auto feature_names = meta_feature_names[ML_KEYWORDS::column_names];
  if (feature_names.size() != input_values.size()) {
    err << "input data columns size does not match the model feature columns size.";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  for (auto &feature_name : feature_names) {
    if (input_values.find(feature_name) == input_values.end()) {
      err << "input data columns does not contain the model feature column: " << feature_name;
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  txt2numeric_map_t txt2numeric;
  if (Utils::get_txt2num_dict(model_meta, txt2numeric)) return HA_ERR_GENERIC;

  Json_object *root_obj = new (std::nothrow) Json_object();
  if (root_obj == nullptr) {
    return HA_ERR_GENERIC;
  }

  std::vector<ml_record_type_t> sample_data;
  for (auto &feature_name : feature_names) {
    std::string value{"0"};
    if (input_values.find(feature_name) != input_values.end()) value = input_values[feature_name][0];
    sample_data.push_back({feature_name, value});
    root_obj->add_alias(feature_name, new (std::nothrow) Json_string(value));
  }

  // prediction
  std::vector<double> predictions;
  root_obj->add_alias(ML_KEYWORDS::Prediction,
                      new (std::nothrow) Json_string(meta_feature_names[ML_KEYWORDS::train_table_name][0]));
  auto ret = Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);
  if (ret) {
    err << "call ML_PREDICT_ROW failed";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  auto rating{0};
  // ml_results
  Json_object *ml_results_obj = new (std::nothrow) Json_object();
  if (ml_results_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  // ml_results: prediction
  Json_object *predictions_obj = new (std::nothrow) Json_object();
  if (predictions_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  predictions_obj->add_alias(ML_KEYWORDS::rating, new (std::nothrow) Json_double(rating));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return ret;
}

int ML_regression::predict_table(std::string &sch_tb_name [[maybe_unused]],
                                 std::string &model_handle_name [[maybe_unused]],
                                 std::string &out_sch_tb_name [[maybe_unused]],
                                 Json_wrapper &options [[maybe_unused]]) {
  return 0;
}

ML_TASK_TYPE_T ML_regression::type() { return ML_TASK_TYPE_T::REGRESSION; }

}  // namespace ML
}  // namespace ShannonBase