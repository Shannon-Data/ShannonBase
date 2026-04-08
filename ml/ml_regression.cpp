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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>

#include "include/my_inttypes.h"
#include "include/mysqld_error.h"
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
#include "ml_utils.h"
#include "storage/innobase/include/ut0dbg.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_regression::SCORE_METRIC_T> ML_regression::score_metrics = {
  {"NEG_MEAN_ABSOLUTE_ERROR",    ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_SQUARED_ERROR",     ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_MEAN_SQUARED_LOG_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR},
  {"NEG_MEDIAN_ABSOLUTE_ERROR",  ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR},
  {"R2",                         ML_regression::SCORE_METRIC_T::R2}
};
// clang-format on

namespace {
double calc_neg_mae(const std::vector<double> &pred, const std::vector<float> &actual) {
  assert(pred.size() == actual.size());
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) sum += std::abs(pred[i] - actual[i]);
  return -(sum / static_cast<double>(pred.size()));
}

double calc_neg_mse(const std::vector<double> &pred, const std::vector<float> &actual) {
  assert(pred.size() == actual.size());
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) {
    double diff = pred[i] - actual[i];
    sum += diff * diff;
  }
  return -(sum / static_cast<double>(pred.size()));
}

double calc_neg_msle(const std::vector<double> &pred, const std::vector<float> &actual) {
  assert(pred.size() == actual.size());
  double sum = 0.0;
  for (size_t i = 0; i < pred.size(); ++i) {
    // Guard against negative predictions (MSLE is only defined for non-negative values)
    double p = std::max(0.0, pred[i]);
    double a = std::max(0.0, static_cast<double>(actual[i]));
    double diff = std::log1p(p) - std::log1p(a);
    sum += diff * diff;
  }
  return -(sum / static_cast<double>(pred.size()));
}

double calc_neg_median_ae(const std::vector<double> &pred, const std::vector<float> &actual) {
  assert(pred.size() == actual.size());
  std::vector<double> abs_errors(pred.size());
  for (size_t i = 0; i < pred.size(); ++i) abs_errors[i] = std::abs(pred[i] - actual[i]);
  std::sort(abs_errors.begin(), abs_errors.end());
  size_t n = abs_errors.size();
  double median = (n % 2 == 0) ? 0.5 * (abs_errors[n / 2 - 1] + abs_errors[n / 2]) : abs_errors[n / 2];
  return -median;
}

double calc_r2(const std::vector<double> &pred, const std::vector<float> &actual) {
  assert(pred.size() == actual.size());
  size_t n = pred.size();
  double mean_actual = 0.0;
  for (auto v : actual) mean_actual += v;
  mean_actual /= static_cast<double>(n);

  double ss_tot = 0.0, ss_res = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double diff_tot = actual[i] - mean_actual;
    double diff_res = actual[i] - pred[i];
    ss_tot += diff_tot * diff_tot;
    ss_res += diff_res * diff_res;
  }
  if (ss_tot == 0.0) return 1.0;  // Perfect constant prediction
  return 1.0 - (ss_res / ss_tot);
}

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
}  // anonymous namespace

