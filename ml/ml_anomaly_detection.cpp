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

#include "include/my_base.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"

#include "ml_utils.h"

namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string, ML_anomaly_detection::SCORE_METRIC_T> ML_anomaly_detection::score_metrics = {
  {"ACCURACY", ML_anomaly_detection::SCORE_METRIC_T::ACCURACY},
  {"BALANCED_ACCURACY", ML_anomaly_detection::SCORE_METRIC_T::BALANCED_ACCURACY},
  {"F1", ML_anomaly_detection::SCORE_METRIC_T::F1},
  {"NEG_LOG_LOSS", ML_anomaly_detection::SCORE_METRIC_T::NEG_LOG_LOSS},
  {"PRECISION", ML_anomaly_detection::SCORE_METRIC_T::PRECISION},
  {"PRECISION_K", ML_anomaly_detection::SCORE_METRIC_T::PRECISION_K},
  {"RECALL", ML_anomaly_detection::SCORE_METRIC_T::RECALL},
  {"ROC_AUC", ML_anomaly_detection::SCORE_METRIC_T::ROC_AUC}
};
// clang-format on

ML_anomaly_detection::ML_anomaly_detection() {}

ML_anomaly_detection::~ML_anomaly_detection() {}

int ML_anomaly_detection::train(THD *, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  if (m_target_name.length()) {
    std::ostringstream err;
    err << "anomaly detection does not support target column, must be set to NULL";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  auto contamination = ML_anomaly_detection::default_contamination;
  if (options.find(ML_KEYWORDS::contamination) != options.end())
    contamination = std::stof(options[ML_KEYWORDS::contamination][0]);

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
  std::string unsupervised_target{""};
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name,
                                   unsupervised_target /*unsupervised mode*/, label_data, n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  auto n_feature = features_name.size();
  std::ostringstream oss;

  // Anomaly detection using regression objective for reconstruction error approach
  // or use a simpler parameter set suitable for unsupervised learning
  oss << "task=train boosting_type=gbdt objective=regression metric=rmse"
      << " max_bin=255 num_trees=50 learning_rate=0.1"
      << " num_leaves=31 tree_learner=serial feature_fraction=0.8"
      << " bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=10"
      << " min_sum_hessian_in_leaf=1.0 is_enable_sparse=true";

  std::string model_content, mode_params(oss.str().c_str());

  std::vector<const char *> feature_names_cstr;
  for (const auto &name : features_name) {
    feature_names_cstr.push_back(name.c_str());
  }

  // clang-format off
  auto start = std::chrono::steady_clock::now();
  // For anomaly detection without labels, we might pass nullptr for label_data
  // or use a synthetic target (like reconstruction error approach)
  if (Utils::ML_train(mode_params,
                      C_API_DTYPE_FLOAT64,
                      train_data.data(),
                      n_sample,
                      feature_names_cstr.data(),
                      n_feature,
                      C_API_DTYPE_FLOAT32,
                      label_data.empty() ? nullptr : label_data.data(),
                      model_content))
    return HA_ERR_GENERIC;
  auto end = std::chrono::steady_clock::now();
  auto train_duration =
    std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f;

  // the definition of this table, ref: `ml_train.sql`
  oss.clear();
  oss.str("");
  oss << m_sch_name <<  "."  << m_table_name;
  std::string sch_tb_name (oss.str().c_str()), notes, opt_metrics;

  auto content_dom = Json_dom::parse(model_content.c_str(),
                             model_content.length(),
                             [](const char *, size_t) { assert(false); },
                             [] { assert(false); });
  if (!content_dom.get()) return HA_ERR_GENERIC;
  model_object = Json_wrapper(std::move(content_dom));

  auto meta_json = Utils::build_up_model_metadata(TASK_NAMES_MAP[type()],  /* task */
                                                m_target_name,  /*labelled col name */
                                                sch_tb_name,    /* trained table */
                                                features_name,  /* feature columns*/
                                                nullptr,        /* model explanation*/
                                                notes,          /* notes*/
                                                MODEL_FORMATS_MAP[MODEL_FORMAT_T::VER_1],   /* model format*/
                                                MODEL_STATUS_MAP[MODEL_STATUS_T::READY],   /* model_status */
                                                MODEL_QUALITIES_MAP[MODEL_QUALITY_T::HIGH],  /* model_qulity */
                                                train_duration,  /*the time in seconds taken to train the model.*/
                                                TASK_NAMES_MAP[type()], /**task algo name */
                                                0,              /*train score*/
                                                n_sample,       /*# of rows in training tbl*/
                                                n_feature,      /*# of columns in training tbl (no target)*/
                                                n_sample,       /*# of rows selected by adaptive sampling*/
                                                n_feature,      /*# of columns selected by feature selection.*/
                                                opt_metrics,    /* optimization metric */
                                                features_name,  /* names of the columns selected by feature selection*/
                                                contamination,  /*contamination*/
                                                &m_options,     /*options of ml_train*/
                                                mode_params,    /**training_params */
                                                nullptr,        /**onnx_inputs_info */
                                                nullptr,        /*onnx_outputs_info*/
                                                nullptr,        /*training_drift_metric*/
                                                1               /* chunks */,
                                                txt2num_dict   /* txt2numeric dict */
                                              );

  // clang-format on
  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_anomaly_detection::load(THD *, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_anomaly_detection::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

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
  // all logical done in ml_model_import stored procedure.
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
      err << metric_str << " is invalid for anomaly detection scoring";
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

  // For ML_SCORE the target_column_name column must only contain the anomaly scores as an integer:
  // 1: an anomaly or 0 normal.
  auto it = std::find_if(label_data.begin(), label_data.end(), [](float value) {
    return static_cast<int>(value) != 0 && static_cast<int>(value) != 1;  // values other than 0 or 1
  });
  if (it != label_data.end()) {
    std::ostringstream err;
    err << sch_tb_name << "the " << target_name << " contains a value other than 0 or 1";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return 0.0;
  }

  // gets the prediction values.
  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;
  double score{0.0};
  switch ((int)ML_anomaly_detection::score_metrics[metrics[0]]) {
    case (int)SCORE_METRIC_T::ACCURACY:
      break;
    case (int)SCORE_METRIC_T::BALANCED_ACCURACY:
      break;
    case (int)SCORE_METRIC_T::F1:
      break;
    case (int)SCORE_METRIC_T::NEG_LOG_LOSS:
      break;
    case (int)SCORE_METRIC_T::PRECISION:
      break;
    case (int)SCORE_METRIC_T::PRECISION_K:
      break;
    case (int)SCORE_METRIC_T::RECALL:
      break;
    case (int)SCORE_METRIC_T::ROC_AUC:
      break;
    default:
      break;
  }

  return score;
}

int ML_anomaly_detection::explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) {
  std::ostringstream err;
  err << "anomaly_detection does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::explain_row(THD *) {
  std::ostringstream err;
  err << "anomaly_detection does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::explain_table(THD *) {
  std::ostringstream err;
  err << "anomaly_detection does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_anomaly_detection::predict_row(THD *, Json_wrapper &input_data, std::string &model_handle_name,
                                      Json_wrapper &option, Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;
  if (!option.empty()) {
    err << "classification does not support option, set to null";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::string keystr;
  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T meta_infos_model, input_values, options;
  if ((!input_data.empty() && Utils::parse_json(input_data, input_values, keystr, 0)) ||
      (!model_meta.empty() && Utils::parse_json(model_meta, meta_infos_model, keystr, 0))) {
    err << "invalid input data or model meta info.";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_infos_model[ML_KEYWORDS::column_names];
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

  if (!option.empty() && Utils::parse_json(option, options, keystr, 0)) return HA_ERR_GENERIC;
  auto threshold [[maybe_unused]] = (options[ML_KEYWORDS::threshold].size())
                                        ? std::stod(options[ML_KEYWORDS::threshold][0])
                                        : (1 - ML_anomaly_detection::default_contamination);

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

  std::vector<double> predictions;
  auto ret = Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);
  if (ret) {
    err << "call ML_PREDICT_ROW failed";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // here, stat to calc the prob of normal and anomaly probabilities.
  auto normal_prob{0.0}, anomaly_prob{0.0}, anomaly_threshold{0.0};
  auto is_anomaly = (anomaly_prob > anomaly_threshold) ? 1 : 0;

  // ml_results
  Json_object *ml_results_obj = new (std::nothrow) Json_object();
  if (ml_results_obj == nullptr) {
    return HA_ERR_GENERIC;
  }

  Json_object *probabilities_obj = new (std::nothrow) Json_object();
  if (probabilities_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  probabilities_obj->add_alias(ML_KEYWORDS::normal, new (std::nothrow) Json_double(normal_prob));
  probabilities_obj->add_alias(ML_KEYWORDS::anomaly, new (std::nothrow) Json_double(anomaly_prob));
  ml_results_obj->add_alias(ML_KEYWORDS::probabilities, probabilities_obj);

  Json_object *prediction_obj = new (std::nothrow) Json_object();
  if (prediction_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  prediction_obj->add_alias(ML_KEYWORDS::is_anomaly, new (std::nothrow) Json_int(is_anomaly));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, prediction_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return ret;
}

int ML_anomaly_detection::predict_table(THD *thd [[maybe_unused]], std::string &sch_tb_name [[maybe_unused]],
                                        std::string &model_handle_name [[maybe_unused]],
                                        std::string &out_sch_tb_name [[maybe_unused]],
                                        Json_wrapper &options [[maybe_unused]]) {
  return 0;
}

ML_TASK_TYPE_T ML_anomaly_detection::type() { return ML_TASK_TYPE_T::ANOMALY_DETECTION; }

}  // namespace ML
}  // namespace ShannonBase