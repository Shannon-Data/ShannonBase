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
#ifndef __SHANNONBASE_ML_UTILS_H__
#define __SHANNONBASE_ML_UTILS_H__

#include <memory>
#include <set>
#include <string>

#include "extra/lightgbm/include/LightGBM/c_api.h"
#include "include/thr_lock.h"  //TL_READ

#include "ml_info.h"

class TABLE;
class handler;
class Json_wrapper;
class Json_object;
using std::string;

namespace ShannonBase {
namespace ML {
class Utils {
 public:
  static int check_table_available(std::string &sch_tb_name);

  // open a table via schema name and table name.
  static TABLE *open_table_by_name(const std::string &schema_name, const std::string &table_name, thr_lock_type mode);

  // get the primary a openned table handler on success, otherwise nullptr.
  static handler *get_primary_handler(TABLE *source_table_ptr);

  // get the secondary handler of a openned table.
  static handler *get_secondary_handler(TABLE *source_table_ptr);

  /**
   * @brief No-op close for TABLE objects.
   *
   * In the current design, the lifecycle of TABLE objects is managed by
   * the SQL layer. Specifically, all opened tables will be automatically
   * closed by the caller through `close_thread_tables()`. Therefore, this
   * function does not need to manually close or free the TABLE instance.
   *
   * @param table The TABLE pointer to close (unused in this implementation).
   * @return Always returns 0.
   */
  static int close_table(TABLE *table);

  /** to training a mode by using specific data. success, returns hanlde of the trained model,
   *  otherwise, returns nullptr.
   *  @param[in] task_mode, task mode, such as classification, regression, etc.
   *  @param[in] data_type, the data type, such as INT32, FLOAT32, etc.
   *  @param[in] training data, the source data.
   *  @param[in] n_data, # of source data.
   *  @param[in] n_feature, # of features used to do training.
   *  @param[in] label_data_type, the type of labelled data.
   *  @param[in] label_data, the source of the labelled data.
   *  @param[in] model_content, the trained model in string format.
   *  @retval handler of trained mode on success. Otherwise, null failed
   */
  static int ML_train(std::string &task_mode, uint data_type, const void *training_data, uint n_data,
                      const char **features_name, uint n_feature, uint label_data_type, const void *label_data,
                      std::string &model_content);

  /**
   * to calcl the pobilities of a trained model with mutl-rows data.
   * @param[in] type, prediction type used.
   * @param[in] model_handle_name, the name of loaded model name.
   * @param[in] testing_data, the test data.
   * @param[in] features, feature names array.
   * @param[in] lable_col_name, label column name.
   * @param[out] predictions, the prediction values.
   * @retval 0 success, otherwise failed.
   */
  static int model_predict(int type, std::string &model_handle_name, size_t n_samples, size_t n_features,
                           std::vector<double> &testing_data, std::vector<double> &predictions);

  /**
   * to predict the result of a model with one user input data row, do normailization with
   * the dictionary before do prediction.
   * @param[in] type, which type of prediction used.
   * @param[in] model_handle_name, the name of loaded model name.
   * @param[in] input_data, the input data.
   * @param[in] txt2numeric_dict, the txt2numeric dict.
   * @param[out] result, the result of prediction.
   * @retval 0 success, otherwise failed.
   */
  static int ML_predict_row(int type, std::string &model_handle_name, std::vector<ml_record_type_t> &input_data,
                            txt2numeric_map_t &txt2numeric_dict, std::vector<double> &predictions);

