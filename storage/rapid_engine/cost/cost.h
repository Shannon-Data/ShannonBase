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
/**
 * Cost Estimator Base Class
 */
class CostEstimator : public MemoryObject {
 public:
  enum class Type : uint8_t { NONE = 0, RPD_ENG };

  // to calc the cost of `query_plan`.
  virtual double cost(const Plan &query_plan) = 0;
  // estimate join cost between two relations.
  virtual double estimate_join_cost(ha_rows left_card, ha_rows right_card) = 0;
  // estimate scan cost given rows and number of imcus.
  virtual double estimate_scan_cost(ha_rows rows, size_t num_imcus) = 0;

  virtual inline double cpu_factor() const { return m_cpu_factor; }
  virtual inline double memory_factor() const { return m_memory_factor; }
  virtual inline double io_factor() const { return m_io_factor; }

  Type m_cost_estimator_type{Type::NONE};
  double m_cpu_factor{0.0};
  double m_memory_factor{0.0};
  double m_io_factor{0.0};

  virtual ~CostEstimator() = default;
};

/**
 * RPD Engine Cost Estimator
 */
class RpdCostEstimator : public CostEstimator {
 public:
  static constexpr double VECTOR_CPU_FACTOR = 0.001;  // vectorized CPU factor
  static constexpr double HASH_BUILD_FACTOR = 0.02;   // Hash building cost factor
  static constexpr double HASH_PROBE_FACTOR = 0.01;   // Hash probing cost factor

  RpdCostEstimator(double cpu_factor, double mem_factor, double io_factor) {
    m_cost_estimator_type = CostEstimator::Type::RPD_ENG;
    m_cpu_factor = cpu_factor;
    m_memory_factor = mem_factor;
    m_io_factor = io_factor;  // IMCS in memory, IO factor is very low
  }

  /**
   * calc the cost of `query_plan`, the total query tree.
   */
  double cost(const Plan &query_plan) override;

  /**
   * estimate join cost between two relations. part of a query plan.
   */
  virtual double estimate_join_cost(ha_rows left_card, ha_rows right_card) override;

  /**
   * estimate scan cost given rows and number of imcus. part of a query plan.
   */
  virtual double estimate_scan_cost(ha_rows rows, size_t num_imcus) override;
};

/**
 * Cost Model Server Singleton
 */
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