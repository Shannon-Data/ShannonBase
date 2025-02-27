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
#ifndef __SHANNONBASE_AUTO_ML_INFO_H__
#define __SHANNONBASE_AUTO_ML_INFO_H__

#include <map>
#include <set>
#include <string>
#include <utility>

namespace ShannonBase {
namespace ML {

#define CHECK_CALL(x)      \
  if ((x) != 0) {          \
    RETURN HA_ERR_GENERIC; \
  }

extern std::map<std::string, std::string> Loaded_models;

// clang-format off
enum class ML_TASK_TYPE_T {
   UNKNOWN = -1,
   CLASSIFICATION,
   REGRESSION,
   FORECASTING,
   ANOMALY_DETECTION,
   RECOMMENDATION
};

enum class MODEL_STATUS_T {
   CREATING = 0,
   READY, ERROR
};

enum class MODEL_FORMAT_T {
   VER_1 = 0,
   ONNX
};

enum class MODEL_QUALITY_T {
   LOW = 0,
   HIGH
};

enum class MODEL_PREDICTION_EXP_T {
   MODEL_PERMUTATION_IMPORTANCE = 0,
   MODEL_SHAP,
   MODEL_FAST_SHAP,
   MODEL_PARTIAL_DEPENDENCE,
   PREDICT_PERMUTATION_IMPORTANCE,
   PREDICT_SHAP
};
// clang-format off

extern std::map<std::string, ML_TASK_TYPE_T> OPT_TASKS_MAP;
extern std::map<ML_TASK_TYPE_T, std::string> TASK_NAMES_MAP;
extern std::map<MODEL_STATUS_T, std::string> MODEL_STATUS_MAP;
extern std::map<MODEL_FORMAT_T, std::string> MODEL_FORMATS_MAP;
extern std::map<MODEL_QUALITY_T, std::string> MODEL_QUALITIES_MAP;
extern std::map<std::string, MODEL_PREDICTION_EXP_T> MODEL_EXPLAINERS_MAP;

class ML_KEYWORDS {
   public:
   static constexpr const char* SHANNON_LIGHTGBM_CONTENT = "SHANNON_LIGHTGBM_CONTENT";
   static constexpr const char* additional_details = "additional_details";
   static constexpr const char* algorithm_name = "algorithm_name";
   static constexpr const char* batch_size = "batch_size";
   static constexpr const char* build_timestamp = "build_timestamp";
   static constexpr const char* classification = "classification";
   static constexpr const char* chunks = "chunks";
   static constexpr const char* column_names = "column_names";
   static constexpr const char* contamination = "contamination";
   static constexpr const char* datetime_index = "datetime_index";
   static constexpr const char* endogenous_variables = "endogenous_variables";
   static constexpr const char* exogenous_variables = "exogenous_variables";
   static constexpr const char* format = "format";
   static constexpr const char* include_column_list = "include_column_list";
   static constexpr const char* kclass = "class";
   static constexpr const char* ml_results = "ml_results";
   static constexpr const char* model_explanation = "model_explanation";
   static constexpr const char* model_list = "model_list";
   static constexpr const char* model_quality = "model_quality";
   static constexpr const char* n_columns = "n_columns";
   static constexpr const char* n_rows = "n_rows";
   static constexpr const char* n_selected_columns = "n_selected_columns";
   static constexpr const char* n_selected_rows = "n_selected_rows";
   static constexpr const char* notes = "notes";
   static constexpr const char* onnx_inputs_info = "onnx_inputs_info";
   static constexpr const char* onnx_outputs_info = "onnx_outputs_info";
   static constexpr const char* optimization_metric = "optimization_metric";
   static constexpr const char* options = "options";
   static constexpr const char* Prediction = "Prediction";
   static constexpr const char* predictions = "predictions";
   static constexpr const char* probabilities = "probabilities";
   static constexpr const char* remove_seen = "remove_seen";
   static constexpr const char* selected_column_names = "selected_column_names";
   static constexpr const char* status = "status";
   static constexpr const char* target_column_name = "target_column_name";
   static constexpr const char* task = "task";
   static constexpr const char* train_table_name = "train_table_name";
   static constexpr const char* training_drift_metric = "training_drift_metric";
   static constexpr const char* training_params = "training_params";
   static constexpr const char* training_score = "training_score";
   static constexpr const char* training_time = "training_time";
   static constexpr const char* txt2num_dict = "txt2num_dict";
 };
 
using OPTION_VALUE_T = std::map<std::string, std::vector<std::string>>;
using txt2numeric_map_t = std::map<std::string, std::set<std::string>>;
// pair of a <key_name, value>.
using ml_record_type_t = std::pair<std::string, std::string>;

constexpr int ANONOALY_METRIC_START = 0;
constexpr int CLASS_METRIC_START = 100;
constexpr int FORCAST_METRIC_START = 200;
constexpr int RECOMMEND_METRIC_START = 300;
constexpr int REGRESSION_METRIC_START = 400;
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTO_ML_INFO_H__