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
#ifndef __SHANNONBASE_OPTIMIZER_H__
#define __SHANNONBASE_OPTIMIZER_H__

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/rapid_engine/include/rapid_object.h"
class THD;
class Query_expression;
class JOIN;
class AccessPath;

namespace ShannonBase {
namespace Optimizer {
class Statistics;
/**
 * @brief Optimization context containing vectorization capability flag and statistics
 *
 * This structure holds context information used during query optimization,
 * including whether operations can be vectorized and associated statistics data.
 */
typedef struct OptimizeContext {
  bool can_vectorized;         ///< Flag indicating if operations can be vectorized.
  Statistics *Rpd_statistics;  // to replace with the real statistics data.
} OptimizeContext;

class Rule;
class CostEstimator;
class CardinalityEstimator;

/**
 * @brief Optimizes and rewrites access paths for secondary engine with custom optimizer
 *
 * This function processes AccessPath trees to create optimized versions for
 * secondary engine execution, including vectorized table scans, GPU joins, etc.
 *
 * @param context Optimization context containing vectorization flags and statistics
 * @param path The AccessPath to optimize and rewrite
 * @param join The JOIN structure containing query information
 * @return AccessPath* Newly created optimized AccessPath
 */
AccessPath *OptimizeAndRewriteAccessPath(OptimizeContext *context, AccessPath *path, const JOIN *join);

/**
 * @brief Main optimizer class for query optimization
 *
 * The Optimizer class handles query optimization using cost-based optimization
 * techniques and rule-based transformations.
 */
class Optimizer : public MemoryObject {
 public:
  explicit Optimizer(std::shared_ptr<Query_expression> &, const std::shared_ptr<CostEstimator> &);
};

/**
 * @brief Metrics collector for optimization rules
 *
 * Tracks performance metrics for individual optimization rules,
 * including rule name and execution duration.
 */
class RuleMetrics {
 public:
  explicit RuleMetrics(const std::string &rule_name, const std::chrono::nanoseconds duration)
      : m_rule_name{rule_name}, m_duration{duration} {}

 private:
  std::string m_rule_name;
  std::chrono::nanoseconds m_duration;
};

/**
 * @brief High-resolution timer for performance measurement
 *
 * Utility class for measuring execution times during optimization.
 * Uses steady_clock for monotonic timing measurements.
 */
class Timer final {
 public:
  Timer();
  std::chrono::nanoseconds lap();
  std::string lap_formatted();

 private:
  std::chrono::steady_clock::time_point m_begin;
};

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OPTIMIZER_H__