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
#ifndef __SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__
#define __SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "storage/rapid_engine/optimizer/rules/rule.h"

class Item;
class Item_func;
class Item_cond;

namespace ShannonBase {
namespace Optimizer {
/**
 * @brief Predicate Pushdown Rule
 *
 * Optimization Strategy:
 * 1. Push filters below joins to reduce join input size
 * 2. Push filters into scan nodes for early filtering
 * 3. Split conjunctive predicates (AND) and push each part independently
 * 4. Respect table dependencies - don't push predicates that reference tables not yet available
 */

/**
 * Test 1: Simple push-down.
 * SQL: SELECT * FROM t WHERE id > 10
 *
 * Before:
 *   Filter (id > 10)
 *     └─ Scan(t)
 *
 * After:
 *   Filter (id > 10)
 *     └─ Scan(t)
 *
 * test 2: Join filter distribution
 * SQL: SELECT * FROM a, b WHERE a.id = b.id AND a.value > 10 AND b.name = 'foo'
 *
 * Before:
 *   Filter (a.id = b.id AND a.value > 10 AND b.name = 'foo')
 *     └─ HashJoin
 *         ├─ Scan(a)
 *         └─ Scan(b)
 *
 * After:
 *   HashJoin (a.id = b.id)
 *     ├─ Filter (a.value > 10)
 *     │   └─ Scan(a)
 *     └─ Filter (b.name = 'foo')
 *         └─ Scan(b)
 *
 * test 3: aggregation push-down
 * SQL: SELECT SUM(value) FROM t WHERE id > 10 GROUP BY category
 *
 * Before:
 *   Filter (id > 10)
 *     └─ LocalAgg (GROUP BY category)
 *         └─ Scan(t)
 *
 * After:
 *   LocalAgg (GROUP BY category)
 *     └─ Filter (id > 10)
 *         └─ Scan(t)
 */

class PredicatePushDown : public Rule {
 public:
  PredicatePushDown() = default;
  virtual ~PredicatePushDown() = default;

  void apply(Plan &root) override;
  std::string name() override { return std::string("PredicatePushDown"); }

 private:
  /**
   * @brief recursively process plan nodes
   * @param node Current plan node
   * @param pending_filters Filters waiting to be pushed down
   * @return Modified plan node
   */
  Plan push_down_recursive(Plan &node, std::vector<Item *> &pending_filters);

  /**
   * @brief Try to push filters below a join
   * @param join Join node (HashJoin or NestLoopJoin)
   * @param pending_filters Filters to push
   * @return Modified join node
   */
  Plan push_below_join(Plan &join, std::vector<Item *> &pending_filters);

  /**
   * @brief Push filters into a scan node
   * @param scan Scan node
   * @param pending_filters Filters to push
   * @return Modified scan with filters attached
   */
  Plan push_into_scan(Plan &scan, std::vector<Item *> &pending_filters);

  /**
   * @brief Split a conjunctive condition (AND) into individual predicates
   * @param condition Input condition
   * @param predicates Output list of individual predicates
   */
  void split_conjunctions(Item *condition, std::vector<Item *> &predicates);

  /**
   * @brief Get all tables referenced by an item
   * @param item Item to analyze
   * @return Set of table aliases/names
   */
  std::unordered_set<std::string> get_referenced_tables(Item *item);

  /**
   * @brief Check if a predicate can be pushed down to a specific subtree
   * @param predicate Predicate to check
   * @param available_tables Tables available in the subtree
   * @return true if pushdown is safe
   */
  bool can_push_to_subtree(Item *predicate, const std::unordered_set<std::string> &available_tables);

  /**
   * @brief Get all tables available in a plan subtree
   * @param node Plan node
   * @return Set of available table names
   */
  std::unordered_set<std::string> get_available_tables(const Plan &node);

  /**
   * @brief Combine multiple predicates with AND
   * @param predicates List of predicates
   * @return Combined AND condition, or single predicate if only one
   */
  Item *combine_with_and(const std::vector<Item *> &predicates);

  /**
   * @brief Create a new Filter node
   * @param child Child node
   * @param condition Filter condition
   * @return New Filter plan node
   */
  Plan create_filter_node(Plan child, Item *condition);

  /**
   * @brief Check if an item is a simple column reference (can benefit from Storage Index)
   * @param item Item to check
   * @return true if it's a simple predicate suitable for pushdown
   */
  bool is_simple_predicate(Item *item);

  /**
   * @brief Estimate selectivity of a predicate (for cost-based decisions)
   * @param predicate Predicate to estimate
   * @return Estimated selectivity [0.0, 1.0]
   */
  double estimate_selectivity(Item *predicate);
  double estimate_function_selectivity(Item_func *func);
  double estimate_equality_selectivity(Item_func *eq_func);
  /**
   * @brief Checks if there are any remaining predicates in pending_filters, and if so, wraps a Filter node above the
   * current node.
   * @param node The already-processed plan node
   * @param pending_filters The vector of predicates being passed down
   * @return The wrapped (or unchanged) plan node
   */
  inline Plan wrap_if_pending(Plan node, std::vector<Item *> &pending_filters) {
    if (!node || pending_filters.empty()) return node;

    Item *combined_cond = combine_with_and(pending_filters);
    pending_filters.clear();
    return create_filter_node(std::move(node), combined_cond);
  }

  /**
   * @brief Checks if the given item contains any aggregate function references.
   * @param item The item to check
   * @return true if the item contains aggregate function references, false otherwise
   */
  bool contains_aggregate_reference(Item *item);
};

class AggregationPushDown : public Rule {
 public:
  AggregationPushDown() = default;
  virtual ~AggregationPushDown() = default;

  std::string name() override { return std::string("AggregationPushDown"); }
  void apply(Plan &root) override;

