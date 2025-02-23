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
#include <set>

#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"  //LightGBM

#include "include/my_inttypes.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"
#include "storage/innobase/include/ut0dbg.h"            //for ut_a
#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

#include "auto_ml.h"
#include "ml_info.h"
#include "ml_utils.h"  //ml utils

namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string, ML_classification::SCORE_METRIC_T> ML_classification::score_metrics =
{{"BALANCED_ACCURACY", ML_classification::SCORE_METRIC_T::BALANCED_ACCURACY},
 {"F1_SAMPLES", ML_classification::SCORE_METRIC_T::F1_SAMPLES},
 {"PRECISION_SAMPLES", ML_classification::SCORE_METRIC_T::PRECISION_SAMPLES},
 {"RECALL_SAMPLES", ML_classification::SCORE_METRIC_T::RECALL_SAMPLES},
 {"F1", ML_classification::SCORE_METRIC_T::F1},
 {"PRECISION", ML_classification::SCORE_METRIC_T::PRECISION},
 {"RECALL", ML_classification::SCORE_METRIC_T::RECALL},
 {"ROC_AUC", ML_classification::SCORE_METRIC_T::ROC_AUC},
 {"ACCURACY", ML_classification::SCORE_METRIC_T::ACCURACY},
 {"F1_MACRO", ML_classification::SCORE_METRIC_T::F1_MACRO},
 {"F1_MICRO", ML_classification::SCORE_METRIC_T::F1_MICRO},
 {"F1_WEIGTHED", ML_classification::SCORE_METRIC_T::F1_WEIGTHED},
 {"NEG_LOG_LOSS", ML_classification::SCORE_METRIC_T::NEG_LOG_LOSS},
 {"PRECISION_MACRO", ML_classification::SCORE_METRIC_T::PRECISION_MACRO},
 {"PRECISION_MICRO", ML_classification::SCORE_METRIC_T::PRECISION_MICRO},
 {"PRECISION_WEIGHTED", ML_classification::SCORE_METRIC_T::PRECISION_WEIGHTED},
 {"RECALL_MACRO", ML_classification::SCORE_METRIC_T::RECALL_MACRO},
 {"RECALL_MICRO", ML_classification::SCORE_METRIC_T::RECALL_MICRO},
 {"RECALL_WEIGHTED", ML_classification::SCORE_METRIC_T::RECALL_WEIGHTED}
};
// clang-format off

ML_classification::ML_classification() {}
ML_classification::~ML_classification() {}

// returuns the # of record read successfully, otherwise 0 read failed.
int ML_classification::read_data(TABLE *table, std::vector<double> &train_data, std::vector<std::string> &features_name,
                                std::string &label_name, std::vector<float> &label_data,
                                int& n_class, txt2numeric_map_t& txt2numeric_dict) {
  THD *thd = current_thd;
  auto n_read{0u};

  txt2numeric_map_t txt2numeric;
  // read the training data from target table.
  for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
    Field *field_ptr = *(table->field + field_id);
    txt2numeric[field_ptr->field_name];

    if (likely(!strcmp(field_ptr->field_name, label_name.c_str()))) continue;
    features_name.push_back(field_ptr->field_name);
  }

  const dd::Table *table_obj{nullptr};
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (!table_obj && table->s->table_category != TABLE_UNKNOWN_CATEGORY) {
    if (thd->dd_client()->acquire(table->s->db.str, table->s->table_name.str, &table_obj)) {
      return n_read;
    }
  }

  // must read from secondary engine.
  unique_ptr_destroy_only<handler> sec_tb_handler(Utils::get_secondary_handler(table));
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);
  sec_tb_handler->ha_open(table, table->s->normalized_path.str, O_RDONLY, HA_OPEN_IGNORE_IF_LOCKED, table_obj);
  if (sec_tb_handler && sec_tb_handler->ha_external_lock(thd, F_RDLCK)) {
    sec_tb_handler->ha_close();
    return n_read;
  }

  if (sec_tb_handler->ha_rnd_init(true)) {
    sec_tb_handler->ha_external_lock(thd, F_UNLCK);
    sec_tb_handler->ha_close();
    return n_read;
  }

  std::map<float, int> n_classes;
  while (sec_tb_handler->ha_rnd_next(table->record[0]) == 0) {
    for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
      Field *field_ptr = *(table->field + field_id);

      auto data_val{0.0};
      switch (field_ptr->type()) {
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
          data_val = field_ptr->val_real();
        } break;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
          my_decimal dval;
          field_ptr->val_decimal(&dval);
          my_decimal2double(10, &dval, &data_val);
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING: { // convert txt string to numeric
          String buf;
          buf.set_charset(field_ptr->charset());
          field_ptr->val_str(&buf);
          txt2numeric[field_ptr->field_name].insert(buf.c_ptr_safe());
          assert(txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()) != txt2numeric[field_ptr->field_name].end());
          data_val = std::distance(txt2numeric[field_ptr->field_name].begin(),
                                   txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()));
          txt2numeric_dict[field_ptr->field_name].insert(buf.c_ptr_safe());
        } break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME: {
          data_val = field_ptr->val_real();
        } break;
        default:
          assert(false);
          break;
      }

      if (likely(strcmp(field_ptr->field_name, label_name.c_str()))) {
        train_data.push_back(data_val);
      } else {  // is label data.
        if (n_classes.find((float)data_val) == n_classes.end())
          n_classes[(float)data_val] = n_classes.size();
        label_data.push_back(n_classes[(float)data_val]);
      }
    }  // for
    n_read++;
  }  // while

  if (old_map) tmp_restore_column_map(table->read_set, old_map);

  sec_tb_handler->ha_rnd_end();
  sec_tb_handler->ha_external_lock(thd, F_UNLCK);
  // to close the secondary engine table.
  sec_tb_handler->ha_close();

  n_class = n_classes.size();
  return n_read;
}

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

