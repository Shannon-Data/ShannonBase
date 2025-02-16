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

#include <string>

#include "include/my_inttypes.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "ml_utils.h"                         //ml utils
#include "storage/innobase/include/ut0dbg.h"  //for ut_a

#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"
#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

// clang-format off
// clang-format on
namespace ShannonBase {
namespace ML {

const std::vector<std::string> ML_regression::metrics = {"neg_mean_absolute_error", "neg_mean_squared_error",
                                                         "neg_mean_squared_log_error", "neg_median_absolute_error",
                                                         "r2"};

ML_regression::ML_regression() {}
ML_regression::~ML_regression() {}

int ML_regression::train() {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine";
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) return HA_ERR_GENERIC;

  return 0;
}

int ML_regression::predict() { return 0; }

int ML_regression::load(std::string &model_content) {
  // the definition of this table, ref: `ml_train.sql`
  BoosterHandle bt_handler;
  int out_num_iterations;
  if (LGBM_BoosterLoadModelFromString(model_content.c_str(), &out_num_iterations, &bt_handler) == -1)
    return HA_ERR_GENERIC;

  m_handler = bt_handler;
  return 0;
}

int ML_regression::load_from_file(std::string &modle_file_full_path, std::string &model_handle_name) {
  // to update the `MODEL_CATALOG.MODEL_OBJECT`
  if (check_valid_path(modle_file_full_path.c_str(), modle_file_full_path.length()) || !model_handle_name.length())
    return HA_ERR_GENERIC;

  return 0;
}

int ML_regression::unload(std::string &model_handle_name) {
  if (!model_handle_name.length()) {
    return HA_ERR_GENERIC;
  }

  if (m_handler) {
    BoosterHandle bt_handler = m_handler;
    LGBM_BoosterFree(bt_handler);
    m_handler = nullptr;
  }
  return 0;
}

int ML_regression::import(std::string &model_handle_name, std::string &user_name, std::string &content) {
  assert(model_handle_name.length() && user_name.length() && content.length());

  BoosterHandle bt_handler;
  int out_num_iterations;
  if (LGBM_BoosterLoadModelFromString(content.c_str(), &out_num_iterations, &bt_handler) == -1) return HA_ERR_GENERIC;

  m_handler = bt_handler;

  return 0;
}

double ML_regression::score(std::string &sch_tb_name [[maybe_unused]], std::string &target_name [[maybe_unused]],
                            std::string &model_handle [[maybe_unused]], std::string &metric_str [[maybe_unused]],
                            Json_wrapper &option [[maybe_unused]]) {
  return 0;
}

int ML_regression::explain(std::string &sch_tb_name [[maybe_unused]], std::string &target_column_name [[maybe_unused]],
                           std::string &model_handle_name [[maybe_unused]],
                           Json_wrapper &exp_options [[maybe_unused]]) {
  return 0;
}

int ML_regression::explain_row() { return 0; }

int ML_regression::explain_table() { return 0; }

int ML_regression::predict_row() { return 0; }

int ML_regression::predict_table() { return 0; }

ML_TASK_TYPE_T ML_regression::type() { return ML_TASK_TYPE_T::REGRESSION; }

}  // namespace ML
}  // namespace ShannonBase