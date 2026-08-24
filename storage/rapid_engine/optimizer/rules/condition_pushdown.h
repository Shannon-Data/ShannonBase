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
#include "storage/rapid_engine/optimizer/utils.h"

class Item;
class Item_func;
class Item_cond;
class TABLE;

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
   * @return Set of TABLE pointers (unique per alias, handles self-joins)
   */
  std::unordered_set<TABLE *> get_referenced_tables(Item *item);

  /**
   * @brief Check if a predicate can be pushed down to a specific subtree
   * @param predicate Predicate to check
   * @param available_tables Tables available in the subtree
   * @return true if pushdown is safe
   */
  bool can_push_to_subtree(Item *predicate, const std::unordered_set<TABLE *> &available_tables);

  /**
   * @brief Get all tables available in a plan subtree
   * @param node Plan node
   * @return Set of TABLE pointers (unique per alias, handles self-joins)
   */
  std::unordered_set<TABLE *> get_available_tables(const Plan &node);

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

    Item *combined_cond = ShannonBase::Optimizer::Utils::combine_with_and(pending_filters);
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
   * @brief Try to push a LocalAgg below its HashJoin child, in place (no clone, no second
   * aggregation phase).
   *
   * Only safe when every one of these holds -- each is a real correctness precondition, not a
   * heuristic:
   *  1. The join is a HashJoin provably of JoinType::INNER (HashJoin::join_type); other join
   *     kinds are not provably safe to push a GROUP BY below.
   *  2. GROUP BY and every aggregate argument resolve exclusively to one side (push_side); the
   *     other side (keep_side) is left untouched.
   *  3. HashJoin::child_multiplicity[keep_side] is kAtMostOne (Utils::prove_at_most_one): keep_side
   *     contributes at most one row per join-key value, so relocating the already-grouped
   *     push_side rows below the join cannot re-introduce fan-out duplicates that would need a
   *     second (global) aggregation pass to collapse -- which Rapid's aggregate iterator has no
   *     way to run today (see LocalAgg::join / PrepareAggregateFields; a query has exactly one
   *     JOIN's group_fields/sum_funcs, so only one physical aggregation phase can exist).
   *  4. push_side is a bare ScanTable or a Filter directly wrapping one (get_pushable_scan_table),
   *     so there is a concrete TABLE to check condition (5) against and a leaf to relocate.
   *  5. Every join-key column the equi-join conditions read from push_side's table is already a
   *     GROUP BY column (group_by_covers_join_keys). Rapid's grouped-aggregate output otherwise
   *     only guarantees GROUP BY/aggregate-source columns are consistent per group; anything else
   *     the join condition might read carries an arbitrary representative row's value.
   * Also declines when the aggregate has ROLLUP (extra super-aggregate rows change join
   * semantics) or a dependent hash_output_order (relocating below the join would scramble an
   * ordering a parent operator relies on).
   *
   * @param agg_node Aggregation plan node whose child is a HashJoin
   * @return The relocated plan (join becomes the new subtree root) if the rewrite applied,
   *         otherwise agg_node unchanged
   */
  Plan try_push_below_join(Plan agg_node);

  /**
   * @brief Relocate agg_node in place to wrap join->children[push_side], leaving the HashJoin as
   * the new subtree root. Reuses agg_node as-is (same JOIN*, same group_by/aggregates Items) --
   * it is moved, not cloned, so there is still exactly one physical aggregation phase.
   * @param agg_node Aggregation plan node (already proven safe to relocate by try_push_below_join)
   * @param join Join plan node
   * @param push_side 0 to push below the join's left child, 1 for its right child
   * @return The join plan, now hosting agg_node in place of join->children[push_side]
   */
  Plan push_aggregation_to_join_side(Plan agg_node, Plan join, int push_side);

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

  /**
   * @brief Resolves a HashJoin child to the single real TABLE it scans, looking through at most
   * one wrapping Filter. Returns nullptr for anything deeper/other (a further join, another
   * aggregate, etc.) since those need transitive proof this rule does not attempt.
   * @param side HashJoin child plan node (join->children[0] or [1])
   * @return The scanned TABLE, or nullptr if side is not a bare Scan or Filter-over-Scan
   */
  TABLE *get_pushable_scan_table(const PlanNode *side);

  /**
   * @brief Checks precondition (5) of try_push_below_join: every column of push_table that the
   * equi-join conditions read is already one of agg's GROUP BY columns.
   * @param agg Aggregation plan node being considered for relocation
   * @param join_conditions The HashJoin's equi-join conditions
   * @param push_table The real TABLE behind the join child agg would be relocated onto
   * @return true if at least one join-key column from push_table was found and every such column
   *         is covered by agg->group_by
   */
  bool group_by_covers_join_keys(const LocalAgg *agg, const std::vector<Item *> &join_conditions, TABLE *push_table);
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
   * @param pending_filesort filersort for TopN(using by order by to do sort)
   * @return Modified plan node
   */
  Plan push_limit_recursive(Plan &node, ha_rows pending_limit, ha_rows pending_offset, ORDER *pending_order,
                            Filesort *pending_filesort = nullptr);

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
   * @param filesort sort agorithm of ORDER BY clause
   * @return New TopN plan node
   */
  Plan create_topn_node(Plan child, ha_rows limit, ha_rows offset, ORDER *order, Filesort *pending_filesort);

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