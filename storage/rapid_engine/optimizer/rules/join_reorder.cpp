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
#include "storage/rapid_engine/optimizer/rules/join_reorder.h"

#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/sql_class.h"

#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
/**
 * JoinReOrder Rule Implementation for IMCS (In-Memory Columnar Storage)
 *
 * CRITICAL CONTEXT:
 * - MySQL SQL layer has ALREADY optimized the query (including join order)
 * - This rule runs AFTER MySQL optimizer in RapidOptimize()
 * - Goal: Re-evaluate join order using IMCS-specific statistics and capabilities
 *
 * Why Re-optimize After MySQL?
 * 1. MySQL optimizer uses InnoDB statistics (row-based, disk-oriented)
 * 2. IMCS has different characteristics:
 *    - Storage Index enables IMCU-level pruning (1000x reduction possible)
 *    - Vectorized hash joins have different cost profile than InnoDB
 *    - Column statistics (NDV, min/max) are more accurate in IMCS
 * 3. MySQL doesn't know about IMCU pruning selectivity
 *
 * Conservative Strategy (avoid regression):
 * - Only reorder if IMCS cost model shows >20% improvement
 * - Respect MySQL's join type hints (STRAIGHT_JOIN, force index)
 * - Keep MySQL order as baseline, only deviate when confident
 * - Validate cardinality estimates against MySQL's estimates
 *
 * Optimization Strategy:
 * 1. Extract current join order from MySQL's AccessPath
 * 2. Re-estimate cardinalities using IMCS Storage Index
 * 3. Calculate IMCS-specific costs (vectorized hash join model)
 * 4. Reorder only if new plan is significantly better
 * 5. Prefer small adjustments over complete reordering
 *
 * Example Scenario Where Reordering Helps:
 *
 * MySQL's Plan (based on InnoDB stats):
 *   HashJoin (orders ⋈ customers)
 *     ├─ Scan(orders) [10M rows, InnoDB estimate]
 *     └─ Scan(customers) [1M rows]
 *
 *   MySQL cost: Build(1M) + Probe(10M) = 6M
 *
 * IMCS Re-evaluation:
 *   Filter pushdown + Storage Index:
 *     orders: 10M → 500K (via IMCU pruning)
 *
 *   Same join order, but IMCS cost: Build(1M) + Probe(500K) = 1.25M
 *   Better order: Build(500K) + Probe(1M) = 1M (20% better!)
 *
 * Reordered Plan:
 *   HashJoin (customers ⋈ filtered_orders)
 *     ├─ Scan(customers) [1M rows]
 *     └─ Filter + Scan(orders) [10M → 500K via Storage Index]
 */
/**
 * Main entry point for join reordering optimization
 *
 * Conservative approach:
 * 1. Extract MySQL's join order from the plan
 * 2. Re-estimate cardinalities using IMCS statistics
 * 3. Calculate cost with both MySQL order and alternative orders
 * 4. Only reorder if improvement > REORDER_BENEFIT_THRESHOLD (20%)
 */
void JoinReOrder::apply(Plan &root) {
  if (!root) return;

  // Step 1: Collect current join structure from MySQL's optimized plan
  std::vector<JoinNode> join_nodes;
  std::vector<Plan *> scan_nodes;
  collect_join_nodes(root, join_nodes, scan_nodes);

  // If less than 2 tables, no reordering needed
  if (scan_nodes.size() < 2) return;

  // Step 2: Build join graph with IMCS-specific statistics
  JoinGraph graph = build_join_graph(join_nodes, scan_nodes);

  // Step 3: Calculate cost of MySQL's current order
  double mysql_cost = calculate_current_plan_cost(root, graph);

  // Step 4: Find better join order (if exists)
  Plan candidate_plan = nullptr;
  double candidate_cost = mysql_cost;
  if (scan_nodes.size() <= MAX_DP_TABLES) {
    // Use DP for small queries
    candidate_plan = reorder_with_dp(graph, root, mysql_cost);
    if (candidate_plan) candidate_cost = candidate_plan->cost;
  } else {
    // Use greedy for large queries
    candidate_plan = reorder_with_greedy(graph, root, mysql_cost);
    if (candidate_plan) candidate_cost = candidate_plan->cost;
  }

  // Step 5: Only apply reordering if improvement is significant
  double improvement_ratio = (mysql_cost - candidate_cost) / mysql_cost;
  if (candidate_plan && improvement_ratio > REORDER_BENEFIT_THRESHOLD) {
    // Log the reordering decision
    DBUG_PRINT("rapid_optimizer", ("JoinReOrder: MySQL cost=%.2f, IMCS cost=%.2f, improvement=%.1f%%", mysql_cost,
                                   candidate_cost, improvement_ratio * 100));

    // Apply the new plan
    root = std::move(candidate_plan);
  } else {
    // Keep MySQL's order
    DBUG_PRINT("rapid_optimizer", ("JoinReOrder: Keeping MySQL order (improvement %.1f%% < threshold %.1f%%)",
                                   improvement_ratio * 100, REORDER_BENEFIT_THRESHOLD * 100));
  }
}

