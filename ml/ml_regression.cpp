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

#include <chrono>
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
#include "ml_utils.h"                                   //ml utils
#include "storage/innobase/include/ut0dbg.h"            //for ut_a
#include "storage/rapid_engine/include/rapid_config.h"  //loaded table.

namespace ShannonBase {
namespace ML {
// clang-format off
std::map<std::string, ML_regression::SCORE_METRIC_T> ML_regression::score_metrics = {
  {"NEG_MEAN_ABSOLUTE_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR},
  {"NEG_MEAN_SQUARED_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR},
  {"NEG_MEAN_SQUARED_LOG_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR},
  {"NEG_MEDIAN_ABSOLUTE_ERROR", ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR},
  {"R2", ML_regression::SCORE_METRIC_T::R2}
};
// clang-format on

ML_regression::ML_regression() {}
ML_regression::~ML_regression() {}

int ML_regression::train(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  assert(thd);
  std::string user_name(thd->security_context()->user().str);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine";
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
  int n_class{0};  // Not used for regression but kept for API compatibility
  txt2numeric_map_t txt2num_dict;
  auto n_sample =
      Utils::read_data(source_table_ptr, train_data, features_name, target_names[0], label_data, n_class, txt2num_dict);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = features_name.size();
  std::ostringstream oss;

  // Regression-specific parameters
  oss << "task=train boosting_type=gbdt objective=regression metric=rmse metric_freq=1"
      << " is_training_metric=true num_trees=100 learning_rate=0.1 num_leaves=63 tree_learner=serial"
      << " feature_fraction=0.8 max_bin=255 bagging_freq=5 bagging_fraction=0.8 min_data_in_leaf=50"
      << " min_sum_hessian_in_leaf=5.0 is_enable_sparse=true use_two_round_loading=false";

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

  // clang-format on
  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_regression::load(THD *, std::string &model_content) {
  // the definition of this table, ref: `ml_train.sql`
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_regression::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

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
  // all logical done in ml_model_import stored procedure.
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
      err << metric_str << " is invalid for regression scoring";
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

  TableGuard tg(source_table_ptr);

  std::vector<double> test_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2numeric;
  auto n_sample = Utils::read_data(tg.get(), test_data, features_name, target_name, label_data, n_class, txt2numeric);
  if (!n_sample) return 0.0;

  // gets the prediction values.
  std::vector<double> predictions;
  if (Utils::model_predict(C_API_PREDICT_NORMAL, model_handle, n_sample, features_name.size(), test_data, predictions))
    return 0.0;
  double score{0.0};
  switch ((int)ML_regression::score_metrics[metrics[0]]) {
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_ABSOLUTE_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEAN_SQUARED_LOG_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::NEG_MEDIAN_ABSOLUTE_ERROR:
      break;
    case (int)ML_regression::SCORE_METRIC_T::R2:
      break;
    default:
      break;
  }
  return score;
}

int ML_regression::explain(THD *, std::string &sch_tb_name [[maybe_unused]],
                           std::string &target_column_name [[maybe_unused]],
                           std::string &model_handle_name [[maybe_unused]],
                           Json_wrapper &exp_options [[maybe_unused]]) {
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

  // prediction - call the underlying ML engine
  std::vector<double> predictions;
  root_obj->add_alias(ML_KEYWORDS::Prediction,
                      new (std::nothrow) Json_string(meta_feature_names[ML_KEYWORDS::train_table_name][0]));
  auto ret = Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);
  if (ret || predictions.empty()) {
    err << "call ML_PREDICT_ROW failed";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // For regression, use the predicted value directly (continuous numerical value)
  double predicted_value = predictions[0];

  // Add prediction to root object
  root_obj->add_alias(ML_KEYWORDS::Prediction, new (std::nothrow) Json_double(predicted_value));

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

  // Use target name from model metadata, or default to "value"
  std::string target_name = "value";
  if (meta_feature_names.find(ML_KEYWORDS::target_column_name) != meta_feature_names.end() &&
      !meta_feature_names[ML_KEYWORDS::target_column_name].empty()) {
    target_name = meta_feature_names[ML_KEYWORDS::target_column_name][0];
  }

  predictions_obj->add_alias(target_name, new (std::nothrow) Json_double(predicted_value));
  ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

  root_obj->add_alias(ML_KEYWORDS::ml_results, ml_results_obj);
  result = Json_wrapper(root_obj);
  return ret;
}

int ML_regression::predict_table(THD *, std::string &sch_tb_name, std::string &model_handle_name,
                                 std::string &out_sch_tb_name, Json_wrapper &options) {
  std::ostringstream err;
  std::string keystr;
  OPTION_VALUE_T parsed_options;

  // Parse options if provided
  if (!options.empty() && Utils::parse_json(options, parsed_options, keystr, 0)) {
    err << "Failed to parse options JSON";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto pos = std::strstr(sch_tb_name.c_str(), ".") - sch_tb_name.c_str();
  std::string in_schema_name(sch_tb_name.c_str(), pos);
  std::string in_table_name(sch_tb_name.c_str() + pos + 1, sch_tb_name.length() - pos);

  // Open input table for reading
  auto input_table_ptr = Utils::open_table_by_name(in_schema_name, in_table_name, TL_READ);
  if (!input_table_ptr) {
    err << "Failed to open input table: " << sch_tb_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  TableGuard in_tg(input_table_ptr);

  // Open output table for writing (already created by stored procedure)
  pos = std::strstr(out_sch_tb_name.c_str(), ".") - out_sch_tb_name.c_str();
  std::string out_schema_name(out_sch_tb_name.c_str(), pos);
  std::string out_table_name(out_sch_tb_name.c_str() + pos + 1, out_sch_tb_name.length() - pos);
  auto output_table_ptr = Utils::open_table_by_name(out_schema_name, out_table_name, TL_WRITE);
  if (!output_table_ptr) {
    err << "Failed to open output table: " << out_sch_tb_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  TableGuard out_tg(output_table_ptr);

  // Read model metadata to get feature information
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

  // Get feature column names from model metadata
  auto feature_names = meta_info[ML_KEYWORDS::column_names];
  if (feature_names.empty()) {
    err << "Model metadata does not contain feature column names";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Get text-to-numeric mapping dictionary
  txt2numeric_map_t txt2numeric;
  if (Utils::get_txt2num_dict(model_meta, txt2numeric)) {
    err << "Failed to get text-to-numeric dictionary from model";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Map feature names to input table field indices
  std::vector<uint> feature_field_indices;
  for (const auto &feature_name : feature_names) {
    bool found = false;
    for (uint i = 0; i < in_tg.get()->s->fields; i++) {
      Field *field = in_tg.get()->field[i];
      if (strcmp(field->field_name, feature_name.c_str()) == 0) {
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

  // Find output table field indices (created by stored procedure)
  int prediction_field_idx = -1;
  int ml_results_field_idx = -1;

  for (uint i = 0; i < out_tg.get()->s->fields; i++) {
    Field *field = out_tg.get()->field[i];
    if (strcmp(field->field_name, "prediction") == 0) {
      prediction_field_idx = i;
    } else if (strcmp(field->field_name, "ml_results") == 0) {
      ml_results_field_idx = i;
    }
  }

  if (prediction_field_idx == -1 || ml_results_field_idx == -1) {
    err << "Output table missing required prediction or ml_results columns";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Initialize table scanning
  int processed_rows = 0;
  int error_rows = 0;

  if (in_tg.get()->file->ha_rnd_init(true)) {
    err << "Failed to initialize table scan";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Process each row
  int read_result;
  while ((read_result = in_tg.get()->file->ha_rnd_next(in_tg.get()->record[0])) == 0) {
    processed_rows++;

    // Extract feature values for current row
    std::vector<ml_record_type_t> sample_data;
    for (size_t i = 0; i < feature_names.size(); i++) {
      Field *field = in_tg.get()->field[feature_field_indices[i]];
      std::string value = "0";  // Default for null fields

      if (!field->is_null()) {
        String str_value;
        field->val_str(&str_value);
        value = str_value.c_ptr_safe();
      }

      sample_data.push_back({feature_names[i], value});
    }

    // Make prediction using the underlying ML engine
    std::vector<double> predictions;
    int predict_ret =
        Utils::ML_predict_row(C_API_PREDICT_NORMAL, model_handle_name, sample_data, txt2numeric, predictions);

    if (predict_ret || predictions.empty()) {
      error_rows++;
      continue;  // Skip failed predictions
    }

    // For regression, get the continuous predicted value
    double predicted_value = predictions[0];

    // Copy all input data to output table
    memcpy(out_tg.get()->record[0], in_tg.get()->record[0], in_tg.get()->s->reclength);

    // Set prediction field (VARCHAR - store as string representation of the number)
    Field *pred_field = out_tg.get()->field[prediction_field_idx];
    pred_field->set_notnull();
    String pred_str(std::to_string(predicted_value).c_str(), std::to_string(predicted_value).length(),
                    &my_charset_utf8mb4_0900_ai_ci);
    pred_field->store(pred_str.ptr(), pred_str.length(), &my_charset_utf8mb4_0900_ai_ci);

    // Create ml_results JSON object
    Json_object *ml_results_obj = new (std::nothrow) Json_object();
    if (ml_results_obj) {
      Json_object *predictions_obj = new (std::nothrow) Json_object();
      if (predictions_obj) {
        // Use target name from model metadata or default
        std::string target_name = "value";
        if (meta_info.find(ML_KEYWORDS::target_column_name) != meta_info.end() &&
            !meta_info[ML_KEYWORDS::target_column_name].empty()) {
          target_name = meta_info[ML_KEYWORDS::target_column_name][0];
        }

        predictions_obj->add_alias(target_name, new (std::nothrow) Json_double(predicted_value));
        ml_results_obj->add_alias(ML_KEYWORDS::predictions, predictions_obj);

        // Set ml_results field
        Field *ml_results_field = out_tg.get()->field[ml_results_field_idx];
        ml_results_field->set_notnull();

        Json_wrapper wrapper(ml_results_obj);
        String json_str;
        wrapper.to_string(&json_str, false, "predict_table", [] { assert(false); });
        ml_results_field->store(json_str.ptr(), json_str.length(), &my_charset_utf8mb4_0900_ai_ci);
      } else {
        // Fallback: empty JSON object
        Field *ml_results_field = out_tg.get()->field[ml_results_field_idx];
        ml_results_field->set_notnull();
        const char *empty_json = "{}";
        ml_results_field->store(empty_json, strlen(empty_json), &my_charset_utf8mb4_0900_ai_ci);
      }
    } else {
      // Fallback: empty JSON object
      Field *ml_results_field = out_tg.get()->field[ml_results_field_idx];
      ml_results_field->set_notnull();
      const char *empty_json = "{}";
      ml_results_field->store(empty_json, strlen(empty_json), &my_charset_utf8mb4_0900_ai_ci);
    }

    // Insert the row into output table
    if (out_tg.get()->file->ha_write_row(out_tg.get()->record[0])) {
      error_rows++;
    }
  }

  // Clean up
  in_tg.get()->file->ha_rnd_end();

  if (read_result != HA_ERR_END_OF_FILE) {
    err << "Error reading input table";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  return (error_rows > 0) ? HA_ERR_GENERIC : 0;
}
ML_TASK_TYPE_T ML_regression::type() { return ML_TASK_TYPE_T::REGRESSION; }
}  // namespace ML
}  // namespace ShannonBase