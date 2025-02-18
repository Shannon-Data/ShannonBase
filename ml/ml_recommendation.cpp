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

#include "ml_recommendation.h"

#include <sstream>
#include <string>

#include "my_base.h"
#include "mysqld_error.h"

namespace ShannonBase {
namespace ML {

const std::vector<std::string> explicit_metrics = {"neg_mean_absolute_error", "neg_mean_squared_error",
                                                   "neg_root_mean_squared_error", "r2"};
const std::vector<std::string> implicit_metrics = {""};
const std::vector<std::string> both_metrics = {"precision_at_k", "precision_at_k", "recall_at_k", "hit_ratio_at_k",
                                               "ndcg_at_k"};
ML_recommendation::ML_recommendation() {}

ML_recommendation::~ML_recommendation() {}

int ML_recommendation::train() { return 0; }

int ML_recommendation::predict() { return 0; }

int ML_recommendation::load(std::string &model_content [[maybe_unused]]) { return 0; }

int ML_recommendation::load_from_file(std::string &modle_file_full_path [[maybe_unused]],
                                      std::string &model_handle_name [[maybe_unused]]) {
  return 0;
}

int ML_recommendation::unload(std::string &model_handle_name [[maybe_unused]]) { return 0; }

int ML_recommendation::import(Json_wrapper &model_object [[maybe_unused]],
                              Json_wrapper &model_metadata [[maybe_unused]],
                              std::string &model_handle_name [[maybe_unused]]) {
  return 0;
}

double ML_recommendation::score(std::string &sch_tb_name [[maybe_unused]], std::string &target_name [[maybe_unused]],
                                std::string &model_handle [[maybe_unused]], std::string &metric_str [[maybe_unused]],
                                Json_wrapper &option [[maybe_unused]]) {
  return 0;
}

int ML_recommendation::explain(std::string &sch_tb_name [[maybe_unused]],
                               std::string &target_column_name [[maybe_unused]],
                               std::string &model_handle_name [[maybe_unused]],
                               Json_wrapper &exp_options [[maybe_unused]]) {
  std::ostringstream err;
  err << "recommendation does not soupport explain operation";
  my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
  return HA_ERR_GENERIC;
}

int ML_recommendation::explain_row() { return 0; }

int ML_recommendation::explain_table() { return 0; }

int ML_recommendation::predict_row() { return 0; }

int ML_recommendation::predict_table() { return 0; }

ML_TASK_TYPE_T ML_recommendation::type() { return ML_TASK_TYPE_T::RECOMMENDATION; }

}  // namespace ML
}  // namespace ShannonBase