 private:
  /**
   * @brief Recursively push aggregation nodes down the plan tree
   * @param node Current plan node
   * @return Modified plan node
   */
  Plan push_aggregation_recursive(Plan &node);

  /**
   * @brief Handle pushing aggregation below its child
   * @param agg_node Aggregation plan node
   * @return Modified plan node
   */
  Plan handle_aggregation_node(Plan &agg_node);

  /**
   * @brief Handle pushing aggregation through a join
   * @param join_node Join plan node
   * @return Modified plan node
   */
  Plan handle_join_with_aggregation(Plan &join_node);

  /**
   * @brief Check if two-phase aggregation can be applied
   * @param agg Aggregation plan node
   * @return true if two-phase aggregation is applicable
   */
  bool can_apply_two_phase_aggregation(const LocalAgg *agg);

  /**
   * @brief Check if an aggregate function is decomposable
   * @param agg_func Aggregate function item
   * @return true if the aggregate function is decomposable
   */
  bool is_decomposable_aggregate(const Item_func *agg_func);

  /**
   * @brief Create a two-phase aggregation plan
   * @param global_agg_node Original aggregation node
   * @return New plan with two-phase aggregation
   */
  Plan create_two_phase_aggregation(Plan global_agg_node);

  /**
   * @brief Try to push aggregation below a join
   * @param agg_node Aggregation plan node
   * @return Modified plan node
   */
  Plan try_push_below_join(Plan agg_node);

  /**
   * @brief Push aggregation to one side of the join
   * @param agg_node Aggregation plan node
   * @param join Join plan node
   * @param push_to_left true to push to left side, false for right side
   * @return Modified plan node
   */
  Plan push_aggregation_to_join_side(Plan agg_node, Plan &join, bool push_to_left);

  /**
   * @brief Get tables referenced by an item
   * @param item Item to analyze
   * @return Set of table aliases/names
   */
  std::unordered_set<std::string> get_item_tables(Item *item);

  /**
   * @brief Get available tables in a plan subtree
   * @param node Plan node
   * @return Set of available table names
   */
  std::unordered_set<std::string> get_available_tables(const Plan &node);
};

/**
 * TopN Pushdown Rule
 *
 * Optimization Strategy:
 * 1. Push LIMIT below joins when safe (no ORDER BY or simple cases)
 * 2. Convert LIMIT + ORDER BY to TopN operation
 * 3. Push TopN as close to table scan as possible
 *
 * Benefits:
 * - Reduces materialization of intermediate results
 * - Enables early termination in scans
 * - Better memory usage
 *
 * Example Transformation:
 *
 * BEFORE:
 *   Limit(100)
 *     └─ Sort(name)
 *         └─ HashJoin
 *             ├─ Scan(customers)  -- 1M rows
 *             └─ Scan(orders)      -- 10M rows
 *
 * AFTER:
 *   TopN(100, name)
 *     └─ HashJoin
 *         ├─ Scan(customers)
 *         └─ Scan(orders)
 */
class TopNPushDown : public Rule {
 public:
  TopNPushDown() = default;
  virtual ~TopNPushDown() = default;

  void apply(Plan &root) override;
  std::string name() override { return std::string("TopNPushDown"); }

 private:
  /**
   * @brief Try to push limit/topn down through the plan tree
   * @param node Current plan node
   * @param pending_limit Pending limit to push down
   * @param pending_offset Pending offset
   * @param pending_order ORDER BY for TopN (nullptr if just LIMIT)
   * @return Modified plan node
   */
  Plan push_limit_recursive(Plan &node, ha_rows pending_limit, ha_rows pending_offset, ORDER *pending_order);

  /**
   * @brief Check if we can push limit below a join
   * @param join Join node
   * @param has_order_by Whether there's an ORDER BY
   * @return true if safe to push
   */
  bool can_push_below_join(const Plan &join, bool has_order_by) const;

  /**
   * @brief Create a TopN node (combines LIMIT + ORDER BY)
   * @param child Child node
   * @param limit Limit value
   * @param offset Offset value
   * @param order ORDER BY clause
   * @return New TopN plan node
   */
  Plan create_topn_node(Plan child, ha_rows limit, ha_rows offset, ORDER *order);

  /**
   * @brief Create a simple Limit node (no ORDER BY)
   * @param child Child node
   * @param limit Limit value
   * @param offset Offset value
   * @return New Limit plan node
   */
  Plan create_limit_node(Plan child, ha_rows limit, ha_rows offset);

  /**
   * @brief Merge two limit operations
   * @param outer_limit Outer limit
   * @param outer_offset Outer offset
   * @param inner_limit Inner limit
   * @param inner_offset Inner offset
   * @param result_limit Output: merged limit
   * @param result_offset Output: merged offset
   */
  void merge_limits(ha_rows outer_limit, ha_rows outer_offset, ha_rows inner_limit, ha_rows inner_offset,
                    ha_rows &result_limit, ha_rows &result_offset);

  /**
   * @brief Check if ORDER BY only references columns from one table
   * (useful for pushing TopN to one side of join)
   * @param order ORDER BY clause
   * @param available_tables Tables available in subtree
   * @return true if ORDER BY only uses columns from available tables
   */
  bool order_by_uses_only_tables(ORDER *order, const std::unordered_set<std::string> &available_tables) const;

  /**
   * @brief Get tables referenced by ORDER BY
   * @param order ORDER BY clause
   * @return Set of table names
   */
  std::unordered_set<std::string> get_order_by_tables(ORDER *order) const;

  /**
   * @brief Get available tables in a plan subtree
   * @param node Plan node
   * @return Set of available table names
   */
  std::unordered_set<std::string> get_available_tables(const Plan &node) const;
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__