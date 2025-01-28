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

#include <string>
#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"
#include "include/thr_lock.h"  //TL_READ

class TABLE;
class handler;
class Json_wrapper;

namespace ShannonBase {
namespace ML {
class Utils {
 public:
  static int open_table_by_name(std::string schema_name, std::string table_name, thr_lock_type mode, TABLE **table_ptr);
  static handler *get_secondary_handler(TABLE *source_table_ptr);
  static handler *get_primary_handler(TABLE *source_table_ptr);
  static int close_table(TABLE *table);

  // do the ML jobs.
  static BoosterHandle ML_train(std::string &task_mode, const void *training_data, uint n_data, uint data_type,
                                const char **features, uint n_feature, std::string &model_content);
  static int store_model_catalog(std::string &mode_type, std::string &oper_type, const Json_wrapper *options,
                                 std::string &user_name, std::string &handler_name, std::string &model_content,
                                 std::string &target_name, std::string &source_name);

 private:
  Utils() = delete;
  virtual ~Utils() = delete;
  // disable copy ctor, operator=, etc.
};

}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ML_REGRESSION_H__