int ML_classification::train() {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " NOT loaded into rapid engine for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name, target_names;
  Utils::splitString(m_target_name, ',', target_names);
  assert(target_names.size() == 1);
  int n_class;
  txt2numeric_map_t txt2num_dict;
  auto n_sample = read_data(source_table_ptr, train_data, features_name, target_names[0], label_data, 
                            n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = features_name.size();
  std::ostringstream oss;
  if (n_class <= 2) { // binary-classification. pay attention to the format of params
    oss << "task=train boosting_type=gbdt objective=binary metric=binary_logloss,auc metric_freq=1" <<
            " is_training_metric=true num_trees=100 learning_rate=0.1 num_leaves=63 tree_learner=serial"<<
            " feature_fraction=0.8 max_bin=255 bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=50" <<
            " min_sum_hessian_in_leaf=5.0 is_enable_sparse=true use_two_round_loading=false";
  } else { //multi classification
    oss << "task=train boosting_type=gbdt objective=multiclass metric=multi_logloss,auc_mu" <<
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
  std::string oper_type("train"), sch_tb_name (oss.str().c_str()), notes, opt_metrics;

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

int ML_classification::predict() { return 0; }

// load the model from model_content.
int ML_classification::load(std::string &model_content) {
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_classification::load_from_file(std::string &modle_file_full_path [[maybe_unused]],
                                      std::string &model_handle_name [[maybe_unused]]) {
  return 0;
}

int ML_classification::unload(std::string &model_handle_name) {
  assert(!Loaded_models.empty());

  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return 0;
}

int ML_classification::import(Json_wrapper &model_object [[maybe_unused]],
                              Json_wrapper &model_metadata [[maybe_unused]],
                              std::string &model_handle_name [[maybe_unused]]) {
  std::string keystr;
  OPTION_VALUE_T model_meta_info;
  Utils::parse_json(model_metadata, model_meta_info, keystr, 0);
  if (model_meta_info.find("schema") != model_meta_info.end()) {  // load from a table.

  } else {  // load from meta info json file.
  }
  return 0;
}

double ML_classification::score(std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                                std::string &metric_str, Json_wrapper &option) {
  assert(!sch_tb_name.empty() && !target_name.empty() && !model_handle.empty() && !metric_str.empty());

  if (!option.empty()) {
    std::ostringstream err;
    err << "option params should be null for classification";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric_str << " is invalid for classification scoring";
      my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  auto pos = std::strstr(sch_tb_name.c_str(), ".") - sch_tb_name.c_str();
  std::string schema_name(sch_tb_name.c_str(), pos);
  std::string table_name(sch_tb_name.c_str() + pos + 1, sch_tb_name.length() - pos);
  auto share = ShannonBase::shannon_loaded_tables->get(schema_name.c_str(), table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << schema_name.c_str() << "." << table_name.c_str() << " NOT loaded into rapid engine for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // load the test data from rapid engine.
  auto source_table_ptr = Utils::open_table_by_name(schema_name, table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << schema_name.c_str() << "." << table_name.c_str() << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> test_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class;
  txt2numeric_map_t txt2numeric;
  auto n_sample = read_data(source_table_ptr, test_data, features_name, target_name, label_data, n_class, txt2numeric);
  Utils::close_table(source_table_ptr);
  if (!n_sample) return 0.0f;

  return Utils::model_score(model_handle, (int)score_metrics[metrics[0]], n_sample, features_name.size(), test_data,
                            label_data);
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
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  txt2numeric_map_t txt2numeric;
  std::string keystr;
  Json_wrapper model_meta;
  if (Utils::read_model_content(model_handle_name, model_meta)) return HA_ERR_GENERIC;

  OPTION_VALUE_T meta_feature_names, input_data_col_names;
  if (Utils::parse_json(input_data, input_data_col_names, keystr, 0) ||
      Utils::parse_json(model_meta, meta_feature_names, keystr, 0)) {
    err << "invalid input data or model meta info.";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto feature_names = meta_feature_names["column_names"];
  if (feature_names.size() != input_data_col_names.size()) {
    err << "input data columns size does not match the model feature columns size.";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  for (auto &feature_name : feature_names) {
    if (input_data_col_names.find(feature_name) == input_data_col_names.end()) {
      err << "input data columns does not contain the model feature column: " << feature_name;
      my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  std::string model_content = Loaded_models[model_handle_name];
  assert(model_content.length());
  BoosterHandle booster = nullptr;
  booster = Utils::load_trained_model_from_string(model_content);
  if (!booster) return HA_ERR_GENERIC;

  auto n_feature = feature_names.size();
  int64 num_results = n_feature + 1;  // contri value + bias
  std::vector<double> sample_data, predictions(num_results, 0.0);
  Json_object *root_obj = new (std::nothrow) Json_object();
  if (root_obj == nullptr) {
    return HA_ERR_GENERIC;
  }

  for (auto &feature_name : feature_names) {
    auto value = 0.0;
    if (meta_feature_names.find(feature_name) == meta_feature_names.end()) {  // not a txt field.
      value = std::stod(input_data_col_names[feature_name][0]);
      root_obj->add_alias(feature_name, new (std::nothrow) Json_string(input_data_col_names[feature_name][0]));
    } else {  // find in txt2num_dict.
      auto txt2num = meta_feature_names[feature_name];
      auto input_val = input_data_col_names[feature_name][0];
      root_obj->add_alias(feature_name, new (std::nothrow) Json_string(input_val));
      value = std::distance(txt2num.begin(), std::find(txt2num.begin(), txt2num.end(), input_val));
    }
    sample_data.push_back(value);
  }

  // clang-format off
  auto ret = LGBM_BoosterPredictForMat(booster,
                                       sample_data.data(),
                                       C_API_DTYPE_FLOAT64,
                                       1,                   // # of row
                                       n_feature,           // # of col
                                       1,                    // row oriented
                                       C_API_PREDICT_CONTRIB,
                                       0,                    // start iter
                                       -1,                   // stop iter
                                       "",                   // contribution params
                                       &num_results,         // # of results
                                       predictions.data());
  // clang-format on
  LGBM_BoosterFree(booster);
  if (ret) {
    return HA_ERR_GENERIC;
  }

  // prediction
  root_obj->add_alias("Prediction", new (std::nothrow) Json_string(meta_feature_names["train_table_name"][0]));

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
  predictions_obj->add_alias("class", new (std::nothrow) Json_string(meta_feature_names["train_table_name"][0]));
  ml_results_obj->add_alias("predictions", predictions_obj);

  Json_object *probabilities_obj = new (std::nothrow) Json_object();
  if (probabilities_obj == nullptr) {
    return HA_ERR_GENERIC;
  }
  auto index{0};
  for (auto &feat_name : feature_names) {
    probabilities_obj->add_alias(feat_name, new (std::nothrow) Json_double(predictions[index]));
    index++;
  }
  ml_results_obj->add_alias("probabilities", probabilities_obj);

  root_obj->add_alias("ml_results", ml_results_obj);
  result = Json_wrapper(root_obj);
  return ret;
}

int ML_classification::predict_table() { return 0; }

ML_TASK_TYPE_T ML_classification::type() { return ML_TASK_TYPE_T::CLASSIFICATION; }

}  // namespace ML
}  // namespace ShannonBase