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

#include "ml_classification.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>

#include "sql/item_func.h"

#include "include/my_inttypes.h"
#include "include/mysqld_error.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"
#include "storage/innobase/include/ut0dbg.h"  //for ut_a
#include "storage/rapid_engine/include/rapid_config.h"

#include "auto_ml.h"
#include "ml_info.h"
#include "ml_utils.h"  //ml utils

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_classification::SCORE_METRIC_T> ML_classification::score_metrics = {
  {"ACCURACY",           ML_classification::SCORE_METRIC_T::ACCURACY},
  {"BALANCED_ACCURACY",  ML_classification::SCORE_METRIC_T::BALANCED_ACCURACY},
  {"F1",                 ML_classification::SCORE_METRIC_T::F1},
  {"F1_MACRO",           ML_classification::SCORE_METRIC_T::F1_MACRO},
  {"F1_MICRO",           ML_classification::SCORE_METRIC_T::F1_MICRO},
  {"F1_SAMPLES",         ML_classification::SCORE_METRIC_T::F1_SAMPLES},
  {"F1_WEIGTHED",        ML_classification::SCORE_METRIC_T::F1_WEIGTHED},
  {"NEG_LOG_LOSS",       ML_classification::SCORE_METRIC_T::NEG_LOG_LOSS},
  {"PRECISION",          ML_classification::SCORE_METRIC_T::PRECISION},
  {"PRECISION_MACRO",    ML_classification::SCORE_METRIC_T::PRECISION_MACRO},
  {"PRECISION_MICRO",    ML_classification::SCORE_METRIC_T::PRECISION_MICRO},
  {"PRECISION_SAMPLES",  ML_classification::SCORE_METRIC_T::PRECISION_SAMPLES},
  {"PRECISION_WEIGHTED", ML_classification::SCORE_METRIC_T::PRECISION_WEIGHTED},
  {"RECALL",             ML_classification::SCORE_METRIC_T::RECALL},
  {"RECALL_MACRO",       ML_classification::SCORE_METRIC_T::RECALL_MACRO},
  {"RECALL_MICRO",       ML_classification::SCORE_METRIC_T::RECALL_MICRO},
  {"RECALL_SAMPLES",     ML_classification::SCORE_METRIC_T::RECALL_SAMPLES},
  {"RECALL_WEIGHTED",    ML_classification::SCORE_METRIC_T::RECALL_WEIGHTED},
  {"ROC_AUC",            ML_classification::SCORE_METRIC_T::ROC_AUC}
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

double calc_accuracy(size_t n, const std::vector<double> &pred, const std::vector<float> &actual) {
  size_t correct = 0;
  for (size_t i = 0; i < n; ++i)
    if (static_cast<int>(std::round(pred[i])) == static_cast<int>(actual[i])) ++correct;
  return static_cast<double>(correct) / static_cast<double>(n);
}

double calc_balanced_accuracy(size_t n, const std::vector<double> &pred, const std::vector<float> &actual) {
  // Find distinct classes
  std::set<int> classes;
  for (auto v : actual) classes.insert(static_cast<int>(v));
  double sum_recall = 0.0;
  for (int c : classes) {
    size_t tp = 0, fn = 0;
    for (size_t i = 0; i < n; ++i) {
      int a = static_cast<int>(actual[i]);
      int p = static_cast<int>(std::round(pred[i]));
      if (a == c) {
        (p == c) ? ++tp : ++fn;
      }
    }
    sum_recall += (tp + fn > 0) ? (double)tp / (tp + fn) : 0.0;
  }
  return sum_recall / static_cast<double>(classes.size());
}

double calc_binary_metric(size_t n, const std::vector<double> &pred, const std::vector<float> &actual,
                          const std::string &kind) {
  size_t tp = 0, tn = 0, fp = 0, fn = 0;
  for (size_t i = 0; i < n; ++i) {
    int p = static_cast<int>(std::round(pred[i]));
    int a = static_cast<int>(actual[i]);
    if (a == 1 && p == 1)
      ++tp;
    else if (a == 0 && p == 0)
      ++tn;
    else if (a == 0 && p == 1)
      ++fp;
    else
      ++fn;
  }
  double prec = (tp + fp > 0) ? (double)tp / (tp + fp) : 0.0;
  double rec = (tp + fn > 0) ? (double)tp / (tp + fn) : 0.0;
  if (kind == "precision") return prec;
  if (kind == "recall") return rec;
  if (kind == "f1") return (prec + rec > 0) ? 2.0 * prec * rec / (prec + rec) : 0.0;
  return 0.0;
}

double calc_neg_log_loss(size_t n, const std::vector<double> &pred, const std::vector<float> &actual) {
  double loss = 0.0;
  constexpr double eps = 1e-15;
  for (size_t i = 0; i < n; ++i) {
    double p = std::max(eps, std::min(1.0 - eps, pred[i]));
    int a = static_cast<int>(actual[i]);
    loss += a * std::log(p) + (1 - a) * std::log(1.0 - p);
  }
  return loss / static_cast<double>(n);
}
}  // anonymous namespace

