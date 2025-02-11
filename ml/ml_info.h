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

extern std::map<std::string, std::string> Loaded_models;

enum class ML_TASK_TYPE { UNKNOWN = -1, CLASSIFICATION, REGRESSION, FORECASTING, ANOMALY_DETECTION, RECOMMENDATION };

enum class model_status { CREATING = 0, READY, ERROR };

enum class model_format { VER_1, ONNX };

enum class model_quality { LOW, HIGH };

extern std::map<std::string, ML_TASK_TYPE> opt_task_map;
extern std::map<ML_TASK_TYPE, std::string> task_name_str;
extern std::map<model_status, std::string> model_status_str;
extern std::map<model_format, std::string> model_format_str;
extern std::map<model_quality, std::string> model_quality_str;

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTO_ML_INFO_H__