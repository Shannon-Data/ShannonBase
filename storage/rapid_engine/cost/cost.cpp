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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/cost/cost.h"

namespace ShannonBase {
namespace Optimizer {
std::unordered_map<CostEstimator::Type, CostEstimator *> CostModelServer::instances_;
std::mutex CostModelServer::instance_mutex_;

double RpdCostEstimator::cost(const Plan &query_plan) {
  double cost = 0.0;
  return cost;
}

CostEstimator *CostModelServer::Instance(CostEstimator::Type type) {
  std::lock_guard<std::mutex> lock(instance_mutex_);

  auto it = instances_.find(type);
  if (it != instances_.end()) {
    return it->second;
  }

  CostEstimator *instance = nullptr;
  switch (type) {
    case CostEstimator::Type::RPD_ENG:
      instance = new RpdCostEstimator();
      break;
    default:
      return nullptr;
  }
  instances_[type] = instance;
  return instance;
}
}  // namespace Optimizer
}  // namespace ShannonBase