  /**
   * to build up a json format model metadata.
   * params defintion ref to: https://dev.mysql.com/doc/heatwave/en/mys-hwaml-ml-model-metadata.html
   * @retrun none-nullptr success, meta_data is the built meta inforation, otherwise failed.
   */
  static Json_object *build_up_model_metadata(
      std::string &task, std::string &target_column_name, std::string &tain_table_name,
      std::vector<std::string> &featurs_name, Json_object *model_explanation, std::string &notes, std::string &format,
      std::string &status, std::string &model_quality, double training_time, std::string &algorithm_name,
      double training_score, size_t n_rows, size_t n_columns, size_t n_selected_rows, size_t n_selected_columns,
      std::string &optimization_metric, std::vector<std::string> &selected_column_names, double contamination,
      Json_wrapper *train_options, std::string &training_params, Json_object *onnx_inputs_info,
      Json_object *onnx_outputs_info, Json_object *training_drift_metric, size_t chunks,
      txt2numeric_map_t &txt2num_dict);

  /* get the model content via handle name, sucess return 0, otherise failed.
   * @param[in] model_handle_name,model user name.
   * @param[in] options, the model option we got. JSON format.
   * @retval 0 success.
   * @retval error code failed.
   */
  static int read_model_content(std::string &model_handle_name, Json_wrapper &options);

  /* get the model object content via handle name, sucess return 0, otherise failed.
   * @param[in] model_user_name, model user name.
   * @param[in/out] model_content, the model content. JSON format.
   * @retval 0 success.
   * @retval error code failed.
   */
  static int read_model_object_content(std::string &model_handle_name, std::string &model_content);

  /**
   * to build a model from string, which is stored by saviing the model to file/string.
   * @param[in] model_content, the trained model content.
   * @return BoosterHandle success, otherwise nullptr.
   */
  static BoosterHandle load_trained_model_from_string(std::string &model_content);

  /**
   * parse the model option into string formation, and get the value. rewrites from `wrapper_to_string` function.
   * @param[in] options, the model option in JSON format.
   * @param[out] option_value, the value of the option.
   * @param[in] key, the key of the option.
   * @param[in] depth, the depth of the option, start from 0.
   * @return 0 success, otherwise failed.f
   */
  static int parse_json(Json_wrapper &options, OPTION_VALUE_T &option_value, std::string &key, size_t depth);

  /**
   * to split a string by a del, and stores into a vector.
   * @param[in]
   *
   * */
  static int splitString(const std::string &str, char delimiter, std::vector<std::string> &output);

  static std::string read_file(std::string &file_path);

  static int get_txt2num_dict(Json_wrapper &model_meta, txt2numeric_map_t &txt2num_dict);

  static int read_data(TABLE *table, std::vector<double> &train_data, std::vector<std::string> &features_name,
                       std::string &label_name, std::vector<float> &label_data, int &n_class,
                       txt2numeric_map_t &txt2numeric_dict);

  static double calculate_accuracy(size_t n_sample, std::vector<double> &predictions, std::vector<float> &label_data);

  static double calculate_balanced_accuracy(size_t n_sample, std::vector<double> &predictions,
                                            std::vector<float> &label_data);

 private:
  Utils() = delete;
  virtual ~Utils() = delete;
  // disable copy ctor, operator=, etc.
};

class TableGuard {
 public:
  explicit TableGuard(TABLE *table = nullptr) : m_table(table) {}

  ~TableGuard() {
    if (m_table != nullptr) {
      Utils::close_table(m_table);
    }
  }

  TableGuard(const TableGuard &) = delete;
  TableGuard &operator=(const TableGuard &) = delete;

  TableGuard(TableGuard &&other) noexcept : m_table(other.m_table) { other.m_table = nullptr; }

  TableGuard &operator=(TableGuard &&other) noexcept {
    if (this != &other) {
      if (m_table != nullptr) {
        Utils::close_table(m_table);
      }
      m_table = other.m_table;
      other.m_table = nullptr;
    }
    return *this;
  }

  TABLE *get() const { return m_table; }

  TABLE *release() {
    TABLE *temp = m_table;
    m_table = nullptr;
    return temp;
  }

  void reset(TABLE *new_table = nullptr) {
    if (m_table != nullptr) {
      Utils::close_table(m_table);
    }
    m_table = new_table;
  }

  explicit operator bool() const { return m_table != nullptr; }

 private:
  TABLE *m_table;
};

}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ML_REGRESSION_H__