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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_OPTIMIZER_H__
#define __SHANNONBASE_OPTIMIZER_H__

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/field_types.h"  //enum_field_types

#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/optimizer/query_plan.h"
#include "storage/rapid_engine/optimizer/rules/rule.h"

class THD;
class Query_expression;
class JOIN;
class AccessPath;
class Item_func;
class Item_cond;
class Item_func_between;
class Item_func_in;
class Item_func_like;
struct Index_lookup;
class SEL_ARG;
class QUICK_RANGE;

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
 * @brief Main optimizer class for query optimization
 *
 * The Optimizer class handles query optimization using cost-based optimization
 * techniques and rule-based transformations.
 */
class Optimizer : public MemoryObject {
 public:
  Optimizer() = default;
  explicit Optimizer(std::shared_ptr<Query_expression> &, const std::shared_ptr<CostEstimator> &) {}

  template <typename T, typename... Args>
  T *add_rule(Args &&...args) {
    static_assert(std::is_base_of<Rule, T>::value, "T must derive from Rule");
    auto rule = std::make_unique<T>(std::forward<Args>(args)...);
    T *ptr = rule.get();
    m_optimize_rules.emplace_back(std::move(rule));
    return ptr;
  }

  void AddDefaultRules();

  Plan Optimize(const OptimizeContext *context, const THD *thd, const JOIN *join);

  /**
   * @brief Builds customized access paths for secondary engine optimization
   *
   * Creates specialized AccessPath types (Vectorized Table Scan, GPU Join, etc.)
   * for secondary engine execution. The function examines each AccessPath type
   * and creates optimized versions when possible.
   *
   * Why not use original AccessPath directly?
   * - Future extensions may require custom AccessPath types (e.g., RapidAccessPath)
   * - Original AccessPath tree will be freed and replaced with rapid_path
   * - Enables specialized iterators in PathGenerator::CreateIteratorFromAccessPath
   *
   * @param context Optimization context with vectorization capabilities
   * @param path Input AccessPath to optimize
   * @param join JOIN structure for query context
   * @return AccessPath* Optimized AccessPath, or nullptr if no optimization applied
   */
  static AccessPath *OptimizeAndRewriteAccessPath(OptimizeContext *context, AccessPath *path, const JOIN *join);

  /**
   * Convert MySQL Item to Rapid Predicate
   *
   * This function translates MySQL's Item expression tree into our custom
   * Predicate representation for optimized execution in IMCS.
   *
   * @param thd Thread handler
   * @param item MySQL Item to convert
   * @return Converted Predicate, or nullptr if not convertible
   */
  static std::unique_ptr<Imcs::Predicate> convert_item_to_predicate(THD *thd, Item *item);

  /**
   * Convert Index_lookup to Predicate
   *
   * Index_lookup contains range conditions for index scans.
   * This function extracts the range conditions and converts them to predicates.
   *
   * The items[] array may contain:
   * 1. Constants lookup(for simple WHERE conditions like "id = 5")
   * 2. Dynamic lookup Field references from other tables (for JOIN conditions like "t1.id = t2.id")
   *
   * For case 2, we CANNOT convert to a simple predicate with a constant value.
   * Instead, we need to recognize this as a JOIN condition that will be evaluated dynamically during execution.
   *
   * @param thd Thread handler
   * @param lookup Index lookup information
   * @param table source table object.
   * @return Converted Predicate representing the index range conditions
   */
  static std::unique_ptr<Imcs::Predicate> convert_item_to_predicate(THD *thd, Index_lookup *lookup,
                                                                    TABLE *table = nullptr);

  // Rapid cost calculator, HGO AccessPath estimation.
  static inline bool RapidEstimateJoinCostHGO(THD *thd, const JOIN &join, double *secondary_engine_cost) {
    // using Rapid Engine cost estimation algorithm.
    auto *cost_model =
        ShannonBase::Optimizer::CostModelServer::Instance(ShannonBase::Optimizer::CostEstimator::Type::RPD_ENG);
    if (cost_model) *secondary_engine_cost = cost_model->cost(&join);

    return false;
  }

 private:
  Plan get_query_plan(OptimizeContext *context, THD *thd, const JOIN *join);
  /**
   * Determines if a given access path can be vectorized based on its type and properties.
   *
   * This function analyzes the access path type and its specific characteristics to decide
   * whether vectorized execution is supported. Some path types are inherently vectorizable
   * (e.g., TABLE_SCAN, AGGREGATE), while others have specific constraints (e.g., HASH_JOIN
   * without disk spilling), and some cannot be vectorized at all (e.g., NESTED_LOOP_JOIN).
   *
   * @param path The access path to check for vectorization capability
   * @return true if the path can be vectorized, false otherwise
   *
   * @note This function only considers the current path in isolation, without recursively
   *       checking its children. Use CheckChildVectorization() for full path tree analysis.
   */
  static bool CanPathBeVectorized(AccessPath *path);

