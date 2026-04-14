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

#include "ml_recommendation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>
#include <string>

#include "include/my_base.h"
#include "include/mysqld_error.h"
#include "mysqld_error.h"
#include "sql/field.h"  //Field
#include "sql/table.h"  //Table

#include "ml_utils.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_recommendation::SCORE_METRIC_T> ML_recommendation::score_metrics = {
  {"HIT_RATIO_AT_K",           ML_recommendation::SCORE_METRIC_T::HIT_RATIO_AT_K},
  {"NDCG_AT_K",                ML_recommendation::SCORE_METRIC_T::NDCG_AT_K},
  {"NEG_MEAN_ABSOLUTE_ERROR",  ML_recommendation::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_SQUARED_ERROR",   ML_recommendation::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_ROOT_MEAN_SQUARED_ERROR", ML_recommendation::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR},
  {"PRECISION_AT_K",           ML_recommendation::SCORE_METRIC_T::PRECISION_AT_K},
  {"R2",                       ML_recommendation::SCORE_METRIC_T::R2},
  {"RECALL_AT_K",              ML_recommendation::SCORE_METRIC_T::RECALL_AT_K}
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

double calc_neg_mae(const std::vector<double> &pred, const std::vector<float> &actual) {
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) sum += std::abs(pred[i] - actual[i]);
  return -(sum / pred.size());
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

double calc_r2(const std::vector<double> &pred, const std::vector<float> &actual) {
  size_t n = pred.size();
  double mean_a = 0.0;
  for (auto v : actual) mean_a += v;
  mean_a /= static_cast<double>(n);
  double ss_tot = 0.0, ss_res = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double da = actual[i] - mean_a;
    ss_tot += da * da;
    double dr = actual[i] - pred[i];
    ss_res += dr * dr;
  }
  return (ss_tot == 0.0) ? 1.0 : 1.0 - (ss_res / ss_tot);
}

std::string get_lgb_metric(const std::string &metric) {
  static const std::map<std::string, std::string> metric_map = {{"NEG_MEAN_ABSOLUTE_ERROR", "mae"},
                                                                {"NEG_MEAN_SQUARED_ERROR", "mse"},
                                                                {"NEG_ROOT_MEAN_SQUARED_ERROR", "rmse"},
                                                                {"R2", "r2"},
                                                                {"HIT_RATIO_AT_K", "map"},  // approximate
                                                                {"NDCG_AT_K", "ndcg"},
                                                                {"PRECISION_AT_K", "pre@k"},
                                                                {"RECALL_AT_K", "recall@k"}};
  auto it = metric_map.find(metric);
  return (it != metric_map.end()) ? it->second : "rmse";
}
}  // anonymous namespace

