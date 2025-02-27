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

#include <iostream>
#include <memory>

//#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"  //LightGBM

#include "include/my_inttypes.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"
#include "storage/innobase/include/ut0dbg.h"  //for ut_a

#include "auto_ml.h"
#include "ml_info.h"
#include "ml_utils.h"  //ml utils

namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string, ML_classification::SCORE_METRIC_T> ML_classification::score_metrics =
{
  {"ACCURACY", ML_classification::SCORE_METRIC_T::ACCURACY},
  {"BALANCED_ACCURACY", ML_classification::SCORE_METRIC_T::BALANCED_ACCURACY},
  {"F1", ML_classification::SCORE_METRIC_T::F1},
  {"F1_MACRO", ML_classification::SCORE_METRIC_T::F1_MACRO},
  {"F1_MICRO", ML_classification::SCORE_METRIC_T::F1_MICRO},
  {"F1_SAMPLES", ML_classification::SCORE_METRIC_T::F1_SAMPLES},
  {"F1_WEIGTHED", ML_classification::SCORE_METRIC_T::F1_WEIGTHED},
  {"NEG_LOG_LOSS", ML_classification::SCORE_METRIC_T::NEG_LOG_LOSS},
  {"PRECISION", ML_classification::SCORE_METRIC_T::PRECISION},
  {"PRECISION_MACRO", ML_classification::SCORE_METRIC_T::PRECISION_MACRO},
  {"PRECISION_MICRO", ML_classification::SCORE_METRIC_T::PRECISION_MICRO},
  {"PRECISION_SAMPLES", ML_classification::SCORE_METRIC_T::PRECISION_SAMPLES},
  {"PRECISION_WEIGHTED", ML_classification::SCORE_METRIC_T::PRECISION_WEIGHTED},
  {"RECALL", ML_classification::SCORE_METRIC_T::RECALL},
  {"RECALL_MACRO", ML_classification::SCORE_METRIC_T::RECALL_MACRO},
  {"RECALL_MICRO", ML_classification::SCORE_METRIC_T::RECALL_MICRO},
  {"RECALL_SAMPLES", ML_classification::SCORE_METRIC_T::RECALL_SAMPLES},
  {"RECALL_WEIGHTED", ML_classification::SCORE_METRIC_T::RECALL_WEIGHTED},
  {"ROC_AUC", ML_classification::SCORE_METRIC_T::ROC_AUC}
};
// clang-format off

ML_classification::ML_classification() {}
ML_classification::~ML_classification() {}

ML_TASK_TYPE_T ML_classification::type() { return ML_TASK_TYPE_T::CLASSIFICATION; }

MODEL_PREDICTION_EXP_T ML_classification::parse_option(Json_wrapper &options) {
  MODEL_PREDICTION_EXP_T explainer_type{MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE};
  auto dom_ptr = options.clone_dom();
  if (!dom_ptr) return explainer_type;

  Json_object *json_obj = down_cast<Json_object *>(dom_ptr.get());
  Json_dom *value_dom_ptr{nullptr};

  value_dom_ptr = json_obj->get("model_explainer");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    auto exp_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(exp_type_str.begin(), exp_type_str.end(), exp_type_str.begin(), ::toupper);
    exp_type_str = "MODEL_" + exp_type_str;
    explainer_type = MODEL_EXPLAINERS_MAP[exp_type_str];
  }

  value_dom_ptr = json_obj->get("prediction_explainer");
  if (value_dom_ptr && value_dom_ptr->json_type() == enum_json_type::J_STRING) {
    auto exp_type_str = down_cast<Json_string *>(value_dom_ptr)->value();
    std::transform(exp_type_str.begin(), exp_type_str.end(), exp_type_str.begin(), ::toupper);
    exp_type_str = "PREDICT_" + exp_type_str;
    explainer_type = MODEL_EXPLAINERS_MAP[exp_type_str];
  }

  return explainer_type;
}

