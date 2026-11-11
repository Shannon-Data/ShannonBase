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
#ifndef __SHANNONBASE_ML_EXPLANATION_H__
#define __SHANNONBASE_ML_EXPLANATION_H__

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "ml_utils.h"
#include "mysqld_error.h"

namespace ShannonBase {
namespace ML {
// Extras (including the target and a primary key) are allowed; absent features
// must never be interpreted as a real zero-valued observation.
inline bool validate_explanation_features(const std::vector<std::string> &features, const OPTION_VALUE_T &input) {
  if (features.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "Model has no explanation features");
    return false;
  }
  for (const auto &feature : features) {
    if (!input.contains(feature)) {
      const std::string message = "Explanation input is missing model feature: " + feature;
      my_error(ER_ML_FAIL, MYF(0), message.c_str());
      return false;
    }
  }
  return true;
}

// Rank per observation, by magnitude (negative contributions are relevant too).
// Original feature order breaks ties, making the 100-feature boundary stable.
inline std::vector<size_t> explanation_feature_order(const std::vector<double> &values) {
  std::vector<size_t> order(values.size());
  std::iota(order.begin(), order.end(), 0);
  const size_t count = std::min<size_t>(100, order.size());
  std::partial_sort(order.begin(), order.begin() + count, order.end(), [&](size_t a, size_t b) {
    const double left = std::abs(values[a]), right = std::abs(values[b]);
    return left == right ? a < b : left > right;
  });
  order.resize(count);
  return order;
}
}  // namespace ML
}  // namespace ShannonBase
#endif