int ML_recommendation::train(THD *, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  std::vector<std::string> target_names;
  Utils::splitString(m_target_name, ',', target_names);
  bool has_target = (target_names.size() == 1 && !target_names[0].empty());

  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  if (options.find(ML_KEYWORDS::users) == options.end() || options.find(ML_KEYWORDS::items) == options.end()) {
    my_error(ER_ML_FAIL, MYF(0), "users and items columns must be specified in options");
    return HA_ERR_GENERIC;
  }

  std::string users_col = options[ML_KEYWORDS::users][0];
  std::string items_col = options[ML_KEYWORDS::items][0];

  // Optional recommendation options
  std::string feedback_type = "explicit";  // explicit or implicit
  if (options.find(ML_KEYWORDS::feedback) != options.end()) feedback_type = options[ML_KEYWORDS::feedback][0];

  double feedback_threshold = 1.0;
  if (options.find(ML_KEYWORDS::feedback_threshold) != options.end())
    feedback_threshold = std::stod(options[ML_KEYWORDS::feedback_threshold][0]);

  std::vector<std::string> model_list;
  if (options.find(ML_KEYWORDS::model_list) != options.end()) model_list = options[ML_KEYWORDS::model_list];

  std::vector<std::string> exclude_model_list;
  if (options.find(ML_KEYWORDS::exclude_model_list) != options.end())
    exclude_model_list = options[ML_KEYWORDS::exclude_model_list];

  std::string optimization_metric;
  if (options.find(ML_KEYWORDS::optimization_metric) != options.end() &&
      !options[ML_KEYWORDS::optimization_metric].empty())
    optimization_metric = options[ML_KEYWORDS::optimization_metric][0];

  std::vector<std::string> include_cols, exclude_cols;
  if (options.find(ML_KEYWORDS::include_column_list) != options.end())
    include_cols = options[ML_KEYWORDS::include_column_list];
  if (options.find(ML_KEYWORDS::exclude_column_list) != options.end())
    exclude_cols = options[ML_KEYWORDS::exclude_column_list];

  // Content-based recommendation options (for implicit feedback)
  Json_wrapper item_metadata_wrapper, user_metadata_wrapper;
  if (options.find(ML_KEYWORDS::item_metadata) != options.end()) {
    // Parse item_metadata JSON object
    MYSQL_LEX_CSTRING key_item = {STRING_WITH_LEN(ML_KEYWORDS::item_metadata)};
    item_metadata_wrapper = m_options.lookup(key_item);
  }
  if (options.find(ML_KEYWORDS::user_metadata) != options.end()) {
    MYSQL_LEX_CSTRING key_user = {STRING_WITH_LEN(ML_KEYWORDS::user_metadata)};
    user_metadata_wrapper = m_options.lookup(key_user);
  }

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

  // Validate users and items columns are string types
  for (auto index = 0u; index < source_table_ptr->s->fields; index++) {
    auto field_ptr = *(source_table_ptr->field + index);
    std::string col_name(field_ptr->field_name);
    if (col_name == users_col || col_name == items_col) {
      if (field_ptr->type() != MYSQL_TYPE_VARCHAR && field_ptr->type() != MYSQL_TYPE_VAR_STRING &&
          field_ptr->type() != MYSQL_TYPE_STRING) {
        std::ostringstream err;
        err << col_name << ": users and items columns must be string type";
        my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
        Utils::close_table(source_table_ptr);
        return HA_ERR_GENERIC;
      }
    }
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  std::string target = has_target ? target_names[0] : "";
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name, target, label_data, n_class,
                                   txt2num_dict, &include_cols, &exclude_cols);
  Utils::close_table(source_table_ptr);

  if (n_sample == 0) {
    my_error(ER_ML_FAIL, MYF(0), "no data read from training table");
    return HA_ERR_GENERIC;
  }

  // For implicit feedback, apply threshold to convert to binary feedback
  if (feedback_type == "implicit" && has_target) {
    for (auto &val : label_data) {
      val = (val >= feedback_threshold) ? 1.0f : 0.0f;
    }
  }

  auto n_feature = features_name.size();
  std::ostringstream oss;
  if (feedback_type == "explicit") {
    // Explicit feedback: regression to predict ratings
    std::string metric = optimization_metric.empty() ? "rmse" : get_lgb_metric(optimization_metric);
    oss << "task=train boosting_type=gbdt objective=regression metric=" << metric
        << " metric_freq=1 is_training_metric=true num_trees=100 learning_rate=0.05"
        << " num_leaves=31 tree_learner=serial feature_fraction=0.8"
        << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=10"
        << " min_sum_hessian_in_leaf=1.0 is_enable_sparse=true use_two_round_loading=false";
  } else {
    // Implicit feedback: ranking objective (LambdaMART)
    std::string metric = optimization_metric.empty() ? "binary_logloss" : get_lgb_metric(optimization_metric);
    oss << "task=train boosting_type=gbdt objective=binary"
        << " metric=" << metric << " max_bin=255 num_trees=100 learning_rate=0.05"
        << " num_leaves=31 tree_learner=serial feature_fraction=0.8"
        << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=10"
        << " min_sum_hessian_in_leaf=1.0 is_enable_sparse=true"
        << " use_two_round_loading=false";
  }

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

  // Add recommendation-specific fields to metadata via options modification
  auto meta_json = Utils::build_up_model_metadata(
      TASK_NAMES_MAP[type()], m_target_name, sch_tb_name, features_name, nullptr, notes,
      MODEL_FORMATS_MAP[MODEL_FORMAT_T::VER_1], MODEL_STATUS_MAP[MODEL_STATUS_T::READY],
      MODEL_QUALITIES_MAP[MODEL_QUALITY_T::HIGH], train_duration, TASK_NAMES_MAP[type()], 0, n_sample,
      n_feature + (has_target ? 1 : 0), n_sample, n_feature, opt_metrics, features_name, 0, &m_options, mode_params,
      nullptr, nullptr, nullptr, 1, txt2num_dict);

  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_recommendation::load(THD *, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_recommendation::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_recommendation::unload(THD *, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_recommendation::import(THD *, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_recommendation::score(THD *, std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                                std::string &metric_str, Json_wrapper &option) {
  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric << " is invalid for recommendation scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }

  OPTION_VALUE_T option_keys;
  std::string strkey;
  if (!option.empty() && Utils::parse_json(option, option_keys, strkey, 0)) return 0.0;

  // FIX: correct string split
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
  switch ((int)ML_recommendation::score_metrics[metrics[0]]) {
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      score_val = calc_neg_mae(predictions, label_data);
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      score_val = calc_neg_mse(predictions, label_data);
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR:
      score_val = calc_neg_rmse(predictions, label_data);
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::R2:
      score_val = calc_r2(predictions, label_data);
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::HIT_RATIO_AT_K:
    case (int)ML_recommendation::SCORE_METRIC_T::NDCG_AT_K:
    case (int)ML_recommendation::SCORE_METRIC_T::PRECISION_AT_K:
    case (int)ML_recommendation::SCORE_METRIC_T::RECALL_AT_K:
      // TODO: ranking metrics require per-user top-K item lists.
      // Needs user/item grouping logic beyond per-row prediction.
      score_val = 0.0;
      break;
    default:
      break;
  }
  return score_val;
}

int ML_recommendation::explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) {
  my_error(ER_ML_FAIL, MYF(0), "recommendation does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_recommendation::explain_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) {
  my_error(ER_ML_FAIL, MYF(0), "recommendation does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_recommendation::explain_table(THD *) {
  my_error(ER_ML_FAIL, MYF(0), "recommendation does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_recommendation::predict_row(THD *, Json_wrapper &input_data, std::string &model_handle_name,
                                   Json_wrapper &option, Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;

  std::string keystr;
  OPTION_VALUE_T meta_feature_names, input_values, options;
  if (!option.empty() && Utils::parse_json(option, options, keystr, 0)) return HA_ERR_GENERIC;

  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  if ((!input_data.empty() && Utils::parse_json(input_data, input_values, keystr, 0)) ||
      (!model_meta.empty() && Utils::parse_json(model_meta, meta_feature_names, keystr, 0))) {
    my_error(ER_ML_FAIL, MYF(0), "invalid input data or model meta info.");
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_feature_names[ML_KEYWORDS::column_names];
  if (feature_names.size() != input_values.size()) {
    my_error(ER_ML_FAIL, MYF(0), "input column count does not match model feature count.");
    return HA_ERR_GENERIC;
  }
  for (auto &feature_name : feature_names) {
    if (input_values.find(feature_name) == input_values.end()) {
      err << "input data missing model feature column: " << feature_name;
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  txt2numeric_map_t txt2numeric;
  if (Utils::get_txt2num_dict(model_meta, txt2numeric)) return HA_ERR_GENERIC;

  Json_object *root_obj = new (std::nothrow) Json_object();
  if (!root_obj) return HA_ERR_GENERIC;

  std::vector<ml_record_type_t> sample_data;
  for (auto &feature_name : feature_names) {
    std::string value{"0"};
    if (input_values.find(feature_name) != input_values.end()) value = input_values[feature_name][0];
    sample_data.push_back({feature_name, value});
    root_obj->add_alias(feature_name, new (std::nothrow) Json_string(value));
  }

  std::vector<double> predictions;
  auto ret = Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);
  if (ret || predictions.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "ML_predict_row failed for recommendation");
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  // The model outputs a predicted rating (continuous score).
  double predicted_rating = predictions[0];
  root_obj->add_alias(ML_KEYWORDS::Prediction, new (std::nothrow) Json_double(predicted_rating));

  // Build ml_results
  Json_object *ml_results_obj = new (std::nothrow) Json_object();
  if (!ml_results_obj) {
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  Json_object *predictions_obj = new (std::nothrow) Json_object();
  if (!predictions_obj) {
    delete ml_results_obj;
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  predictions_obj->add_alias(ML_KEYWORDS::rating, new (std::nothrow) Json_double(predicted_rating));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return 0;
}

int ML_recommendation::predict_table(THD * /*thd*/, std::string & /*sch_tb_name*/, std::string & /*model_handle_name*/,
                                     std::string & /*out_sch_tb_name*/, Json_wrapper & /*options*/) {
  return 0;
}
}  // namespace ML
}  // namespace ShannonBase