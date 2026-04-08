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

#include "ml_anomaly_detection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>

#include "include/my_base.h"
#include "include/mysqld_error.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"

#include "ml_utils.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_anomaly_detection::SCORE_METRIC_T> ML_anomaly_detection::score_metrics = {
  {"ACCURACY",          ML_anomaly_detection::SCORE_METRIC_T::ACCURACY},
  {"BALANCED_ACCURACY", ML_anomaly_detection::SCORE_METRIC_T::BALANCED_ACCURACY},
  {"F1",                ML_anomaly_detection::SCORE_METRIC_T::F1},
  {"NEG_LOG_LOSS",      ML_anomaly_detection::SCORE_METRIC_T::NEG_LOG_LOSS},
  {"PRECISION",         ML_anomaly_detection::SCORE_METRIC_T::PRECISION},
  {"PRECISION_K",       ML_anomaly_detection::SCORE_METRIC_T::PRECISION_K},
  {"RECALL",            ML_anomaly_detection::SCORE_METRIC_T::RECALL},
  {"ROC_AUC",           ML_anomaly_detection::SCORE_METRIC_T::ROC_AUC}
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

/**
 * Strategy: use a statistical z-score approach to create synthetic labels.
 * For each sample we compute the mean absolute z-score across all features.
 * Samples whose score exceeds the threshold implied by `contamination` are
 * labelled 1 (anomaly); the rest are labelled 0 (normal).
 * The resulting binary labels let us train LightGBM as a binary classifier
 * whose output probability serves as an anomaly score at inference time.
 *
 * @param data         flat row-major feature matrix, n_samples × n_features
 * @param n_samples    number of samples
 * @param n_features   number of features
 * @param contamination expected fraction of anomalies (0 < contamination < 1)
 * @return             synthetic binary label vector (float, 0.0 or 1.0)
 */
static std::vector<float> generate_synthetic_labels(const std::vector<double> &data, size_t n_samples,
                                                    size_t n_features, float contamination) {
  if (n_samples == 0 || n_features == 0) return {};

  // Compute per-feature mean and standard deviation
  std::vector<double> feat_mean(n_features, 0.0), feat_std(n_features, 0.0);
  for (size_t s = 0; s < n_samples; ++s)
    for (size_t f = 0; f < n_features; ++f) feat_mean[f] += data[s * n_features + f];
  for (auto &m : feat_mean) m /= static_cast<double>(n_samples);

  for (size_t s = 0; s < n_samples; ++s)
    for (size_t f = 0; f < n_features; ++f) {
      double diff = data[s * n_features + f] - feat_mean[f];
      feat_std[f] += diff * diff;
    }
  for (size_t f = 0; f < n_features; ++f) {
    feat_std[f] = std::sqrt(feat_std[f] / static_cast<double>(n_samples));
    if (feat_std[f] < 1e-9) feat_std[f] = 1e-9;  // avoid division by zero
  }

  // Per-sample mean absolute z-score
  std::vector<double> scores(n_samples, 0.0);
  for (size_t s = 0; s < n_samples; ++s) {
    for (size_t f = 0; f < n_features; ++f)
      scores[s] += std::abs((data[s * n_features + f] - feat_mean[f]) / feat_std[f]);
    scores[s] /= static_cast<double>(n_features);
  }

  // Determine threshold: top `contamination` fraction → anomaly
  std::vector<double> sorted_scores = scores;
  std::sort(sorted_scores.begin(), sorted_scores.end());
  size_t threshold_idx = static_cast<size_t>((1.0 - contamination) * n_samples);
  if (threshold_idx >= n_samples) threshold_idx = n_samples - 1;
  double threshold = sorted_scores[threshold_idx];

  std::vector<float> labels(n_samples);
  for (size_t s = 0; s < n_samples; ++s) labels[s] = (scores[s] >= threshold) ? 1.0f : 0.0f;

  return labels;
}

// Score-metric helpers (binary classification style)
static double calc_accuracy(size_t n, const std::vector<double> &pred, const std::vector<float> &actual,
                            double threshold) {
  size_t correct = 0;
  for (size_t i = 0; i < n; ++i)
    if ((int)(pred[i] >= threshold) == (int)actual[i]) ++correct;
  return static_cast<double>(correct) / static_cast<double>(n);
}