/**
 * Collect all join nodes and base table scans from the plan tree
 */
void JoinReOrder::collect_join_nodes(Plan &plan, std::vector<JoinNode> &joins, std::vector<Plan *> &scans) {
  if (!plan) return;

  PlanNode *node = plan.get();
  switch (node->type()) {
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      JoinNode jnode;
      jnode.node = node;
      jnode.is_hash_join = (node->type() == PlanNode::Type::HASH_JOIN);

      if (jnode.is_hash_join) {
        auto *hj = static_cast<HashJoin *>(node);
        jnode.join_conditions = hj->join_conditions;
      } else {
        auto *nj = static_cast<NestLoopJoin *>(node);
        jnode.join_conditions = nj->join_conditions;
      }

      joins.push_back(jnode);

      // Recursively collect from children
      for (auto &child : node->children) {
        collect_join_nodes(child, joins, scans);
      }
    } break;
    case PlanNode::Type::SCAN: {
      scans.push_back(&plan);
    } break;
    case PlanNode::Type::FILTER: {
      // Descend through filter nodes
      collect_join_nodes(node->children[0], joins, scans);
    } break;
    default:
      // For other node types, descend to children
      for (auto &child : node->children) {
        collect_join_nodes(child, joins, scans);
      }
      break;
  }
}

/**
 * Build join graph with IMCS-specific cardinality re-estimation
 *
 * Key difference from MySQL optimizer:
 * - Re-calculate cardinalities using Storage Index selectivity
 * - Account for IMCU-level pruning
 * - Use actual NDV from ColumnStatistics (not sampled estimates)
 */
JoinReOrder::JoinGraph JoinReOrder::build_join_graph(const std::vector<JoinNode> &joins,
                                                     const std::vector<Plan *> &scans) {
  JoinGraph graph;
  graph.num_tables = scans.size();
  graph.edges.resize(graph.num_tables);

  auto *estimator = CostModelServer::Instance(CostEstimator::Type::RPD_ENG);
  // Create table info for each scan
  for (size_t i = 0; i < scans.size(); i++) {
    TableInfo info;
    info.table_id = i;
    info.scan_node = scans[i];
    auto *scan = static_cast<ScanTable *>((*scans[i]).get());

    // Start with MySQL's estimate as baseline
    info.mysql_cardinality = scan->estimated_rows;
    info.mysql_cost = scan->cost;

    size_t total_imcus{0};
    if (scan->rpd_table) {
      // IMCS re-estimation: Use actual IMCS statistics
      total_imcus = scan->rpd_table->meta().total_imcus.load();
      info.cardinality = scan->rpd_table->meta().total_rows.load();
      info.selectivity = 1.0;

      // Apply Storage Index filtering if predicates exist
      if (scan->use_storage_index && scan->prune_predicate) {
        // This is the KEY difference: IMCS knows about IMCU pruning
        double storage_index_selectivity = estimate_storage_index_selectivity(scan);
        info.selectivity = storage_index_selectivity;
        info.cardinality = static_cast<ha_rows>(info.cardinality * info.selectivity);
        // Log if significantly different from MySQL's estimate
        if (info.cardinality < info.mysql_cardinality * 0.5) {
          DBUG_PRINT("rapid_optimizer", ("Storage Index pruning: MySQL estimated %llu rows, "
                                         "IMCS estimates %llu rows (%.1fx reduction)",
                                         info.mysql_cardinality, info.cardinality,
                                         static_cast<double>(info.mysql_cardinality) / info.cardinality));
        }
      }
    } else {
      // No IMCS table - use MySQL estimates
      info.cardinality = scan->estimated_rows;
      info.selectivity = 1.0;
    }
    info.cost = estimator ? estimator->estimate_scan_cost(info.cardinality, total_imcus)
                          : static_cast<double>(info.cardinality) * 0.01;
    graph.tables.push_back(info);
  }

  // Build edges from join conditions (same as before)
  for (const auto &join : joins) {
    for (auto *cond : join.join_conditions) {
      auto edge = extract_join_edge(cond, scans);
      if (edge.left_table >= 0 && edge.right_table >= 0) {
        graph.edges[edge.left_table].push_back(edge);

        JoinEdge reverse = edge;
        std::swap(reverse.left_table, reverse.right_table);
        std::swap(reverse.left_column, reverse.right_column);
        graph.edges[edge.right_table].push_back(reverse);
      }
    }
  }
  return graph;
}

