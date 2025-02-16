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
                                 std::string &label_name, std::vector<float> &label_data) {
  THD *thd = current_thd;
  auto n_read{0u};

  // read the training data from target table.
  std::map<std::string, std::set<std::string>> txt2numeric;
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

  while (sec_tb_handler->ha_rnd_next(table->record[0]) == 0) {
    for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
      Field *field_ptr = *(table->field + field_id);
      // if (field_ptr->is_real_null())
      auto data_val{0.0};
      String buf;
      my_decimal dval;
      switch (field_ptr->type()) {
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
          data_val = field_ptr->val_real();
          break;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
          field_ptr->val_decimal(&dval);
          my_decimal2double(10, &dval, &data_val);
          break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:  // convert txt string to numeric
          buf.set_charset(field_ptr->charset());
          field_ptr->val_str(&buf);
          txt2numeric[field_ptr->field_name].insert(buf.c_ptr_safe());
          assert(txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()) != txt2numeric[field_ptr->field_name].end());
          data_val = std::distance(txt2numeric[field_ptr->field_name].begin(),
                                   txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()));
          break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME:
          data_val = field_ptr->val_real();
        default:
          break;
      }

      if (likely(strcmp(field_ptr->field_name, label_name.c_str()))) {
        train_data.push_back(data_val);
      } else {  // is label data.
        label_data.push_back(data_val);
      }
    }  // for
    n_read++;
  }  // while

  if (old_map) tmp_restore_column_map(table->read_set, old_map);

  sec_tb_handler->ha_rnd_end();
  sec_tb_handler->ha_external_lock(thd, F_UNLCK);
  // to close the secondary engine table.
  sec_tb_handler->ha_close();

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
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  std::vector<std::string> target_names;
  Utils::splitString(m_target_name, ',', target_names);
  assert(target_names.size() == 1);

  auto n_sample = read_data(source_table_ptr, train_data, features_name, target_names[0], label_data);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = features_name.size();
  std::string mode_params =
      "task=train "
      "boosting_type=gbdt "
      "objective=binary "
      "metric=binary_logloss,auc "
      "metric_freq=1 "
      "num_trees=100 "
      "learning_rate=0.1 "
      "num_leaves=63 "
      "max_bin=254 ";
  std::string model_content;

  std::vector<const char*> feature_names_cstr;
  for (const auto& name : features_name) {
      feature_names_cstr.push_back(name.c_str());
  }  
  // clang-format off
  auto start = std::chrono::steady_clock::now();
  if (Utils::ML_train(mode_params,
                      C_API_DTYPE_FLOAT64,
                      static_cast<const void *>(train_data.data()),
                      n_sample,
                      feature_names_cstr.data(),
                      n_feature,
                      C_API_DTYPE_FLOAT32,
                      static_cast<const void *>(label_data.data()),
                      model_content))
    return HA_ERR_GENERIC;
  auto end = std::chrono::steady_clock::now();
  auto train_duration =
    std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f;

  // the definition of this table, ref: `ml_train.sql`
  std::string oper_type("train");
  std::string sch_tb_name = m_sch_name + "." ;
  sch_tb_name += m_table_name;
  std::string notes, opt_metrics;

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
                                                nullptr,        /**training_params */
                                                nullptr,        /**onnx_inputs_info */
                                                nullptr,        /*onnx_outputs_info*/
                                                nullptr,        /*training_drift_metric*/
                                                1               /* chunks */
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

int ML_classification::import(std::string &model_handle_name [[maybe_unused]], std::string &user_name [[maybe_unused]],
                              std::string &content [[maybe_unused]]) {
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
  auto n_sample = read_data(source_table_ptr, test_data, features_name, target_name, label_data);
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

int ML_classification::predict_row() { return 0; }

int ML_classification::predict_table() { return 0; }

ML_TASK_TYPE_T ML_classification::type() { return ML_TASK_TYPE_T::CLASSIFICATION; }

}  // namespace ML
}  // namespace ShannonBase