static double calc_balanced_accuracy(size_t n, const std::vector<double> &pred, const std::vector<float> &actual,
                                     double threshold) {
  size_t tp = 0, tn = 0, fp = 0, fn = 0;
  for (size_t i = 0; i < n; ++i) {
    int p = (pred[i] >= threshold) ? 1 : 0;
    int a = (int)actual[i];
    if (a == 1 && p == 1)
      ++tp;
    else if (a == 0 && p == 0)
      ++tn;
    else if (a == 0 && p == 1)
      ++fp;
    else
      ++fn;
  }
  double sensitivity = (tp + fn > 0) ? (double)tp / (tp + fn) : 0.0;
  double specificity = (tn + fp > 0) ? (double)tn / (tn + fp) : 0.0;
  return 0.5 * (sensitivity + specificity);
}

static double calc_precision(size_t n, const std::vector<double> &pred, const std::vector<float> &actual,
                             double threshold) {
  size_t tp = 0, fp = 0;
  for (size_t i = 0; i < n; ++i) {
    int p = (pred[i] >= threshold) ? 1 : 0;
    if (p == 1) {
      if ((int)actual[i] == 1)
        ++tp;
      else
        ++fp;
    }
  }
  return (tp + fp > 0) ? (double)tp / (tp + fp) : 0.0;
}

static double calc_recall(size_t n, const std::vector<double> &pred, const std::vector<float> &actual,
                          double threshold) {
  size_t tp = 0, fn = 0;
  for (size_t i = 0; i < n; ++i) {
    int a = (int)actual[i];
    int p = (pred[i] >= threshold) ? 1 : 0;
    if (a == 1) {
      if (p == 1)
        ++tp;
      else
        ++fn;
    }
  }
  return (tp + fn > 0) ? (double)tp / (tp + fn) : 0.0;
}

static double calc_f1(size_t n, const std::vector<double> &pred, const std::vector<float> &actual, double threshold) {
  double prec = calc_precision(n, pred, actual, threshold);
  double rec = calc_recall(n, pred, actual, threshold);
  return (prec + rec > 0.0) ? 2.0 * prec * rec / (prec + rec) : 0.0;
}

static double calc_neg_log_loss(size_t n, const std::vector<double> &pred, const std::vector<float> &actual) {
  double loss = 0.0;
  constexpr double eps = 1e-15;
  for (size_t i = 0; i < n; ++i) {
    double p = std::max(eps, std::min(1.0 - eps, pred[i]));
    int a = (int)actual[i];
    loss += a * std::log(p) + (1 - a) * std::log(1.0 - p);
  }
  return -(loss / static_cast<double>(n));
}
}  // namespace