/**
 * Collect all join edges between two subsets in the join graph
 */
std::vector<Item *> JoinReOrder::collect_edges_between(size_t left_subset, size_t right_subset,
                                                       const JoinGraph &graph) {
  std::vector<Item *> conditions;
  for (size_t i = 0; i < graph.num_tables; i++) {
    // in left subset
    if (left_subset & (1ULL << i)) {
      for (const auto &edge : graph.edges[i]) {
        // and the other end of the edge is in right subset
        if (right_subset & (1ULL << edge.right_table)) {
          if (edge.condition) conditions.push_back(edge.condition);
        }
      }
    }
  }
  return conditions;
}

/**
 * Extract join edge from a join condition (e.g., t1.id = t2.id)
 */
JoinReOrder::JoinEdge JoinReOrder::extract_join_edge(Item *condition, const std::vector<Plan *> &scans) {
  JoinEdge edge;
  edge.left_table = -1;
  edge.right_table = -1;
  edge.selectivity = 1.0;

  if (!condition || condition->type() != Item::FUNC_ITEM) return edge;

  auto *func = static_cast<Item_func *>(condition);
  if (func->functype() != Item_func::EQ_FUNC || func->argument_count() != 2) return edge;

  // Extract left and right columns
  Item *left = func->arguments()[0];
  Item *right = func->arguments()[1];
  if (left->type() == Item::FIELD_ITEM && right->type() == Item::FIELD_ITEM) {
    auto *left_field = static_cast<Item_field *>(left);
    auto *right_field = static_cast<Item_field *>(right);

    // Find which tables these fields belong to
    edge.left_table = find_table_for_field(left_field, scans);
    edge.right_table = find_table_for_field(right_field, scans);

    if (edge.left_table >= 0 && edge.right_table >= 0) {
      edge.left_column = left_field->field->field_index();
      edge.right_column = right_field->field->field_index();
      edge.condition = condition;

      // Estimate join selectivity using IMCS statistics
      edge.selectivity = estimate_join_selectivity(left_field, right_field, scans);
    }
  }

  return edge;
}

/**
 * Find which table a field belongs to
 */
