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

#include "ml_forecasting.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>

#include "include/my_base.h"
#include "include/mysqld_error.h"

#include "ml_info.h"
#include "ml_utils.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_forecasting::SCORE_METRIC_T> ML_forecasting::score_metrics = {
  {"NEG_MAX_ABSOLUTE_ERROR",            ML_forecasting::SCORE_METRIC_T::NEG_MAX_ABSOLUTE_ERROR},
  {"NEG_MEAN_ABSOLUTE_ERROR",           ML_forecasting::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_ABS_SCALED_ERROR",         ML_forecasting::SCORE_METRIC_T::NEG_MEAN_ABS_SCALED_ERROR},
  {"NEG_MEAN_SQUARED_ERROR",            ML_forecasting::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_ROOT_MEAN_SQUARED_ERROR",       ML_forecasting::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR},
  {"NEG_ROOT_MEAN_SQUARED_PERCENT_ERROR", ML_forecasting::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_PERCENT_ERROR},
  {"NEG_SYM_MEAN_ABS_PERCENT_ERROR",    ML_forecasting::SCORE_METRIC_T::NEG_SYM_MEAN_ABS_PERCENT_ERROR}
};
// clang-format on

namespace {
void split_sch_table(const std::string &sch_tb_name, std::string &schema_name, std::string &table_name) {
  auto dot = sch_tb_name.find('.');
  if (dot == std::string::npos) {
    schema_name = "";
    table_name = sch_tb_name;
    return;
  }
  schema_name = sch_tb_name.substr(0, dot);
  table_name = sch_tb_name.substr(dot + 1);
}

double calc_neg_max_ae(const std::vector<double> &pred, const std::vector<float> &actual) {
  double max_err = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) max_err = std::max(max_err, std::abs(pred[i] - actual[i]));
  return -max_err;
}

double calc_neg_mae(const std::vector<double> &pred, const std::vector<float> &actual) {
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) sum += std::abs(pred[i] - actual[i]);
  return -(sum / pred.size());
}

/**
 * Mean Absolute Scaled Error (MASE).
 * Uses the naive one-step-ahead forecast (shifted by 1) as the scaling baseline.
 */
double calc_neg_mase(const std::vector<double> &pred, const std::vector<float> &actual) {
  size_t n = pred.size();
  if (n < 2) return 0.0;
  // Scaling factor: mean absolute one-step naïve error
  double naive_sum = 0.0;
  for (size_t i = 1; i < n; ++i) naive_sum += std::abs(actual[i] - actual[i - 1]);
  double scale = naive_sum / static_cast<double>(n - 1);
  if (scale < 1e-9) scale = 1e-9;
  double mae = 0.0;
  for (size_t i = 0; i < n; ++i) mae += std::abs(pred[i] - actual[i]);
  mae /= static_cast<double>(n);
  return -(mae / scale);
}

double calc_neg_mse(const std::vector<double> &pred, const std::vector<float> &actual) {
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) {
    double d = pred[i] - actual[i];
    sum += d * d;
  }
  return -(sum / pred.size());
}

double calc_neg_rmse(const std::vector<double> &pred, const std::vector<float> &actual) {
  return -std::sqrt(-calc_neg_mse(pred, actual));
}

double calc_neg_rmspe(const std::vector<double> &pred, const std::vector<float> &actual) {
  double sum = 0.0;
  size_t cnt = 0;
  for (size_t i = 0; i < pred.size(); ++i) {
    if (std::abs(actual[i]) < 1e-9) continue;  // skip zero actuals
    double r = (pred[i] - actual[i]) / actual[i];
    sum += r * r;
    ++cnt;
  }
  return cnt > 0 ? -std::sqrt(sum / cnt) : 0.0;
}

double calc_neg_smape(const std::vector<double> &pred, const std::vector<float> &actual) {
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) {
    double denom = (std::abs(pred[i]) + std::abs(actual[i])) / 2.0;
    if (denom < 1e-9) continue;
    sum += std::abs(pred[i] - actual[i]) / denom;
  }
  return -(sum / pred.size());
}
}  // anonymous namespace

