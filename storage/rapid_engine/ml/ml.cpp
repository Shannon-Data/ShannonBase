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

void Query_arbitrator::load_model() {}

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
  BoosterHandle booster;
  int out_num_iterations;
  int status = LGBM_BoosterCreateFromModelfile(m_model_path.c_str(), &out_num_iterations, &booster);
  if (status != 0) {
    return Query_arbitrator::WHERE2GO::TO_INNODB;
  }

  std::vector<double> features;
  int num_features = features.size();
  int num_samples = 1;

  std::vector<double> out_result(1);
  int64_t out_len;
  // predict[for an example]
  status = LGBM_BoosterPredictForMat(booster, features.data(), C_API_DTYPE_FLOAT64, num_samples, num_features, 1, 0,
                                     C_API_PREDICT_NORMAL, -1, "", &out_len, out_result.data());

  if (status != 0) {
    LGBM_BoosterFree(booster);
    return Query_arbitrator::WHERE2GO::TO_INNODB;
  }

  // std::cout << "Prediction result: " << out_result[0] << std::endl;
  LGBM_BoosterFree(booster);
  return Query_arbitrator::WHERE2GO::TO_RAPID;
}

}  // namespace ML
}  // namespace ShannonBase