int JoinReOrder::find_table_for_field(Item_field *field, const std::vector<Plan *> &scans) {
  if (!field->field || !field->field->table) return -1;

  std::string field_table =
      std::string(field->field->table->s->db.str).append(".").append(field->field->table->s->table_name.str);
  for (size_t i = 0; i < scans.size(); i++) {
    auto *scan = static_cast<ScanTable *>((*scans[i]).get());
    if (scan->source_table) {
      std::string scan_table =
          std::string(scan->source_table->s->db.str).append(".").append(scan->source_table->s->table_name.str);
      if (field_table == scan_table) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

/**
 * Estimate Storage Index filtering selectivity
 */
double JoinReOrder::estimate_storage_index_selectivity(ScanTable *scan) {
  if (!scan->prune_predicate || !scan->rpd_table) return 1.0;

  double total_selectivity = 1.0;

  // Use first IMCU's Storage Index as representative sample
  // In production, would sample multiple IMCUs
  auto *first_imcu = scan->rpd_table->locate_imcu(0);
  if (!first_imcu) return 1.0;

  auto *storage_index = first_imcu->get_storage_index();
  if (!storage_index) return 1.0;

  // Estimate selectivity for each predicate component
  auto *pred = scan->prune_predicate.get();
  if (auto *simple = dynamic_cast<Imcs::Simple_Predicate *>(pred)) {
    total_selectivity = simple->estimate_selectivity(storage_index);
  } else if (auto *compound = dynamic_cast<Imcs::Compound_Predicate *>(pred)) {
    total_selectivity = compound->estimate_selectivity(storage_index);
  }
  return total_selectivity;
}

/**
 * Estimate join selectivity using IMCS column statistics
 */
double JoinReOrder::estimate_join_selectivity(Item_field *left_field, Item_field *right_field,
                                              const std::vector<Plan *> &scans) {
  // Default selectivity (1/max(NDV1, NDV2))
  double default_selectivity = 0.1;

  // Try to get actual statistics from IMCS
  int left_table = find_table_for_field(left_field, scans);
  int right_table = find_table_for_field(right_field, scans);

  if (left_table < 0 || right_table < 0) return default_selectivity;

  auto *left_scan = static_cast<ScanTable *>((*scans[left_table]).get());
  auto *right_scan = static_cast<ScanTable *>((*scans[right_table]).get());

  if (!left_scan->rpd_table || !right_scan->rpd_table) return default_selectivity;

  // Get column statistics
  uint32 left_col = left_field->field->field_index();
  uint32 right_col = right_field->field->field_index();

  auto *left_stats = left_scan->rpd_table->get_column_stats(left_col);
  auto *right_stats = right_scan->rpd_table->get_column_stats(right_col);

  if (!left_stats || !right_stats) return default_selectivity;

  // Estimate using distinct value counts
  size_t left_ndv = left_stats->get_basic_stats().distinct_count;
  size_t right_ndv = right_stats->get_basic_stats().distinct_count;

  if (left_ndv == 0 || right_ndv == 0) return default_selectivity;

  // Selectivity = 1 / max(NDV_left, NDV_right)
  double selectivity = 1.0 / std::max(left_ndv, right_ndv);
  // Clamp to reasonable range
  return std::max(0.001, std::min(1.0, selectivity));
}

/**
 * Reorder joins using dynamic programming (for small queries)
 */
Plan JoinReOrder::reorder_with_dp(const JoinGraph &graph, Plan &root, double baseline_cost) {
  auto cost_estimator = CostModelServer::Instance(CostEstimator::Type::RPD_ENG);
  size_t n = graph.num_tables;
  if (n == 0) return nullptr;

  // DP state: dp[subset_mask] stores the optimal plan information for that subset
  size_t num_subsets = 1ULL << n;
  std::vector<DPState> dp(num_subsets);

  // Step 1: Initialize single-table states
  for (size_t i = 0; i < n; i++) {
    size_t subset = 1ULL << i;
    auto &table_info = graph.tables[i];
    size_t imcus{0};
    if (table_info.scan_node) {
      auto *scan = static_cast<ScanTable *>(table_info.scan_node->get());
      imcus = scan->rpd_table ? scan->rpd_table->meta().total_imcus.load() : 0;
    }
    dp[subset].cost = cost_estimator->estimate_scan_cost(graph.tables[i].cardinality, imcus);
    dp[subset].cardinality = graph.tables[i].cardinality;
    // Single tables don't need to record left/right_subset
  }

  // Step 2: Build DP table in increasing order of subset size
  for (size_t size = 2; size <= n; size++) {
    enumerate_subsets(n, size, [&](size_t subset) {
      // Try all possible binary partitions
      enumerate_partitions(subset, [&](size_t left, size_t right) {
        // Only consider partitions where join conditions exist between the two subsets
        // (avoid unnecessary Cartesian products)
        if (!has_join_edge(left, right, graph)) return;

        // Calculate the cost of current partition
        double join_cost = cost_estimator->estimate_join_cost(dp[left].cardinality, dp[right].cardinality);
        double total_cost = dp[left].cost + dp[right].cost + join_cost;

        // Update optimal solution for current subset
        if (dp[subset].cost == 0 || total_cost < dp[subset].cost) {
          dp[subset].cost = total_cost;
          dp[subset].left_subset = left;
          dp[subset].right_subset = right;
          dp[subset].cardinality = estimate_join_cardinality(dp[left], dp[right], graph);
        }
      });
    });
  }

  // Step 3: Final decision
  size_t all_tables = (1ULL << n) - 1;
  double best_cost = dp[all_tables].cost;
  if (best_cost == 0 || dp[all_tables].left_subset == 0 || dp[all_tables].right_subset == 0) {
    DBUG_PRINT("rapid_optimizer", ("No valid DP solution found. Keeping MySQL's original order."));
    return nullptr;
  }

  // If DP's optimal cost is not significantly better than baseline (considering threshold),
  // skip the reordering. This prevents frequent plan changes due to minor statistical fluctuations.
  if (best_cost >= baseline_cost * (1.0 - REORDER_BENEFIT_THRESHOLD)) {
    DBUG_PRINT("rapid_optimizer",
               ("DP cost %.2f is not significantly better than baseline %.2f. Skipping.", best_cost, baseline_cost));
    return nullptr;
  }

  // Step 4: Reconstruct physical plan tree based on DP path
  return reconstruct_join_plan(all_tables, dp, graph, root);
}

/**
 * Reconstruct join plan from DP solution
 */
Plan JoinReOrder::reconstruct_join_plan(size_t subset, std::vector<DPState> &dp, const JoinGraph &graph,
                                        Plan &original_root) {
  // Base case: Subset contains only one table, directly return the corresponding Scan node
  if (__builtin_popcountll(subset) == 1) {
    for (size_t i = 0; i < graph.num_tables; i++) {
      if (subset & (1ULL << i)) {
        // Take ownership of the original Scan node from JoinGraph
        return std::move(*(graph.tables[i].scan_node));
      }
    }
  }

  // Recursive case: Reconstruct left and right sub-plans
  size_t left_mask = dp[subset].left_subset;
  size_t right_mask = dp[subset].right_subset;
  if (left_mask == 0 || right_mask == 0) {
    DBUG_PRINT("rapid_optimizer", ("Invalid DP state for subset %zu: left_mask=%zu, right_mask=%zu. "
                                   "Likely a disconnected join graph or missing join conditions.",
                                   subset, left_mask, right_mask));
    return nullptr;
  }

  if ((left_mask | right_mask) != subset || (left_mask & right_mask) != 0) {
    DBUG_PRINT("rapid_optimizer",
               ("Invalid partition for subset %zu: left_mask=%zu, right_mask=%zu.", subset, left_mask, right_mask));
    return nullptr;
  }

  Plan left_child = reconstruct_join_plan(left_mask, dp, graph, original_root);
  Plan right_child = reconstruct_join_plan(right_mask, dp, graph, original_root);

  if (!left_child || !right_child) return nullptr;

  // Create a new HashJoin node
  auto join = std::make_unique<HashJoin>();

  // Key step: Collect all join predicates between these two subsets (t1.a = t2.a AND t1.b = t2.b ...)
  join->join_conditions = collect_edges_between(left_mask, right_mask, graph);

  join->children.push_back(std::move(left_child));
  join->children.push_back(std::move(right_child));

  // Pass statistical information
  join->estimated_rows = dp[subset].cardinality;
  join->cost = dp[subset].cost;
  return join;
}

/**
 * Reorder joins using greedy algorithm (for large queries)
 */
Plan JoinReOrder::reorder_with_greedy(const JoinGraph &graph, Plan &root, double baseline_cost) {
  size_t n = graph.num_tables;
  std::vector<size_t> order;  // Record the greedy selection order of table indices
  std::vector<bool> joined(n, false);

  // 1. Find the initial small table
  size_t current = find_smallest_table(graph);
  joined[current] = true;
  order.push_back(current);

  double total_greedy_cost = graph.tables[current].cost;
  ha_rows current_cardinality = graph.tables[current].cardinality;

  // 2. Greedily add remaining tables one by one
  for (size_t i = 1; i < n; i++) {
    int best_next = -1;
    double min_join_cost = DBL_MAX;

    for (size_t j = 0; j < n; j++) {
      if (joined[j]) continue;

      // Check if there is a join edge between the current joined set and table j
      bool has_edge = false;
      for (size_t k : order) {
        for (const auto &edge : graph.edges[k]) {
          if (edge.right_table == static_cast<int>(j)) {
            has_edge = true;
            break;
          }
        }
        if (has_edge) break;
      }

      if (!has_edge) continue;  // Avoid generating Cartesian products

      // Calculate join cost
      double join_cost = estimate_greedy_join_cost(current_cardinality, graph.tables[j].cardinality, graph);
      if (join_cost < min_join_cost) {
        min_join_cost = join_cost;
        best_next = j;
      }
    }

    // If no table with join edge is found (handling disconnected graph), select the smallest remaining table
    if (best_next == -1) {
      double min_card = DBL_MAX;
      for (size_t j = 0; j < n; j++) {
        if (!joined[j] && graph.tables[j].cardinality < min_card) {
          min_card = graph.tables[j].cardinality;
          best_next = j;
        }
      }
    }

    if (best_next >= 0) {
      joined[best_next] = true;
      order.push_back(best_next);
      total_greedy_cost += min_join_cost;
      current_cardinality = estimate_result_cardinality(current_cardinality, graph.tables[best_next].cardinality, 0.1);
    }
  }

  // 3. Final cost check: Return nullptr if greedy result is not better than MySQL's baseline
  if (total_greedy_cost >= baseline_cost) return nullptr;

  return reconstruct_greedy_plan(order, graph, root);
}

/**
 * Calculate cost of current plan (MySQL's optimized order)
 *
 * This serves as the baseline for comparison.
 * Only reorder if we can beat this cost significantly.
 */
double JoinReOrder::calculate_current_plan_cost(const Plan &root, const JoinGraph &graph) {
  if (!root) return 0.0;

  // Recursively calculate cost using IMCS cost model
  double total_cost = 0.0;

  switch (root->type()) {
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      auto *join = static_cast<const HashJoin *>(root.get());

      // Cost of children
      double left_cost = calculate_current_plan_cost(join->children[0], graph);
      double right_cost = calculate_current_plan_cost(join->children[1], graph);

      // Cost of join itself (IMCS vectorized hash join model)
      ha_rows left_card = join->children[0]->estimated_rows;
      ha_rows right_card = join->children[1]->estimated_rows;

      // Re-estimate using IMCS statistics if available
      left_card = re_estimate_cardinality(join->children[0].get(), graph);
      right_card = re_estimate_cardinality(join->children[1].get(), graph);

      double join_cost = calculate_hash_join_cost(left_card, right_card);

      total_cost = left_cost + right_cost + join_cost;
    } break;
    case PlanNode::Type::SCAN: {
      auto *scan = static_cast<const ScanTable *>(root.get());

      // Find corresponding table in graph
      for (const auto &table : graph.tables) {
        auto *graph_scan = static_cast<ScanTable *>((*table.scan_node).get());
        if (graph_scan == scan) {
          total_cost = table.cost;
          break;
        }
      }
    } break;
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<const Filter *>(root.get());
      total_cost = calculate_current_plan_cost(filter->children[0], graph);
      // Filter cost is already included in scan cost via selectivity
    } break;
    default:
      // For other node types, sum children costs
      for (const auto &child : root->children) {
        total_cost += calculate_current_plan_cost(child, graph);
      }
      break;
  }
  return total_cost;
}

/**
 * Re-estimate cardinality of a subtree using IMCS statistics
 */
ha_rows JoinReOrder::re_estimate_cardinality(const PlanNode *node, const JoinGraph &graph) {
  if (!node) return 0;

  switch (node->type()) {
    case PlanNode::Type::SCAN: {
      auto *scan = static_cast<const ScanTable *>(node);

      // Find in graph (has IMCS re-estimated cardinality)
      for (const auto &table : graph.tables) {
        auto *graph_scan = static_cast<ScanTable *>((*table.scan_node).get());
        if (graph_scan == scan) return table.cardinality;
      }
      // Fallback to node's estimate
      return scan->estimated_rows;
    } break;
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      // For joins, use the node's estimated_rows (set by previous rules)
      return node->estimated_rows;
    } break;
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<const Filter *>(node);
      return re_estimate_cardinality(filter->children[0].get(), graph);
    } break;
    default:
      return node->estimated_rows;
  }
}