int ML_classification::get_txt2num_dict(Json_wrapper&input, std::string& key, txt2numeric_map_t& txt2num_dict) {
  MYSQL_LEX_CSTRING lex_key;
  lex_key.str = key.c_str();
  lex_key.length = key.length();
  auto result = input.lookup(lex_key);
  assert(!result.empty());
  if (result.type() != enum_json_type::J_OBJECT) return HA_ERR_GENERIC;

  OPTION_VALUE_T dict_opt;
  std::string strkey;
  if (Utils::parse_json(result, dict_opt, strkey, 0)) return HA_ERR_GENERIC;
  for (auto& it : dict_opt) {
    auto name = it.first;
    std::set<std::string> val_set;
    for (auto val_str : it.second) {
      val_set.insert(val_str);
    }
    txt2num_dict.insert({name, val_set});
  }
  return 0;
}

int ML_classification::train() {
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
  int n_class {0};
  txt2numeric_map_t txt2num_dict;
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name, target_names[0], label_data, 
                            n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = features_name.size();
  std::ostringstream oss;
  if (n_class <= 2) { // binary-classification. pay attention to the format of params
    oss << "task=train boosting_type=gbdt objective=binary metric_freq=1" <<
            " is_training_metric=true num_trees=100 learning_rate=0.1 num_leaves=63 tree_learner=serial"<<
            " feature_fraction=0.8 max_bin=255 bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=50" <<
            " min_sum_hessian_in_leaf=5.0 is_enable_sparse=true use_two_round_loading=false";
  } else { //multi classification
    oss << "task=train boosting_type=gbdt objective=multiclass" <<
           " num_class=" << n_class << " metric_freq=1 is_training_metric=true max_bin=255" <<
           " early_stopping=10 num_trees=100 learning_rate=0.05 num_leaves=31";
  }
  std::string model_content, mode_params(oss.str().c_str());

  std::vector<const char*> feature_names_cstr;
  for (const auto& name : features_name) {
      feature_names_cstr.push_back(name.c_str());
  }  
  // clang-format off
  auto start = std::chrono::steady_clock::now();
  if (Utils::ML_train(mode_params,
                      C_API_DTYPE_FLOAT64,
                      train_data.data(),
                      n_sample,
                      feature_names_cstr.data(),
                      n_feature,
                      C_API_DTYPE_FLOAT32,
                      label_data.data(),
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
  Json_wrapper content_json(content_dom.get(), true);

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
                                                n_feature + 1,  /*# of columns in training tbl*/
                                                n_sample,       /*# of rows selected by adaptive sampling*/
                                                n_feature,      /*# of columns selected by feature selection.*/
                                                opt_metrics,    /* optimization metric */
                                                features_name,  /* names of the columns selected by feature selection*/
                                                0,              /*contamination*/
                                                &m_options,     /*options of ml_train*/
                                                mode_params,    /**training_params */
                                                nullptr,        /**onnx_inputs_info */
                                                nullptr,        /*onnx_outputs_info*/
                                                nullptr,        /*training_drift_metric*/
                                                1               /* chunks */,
                                                txt2num_dict   /* txt2numeric dict */
                                              );

  if (!meta_json)
    return HA_ERR_GENERIC;
  Json_wrapper model_meta(meta_json);
  if (Utils::store_model_catalog(model_content.length(),
                                 &model_meta,
                                 m_handler_name))
    return HA_ERR_GENERIC;

  if (Utils::store_model_object_catalog(m_handler_name, &content_json))
    return HA_ERR_GENERIC;
  // clang-format on
  return 0;
}

// load the model from model_content.
int ML_classification::load(std::string &model_content) {
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_classification::load_from_file(std::string &model_file_full_path, std::string &model_handle_name) {
  if (!model_file_full_path.length() || !model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_classification::unload(std::string &model_handle_name) {
  assert(!Loaded_models.empty());

  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return 0;
}

int ML_classification::import(Json_wrapper &, Json_wrapper &, std::string &) {
  // all logical done in ml_model_import stored procedure.
  assert(false);
  return 0;
}

double ML_classification::score(std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                                std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  if (!option.empty()) {
    std::ostringstream err;
    err << "option params should be null for classification";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return 0.0;
  }

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric_str << " is invalid for classification scoring";
      my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
      return 0.0;
    }
  }

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

  std::vector<double> predictions;
  if (Utils::model_score(model_handle, n_sample, features_name.size(), test_data, predictions)) return 0.0;
  double score{0.0};
  switch ((int)ML_classification::score_metrics[metrics[0]]) {
    case (int)ML_classification::SCORE_METRIC_T::ACCURACY:
      score = Utils::calculate_accuracy(n_sample, predictions, label_data);
      break;
    case (int)ML_classification::SCORE_METRIC_T::BALANCED_ACCURACY:
      score = Utils::calculate_balanced_accuracy(n_sample, predictions, label_data);
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1:
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1_MACRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1_MICRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1_SAMPLES:
      break;
    case (int)ML_classification::SCORE_METRIC_T::F1_WEIGTHED:
      break;
    case (int)ML_classification::SCORE_METRIC_T::NEG_LOG_LOSS:
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION:
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_MACRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_MICRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_SAMPLES:
      break;
    case (int)ML_classification::SCORE_METRIC_T::PRECISION_WEIGHTED:
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL:
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL_MACRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL_MICRO:
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL_SAMPLES:
      break;
    case (int)ML_classification::SCORE_METRIC_T::RECALL_WEIGHTED:
      break;
    case (int)ML_classification::SCORE_METRIC_T::ROC_AUC:
      break;
    default:
      break;
  }
  return score;
}

int ML_classification::explain(std::string &sch_tb_name, std::string &target_name, std::string &model_handle_name,
                               Json_wrapper &exp_options) {
  assert(sch_tb_name.length() && target_name.length());

  OPTION_VALUE_T explaination_values;
  std::string keystr;
  Utils::parse_json(exp_options, explaination_values, keystr, 0);
  assert(explaination_values.size());
  auto model_prediction_type = explaination_values["columns_to_explain"];

  std::string model_content = Loaded_models[model_handle_name];
  assert(model_content.length());

  int importance_type{0};
  MODEL_PREDICTION_EXP_T model_predict_type = MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE;
  switch (model_predict_type) {
    case MODEL_PREDICTION_EXP_T::MODEL_PERMUTATION_IMPORTANCE: {
      importance_type = C_API_FEATURE_IMPORTANCE_SPLIT;
    } break;
    case MODEL_PREDICTION_EXP_T::MODEL_SHAP:
    case MODEL_PREDICTION_EXP_T::MODEL_FAST_SHAP:
    case MODEL_PREDICTION_EXP_T::MODEL_PARTIAL_DEPENDENCE:
    case MODEL_PREDICTION_EXP_T::PREDICT_PERMUTATION_IMPORTANCE:
    case MODEL_PREDICTION_EXP_T::PREDICT_SHAP: {
      importance_type = C_API_FEATURE_IMPORTANCE_GAIN;
    } break;
    default:
      importance_type = C_API_FEATURE_IMPORTANCE_SPLIT;
  }

  BoosterHandle booster = nullptr;
  int num_iterations;
  if (LGBM_BoosterLoadModelFromString(model_content.c_str(), &num_iterations, &booster)) return HA_ERR_GENERIC;

  int num_features;
  if (LGBM_BoosterGetNumFeature(booster, &num_features)) return HA_ERR_GENERIC;

  std::vector<double> feature_importance(num_features);
  if (LGBM_BoosterFeatureImportance(booster, importance_type, 0 /*last iter*/, feature_importance.data()))
    return HA_ERR_GENERIC;

  LGBM_BoosterFree(booster);
  return 0;
}

int ML_classification::explain_row() { return 0; }

int ML_classification::explain_table() { return 0; }

int ML_classification::predict_row(Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
                                   Json_wrapper &result) {
  assert(result.empty());
  std::ostringstream err;
  if (!option.empty()) {
    err << "classification does not support option.";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::string keystr;
  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T meta_feature_names, input_values;
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
  std::string key(ML_KEYWORDS::txt2num_dict);
  if (get_txt2num_dict(model_meta, key, txt2numeric)) return HA_ERR_GENERIC;

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
  auto ret = Utils::ML_predict_row(model_handle_name, sample_data, txt2numeric, predictions);
  if (ret) {
    err << "call ML_PREDICT_ROW failed";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

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
  predictions_obj->add_alias(ML_KEYWORDS::kclass,
                             new (std::nothrow) Json_string(meta_feature_names[ML_KEYWORDS::train_table_name][0]));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  Json_object *probabilities_obj = new (std::nothrow) Json_object();
  if (probabilities_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  auto index{0};
  for (auto &feat_name : feature_names) {
    probabilities_obj->add_alias(feat_name, new (std::nothrow) Json_double(predictions[index]));
    index++;
  }
  ml_results_obj->add_alias(ML_KEYWORDS::probabilities, probabilities_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return ret;
}

int ML_classification::predict_table_row(TABLE *in_table, std::vector<std::string> &feature_names,
                                         std::string &label_name, txt2numeric_map_t &txt2numeric_dict) {
  assert(in_table && label_name.length() && feature_names.size() && txt2numeric_dict.size());
  auto thd = current_thd;
  auto n_read{0u};
  const dd::Table *table_obj{nullptr};
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (!table_obj && in_table->s->table_category != TABLE_UNKNOWN_CATEGORY) {
    if (thd->dd_client()->acquire(in_table->s->db.str, in_table->s->table_name.str, &table_obj)) {
      return n_read;
    }
  }
  return 0;
}
int ML_classification::predict_table(std::string &sch_tb_name, std::string &model_handle_name,
                                     std::string &out_sch_tb_name, Json_wrapper &options) {
  std::ostringstream err;
  std::string model_content = Loaded_models[model_handle_name];
  assert(model_content.length());
  BoosterHandle booster = nullptr;
  booster = Utils::load_trained_model_from_string(model_content);
  if (!booster) return HA_ERR_GENERIC;

  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T option_values, model_meta_option;
  std::string keystr;
  if (!options.empty() && Utils::parse_json(options, option_values, keystr, 0)) return HA_ERR_GENERIC;
  if (!model_meta.empty() && Utils::parse_json(model_meta, model_meta_option, keystr, 0)) return HA_ERR_GENERIC;

  // the thresholds of the prediction. pls refer to `ML_PREDICT_TABLE of heatwave`
  auto remove_seen_str =
      (option_values[ML_KEYWORDS::remove_seen].size()) ? option_values[ML_KEYWORDS::remove_seen][0] : "true";
  std::transform(remove_seen_str.begin(), remove_seen_str.end(), remove_seen_str.begin(), ::toupper);
  auto remove_seen = (remove_seen_str == "TRUE") ? true : false;
  auto batch_size =
      (option_values[ML_KEYWORDS::batch_size].size()) ? std::stoi(option_values[ML_KEYWORDS::batch_size][0]) : 1000;
  auto additional_details_str = (option_values[ML_KEYWORDS::additional_details].size())
                                    ? option_values[ML_KEYWORDS::additional_details][0]
                                    : "true";
  auto prediction_interval = (option_values[ML_KEYWORDS::additional_details].size())
                                 ? std::stof(option_values[ML_KEYWORDS::additional_details][0])
                                 : 0.95f;
  if ((batch_size < 1 || batch_size > 1000) && (prediction_interval <= 0 || prediction_interval >= 1)) {
    err << sch_tb_name << "wrong the params of options";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto pos = std::strstr(sch_tb_name.c_str(), ".") - sch_tb_name.c_str();
  std::string schema_name(sch_tb_name.c_str(), pos);
  std::string table_name(sch_tb_name.c_str() + pos + 1, sch_tb_name.length() - pos);

  auto in_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!in_table_ptr) {
    err << sch_tb_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  if (remove_seen) {
    // TODO: remove the intersection of the input table and the train table.
  } else {
  }
  Utils::close_table(in_table_ptr);

  pos = std::strstr(out_sch_tb_name.c_str(), ".") - out_sch_tb_name.c_str();
  std::string out_schema_name(out_sch_tb_name.c_str(), pos);
  std::string out_table_name(out_sch_tb_name.c_str() + pos + 1, out_sch_tb_name.length() - pos);

  return 0;
}

}  // namespace ML
}  // namespace ShannonBase