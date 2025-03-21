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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#include "ml.h"

#include "LightGBM/c_api.h"  //lightgbm
#include "sql/sql_class.h"
#include "sql/sql_optimizer.h"

namespace ShannonBase {
namespace ML {

void Query_arbitrator::load_model(const std::string &model_path) {}

/**
 * Here, the features of model, can be:
 * (1) f_mysql_total_ts_nrows,
 * (2) f_MySQLCost,
 * (3) f_count_all_base_tables
 * (4) f_count_ref_index_ts
 * (5) f_BaseTableSumNrows
 * (6) f_are_all_ts_index_ref
 *
 */
Query_arbitrator::WHERE2GO Query_arbitrator::predict(JOIN *join) {
  // to the all query plan info then use these features to do classification.
  DatasetHandle train_dataset_handler{nullptr};
  int out_num_iterations;
  int status = LGBM_BoosterCreateFromModelfile(m_model_path.c_str(), &out_num_iterations, &train_dataset_handler);
  if (status != 0) {
    return Query_arbitrator::WHERE2GO::TO_PRIMARY;
  }

  std::vector<double> features, label;
  std::vector<const char *> feature_names;
  // list the avaliable features we used in this mode, therefore, you can your own
  // mode to predict the result.
  feature_names.push_back("table_count");
  features.push_back(0.0);

  feature_names.push_back("has_having");
  features.push_back(0.0);

  feature_names.push_back("has_group_by");
  features.push_back(0.0);

  feature_names.push_back("has_rollup");
  features.push_back(0.0);

  // to add more features below.

  assert(features.size() == feature_names.size());
  int num_features = features.size();
  int num_samples = 1;

  std::vector<double> out_result(1);
  int64_t out_len;
  // predict[for an example]
  // clang-format off
  status = LGBM_BoosterPredictForMat(train_dataset_handler,
                                     features.data(),
                                     C_API_DTYPE_FLOAT64,
                                     num_samples,
                                     num_features,
                                     1,
                                     0,
                                     C_API_PREDICT_NORMAL,
                                     -1,
                                     "",
                                     &out_len, out_result.data());
  // clang-format on

  LGBM_DatasetSetField(train_dataset_handler, "label", label.data(), label.size(), C_API_DTYPE_FLOAT64);
  LGBM_DatasetSetFeatureNames(train_dataset_handler, feature_names.data(), feature_names.size());

  if (status != 0) {
    LGBM_BoosterFree(train_dataset_handler);
    return Query_arbitrator::WHERE2GO::TO_PRIMARY;
  }

  // std::cout << "Prediction result: " << out_result[0] << std::endl;
  LGBM_BoosterFree(train_dataset_handler);
  return Query_arbitrator::WHERE2GO::TO_SECONDARY;
}

}  // namespace ML
}  // namespace ShannonBase