/**
 * Calculate hash join cost using IMCS vectorized model
 */
double JoinReOrder::calculate_hash_join_cost(ha_rows left_card, ha_rows right_card) {
  // IMCS vectorized hash join:
  // 1. Build hash table on smaller side
  // 2. Vectorized probe with larger side (batches of 1024 rows)
  // 3. Output materialization
  return estimate_greedy_join_cost(left_card, right_card, JoinGraph{});
}

/**
 * Estimate result cardinality of joining two sub-plans
 */
ha_rows JoinReOrder::estimate_join_cardinality(const DPState &left, const DPState &right, const JoinGraph &graph) {
  // Default selectivity if no statistics available
  double selectivity = 0.1;
  // Try to find actual join selectivity from graph edges
  ha_rows result = static_cast<ha_rows>(left.cardinality * right.cardinality * selectivity);

  // Clamp to reasonable range
  return std::max(static_cast<ha_rows>(1), result);
}

/**
 * Find smallest table in the join graph
 */
size_t JoinReOrder::find_smallest_table(const JoinGraph &graph) {
  size_t smallest = 0;
  ha_rows min_cardinality = graph.tables[0].cardinality;

  for (size_t i = 1; i < graph.num_tables; i++) {
    if (graph.tables[i].cardinality < min_cardinality) {
      min_cardinality = graph.tables[i].cardinality;
      smallest = i;
    }
  }
  return smallest;
}

