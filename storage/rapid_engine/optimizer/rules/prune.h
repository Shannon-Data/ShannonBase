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
#ifndef __SHANNONBASE_OBJECT_PRUNE_RULE_H__
#define __SHANNONBASE_OBJECT_PRUNE_RULE_H__
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "sql/item_func.h"
#include "storage/rapid_engine/optimizer/rules/rule.h"
class Query_expression;
class Query_block;
class Item;
class Item_func;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
}
namespace Optimizer {

/**
 * Storage Index Pruning Rule
 *
 * Optimization Strategy:
 * 1. Analyze predicates applicable for storage index pruning
 * 2. Mark ScanTable nodes to use StorageIndex for IMCU-level pruning
 * 3. Estimate cost reduction based on expected IMCU skips
 * 4. Enable vectorized execution for better performance
 *
 * Benefits:
 * - Reduces number of IMCUs read from disk
 * - Lowers I/O and CPU usage during scans
 * - Improves overall query performance, especially for selective predicates
 * Best Suited For:
 * - Range predicates on sorted/clustered columns
 * - Equality predicates with high selectivity
 * - Temporal predicates (timestamp ranges)
 */
class StorageIndexPrune : public Rule {
 public:
  StorageIndexPrune() = default;
  virtual ~StorageIndexPrune() = default;

  std::string name() override { return std::string("StorageIndexPrune"); }
  void apply(Plan &root) override;

 private:
  /**
   * Recursively collect predicates flowing to each scan node
   *
   * @param node Current plan node
   * @param scan_predicates Output map: ScanTable -> list of predicates
   * @param current_predicates Predicates accumulated from ancestors
   */
  void collect_scan_predicates(PlanNode *node, std::map<ScanTable *, std::vector<Item *>> &scan_predicates,
                               std::vector<Item *> &current_predicates);

  /**
   * Check if a predicate is suitable for Storage Index pruning
   *
   * Criteria:
   * - Simple column comparisons (col OP value)
   * - Range predicates on numeric/temporal types
   * - Equality/IN predicates with good selectivity
   *
   * @param pred Predicate to analyze
   * @param rpd_table Table metadata for column analysis
   * @return true if predicate can benefit from Storage Index
   */
  bool is_storage_index_candidate(Item *pred, Imcs::RpdTable *rpd_table);

  /**
   * Check if a specific column is suitable for Storage Index
   *
   * @param rpd_table Table metadata
   * @param col_idx Column index
   * @param func_type Type of predicate (EQ, LT, GT, etc.)
   * @return true if column benefits from indexing
   */
  bool is_column_suitable_for_index(Imcs::RpdTable *rpd_table, uint32_t col_idx, Item_func::Functype func_type);

  /**
   * Estimate pruning benefit and update scan node costs
   *
   * Updates:
   * - scan->estimated_rows (based on selectivity)
   * - scan->cost (based on IMCU skip ratio)
   *
   * @param scan Scan node to update
   * @param predicates Predicates used for pruning
   */
  void estimate_pruning_benefit(ScanTable *scan, const std::vector<Item *> &predicates);

  /**
   * Estimate row-level selectivity of a predicate
   *
   * @param pred Predicate to estimate
   * @param rpd_table Table for statistics lookup
   * @return Selectivity [0.0, 1.0]
   */
  double estimate_predicate_selectivity(Item *pred, Imcs::RpdTable *rpd_table);

  /**
   * Estimate IMCU-level skip ratio
   *
   * This estimates what fraction of IMCUs can be skipped entirely
   * based on min/max checks without reading IMCU data.
   *
   * @param pred Predicate to analyze
   * @param rpd_table Table metadata
   * @return Fraction of IMCUs that can be skipped [0.0, 1.0]
   */
  double estimate_imcu_skip_ratio(Item *pred, Imcs::RpdTable *rpd_table);

  /**
   * Split conjunctive condition (AND) into individual predicates
   */
  void split_conjunctions(Item *condition, std::vector<Item *> &predicates);

  /**
   * Get all tables referenced by an item
   */
  std::unordered_set<std::string> get_referenced_tables(Item *item);

  /**
   * Get available tables in a plan subtree
   */
  std::unordered_set<std::string> get_available_tables(const Plan &node);

  /**
   * Check if item contains aggregate function references
   */
  bool contains_aggregate_reference(Item *item);
};

/**
 * Projection Pruning Rule
 *
 * Optimization Strategy:
 * 1. Identify columns referenced in query (SELECT, WHERE, JOIN, GROUP BY, ORDER BY)
 * 2. Remove unnecessary columns from scan operations
 * 3. Reduce memory footprint and I/O bandwidth
 *
 * Benefits:
 * - Lower memory usage during query execution
 * - Reduced I/O bandwidth (read fewer CUs)
 * - Better cache utilization
 * - Improved performance for wide tables
 */
class ProjectionPruning : public Rule {
 public:
  ProjectionPruning() = default;
  virtual ~ProjectionPruning() = default;

  std::string name() override { return std::string("ProjectionPruning"); }
  void apply(Plan &root) override;

 private:
  /**
   * Collect all referenced columns from the plan tree
   * @param root Plan root node
   * @return Map of "db.table" -> set of column indices
   */
  std::map<std::string, std::set<uint32_t>> collect_referenced_columns(Plan &root);

  /**
   * Extract columns from join conditions
   */
  void collect_from_join_conditions(const std::vector<Item *> &conditions,
                                    std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Extract columns from aggregation node
   */
  void collect_from_aggregation(const LocalAgg *agg, std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Extract columns from filter predicates
   */
  void collect_from_filter(const Filter *filter, std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Helper function, visit an Item and collect field references
   */
  void collect_from_item(Item *item, std::map<std::string, std::set<uint32_t>> &columns);
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OBJECT_PRUNE_RULE_H__