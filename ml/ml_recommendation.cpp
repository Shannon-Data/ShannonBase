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

#include <chrono>
#include <sstream>
#include <string>

#include "include/my_base.h"
#include "mysqld_error.h"
#include "sql/field.h"  //Field
#include "sql/table.h"  //Table

#include "ml_utils.h"

namespace ShannonBase {
namespace ML {

// clang-format off
std::map<std::string, ML_recommendation::SCORE_METRIC_T> ML_recommendation::score_metrics = {
  {"HIT_RATIO_AT_K", ML_recommendation::SCORE_METRIC_T::HIT_RATIO_AT_K},
  {"NDCG_AT_K", ML_recommendation::SCORE_METRIC_T::NDCG_AT_K},
  {"NEG_MEAN_ABSOLUTE_ERROR", ML_recommendation::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_SQUARED_ERROR", ML_recommendation::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_ROOT_MEAN_SQUARED_ERROR", ML_recommendation::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR},
  {"PRECISION_AT_K", ML_recommendation::SCORE_METRIC_T::PRECISION_AT_K},
  {"R2", ML_recommendation::SCORE_METRIC_T::R2},
  {"RECALL_AT_K", ML_recommendation::SCORE_METRIC_T::RECALL_AT_K}
};
// clang-format on

ML_recommendation::ML_recommendation() {}

ML_recommendation::~ML_recommendation() {}

ML_TASK_TYPE_T ML_recommendation::type() { return ML_TASK_TYPE_T::RECOMMENDATION; }

int ML_recommendation::train() {
  std::vector<std::string> target_names;
  Utils::splitString(m_target_name, ',', target_names);
  assert(target_names.size() == 0 || target_names.size() == 1);

  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  if (options.find(ML_KEYWORDS::users) == options.end() || options.find(ML_KEYWORDS::items) == options.end()) {
    std::ostringstream err;
    err << "users or items must be specified";
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

  if (!m_options.empty()) {
    auto users_nmae{options[ML_KEYWORDS::users][0]}, items_name{options[ML_KEYWORDS::items][0]};
    for (auto index = 0u; index < source_table_ptr->s->fields; index++) {
      auto field_ptr = *(source_table_ptr->field + index);
      if (!strcmp(field_ptr->field_name, users_nmae.c_str()) || !strcmp(field_ptr->field_name, items_name.c_str())) {
        if (field_ptr->type() == MYSQL_TYPE_VARCHAR || field_ptr->type() == MYSQL_TYPE_VAR_STRING ||
            field_ptr->type() == MYSQL_TYPE_STRING)
          continue;
        else {
          std::ostringstream err;
          err << field_ptr->field_name << " users or items field should be string type";
          my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
          return HA_ERR_GENERIC;
        }
      }
    }
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  std::string target = (target_names.size()) ? target_names[0] : "";
  auto n_sample =
      Utils::read_data(source_table_ptr, train_data, features_name, target, label_data, n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = features_name.size();
  std::ostringstream oss;
  oss << "task=train boosting_type=gbdt objective=rank_xendcg"
      << " max_bin=255 num_trees=100 learning_rate=0.05"
      << " num_leaves=31 tree_learner=serial";
  std::string model_content, mode_params(oss.str().c_str());

  std::vector<const char *> feature_names_cstr;
  for (const auto &name : features_name) {
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

int ML_recommendation::load(std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_recommendation::load_from_file(std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_recommendation::unload(std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());

  auto cnt = Loaded_models.erase(model_handle_name);
  assert(cnt == 1);
  return 0;
}

int ML_recommendation::import(Json_wrapper &, Json_wrapper &, std::string &) {
  // all logical done in ml_model_import stored procedure.
  assert(false);
  return 0;
}

double ML_recommendation::score(std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                                std::string &metric_str, Json_wrapper &option) {
  std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
  std::vector<std::string> metrics;
  Utils::splitString(metric_str, ',', metrics);
  for (auto &metric : metrics) {
    if (score_metrics.find(metric) == score_metrics.end()) {
      std::ostringstream err;
      err << metric_str << " is invalid for recommendation scoring";
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
  switch ((int)ML_recommendation::score_metrics[metrics[0]]) {
    case (int)ML_recommendation::SCORE_METRIC_T::HIT_RATIO_AT_K:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NDCG_AT_K:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::NEG_ROOT_MEAN_SQUARED_ERROR:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::PRECISION_AT_K:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::R2:
      break;
    case (int)ML_recommendation::SCORE_METRIC_T::RECALL_AT_K:
      break;
    default:
      break;
  }
  return score;
}

int ML_recommendation::explain(std::string &, std::string &, std::string &, Json_wrapper &) {
  std::ostringstream err;
  err << "recommendation does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_recommendation::explain_row() {
  std::ostringstream err;
  err << "recommendation does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_recommendation::explain_table() {
  std::ostringstream err;
  err << "recommendation does not soupport explain operation";
  my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_recommendation::predict_row(Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
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

int ML_recommendation::predict_table(std::string &sch_tb_name [[maybe_unused]],
                                     std::string &model_handle_name [[maybe_unused]],
                                     std::string &out_sch_tb_name [[maybe_unused]],
                                     Json_wrapper &options [[maybe_unused]]) {
  return 0;
}

}  // namespace ML
}  // namespace ShannonBase