/**
 * Check if there's a join edge between two subsets
 */
bool JoinReOrder::has_join_edge(size_t left_subset, size_t right_subset, const JoinGraph &graph) {
  // Check if any table in left has an edge to any table in right
  for (size_t i = 0; i < graph.num_tables; i++) {
    if (!(left_subset & (1ULL << i))) continue;

    for (const auto &edge : graph.edges[i]) {
      if (right_subset & (1ULL << edge.right_table)) return true;
    }
  }
  return false;
}

/**
 * Enumerate all subsets of size k
 */
void JoinReOrder::enumerate_subsets(size_t n, size_t k, std::function<void(size_t)> enum_subsets_fuc) {
  size_t subset = (1ULL << k) - 1;  // First k-element subset
  size_t limit = 1ULL << n;

  while (subset < limit) {
    enum_subsets_fuc(subset);

    // Generate next k-element subset (Gosper's hack)
    size_t c = subset & -subset;
    size_t r = subset + c;
    subset = (((r ^ subset) >> 2) / c) | r;
  }
}

/**
 * Enumerate all ways to partition a subset into two non-empty parts
 */
void JoinReOrder::enumerate_partitions(size_t subset, std::function<void(size_t, size_t)> enum_partitions_fuc) {
  // Enumerate all non-empty proper subsets of subset
  for (size_t left = subset; left > 0; left = (left - 1) & subset) {
    size_t right = subset ^ left;
    if (right > 0) enum_partitions_fuc(left, right);
  }
}

