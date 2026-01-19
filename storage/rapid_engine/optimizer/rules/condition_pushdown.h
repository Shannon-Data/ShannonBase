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
 * Predicate Pushdown Rule
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
   * Main pushdown logic - recursively process plan nodes
   * @param node Current plan node
   * @param pending_filters Filters waiting to be pushed down
   * @return Modified plan node
   */
  Plan push_down_recursive(Plan &node, std::vector<Item *> &pending_filters);

  /**
   * Try to push filters below a join
   * @param join Join node (HashJoin or NestLoopJoin)
   * @param pending_filters Filters to push
   * @return Modified join node
   */
  Plan push_below_join(Plan &join, std::vector<Item *> &pending_filters);

  /**
   * Push filters into a scan node
   * @param scan Scan node
   * @param pending_filters Filters to push
   * @return Modified scan with filters attached
   */
  Plan push_into_scan(Plan &scan, std::vector<Item *> &pending_filters);

  /**
   * Split a conjunctive condition (AND) into individual predicates
   * @param condition Input condition
   * @param predicates Output list of individual predicates
   */
  void split_conjunctions(Item *condition, std::vector<Item *> &predicates);

  /**
   * Get all tables referenced by an item
   * @param item Item to analyze
   * @return Set of table aliases/names
   */
  std::unordered_set<std::string> get_referenced_tables(Item *item);

  /**
   * Check if a predicate can be pushed down to a specific subtree
   * @param predicate Predicate to check
   * @param available_tables Tables available in the subtree
   * @return true if pushdown is safe
   */
  bool can_push_to_subtree(Item *predicate, const std::unordered_set<std::string> &available_tables);

  /**
   * Get all tables available in a plan subtree
   * @param node Plan node
   * @return Set of available table names
   */
  std::unordered_set<std::string> get_available_tables(const Plan &node);

  /**
   * Combine multiple predicates with AND
   * @param predicates List of predicates
   * @return Combined AND condition, or single predicate if only one
   */
  Item *combine_with_and(const std::vector<Item *> &predicates);

  /**
   * Create a new Filter node
   * @param child Child node
   * @param condition Filter condition
   * @return New Filter plan node
   */
  Plan create_filter_node(Plan child, Item *condition);

  /**
   * Check if an item is a simple column reference (can benefit from Storage Index)
   * @param item Item to check
   * @return true if it's a simple predicate suitable for pushdown
   */
  bool is_simple_predicate(Item *item);

  /**
   * Estimate selectivity of a predicate (for cost-based decisions)
   * @param predicate Predicate to estimate
   * @return Estimated selectivity [0.0, 1.0]
   */
  double estimate_selectivity(Item *predicate);
};

class AggregationPushDown : public Rule {
 public:
  AggregationPushDown() = default;
  virtual ~AggregationPushDown() = default;

  std::string name() override { return std::string("AggregationPushDown"); }
  void apply(Plan &root) override;
};

class TopNPushDown : public Rule {
 public:
  TopNPushDown() = default;
  virtual ~TopNPushDown() = default;

  std::string name() override { return std::string("TopNPushDown"); }
  void apply(Plan &root) override;
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__