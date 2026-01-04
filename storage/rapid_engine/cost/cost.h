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
#ifndef __SHANNONBASE_COST_H__
#define __SHANNONBASE_COST_H__
#include "storage/rapid_engine/include/rapid_types.h"

#include "include/my_base.h"
#include "sql/handler.h"  //Cost_estimator

#include "sql/opt_costconstants.h"
#include "sql/opt_costmodel.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
class CostEstimator : public MemoryObject {
 public:
  enum class Type : uint8_t { NONE = 0, RPD_ENG };

  // to calc the cost of `query_plan`.
  virtual double cost(const PlanPtr &query_plan) = 0;
  virtual inline double cpu_factor() const { return m_cpu_factor; }
  virtual inline double memory_factor() const { return m_memory_factor; }
  virtual inline double io_factor() const { return m_io_factor; }

  Type m_cost_estimator_type{Type::NONE};
  double m_cpu_factor{0.0};
  double m_memory_factor{0.0};
  double m_io_factor{0.0};

  virtual ~CostEstimator() = default;
};

class RpdCostEstimator : public CostEstimator {
 public:
  RpdCostEstimator() {
    m_cost_estimator_type = CostEstimator::Type::RPD_ENG;
    m_cpu_factor = 0.01;
    m_memory_factor = 0.25;
    m_io_factor = 1.0;
  }

  double cost(const PlanPtr &query_plan) override;
};

class CostModelServer : public Cost_model_server {
 public:
  CostModelServer() = default;

  // get a costEstimator instance.
  static CostEstimator *Instance(CostEstimator::Type type);

  static void Cleanup() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    for (auto &pair : instances_) {
      delete pair.second;
    }
    instances_.clear();
  }

 private:
  static std::unordered_map<CostEstimator::Type, CostEstimator *> instances_;
  static std::mutex instance_mutex_;
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COST_H__