MODEL_PREDICTION_EXP_T ML_classification::parse_option(Json_wrapper &options) {
  MODEL_PREDICTION_EXP_T explainer_type{MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE};
  auto dom_ptr = options.clone_dom();
  if (!dom_ptr) return explainer_type;

  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr = json_obj->get("model_explainer");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    auto exp_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(exp_type_str.begin(), exp_type_str.end(), exp_type_str.begin(), ::toupper);
    explainer_type = MODEL_EXPLAINERS_MAP["MODEL_" + exp_type_str];
  }
  value_dom_ptr = json_obj->get("prediction_explainer");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    auto exp_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(exp_type_str.begin(), exp_type_str.end(), exp_type_str.begin(), ::toupper);
    explainer_type = MODEL_EXPLAINERS_MAP["PREDICT_" + exp_type_str];
  }
  return explainer_type;
}

int ML_classification::train(THD * /*thd*/, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  std::vector<std::string> include_cols, exclude_cols, model_list, exclude_model_list;
  std::string optimization_metric;
  Utils::parse_common_options(m_options, include_cols, exclude_cols, model_list, exclude_model_list,
                              optimization_metric);

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
  std::vector<std::string> features_name, target_names;
  Utils::splitString(m_target_name, ',', target_names);
  assert(target_names.size() == 1);
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name, target_names[0], label_data, n_class,
                                   txt2num_dict, &include_cols, &exclude_cols);
  Utils::close_table(source_table_ptr);

  auto n_feature = features_name.size();
  std::ostringstream oss;
  std::string metric_str = (optimization_metric.empty()) ? "" : Utils::METRIC_MAP.at(optimization_metric);
  if (n_class <= 2) {
    oss << "task=train boosting_type=gbdt objective=binary";
    if (!metric_str.empty())
      oss << " metric=" << metric_str;
    else
      oss << " metric=binary_logloss";
    oss << " metric_freq=1 is_training_metric=true num_trees=100 learning_rate=0.1"
        << " num_leaves=63 tree_learner=serial feature_fraction=0.8 max_bin=255"
        << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=50"
        << " min_sum_hessian_in_leaf=5.0 is_enable_sparse=true use_two_round_loading=false";
  } else {
    oss << "task=train boosting_type=gbdt objective=multiclass num_class=" << n_class;
    if (!metric_str.empty())
      oss << " metric=" << metric_str;
    else
      oss << " metric=multi_logloss";
    oss << " metric_freq=1 is_training_metric=true max_bin=255"
        << " early_stopping=10 num_trees=100 learning_rate=0.05 num_leaves=31";
  }

  std::string model_content, mode_params(oss.str().c_str());

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

