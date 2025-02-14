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
#include <string>

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


using OPTION_VALUE_T = std::map<std::string, std::vector<std::string>>;

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTO_ML_INFO_H__