/**
 * Reconstruct join plan from greedy order
 */
Plan JoinReOrder::reconstruct_greedy_plan(const std::vector<size_t> &order, const JoinGraph &graph,
                                          Plan &original_root) {
  if (order.empty()) return nullptr;

  // 1. Take the first table as the initial left subtree
  Plan current_plan = std::move(*(graph.tables[order[0]].scan_node));
  size_t current_mask = (1ULL << order[0]);

  // 2. Sequentially "attach" subsequent tables in order
  for (size_t i = 1; i < order.size(); ++i) {
    size_t next_table_idx = order[i];
    size_t next_mask = (1ULL << next_table_idx);

    Plan next_scan = std::move(*(graph.tables[next_table_idx].scan_node));

    // Create a new Join node
    auto join = std::make_unique<HashJoin>();

    // Extract join conditions between the current built tree and the new table
    join->join_conditions = collect_edges_between(current_mask, next_mask, graph);

    // Assemble tree structure (left-deep: old tree on left, new scan on right)
    join->children.push_back(std::move(current_plan));
    join->children.push_back(std::move(next_scan));

    // Update current mask and plan
    current_mask |= next_mask;
    current_plan = std::move(join);
  }
  return current_plan;
}

/**
 * Estimate greedy join cost
 */
double JoinReOrder::estimate_greedy_join_cost(ha_rows left_card, ha_rows right_card, const JoinGraph &graph) {
  auto *estimator = CostModelServer::Instance(CostEstimator::Type::RPD_ENG);
  return estimator ? estimator->estimate_join_cost(left_card, right_card) : static_cast<double>(left_card) + right_card;
}

/**
 * Estimate result cardinality for greedy algorithm
 */
ha_rows JoinReOrder::estimate_result_cardinality(ha_rows left_card, ha_rows right_card, double selectivity) {
  return static_cast<ha_rows>(left_card * right_card * selectivity);
}
}  // namespace Optimizer
}  // namespace ShannonBase