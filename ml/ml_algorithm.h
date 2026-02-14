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
#include "ml_info.h"

#include "sql-common/json_dom.h"  //Json_wrapper.
class THD;
namespace ShannonBase {
namespace ML {
enum class MODEL_CATALOG_FIELD_INDEX : int {
  MODEL_ID = 0,        // Auto-increment primary key
  MODEL_HANDLE,        // Unique model handle (string)
  MODEL_OBJECT,        // Currently NULL in main table (large model stored in sharding table)
  MODEL_OWNER,         // Model owner
  BUILD_TIMESTAMP,     // New: Model build timestamp
  TARGET_COLUMN_NAME,  // New: Target column name
  TRAIN_TABLE_NAME,    // New: Training table name
  MODEL_OBJECT_SIZE,   // Total bytes of model object
  MODEL_TYPE,          // Reserved (currently can be NULL)
  TASK,                // Task type: CLASSIFICATION / REGRESSION / ...
  COLUMN_NAMES,        // Feature column names JSON array
  MODEL_EXPLANATION,   // Model explainability (SHAP, etc., for future extension)
  LAST_ACCESSED,       // Last accessed time (for LRU cleanup)
  MODEL_METADATA,      // Complete metadata large JSON (includes txt2num_dict, training_params, etc.)
  NOTES,               // Notes

  // Always placed at the end, used to calculate total field count
  END_OF_COLUMN_COUNT
};

enum class MODEL_OBJECT_CATALOG_FIELD_INDEX : int { CHUNK_ID = 0, MODEL_HANDLE, MODEL_OBJECT, END_OF_COLUMN_ID };

// the interface of ml tasks.
class ML_algorithm {
 public:
  ML_algorithm() {}
  virtual ~ML_algorithm() = default;

  virtual int train(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata) = 0;
  virtual int load(THD *thd, std::string &model_content) = 0;
  virtual int load_from_file(THD *thd, std::string &model_file_full_path, std::string &model_handle_name) = 0;
  virtual int unload(THD *thd, std::string &model_handle_name) = 0;
  virtual int import(THD *thd, Json_wrapper &model_object, Json_wrapper &model_metadata,
                     std::string &model_handle_name) = 0;
  virtual double score(THD *thd, std::string &sch_tb_name, std::string &target_name, std::string &model_handle,
                       std::string &metric_str, Json_wrapper &option) = 0;

  virtual int explain(THD *thd, std::string &sch_tb_name, std::string &target_column_name,
                      std::string &model_handle_name, Json_wrapper &exp_option) = 0;
  virtual int explain_row(THD *thd) = 0;
  virtual int explain_table(THD *thd) = 0;
  virtual int predict_row(THD *thd, Json_wrapper &input_data, std::string &model_handle_name, Json_wrapper &option,
                          Json_wrapper &result) = 0;
  virtual int predict_table(THD *thd, std::string &sch_tb_name, std::string &model_handle_name,
                            std::string &out_sch_tb_name, Json_wrapper &options) = 0;
  virtual ML_TASK_TYPE_T type() = 0;
};
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_ALGORITHM_H__