int ML_classification::load(THD * /*thd*/, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_classification::load_from_file(THD * /*thd*/, std::string &model_file_full_path,
                                      std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_classification::unload(THD * /*thd*/, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_classification::import(THD *, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_classification::score(THD * /*thd*/, std::string &sch_tb_name, std::string &target_name,
                                std::string &model_handle, std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  if (!option.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "option params should be null for classification");
    return 0.0;
  }

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric << " is invalid for classification scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }

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
  switch ((int)ML_classification::score_metrics[metrics[0]]) {
    case (int)ML_classification::SCORE_METRIC_T::ACCURACY:
      score_val = calc_accuracy(n_sample, predictions, label_data);
      break;
    case (int)ML_classification::SCORE_METRIC_T::BALANCED_ACCURACY:
      score_val = calc_balanced_accuracy(n_sample, predictions, label_data);
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1:
    case (int)ML_classification::SCORE_METRIC_T::F1_MACRO:
    case (int)ML_classification::SCORE_METRIC_T::F1_MICRO:
    case (int)ML_classification::SCORE_METRIC_T::F1_SAMPLES:
    case (int)ML_classification::SCORE_METRIC_T::F1_WEIGTHED:
      score_val = calc_binary_metric(n_sample, predictions, label_data, "f1");
      break;
    case (int)ML_classification::SCORE_METRIC_T::NEG_LOG_LOSS:
      score_val = calc_neg_log_loss(n_sample, predictions, label_data);
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION:
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_MACRO:
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_MICRO:
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_SAMPLES:
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_WEIGHTED:
      score_val = calc_binary_metric(n_sample, predictions, label_data, "precision");
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL:
    case (int)ML_classification::SCORE_METRIC_T::RECALL_MACRO:
    case (int)ML_classification::SCORE_METRIC_T::RECALL_MICRO:
    case (int)ML_classification::SCORE_METRIC_T::RECALL_SAMPLES:
    case (int)ML_classification::SCORE_METRIC_T::RECALL_WEIGHTED:
      score_val = calc_binary_metric(n_sample, predictions, label_data, "recall");
      break;
    case (int)ML_classification::SCORE_METRIC_T::ROC_AUC: {
      score_val = Utils::calculate_accuracy(n_sample, predictions, label_data);
    } break;
    default:
      break;
  }
  return score_val;
}

int ML_classification::explain(THD * /*thd*/, std::string &sch_tb_name, std::string &target_name,
                               std::string &model_handle_name, Json_wrapper &exp_options) {
  assert(sch_tb_name.length() && target_name.length());
  std::ostringstream err;

  // FIX: correct string split
  std::string schema_name, table_name;
  split_sch_table(sch_tb_name, schema_name, table_name);

  auto in_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!in_table_ptr) {
    err << sch_tb_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name, target_names;
  Utils::splitString(target_name, ',', target_names);
  assert(target_names.size() == 1);
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  auto n_sample [[maybe_unused]] =
      Utils::read_data(in_table_ptr, train_data, features_name, target_names[0], label_data, n_class, txt2num_dict);
  Utils::close_table(in_table_ptr);

  OPTION_VALUE_T explaination_values;
  std::string keystr;
  Utils::parse_json(exp_options, explaination_values, keystr, 0);
  assert(explaination_values.size());

  std::string model_content;
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    model_content = Loaded_models[model_handle_name];
    assert(model_content.length());
  }

  int importance_type{C_API_FEATURE_IMPORTANCE_SPLIT};
  MODEL_PREDICTION_EXP_T model_predict_type = MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE;
  switch (model_predict_type) {
    case MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE:
      importance_type = C_API_FEATURE_IMPORTANCE_SPLIT;
      break;
    case MODEL_PREDICTION_EXP_T::MODEL_SHAP:
    case MODEL_PREDICTION_EXP_T::MODEL_FAST_SHAP:
    case MODEL_PREDICTION_EXP_T::MODEL_PARTIAL_DEPENDENCE:
    case MODEL_PREDICTION_EXP_T::PREDICT_PERMUTATION_IMPORTANCE:
    case MODEL_PREDICTION_EXP_T::PREDICT_SHAP:
      importance_type = C_API_FEATURE_IMPORTANCE_GAIN;
      break;
    default:
      importance_type = C_API_FEATURE_IMPORTANCE_SPLIT;
  }

  BoosterHandle booster = nullptr;
  int num_iterations;
  if (LGBM_BoosterLoadModelFromString(model_content.c_str(), &num_iterations, &booster)) return HA_ERR_GENERIC;

  int num_features;
  if (LGBM_BoosterGetNumFeature(booster, &num_features)) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  std::vector<double> feature_importance(num_features);
  if (LGBM_BoosterFeatureImportance(booster, importance_type, 0, feature_importance.data())) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  LGBM_BoosterFree(booster);
  return 0;
}

int ML_classification::explain_row(THD *) { return 0; }
int ML_classification::explain_table(THD *) { return 0; }

int ML_classification::predict_row(THD * /*thd*/, Json_wrapper &input_data, std::string &model_handle_name,
                                   Json_wrapper &option, Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;
  if (!option.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "classification does not support option, set to null");
    return HA_ERR_GENERIC;
  }

  std::string keystr;
  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T meta_infos_names, input_values;
  if ((!input_data.empty() && Utils::parse_json(input_data, input_values, keystr, 0)) ||
      (!model_meta.empty() && Utils::parse_json(model_meta, meta_infos_names, keystr, 0))) {
    my_error(ER_ML_FAIL, MYF(0), "invalid input data or model meta info.");
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_infos_names[ML_KEYWORDS::column_names];
  if (feature_names.size() != input_values.size()) {
    my_error(ER_ML_FAIL, MYF(0), "input data column count does not match model feature count.");
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

  // FIX: use C_API_PREDICT_NORMAL instead of C_API_PREDICT_CONTRIB.
  // PREDICT_CONTRIB returns SHAP attribution values, not class probabilities.
  // PREDICT_NORMAL returns the model output (probability for binary, or
  // per-class probabilities for multiclass).
  std::vector<double> predictions;
  auto ret = Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);
  if (ret || predictions.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "ML_predict_row failed");
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  // For binary: predictions[0] is P(class=1); predicted class = round(predictions[0]).
  // For multiclass: predictions has one entry per class; predicted class = argmax.
  int predicted_class = 0;
  if (predictions.size() == 1) {
    predicted_class = (predictions[0] >= 0.5) ? 1 : 0;
  } else {
    predicted_class = static_cast<int>(std::max_element(predictions.begin(), predictions.end()) - predictions.begin());
  }

  // FIX: add Prediction exactly once, with the real predicted class value.
  root_obj->add_alias(ML_KEYWORDS::Prediction, new (std::nothrow) Json_int(predicted_class));

  // Build ml_results: predictions sub-object with the winning class label
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
  predictions_obj->add_alias(ML_KEYWORDS::kclass, new (std::nothrow) Json_int(predicted_class));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  // Build ml_results: probabilities sub-object
  Json_object *probabilities_obj = new (std::nothrow) Json_object();
  if (!probabilities_obj) {
    delete ml_results_obj;
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  if (predictions.size() == 1) {
    // Binary: two probabilities
    probabilities_obj->add_alias("0", new (std::nothrow) Json_double(1.0 - predictions[0]));
    probabilities_obj->add_alias("1", new (std::nothrow) Json_double(predictions[0]));
  } else {
    // Multiclass: one probability per class
    for (size_t i = 0; i < predictions.size(); ++i)
      probabilities_obj->add_alias(std::to_string(i), new (std::nothrow) Json_double(predictions[i]));
  }
  ml_results_obj->add_alias(ML_KEYWORDS::probabilities, probabilities_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return 0;
}

int ML_classification::predict_table(THD * /*thd*/, std::string &sch_tb_name, std::string &model_handle_name,
                                     std::string &out_sch_tb_name, Json_wrapper &options) {
  std::ostringstream err;
  std::string model_content;
  {
    std::lock_guard<std::mutex> lock(models_mutex);
    model_content = Loaded_models[model_handle_name];
    assert(model_content.length());
  }

  BoosterHandle booster = Utils::load_trained_model_from_string(model_content);
  if (!booster) return HA_ERR_GENERIC;

  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  OPTION_VALUE_T option_values, model_meta_option;
  std::string keystr;
  if (!options.empty() && Utils::parse_json(options, option_values, keystr, 0)) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }
  if (!model_meta.empty() && Utils::parse_json(model_meta, model_meta_option, keystr, 0)) {
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  auto remove_seen_str =
      (option_values[ML_KEYWORDS::remove_seen].size()) ? option_values[ML_KEYWORDS::remove_seen][0] : "true";
  std::transform(remove_seen_str.begin(), remove_seen_str.end(), remove_seen_str.begin(), ::toupper);
  auto remove_seen = (remove_seen_str == "TRUE");
  auto batch_size =
      (option_values[ML_KEYWORDS::batch_size].size()) ? std::stoi(option_values[ML_KEYWORDS::batch_size][0]) : 1000;
  if (batch_size < 1 || batch_size > 1000) {
    err << sch_tb_name << " wrong batch_size option";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  std::string schema_name, table_name;
  split_sch_table(sch_tb_name, schema_name, table_name);

  auto in_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!in_table_ptr) {
    err << sch_tb_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    LGBM_BoosterFree(booster);
    return HA_ERR_GENERIC;
  }

  if (remove_seen) {
    // TODO: remove the intersection of the input table and the train table.
  }
  Utils::close_table(in_table_ptr);

  std::string out_schema_name, out_table_name;
  split_sch_table(out_sch_tb_name, out_schema_name, out_table_name);

  LGBM_BoosterFree(booster);
  return 0;
}
}  // namespace ML
}  // namespace ShannonBase