  /**
   * Recursively checks if all children in an access path tree support vectorization.
   *
   * This function performs a depth-first traversal of the access path tree, verifying
   * that every node in the execution path supports vectorized execution. It combines
   * the capabilities of the current node (via CanPathBeVectorized()) with recursive
   * checks of all child paths.
   *
   * For AGGREGATE operations to be vectorized, this ensures that the entire input
   * pipeline feeding the aggregation can be executed in vectorized mode.
   *
   * @param child_path The root of the access path subtree to check
   * @return true if all paths in the subtree support vectorization, false if any
   *         node in the path tree cannot be vectorized
   *
   * @note Returns true for nullptr inputs, treating empty paths as trivially vectorizable.
   * @see CanPathBeVectorized()
   */
  static bool CheckChildVectorization(AccessPath *child_path);

  Plan translate_access_path(OptimizeContext *ctx, THD *thd, AccessPath *path, const JOIN *join);

  void fill_aggregate_info(LocalAgg *node, const JOIN *join);

  /**
   * Helper functions for Item to Predicate conversion
   */
  static std::unique_ptr<Imcs::Predicate> convert_func_item_to_predicate(THD *thd, Item_func *func);
  static std::unique_ptr<Imcs::Predicate> convert_cond_item_to_predicate(THD *thd, Item_cond *cond);

  static std::unique_ptr<Imcs::Predicate> convert_comparison_to_predicate(THD *thd, Item_func *func,
                                                                          Imcs::PredicateOperator op);
  static std::unique_ptr<Imcs::Predicate> convert_between_to_predicate(THD *thd, Item_func_between *between);
  static std::unique_ptr<Imcs::Predicate> convert_in_to_predicate(THD *thd, Item_func_in *in_func);
  static std::unique_ptr<Imcs::Predicate> convert_isnull_to_predicate(THD *thd, Item_func *func);
  static std::unique_ptr<Imcs::Predicate> convert_isnotnull_to_predicate(THD *thd, Item_func *func);
  static std::unique_ptr<Imcs::Predicate> convert_like_to_predicate(THD *thd, Item_func_like *like_func,
                                                                    bool is_negated);
  /**
   * Convert QUICK_RANGE to Predicate
   *
   * QUICK_RANGE represents a range scan on an index.
   * It contains min/max values and flags for each key part.
   *
   * @param thd Thread handler
   * @param range Quick range information
   * @param key_part Key part information
   * @return Converted Predicate representing the range
   */
  static std::unique_ptr<Imcs::Predicate> convert_quick_range_to_predicate(THD *thd, const QUICK_RANGE *range,
                                                                           const KEY_PART_INFO *key_part);
  /**
   * Convert SEL_ARG tree to Predicate
   *
   * SEL_ARG represents the range tree for index optimization.
   * It's a red-black tree structure containing range intervals.
   *
   * @param thd Thread handler
   * @param sel_arg Range tree root
   * @param key_part Key part information
   * @return Converted Predicate
   */
  static std::unique_ptr<Imcs::Predicate> convert_sel_arg_to_predicate(THD *thd, const SEL_ARG *sel_arg,
                                                                       const KEY_PART_INFO *key_part);
  static Imcs::PredicateValue extract_value_from_key_part(THD *thd, const uchar *key_ptr, const KEY_PART_INFO *key_part,
                                                          enum_field_types field_type);
  static Imcs::PredicateValue extract_value_from_sel_arg(THD *thd, const SEL_ARG *sel_arg, enum_field_types field_type);
  static Imcs::PredicateValue extract_value_from_sel_arg_min(THD *thd, const SEL_ARG *sel_arg,
                                                             enum_field_types field_type);
  static Imcs::PredicateValue extract_value_from_sel_arg_max(THD *thd, const SEL_ARG *sel_arg,
                                                             enum_field_types field_type);
  static Imcs::PredicateValue extract_value_from_item(THD *thd, Item *item,
                                                      enum_field_types target_type = MYSQL_TYPE_NULL,
                                                      Field *target_field = nullptr);
  static Imcs::PredicateOperator swap_operator(Imcs::PredicateOperator op);

  std::atomic<bool> m_registered{false};
  std::vector<std::unique_ptr<Rule>> m_optimize_rules;
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