int ML_forecasting::train(THD * /*thd*/, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  std::vector<std::string> target_names;
  Utils::splitString(m_target_name, ',', target_names);
  assert(target_names.size() == 0 || target_names.size() == 1);

  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  std::string dt_index;
  if (options.find(ML_KEYWORDS::datetime_index) != options.end()) dt_index = options[ML_KEYWORDS::datetime_index][0];

  std::vector<std::string> endogenous;
  if (options.find(ML_KEYWORDS::endogenous_variables) != options.end())
    endogenous = options[ML_KEYWORDS::endogenous_variables];

  std::vector<std::string> exogenous;
  if (options.find(ML_KEYWORDS::exogenous_variables) != options.end())
    exogenous = options[ML_KEYWORDS::exogenous_variables];

  std::vector<std::string> include_cols, exclude_cols, model_list, exclude_model_list;
  std::string optimization_metric;
  Utils::parse_common_options(m_options, include_cols, exclude_cols, model_list, exclude_model_list,
                              optimization_metric);

  // The target must be one of the endogenous variables
  std::string target = (m_target_name.empty() && !endogenous.empty()) ? endogenous[0] : m_target_name;

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " NOT loaded into rapid engine";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name, target, label_data, n_class,
                                   txt2num_dict, &include_cols, &exclude_cols);
  Utils::close_table(source_table_ptr);

  auto n_feature = features_name.size();
  std::ostringstream oss;
  std::string metric_str = (optimization_metric.empty()) ? "rmse" : Utils::METRIC_MAP.at(optimization_metric);
  oss << "task=train boosting_type=gbdt objective=regression metric=" << metric_str
      << " metric_freq=1 is_training_metric=true max_bin=255 num_trees=100 learning_rate=0.05"
      << " num_leaves=31 tree_learner=serial feature_fraction=0.8"
      << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=20"
      << " min_sum_hessian_in_leaf=1.0 is_enable_sparse=true use_two_round_loading=false";

  std::string model_content, mode_params(oss.str());

  std::vector<const char *> feature_names_cstr;
  for (const auto &name : features_name) feature_names_cstr.push_back(name.c_str());

  // clang-format off
  auto start = std::chrono::steady_clock::now();
  if (Utils::ML_train(mode_params,
                      C_API_DTYPE_FLOAT64, train_data.data(), n_sample,
                      feature_names_cstr.data(), n_feature,
                      C_API_DTYPE_FLOAT32, label_data.data(),
                      model_content))
    return HA_ERR_GENERIC;
  auto end = std::chrono::steady_clock::now();
  auto train_duration =
    std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f;
  // clang-format on

  oss.clear();
  oss.str("");
  oss << m_sch_name << "." << m_table_name;
  std::string sch_tb_name(oss.str()), notes, opt_metrics;

  auto content_dom = Json_dom::parse(
      model_content.c_str(), model_content.length(), [](const char *, size_t) { assert(false); },
      [] { assert(false); });
  if (!content_dom.get()) return HA_ERR_GENERIC;
  model_object = Json_wrapper(std::move(content_dom));

  auto meta_json =
      Utils::build_up_model_metadata(TASK_NAMES_MAP[type()], m_target_name, sch_tb_name, features_name, nullptr, notes,
                                     MODEL_FORMATS_MAP[MODEL_FORMAT_T::VER_1], MODEL_STATUS_MAP[MODEL_STATUS_T::READY],
                                     MODEL_QUALITIES_MAP[MODEL_QUALITY_T::HIGH], train_duration, TASK_NAMES_MAP[type()],
                                     0, n_sample, n_feature + 1, n_sample, n_feature, opt_metrics, features_name, 0,
                                     &m_options, mode_params, nullptr, nullptr, nullptr, 1, txt2num_dict);

  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_forecasting::load(THD * /*thd*/, std::string &model_content) {
  assert(model_content.length() && m_handler_name.length());
  std::lock_guard<std::mutex> lock(models_mutex);
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_forecasting::load_from_file(THD * /*thd*/, std::string &model_file_full_path, std::string &model_handle_name) {
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  std::lock_guard<std::mutex> lock(models_mutex);
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_forecasting::unload(THD * /*thd*/, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_forecasting::import(THD * /*thd*/, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_forecasting::score(THD * /*thd*/, std::string &sch_tb_name, std::string &target_name,
                             std::string &model_handle, std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric << " is invalid for forecasting scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }

  OPTION_VALUE_T option_keys;
  std::string strkey;
  if (!option.empty() && Utils::parse_json(option, option_keys, strkey, 0)) return 0.0;

  std::string schema_name, table_name;
  split_sch_table(sch_tb_name, schema_name, table_name);

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

  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;

  double score_val = 0.0;
  switch ((int)ML_forecasting::score_metrics[metrics[0]]) {
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_MAX_ABSOLUTE_ERROR:
      score_val = calc_neg_max_ae(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      score_val = calc_neg_mae(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_MEAN_ABS_SCALED_ERROR:
      score_val = calc_neg_mase(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      score_val = calc_neg_mse(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR:
      score_val = calc_neg_rmse(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_PERCENT_ERROR:
      score_val = calc_neg_rmspe(predictions, label_data);
      break;
    case (int)ML_forecasting::SCORE_METRIC_T::NEG_SYM_MEAN_ABS_PERCENT_ERROR:
      score_val = calc_neg_smape(predictions, label_data);
      break;
    default:
      break;
  }
  return score_val;
}

int ML_forecasting::explain(THD * /*thd*/, std::string & /*sch_tb_name*/, std::string & /*target_column_name*/,
                            std::string & /*model_handle_name*/, Json_wrapper & /*exp_options*/) {
  my_error(ER_ML_FAIL, MYF(0), "forecasting does not support explain.");
  return HA_ERR_GENERIC;
}

int ML_forecasting::explain_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) {
  my_error(ER_ML_FAIL, MYF(0), "forecasting does not support explain.");
  return HA_ERR_GENERIC;
}

int ML_forecasting::explain_table(THD *) {
  my_error(ER_ML_FAIL, MYF(0), "forecasting does not support explain.");
  return HA_ERR_GENERIC;
}

int ML_forecasting::predict_row(THD * /*thd*/, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) {
  std::ostringstream err;
  err << "forecasting does not support predict_row.";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_forecasting::predict_table(THD * /*thd*/, std::string & /*sch_tb_name*/, std::string & /*model_handle_name*/,
                                  std::string & /*out_sch_tb_name*/, Json_wrapper & /*options*/) {
  return 0;
}
}  // namespace ML
}  // namespace ShannonBase