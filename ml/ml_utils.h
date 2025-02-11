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
#include <string>

#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"
#include "include/thr_lock.h"  //TL_READ

class TABLE;
class handler;
class Json_wrapper;
class Json_object;

namespace ShannonBase {
namespace ML {
class Utils {
 public:
  // open a table via schema name and table name.
  static TABLE *open_table_by_name(std::string schema_name, std::string table_name, thr_lock_type mode);

  // get the primary a openned table handler on success, otherwise nullptr.
  static handler *get_primary_handler(TABLE *source_table_ptr);

  // get the secondary handler of a openned table.
  static handler *get_secondary_handler(TABLE *source_table_ptr);

  // close a opened table.
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
   *  @retval handler of trained mode.
   *  @retval nullptr failed.
   */
  static BoosterHandle ML_train(std::string &task_mode, uint data_type, const void *training_data, uint n_data,
                                uint n_feature, uint label_data_type, const void *label_data,
                                std::string &model_content);

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
      Json_wrapper *train_options, Json_object *training_params, Json_object *onnx_inputs_info,
      Json_object *onnx_outputs_info, Json_object *training_drift_metric, size_t chunks);

  /** to store the trained model into ML_SCHEMA_xxx.MODEL_CATALOG.
   *  @param[in] model_content, the trainned model in string formation.
   *  @param[in] option, the model option, in JSON formation.
   *  @param[in] user_name, the who create/build this model.
   *  @param[in] handler_name, the handler name, unique key.
   *  @retval 0 success.
   *  @retval errcode failed.
   *  */
  static int store_model_catalog(size_t model_obj_size, const Json_wrapper *model_meta, std::string &handler_name);

  /**
   * store the model meta info into model_object_catalog table.
   * @param[in] model_handle_name, the name of this model handler.
   * @param[in] model_meta, the json wrapper handler of this json-formattted model.
   * @return 0 success, otherwise failed.
   */
  static int store_model_object_catalog(std::string &model_handle_name, Json_wrapper *model_meta);

  /* get the model content via handle name, sucess return 0, otherise failed.
   * @param[in] model_user_name, model user name.
   * @param[in] model_handle_name,model user name.
   * @param[in] options, the model option we got. JSON format.
   * @retval 0 success.
   * @retval error code failed.
   */
  static int read_model_content(std::string &model_user_name, std::string &model_handle_name, Json_wrapper &options);

  /* get the model object content via handle name, sucess return 0, otherise failed.
   * @param[in] model_user_name, model user name.
   * @param[in/out] model_content, the model content. JSON format.
   * @retval 0 success.
   * @retval error code failed.
   */
  static int read_model_object_content(std::string &model_user_name, std::string &model_handle_name,
                                       std::string &model_content);

  /**
   * to build a model from string, which is stored by saviing the model to file/string.
   * @param[in] model_content, the trained model content.
   * @return BoosterHandle success, otherwise nullptr.
   */
  static BoosterHandle load_trained_model_from_string(std::string &model_content);

 private:
  Utils() = delete;
  virtual ~Utils() = delete;
  // disable copy ctor, operator=, etc.
};

}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ML_REGRESSION_H__