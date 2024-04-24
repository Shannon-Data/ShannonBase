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
#ifndef __SHANNONBASE_ML_ALGORITHM_H__
#define __SHANNONBASE_ML_ALGORITHM_H__

#include <string>

namespace ShannonBase {
namespace ML {

enum class ML_TASK_TYPE : int {
  UNKNOWN = -1,
  CLASSIFICATION,
  REGRESSION,
  FORECASTING,
  ANOMALY_DETECTION,
  RECOMMENDATION
};

enum class MODEL_CATALOG_FIELD_INDEX : int {
  MODEL_ID = 0,
  MODEL_HANDLE,
  MODEL_OBJECT,
  MODEL_OWNER,
  BUILD_TIMESTAMP,
  TARGET_COLUMN_NAME,
  TRAIN_TABLE_NAME,
  MODEL_OBJECT_SIZE,
  MODEL_TYPE,
  TASK,
  COLUMN_NAMES,
  MODEL_EXPLANATION,
  LAST_ACCESSED,
  MODEL_METADATA,
  NOTES
};

//the interface of ml tasks.
class ML_algorithm {
  public:
    ML_algorithm() {}
    virtual ~ML_algorithm() {}

    virtual int train() = 0;
    virtual int predict() = 0;
    virtual int load(std::string model_handle_name, std::string user_name) = 0;
    virtual int load_from_file (std::string modle_file_full_path, std::string model_handle_name) = 0;
    virtual int unload(std::string model_handle_name) = 0;
    virtual int import(std::string model_handle_name, std::string user_name, std::string& content) = 0;
    virtual double score() = 0;
    virtual int explain_row() = 0;
    virtual int explain_table() = 0;
    virtual int predict_row() = 0;
    virtual int predict_table() =0;
    virtual ML_TASK_TYPE type() =0;
};

} //ml
} //shannonbase
#endif //__SHANNONBASE_ML_ALGORITHM_H__