int ML_anomaly_detection::train(THD *, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  if (m_target_name.length()) {
    my_error(ER_ML_FAIL, MYF(0), "anomaly detection does not support a target column; set it to NULL");
    return HA_ERR_GENERIC;
  }

  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  double contamination = ML_anomaly_detection::default_contamination;
  if (options.find(ML_KEYWORDS::contamination) != options.end())
    contamination = std::stod(options[ML_KEYWORDS::contamination][0]);

  std::vector<std::string> model_list;
  if (options.find(ML_KEYWORDS::model_list) != options.end()) model_list = options[ML_KEYWORDS::model_list];

  bool semisupervised = false;
  int min_labels = 20, n_neighbors = 5;
  std::string ensemble_score = "f1";
  if (options.find("experimental") != options.end()) {
    std::string experimental_json = options["experimental"][0];
    auto dom = Json_dom::parse(
        experimental_json.c_str(), experimental_json.length(), [](const char *, size_t) { assert(false); },
        [] { assert(false); });
    if (dom) {
      Json_wrapper w(std::move(dom));
      Utils::parse_semisupervised_options(w, semisupervised, min_labels, n_neighbors, ensemble_score);
    }
  }

  if (!semisupervised && !m_target_name.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "unsupervised anomaly_detection requires target_column_name = NULL");
    return HA_ERR_GENERIC;
  }
  if (semisupervised && m_target_name.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "semi-supervised anomaly_detection requires a non-NULL target_column_name");
    return HA_ERR_GENERIC;
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

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  std::string target_name = semisupervised ? m_target_name : "";
  auto n_sample =
      Utils::read_data(source_table_ptr, train_data, features_name, target_name, label_data, n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  if (n_sample == 0 || features_name.empty()) {
    std::ostringstream err;
    err << "No data read from " << m_sch_name << "." << m_table_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto n_feature = features_name.size();

  // Generate synthetic labels for unsupervised training
  std::vector<float> synthetic_labels;
  if (!semisupervised) {
    synthetic_labels = generate_synthetic_labels(train_data, n_sample, n_feature, contamination);
    if (synthetic_labels.empty()) return HA_ERR_GENERIC;
  }

  std::ostringstream oss;
  // Use binary classification objective so output is a probability in [0,1]
  oss << "task=train boosting_type=gbdt objective=binary metric=binary_logloss"
      << " max_bin=255 num_trees=50 learning_rate=0.1"
      << " num_leaves=31 tree_learner=serial feature_fraction=0.8"
      << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=10"
      << " min_sum_hessian_in_leaf=1.0 is_enable_sparse=true";

  std::string model_content, mode_params(oss.str());

  std::vector<const char *> feature_names_cstr;
  for (const auto &name : features_name) feature_names_cstr.push_back(name.c_str());

  // Choose labels: synthetic for unsupervised, real labels for semi-supervised
  const float *label_ptr = semisupervised ? label_data.data() : synthetic_labels.data();
  if (!label_ptr && !semisupervised) {
    my_error(ER_ML_FAIL, MYF(0), "Failed to generate training labels");
    return HA_ERR_GENERIC;
  }

  // clang-format off
  auto start = std::chrono::steady_clock::now();
  if (Utils::ML_train(mode_params,
                      C_API_DTYPE_FLOAT64, train_data.data(), n_sample,
                      feature_names_cstr.data(), n_feature,
                      C_API_DTYPE_FLOAT32, label_ptr,
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

  auto meta_json = Utils::build_up_model_metadata(
      TASK_NAMES_MAP[type()], m_target_name, sch_tb_name, features_name, nullptr, notes,
      MODEL_FORMATS_MAP[MODEL_FORMAT_T::VER_1], MODEL_STATUS_MAP[MODEL_STATUS_T::READY],
      MODEL_QUALITIES_MAP[MODEL_QUALITY_T::HIGH], train_duration, TASK_NAMES_MAP[type()], 0, n_sample, n_feature,
      n_sample, n_feature, opt_metrics, features_name, contamination, &m_options, mode_params, nullptr, nullptr,
      nullptr, 1, txt2num_dict);

  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_anomaly_detection::load(THD *, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_anomaly_detection::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_anomaly_detection::unload(THD *, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_anomaly_detection::import(THD *, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_anomaly_detection::score(THD *, std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                                   std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric << " is invalid for anomaly detection scoring";
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

  // Validate labels: anomaly detection ground-truth must be 0 or 1
  auto bad_it = std::find_if(label_data.begin(), label_data.end(),
                             [](float v) { return static_cast<int>(v) != 0 && static_cast<int>(v) != 1; });
  if (bad_it != label_data.end()) {
    std::ostringstream err;
    err << "Column '" << target_name << "' in " << sch_tb_name << " must contain only 0 (normal) or 1 (anomaly)";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return 0.0;
  }

  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;

  // The model outputs probabilities in [0,1] (binary classifier).
  // Use contamination-aware threshold from options if present, else 0.5.
  double threshold = 0.5;
  if (option_keys.find(ML_KEYWORDS::threshold) != option_keys.end() && !option_keys[ML_KEYWORDS::threshold].empty())
    threshold = std::stod(option_keys[ML_KEYWORDS::threshold][0]);

  double score_val = 0.0;
  switch ((int)ML_anomaly_detection::score_metrics[metrics[0]]) {
    case (int)SCORE_METRIC_T::ACCURACY:
      score_val = calc_accuracy(n_sample, predictions, label_data, threshold);
      break;
    case (int)SCORE_METRIC_T::BALANCED_ACCURACY:
      score_val = calc_balanced_accuracy(n_sample, predictions, label_data, threshold);
      break;
    case (int)SCORE_METRIC_T::F1:
      score_val = calc_f1(n_sample, predictions, label_data, threshold);
      break;
    case (int)SCORE_METRIC_T::NEG_LOG_LOSS:
      score_val = calc_neg_log_loss(n_sample, predictions, label_data);
      break;
    case (int)SCORE_METRIC_T::PRECISION:
    case (int)SCORE_METRIC_T::PRECISION_K:
      score_val = calc_precision(n_sample, predictions, label_data, threshold);
      break;
    case (int)SCORE_METRIC_T::RECALL:
      score_val = calc_recall(n_sample, predictions, label_data, threshold);
      break;
    case (int)SCORE_METRIC_T::ROC_AUC: {
      // Trapezoidal AUC: sort by descending predicted score, sweep TP/FP curve
      size_t n_pos = 0, n_neg = 0;
      for (auto v : label_data) (v == 1.0f) ? ++n_pos : ++n_neg;
      if (n_pos == 0 || n_neg == 0) {
        score_val = 0.5;
        break;
      }
      std::vector<size_t> idx(n_sample);
      std::iota(idx.begin(), idx.end(), 0);
      std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return predictions[a] > predictions[b]; });
      double auc = 0.0;
      double cum_tp = 0.0, cum_fp = 0.0, prev_fp = 0.0, prev_tp = 0.0;
      for (size_t i : idx) {
        if ((int)label_data[i] == 1)
          cum_tp += 1.0;
        else
          cum_fp += 1.0;
        auc += (cum_fp - prev_fp) * (cum_tp + prev_tp) / 2.0;
        prev_fp = cum_fp;
        prev_tp = cum_tp;
      }
      score_val = auc / (n_pos * n_neg);
    } break;
    default:
      break;
  }
  return score_val;
}

int ML_anomaly_detection::explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) {
  my_error(ER_ML_FAIL, MYF(0), "anomaly_detection does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::explain_row(THD *) {
  my_error(ER_ML_FAIL, MYF(0), "anomaly_detection does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::explain_table(THD *) {
  my_error(ER_ML_FAIL, MYF(0), "anomaly_detection does not support explain operation");
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::predict_row(THD *, Json_wrapper &input_data, std::string &model_handle_name,
                                      Json_wrapper &option, Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;

  OPTION_VALUE_T options;
  std::string keystr;
  if (!option.empty() && Utils::parse_json(option, options, keystr, 0)) {
    my_error(ER_ML_FAIL, MYF(0), "anomaly_detection: failed to parse options");
    return HA_ERR_GENERIC;
  }

  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T meta_infos_model, input_values;
  if ((!input_data.empty() && Utils::parse_json(input_data, input_values, keystr, 0)) ||
      (!model_meta.empty() && Utils::parse_json(model_meta, meta_infos_model, keystr, 0))) {
    my_error(ER_ML_FAIL, MYF(0), "invalid input data or model meta info.");
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_infos_model[ML_KEYWORDS::column_names];
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

  // Determine anomaly threshold: use option if provided, else use default contamination
  double anomaly_threshold = 1.0 - ML_anomaly_detection::default_contamination;
  if (!options.empty() && options.find(ML_KEYWORDS::threshold) != options.end() &&
      !options[ML_KEYWORDS::threshold].empty())
    anomaly_threshold = std::stod(options[ML_KEYWORDS::threshold][0]);

  auto *root_obj = new (std::nothrow) Json_object();
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
    my_error(ER_ML_FAIL, MYF(0), "ML_predict_row failed for anomaly detection");
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  double anomaly_prob = predictions[0];     // P(anomaly)
  double normal_prob = 1.0 - anomaly_prob;  // P(normal)
  int is_anomaly = (anomaly_prob >= anomaly_threshold) ? 1 : 0;

  // Build ml_results JSON
  Json_object *ml_results_obj = new (std::nothrow) Json_object();
  if (!ml_results_obj) {
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  Json_object *probabilities_obj = new (std::nothrow) Json_object();
  if (!probabilities_obj) {
    delete ml_results_obj;
    delete root_obj;
    return HA_ERR_GENERIC;
  }
  probabilities_obj->add_alias(ML_KEYWORDS::normal, new (std::nothrow) Json_double(normal_prob));
  probabilities_obj->add_alias(ML_KEYWORDS::anomaly, new (std::nothrow) Json_double(anomaly_prob));
  ml_results_obj->add_alias(ML_KEYWORDS::probabilities, probabilities_obj);

  Json_object *prediction_obj = new (std::nothrow) Json_object();
  if (!prediction_obj) {
    delete ml_results_obj;
    delete root_obj;
    return HA_ERR_GENERIC;
  }
  prediction_obj->add_alias(ML_KEYWORDS::is_anomaly, new (std::nothrow) Json_int(is_anomaly));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, prediction_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return 0;
}

int ML_anomaly_detection::predict_table(THD * /*thd*/, std::string & /*sch_tb_name*/,
                                        std::string & /*model_handle_name*/, std::string & /*out_sch_tb_name*/,
                                        Json_wrapper & /*options*/) {
  return 0;
}

ML_TASK_TYPE_T ML_anomaly_detection::type() { return ML_TASK_TYPE_T::ANOMALY_DETECTION; }
}  // namespace ML
}  // namespace ShannonBase