int ML_regression::train(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  assert(thd);
  std::string user_name(thd->security_context()->user().str);

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
  std::string metric_str = (optimization_metric.empty()) ? "rmse" : Utils::METRIC_MAP.at(optimization_metric);
  oss << "task=train boosting_type=gbdt objective=regression metric=" << metric_str
      << " metric_freq=1 is_training_metric=true num_trees=100 learning_rate=0.1"
      << " num_leaves=63 tree_learner=serial feature_fraction=0.8 max_bin=255"
      << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=50"
      << " min_sum_hessian_in_leaf=5.0 is_enable_sparse=true use_two_round_loading=false";

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

int ML_regression::load(THD *, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_regression::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_regression::unload(THD *, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_regression::import(THD *, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_regression::score(THD *, std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                            std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric << " is invalid for regression scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }

  OPTION_VALUE_T option_keys;
  std::string strkey;
  if (!option.empty() && Utils::parse_json(option, option_keys, strkey, 0)) return 0.0;

  // FIX: use correct split helper instead of manual pointer arithmetic
  std::string schema_name, table_name;
  split_sch_table(sch_tb_name, schema_name, table_name);

  auto source_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << sch_tb_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return 0.0;
  }

  TableGuard tg(source_table_ptr);
  std::vector<double> test_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2numeric;
  auto n_sample = Utils::read_data(tg.get(), test_data, features_name, target_name, label_data, n_class, txt2numeric);
  if (!n_sample) return 0.0;

  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;

  double score_val = 0.0;
  switch ((int)ML_regression::score_metrics[metrics[0]]) {
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      score_val = calc_neg_mae(predictions, label_data);
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      score_val = calc_neg_mse(predictions, label_data);
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR:
      score_val = calc_neg_msle(predictions, label_data);
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR:
      score_val = calc_neg_median_ae(predictions, label_data);
      break;
    case (int)ML_regression::SCORE_METRIC_T::R2:
      score_val = calc_r2(predictions, label_data);
      break;
    default:
      break;
  }
  return score_val;
}

int ML_regression::explain(THD *, std::string & /*sch_tb_name*/, std::string & /*target_column_name*/,
                           std::string & /*model_handle_name*/, Json_wrapper & /*exp_options*/) {
  return 0;
}
int ML_regression::explain_row(THD *) { return 0; }
int ML_regression::explain_table(THD *) { return 0; }

int ML_regression::predict_row(THD *, Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
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
    my_error(ER_ML_FAIL, MYF(0), "invalid input data or model meta info.");
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_feature_names[ML_KEYWORDS::column_names];
  if (feature_names.size() != input_values.size()) {
    my_error(ER_ML_FAIL, MYF(0), "input data column count does not match the model feature count.");
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
    my_error(ER_ML_FAIL, MYF(0), "ML_predict_row failed");
    delete root_obj;
    return HA_ERR_GENERIC;
  }

  double predicted_value = predictions[0];
  root_obj->add_alias(ML_KEYWORDS::Prediction, new (std::nothrow) Json_double(predicted_value));

  // Build ml_results sub-object
  std::string target_name = "value";
  if (meta_feature_names.find(ML_KEYWORDS::target_column_name) != meta_feature_names.end() &&
      !meta_feature_names[ML_KEYWORDS::target_column_name].empty()) {
    target_name = meta_feature_names[ML_KEYWORDS::target_column_name][0];
  }

  Json_object *predictions_obj = new (std::nothrow) Json_object();
  if (!predictions_obj) {
    delete root_obj;
    return HA_ERR_GENERIC;
  }
  predictions_obj->add_alias(target_name, new (std::nothrow) Json_double(predicted_value));

  Json_object *ml_results_obj = new (std::nothrow) Json_object();
  if (!ml_results_obj) {
    delete predictions_obj;
    delete root_obj;
    return HA_ERR_GENERIC;
  }
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return 0;
}

int ML_regression::predict_table(THD *, std::string &sch_tb_name, std::string &model_handle_name,
                                 std::string &out_sch_tb_name, Json_wrapper &options) {
  std::ostringstream err;
  std::string keystr;
  OPTION_VALUE_T parsed_options;

  if (!options.empty() && Utils::parse_json(options, parsed_options, keystr, 0)) {
    my_error(ER_ML_FAIL, MYF(0), "Failed to parse options JSON");
    return HA_ERR_GENERIC;
  }

  std::string in_schema_name, in_table_name;
  split_sch_table(sch_tb_name, in_schema_name, in_table_name);

  auto input_table_ptr = Utils::open_table_by_name(in_schema_name, in_table_name, TL_READ);
  if (!input_table_ptr) {
    err << "Failed to open input table: " << sch_tb_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  TableGuard in_tg(input_table_ptr);

  std::string out_schema_name, out_table_name;
  split_sch_table(out_sch_tb_name, out_schema_name, out_table_name);

  auto output_table_ptr = Utils::open_table_by_name(out_schema_name, out_table_name, TL_WRITE);
  if (!output_table_ptr) {
    err << "Failed to open output table: " << out_sch_tb_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  TableGuard out_tg(output_table_ptr);

  // Read model metadata
  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) {
    err << "Failed to read model metadata for: " << model_handle_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  OPTION_VALUE_T meta_info;
  if (!model_meta.empty() && Utils::parse_json(model_meta, meta_info, keystr, 0)) {
    err << "Failed to parse model metadata";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_info[ML_KEYWORDS::column_names];
  if (feature_names.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "Model metadata missing feature column names");
    return HA_ERR_GENERIC;
  }

  txt2numeric_map_t txt2numeric;
  if (Utils::get_txt2num_dict(model_meta, txt2numeric)) {
    my_error(ER_ML_FAIL, MYF(0), "Failed to get txt2num dict from model");
    return HA_ERR_GENERIC;
  }

  // Map feature names to input table field indices
  std::vector<uint> feature_field_indices;
  for (const auto &feature_name : feature_names) {
    bool found = false;
    for (uint i = 0; i < in_tg.get()->s->fields; i++) {
      if (strcmp(in_tg.get()->field[i]->field_name, feature_name.c_str()) == 0) {
        feature_field_indices.push_back(i);
        found = true;
        break;
      }
    }
    if (!found) {
      err << "Input table missing required feature column: " << feature_name;
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  // Locate output-table columns by name
  int prediction_field_idx = -1;
  int ml_results_field_idx = -1;
  for (uint i = 0; i < out_tg.get()->s->fields; i++) {
    const char *fn = out_tg.get()->field[i]->field_name;
    if (strcmp(fn, "prediction") == 0) prediction_field_idx = (int)i;
    if (strcmp(fn, "ml_results") == 0) ml_results_field_idx = (int)i;
  }
  if (prediction_field_idx == -1 || ml_results_field_idx == -1) {
    my_error(ER_ML_FAIL, MYF(0), "Output table missing required 'prediction' or 'ml_results' column");
    return HA_ERR_GENERIC;
  }

  // Build a map: input field name → input field index, for field-by-field copy
  std::unordered_map<std::string, uint> in_field_map;
  for (uint i = 0; i < in_tg.get()->s->fields; i++) in_field_map[in_tg.get()->field[i]->field_name] = i;

  if (in_tg.get()->file->ha_rnd_init(true)) {
    my_error(ER_ML_FAIL, MYF(0), "Failed to initialize table scan");
    return HA_ERR_GENERIC;
  }

  std::string target_name = "value";
  if (meta_info.find(ML_KEYWORDS::target_column_name) != meta_info.end() &&
      !meta_info[ML_KEYWORDS::target_column_name].empty())
    target_name = meta_info[ML_KEYWORDS::target_column_name][0];

  int error_rows = 0;
  int read_result;

  while ((read_result = in_tg.get()->file->ha_rnd_next(in_tg.get()->record[0])) == 0) {
    // Build sample from current input row
    std::vector<ml_record_type_t> sample_data;
    for (size_t i = 0; i < feature_names.size(); i++) {
      Field *field = in_tg.get()->field[feature_field_indices[i]];
      std::string value = "0";
      if (!field->is_null()) {
        String str_val;
        field->val_str(&str_val);
        value = str_val.c_ptr_safe();
      }
      sample_data.push_back({feature_names[i], value});
    }

    std::vector<double> preds;
    if (Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, preds) ||
        preds.empty()) {
      error_rows++;
      continue;
    }
    double predicted_value = preds[0];

    memset(out_tg.get()->record[0], 0, out_tg.get()->s->reclength);

    // Copy matching input fields into the output table
    for (uint oi = 0; oi < out_tg.get()->s->fields; oi++) {
      const char *out_fn = out_tg.get()->field[oi]->field_name;
      if (strcmp(out_fn, "prediction") == 0 || strcmp(out_fn, "ml_results") == 0) continue;
      auto it = in_field_map.find(out_fn);
      if (it == in_field_map.end()) continue;
      Field *src = in_tg.get()->field[it->second];
      Field *dst = out_tg.get()->field[oi];
      if (src->is_null()) {
        dst->set_null();
      } else {
        dst->set_notnull();
        String tmp;
        src->val_str(&tmp);
        dst->store(tmp.ptr(), tmp.length(), tmp.charset());
      }
    }

    // Write prediction column
    {
      std::string pred_str = std::to_string(predicted_value);
      Field *pred_field = out_tg.get()->field[prediction_field_idx];
      pred_field->set_notnull();
      pred_field->store(pred_str.c_str(), pred_str.length(), &my_charset_utf8mb4_0900_ai_ci);
    }

    // Write ml_results JSON column
    {
      Json_object *ml_results_obj = new (std::nothrow) Json_object();
      if (ml_results_obj) {
        Json_object *preds_obj = new (std::nothrow) Json_object();
        if (preds_obj) {
          preds_obj->add_alias(target_name, new (std::nothrow) Json_double(predicted_value));
          ml_results_obj->add_alias(ML_KEYWORDS::predictions, preds_obj);
        }
        Json_wrapper wrapper(ml_results_obj);
        String json_str;
        wrapper.to_string(&json_str, false, "predict_table", [] { assert(false); });
        Field *ml_field = out_tg.get()->field[ml_results_field_idx];
        ml_field->set_notnull();
        ml_field->store(json_str.ptr(), json_str.length(), &my_charset_utf8mb4_0900_ai_ci);
      }
    }

    if (out_tg.get()->file->ha_write_row(out_tg.get()->record[0])) {
      error_rows++;
    }
  }

  in_tg.get()->file->ha_rnd_end();

  if (read_result != HA_ERR_END_OF_FILE) {
    my_error(ER_ML_FAIL, MYF(0), "Error reading input table rows");
    return HA_ERR_GENERIC;
  }

  return (error_rows > 0) ? HA_ERR_GENERIC : 0;
}
ML_TASK_TYPE_T ML_regression::type() { return ML_TASK_TYPE_T::REGRESSION; }
}  // namespace ML
}  // namespace ShannonBase