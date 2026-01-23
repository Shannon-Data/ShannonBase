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
#ifndef __SHANNONBASE_JOIN_REORDER_RULE_H__
#define __SHANNONBASE_JOIN_REORDER_RULE_H__

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "include/my_base.h"  // ha_rows

#include "storage/rapid_engine/optimizer/query_plan.h"
#include "storage/rapid_engine/optimizer/rules/rule.h"

class Item;
class Item_field;

namespace ShannonBase {
namespace Optimizer {
/**
 * JoinReOrder Rule - Header File
 *
 * CONTEXT: Runs AFTER MySQL optimizer in RapidOptimize()
 *
 * Key Challenge:
 * - MySQL has already optimized join order using InnoDB statistics
 * - We need to re-evaluate using IMCS-specific capabilities
 *
 * Conservative Strategy:
 * - Keep MySQL order as baseline (cost_mysql)
 * - Only reorder if IMCS cost improvement > 20% threshold
 * - Use IMCS Storage Index selectivity (MySQL doesn't know about IMCU pruning)
 * - Account for vectorized execution costs
 *
 * When to Reorder:
 * ✓ Storage Index reduces cardinality significantly (>2x vs MySQL estimate)
 * ✓ Small table with high NDV joins large filtered table
 * ✓ IMCS cost model shows >20% improvement
 *
 * When NOT to Reorder:
 * ✗ STRAIGHT_JOIN hint present
 * ✗ Improvement < 20% threshold (avoid risk of regression)
 * ✗ Query uses features MySQL optimized well (e.g., covering index)
 */
/**
 * JoinReOrder Rule
 *
 * Re-evaluates MySQL's join order using IMCS-specific statistics
 */
class JoinReOrder : public Rule {
 public:
  JoinReOrder() = default;
  virtual ~JoinReOrder() = default;

  std::string name() override { return std::string("JoinReOrder"); }
  void apply(Plan &root) override;

  /**
   * Join node representation
   */
  struct JoinNode {
    PlanNode *node{nullptr};
    bool is_hash_join{false};
    std::vector<Item *> join_conditions;
  };

  /**
   * Table information from IMCS (with MySQL comparison)
   */
  struct TableInfo {
    size_t table_id{0};
    Plan *scan_node{nullptr};

    // IMCS estimates
    ha_rows cardinality{0};  // After IMCU pruning
    double selectivity{1.0};
    double cost{0.0};

    // MySQL estimates (for comparison)
    ha_rows mysql_cardinality{0};  // MySQL's estimate
    double mysql_cost{0.0};
  };

  /**
   * Join edge in the join graph
   */
  struct JoinEdge {
    int left_table{-1};
    int right_table{-1};
    uint32 left_column{0};
    uint32 right_column{0};
    Item *condition{nullptr};
    double selectivity{1.0};
  };

  /**
   * Join graph representation
   */
  struct JoinGraph {
    size_t num_tables{0};
    std::vector<TableInfo> tables;
    std::vector<std::vector<JoinEdge>> edges;
  };

  /**
   * DP state for join reordering
   */
  struct DPState {
    Plan plan{nullptr};
    double cost{0.0};
    ha_rows cardinality{0};
    size_t left_subset{0};
    size_t right_subset{0};
  };

 private:
  // Thresholds and limits
  static constexpr size_t MAX_DP_TABLES = 8;                 // Use DP for ≤8 tables
  static constexpr double REORDER_BENEFIT_THRESHOLD = 0.20;  // Only reorder if >20% improvement

  /**
   * Calculate cost of current plan (MySQL's order)
   */
  double calculate_current_plan_cost(const Plan &root, const JoinGraph &graph);

  /**
   * Re-estimate cardinality using IMCS statistics
   */
  ha_rows re_estimate_cardinality(const PlanNode *node, const JoinGraph &graph);

  /**
   * Calculate hash join cost (IMCS vectorized model)
   */
  double calculate_hash_join_cost(ha_rows left_card, ha_rows right_card);

  /**
   * Collect all join nodes and base table scans
   */
  void collect_join_nodes(PlanNode *node, std::vector<JoinNode> &joins, std::vector<Plan *> &scans);

  /**
   * Build join graph with IMCS re-estimation
   */
  JoinGraph build_join_graph(const std::vector<JoinNode> &joins, const std::vector<Plan *> &scans);

  /**
   * Collect all join edges between two subsets
   */
  std::vector<Item *> collect_edges_between(size_t left_subset, size_t right_subset, const JoinGraph &graph);

  /**
   * Extract join edge from condition
   */
  JoinEdge extract_join_edge(Item *condition, const std::vector<Plan *> &scans);

  /**
   * Find which table a field belongs to
   */
  int find_table_for_field(Item_field *field, const std::vector<Plan *> &scans);

  /**
   * Estimate Storage Index filtering selectivity
   */
  double estimate_storage_index_selectivity(ScanTable *scan);

  /**
   * Estimate join selectivity using IMCS statistics
   */
  double estimate_join_selectivity(Item_field *left_field, Item_field *right_field, const std::vector<Plan *> &scans);

  /**
   * Reorder using DP (returns nullptr if MySQL order is better)
   */
  Plan reorder_with_dp(const JoinGraph &graph, Plan &root, double baseline_cost);

  /**
   * Reorder using greedy (returns nullptr if MySQL order is better)
   */
  Plan reorder_with_greedy(const JoinGraph &graph, Plan &root, double baseline_cost);

  /**
   * Estimate result cardinality of join
   */
  ha_rows estimate_join_cardinality(const DPState &left, const DPState &right, const JoinGraph &graph);

  /**
   * Find smallest table in join graph
   */
  size_t find_smallest_table(const JoinGraph &graph);

  /**
   * Check if there's a join edge between two subsets
   */
  bool has_join_edge(size_t left_subset, size_t right_subset, const JoinGraph &graph);

  /**
   * Enumerate all subsets of size k
   */
  void enumerate_subsets(size_t n, size_t k, std::function<void(size_t)> enum_subsets_fuc);

  /**
   * Enumerate all partitions of a subset
   */
  void enumerate_partitions(size_t subset, std::function<void(size_t, size_t)> enum_partitions_fuc);

  /**
   * Reconstruct join plan from DP solution
   */
  Plan reconstruct_join_plan(size_t subset, std::vector<DPState> &dp, const JoinGraph &graph, Plan &original_root);

  /**
   * Reconstruct plan from greedy algorithm
   */
  Plan reconstruct_greedy_plan(const std::vector<size_t> &order, const JoinGraph &graph, Plan &original_root);

  /**
   * Estimate greedy join cost
   */
  double estimate_greedy_join_cost(ha_rows left_card, ha_rows right_card, const JoinGraph &graph);

  /**
   * Estimate result cardinality for greedy
   */
  ha_rows estimate_result_cardinality(ha_rows left_card, ha_rows right_card, double selectivity);
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_PREDICATE_PUSHDOWN_RULE_H__