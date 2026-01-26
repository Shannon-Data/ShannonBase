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
#include "storage/rapid_engine/optimizer/rules/prune.h"

#include "sql/item.h"     //Item
#include "sql/sql_lex.h"  //query_expression
#include "sql/sql_list.h"

#include "storage/rapid_engine/imcs/table.h"  //RpdTable
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
/**
 * Strategy:
 * 1. Collect predicates that flow to each scan node
 * 2. Analyze which predicates are suitable for IMCU-level pruning
 * 3. Estimate pruning benefits and update costs
 * 4. Mark scans for Storage Index usage
 */
void StorageIndexPrune::apply(Plan &root) {
  if (!root) return;

  // Phase 1: Collect predicates flowing to each scan node
  std::map<ScanTable *, std::vector<Item *>> scan_predicates;
  std::vector<Item *> current_predicates;
  collect_scan_predicates(root.get(), scan_predicates, current_predicates);

  // Phase 2: Analyze and mark scans for Storage Index pruning
  auto prune_func = [&](PlanNode *node) {
    if (node->type() != PlanNode::Type::SCAN) return;

    auto *scan = static_cast<ScanTable *>(node);

    // Skip if no RpdTable or already configured
    if (!scan->rpd_table || scan->use_storage_index) return;

    // Find predicates for this scan
    auto it = scan_predicates.find(scan);
    if (it == scan_predicates.end() || it->second.empty()) return;  // No predicates -> no pruning benefit
    // Analyze predicates to determine Storage Index candidates
    std::vector<Item *> storage_index_predicates;
    for (auto *pred : it->second) {
      if (is_storage_index_candidate(pred, scan->rpd_table)) {
        storage_index_predicates.push_back(pred);
      }
    }

    if (storage_index_predicates.empty()) return;  // No suitable predicates for Storage Index

    // Enable Storage Index pruning
    scan->use_storage_index = true;
    // Estimate pruning benefit and update cost/cardinality
    estimate_pruning_benefit(scan, storage_index_predicates);
  };

  WalkPlan(root.get(), prune_func);
}

/**
 * Recursively collect predicates that flow to each scan node
 *
 * @param node Current plan node
 * @param scan_predicates Output map: ScanTable -> predicates
 * @param current_predicates Predicates accumulated from ancestors
 */
void StorageIndexPrune::collect_scan_predicates(PlanNode *node,
                                                std::map<ScanTable *, std::vector<Item *>> &scan_predicates,
                                                std::vector<Item *> &current_predicates) {
  if (!node) return;

  switch (node->type()) {
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node);

      // Add this filter's predicates to the flow
      std::vector<Item *> child_predicates = current_predicates;

      // Split filter condition into conjuncts
      if (filter->condition) {
        std::vector<Item *> conjuncts;
        split_conjunctions(filter->condition, conjuncts);
        child_predicates.insert(child_predicates.end(), conjuncts.begin(), conjuncts.end());
      }

      // Continue down to children
      for (auto &child : filter->children) {
        collect_scan_predicates(child.get(), scan_predicates, child_predicates);
      }
    } break;
    case PlanNode::Type::SCAN: {
      auto *scan = static_cast<ScanTable *>(node);

      // Associate accumulated predicates with this scan
      if (!current_predicates.empty()) {
        // Filter predicates that reference this scan's table
        std::string table_key;
        if (scan->source_table) {
          table_key =
              std::string(scan->source_table->s->db.str) + "." + std::string(scan->source_table->s->table_name.str);
        }

        for (auto *pred : current_predicates) {
          auto referenced = get_referenced_tables(pred);

          // Only include predicates that:
          // 1. Reference only this table, or
          // 2. Are constant expressions
          bool applicable =
              referenced.empty() || (referenced.size() == 1 && referenced.find(table_key) != referenced.end());

          if (applicable) {
            scan_predicates[scan].push_back(pred);
          }
        }
      }
    } break;
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      // For joins, predicates flow to both children
      // But we need to partition them based on table references

      auto left_tables = get_available_tables(node->children[0]);
      auto right_tables = get_available_tables(node->children[1]);

      std::vector<Item *> left_predicates;
      std::vector<Item *> right_predicates;

      for (auto *pred : current_predicates) {
        auto referenced = get_referenced_tables(pred);

        bool all_in_left = true;
        bool all_in_right = true;

        for (const auto &table : referenced) {
          if (left_tables.find(table) == left_tables.end()) {
            all_in_left = false;
          }
          if (right_tables.find(table) == right_tables.end()) {
            all_in_right = false;
          }
        }

        if (all_in_left) {
          left_predicates.push_back(pred);
        }
        if (all_in_right) {
          right_predicates.push_back(pred);
        }
        // Note: Join predicates (reference both sides) are not pushed down
      }

      // Recursively process children with appropriate predicates
      collect_scan_predicates(node->children[0].get(), scan_predicates, left_predicates);
      collect_scan_predicates(node->children[1].get(), scan_predicates, right_predicates);
    } break;
    case PlanNode::Type::LOCAL_AGGREGATE: {
      // Predicates BEFORE aggregation can flow through
      // Predicates AFTER aggregation (HAVING) cannot
      std::vector<Item *> child_predicates;

      for (auto *pred : current_predicates) {
        // Only push down predicates that don't reference aggregates
        if (!contains_aggregate_reference(pred)) {
          child_predicates.push_back(pred);
        }
      }

      for (auto &child : node->children) {
        collect_scan_predicates(child.get(), scan_predicates, child_predicates);
      }
    } break;
    default: {
      // For other node types, predicates flow through to children
      for (auto &child : node->children) {
        collect_scan_predicates(child.get(), scan_predicates, current_predicates);
      }
    } break;
  }
}

/**
 * Check if a predicate is suitable for Storage Index pruning
 *
 * Storage Index works best for:
 * 1. Range predicates on monotonic columns (col > value, col < value, col BETWEEN)
 * 2. Equality predicates (col = value)
 * 3. IN predicates with small lists (col IN (1,2,3))
 * 4. Simple column comparisons (no complex functions)
 *
 * @param pred Predicate to analyze
 * @param rpd_table RpdTable to check column properties
 * @return true if predicate is suitable for Storage Index
 */
bool StorageIndexPrune::is_storage_index_candidate(Item *pred, Imcs::RpdTable *rpd_table) {
  if (!pred || !rpd_table) return false;

  switch (pred->type()) {
    case Item::FUNC_ITEM: {
      auto *func = static_cast<Item_func *>(pred);
      Item_func::Functype func_type = func->functype();
      switch (func_type) {
        case Item_func::EQ_FUNC:
        case Item_func::NE_FUNC:
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC: {
          // Simple comparison: col OP value
          Item **args = func->arguments();
          // Check if one side is a field and the other is a constant
          Item_field *field_item = nullptr;
          Item *value_item = nullptr;
          if (args[0]->type() == Item::FIELD_ITEM && args[1]->const_item()) {
            field_item = static_cast<Item_field *>(args[0]);
            value_item = args[1];
          } else if (args[1]->type() == Item::FIELD_ITEM && args[0]->const_item()) {
            field_item = static_cast<Item_field *>(args[1]);
            value_item = args[0];
          }
          if (!field_item || !value_item) return false;  // Not simple col OP value pattern

          // Check if this column has good Storage Index properties
          uint32_t col_idx = field_item->field->field_index();
          return is_column_suitable_for_index(rpd_table, col_idx, func_type);
        } break;
        case Item_func::BETWEEN: {
          // col BETWEEN low AND high
          auto *between = static_cast<Item_func_between *>(func);
          Item **args = between->arguments();
          if (args[0]->type() == Item::FIELD_ITEM && args[1]->const_item() && args[2]->const_item()) {
            auto *field_item = static_cast<Item_field *>(args[0]);
            uint32_t col_idx = field_item->field->field_index();
            // BETWEEN is excellent for Storage Index (range check)
            return is_column_suitable_for_index(rpd_table, col_idx, func_type);
          }
          return false;
        } break;
        case Item_func::IN_FUNC: {
          // col IN (v1, v2, ..., vN)
          auto *in_func = static_cast<Item_func_in *>(func);
          Item **args = in_func->arguments();
          if (args[0]->type() != Item::FIELD_ITEM) return false;
          // Check list size - Storage Index is efficient for small IN lists
          uint32_t list_size = in_func->argument_count() - 1;
          if (list_size > 100) return false;  // Too many values, not efficient for IMCU min/max check

          auto *field_item = static_cast<Item_field *>(args[0]);
          uint32_t col_idx = field_item->field->field_index();
          return is_column_suitable_for_index(rpd_table, col_idx, func_type);
        } break;
        case Item_func::ISNULL_FUNC:
        case Item_func::ISNOTNULL_FUNC: {
          // col IS NULL / col IS NOT NULL
          Item **args = func->arguments();
          if (args[0]->type() == Item::FIELD_ITEM) {
            auto *field_item = static_cast<Item_field *>(args[0]);
            uint32_t col_idx = field_item->field->field_index();
            return rpd_table->meta().fields[col_idx].nullable;
          }
          return false;
        } break;
        default:
          // Other functions (ABS, SUBSTR, etc.) - not suitable
          return false;
      }
    } break;
    case Item::COND_ITEM: {
      // AND/OR conditions - recursively check children
      auto *cond = static_cast<Item_cond *>(pred);
      if (cond->functype() == Item_func::COND_AND_FUNC) {
        // For AND: All children should be candidates
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          if (!is_storage_index_candidate(item, rpd_table)) return false;
        }
        return true;
      } else if (cond->functype() == Item_func::COND_OR_FUNC) {
        // For OR: At least one child should be a candidate
        // (but OR is less efficient for IMCU pruning)
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          if (is_storage_index_candidate(item, rpd_table)) return true;
        }
        return false;
      }
    } break;
    default:
      return false;
  }
  return false;
}

/**
 * Check if a column is suitable for Storage Index optimization
 *
 * Good columns for Storage Index:
 * - Sorted/semi-sorted columns (timestamp, id)
 * - Columns with good clustering (same values grouped together)
 * - Columns with high selectivity predicates
 *
 * @param rpd_table Table metadata
 * @param col_idx Column index
 * @param func_type Type of predicate
 * @return true if column benefits from Storage Index
 */
bool StorageIndexPrune::is_column_suitable_for_index(Imcs::RpdTable *rpd_table, uint32_t col_idx,
                                                     Item_func::Functype func_type) {
  if (!rpd_table || col_idx >= rpd_table->meta().num_columns) return false;

  auto &col_meta = rpd_table->meta().fields[col_idx];
  // 1. Check data type - some types work better
  switch (col_meta.type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME:
      // Numeric and temporal types - excellent for min/max range checking
      break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
      // String types - good for equality, less so for ranges
      if (func_type == Item_func::EQ_FUNC || func_type == Item_func::IN_FUNC) break;  // OK for equality
      // For range predicates on strings, benefit is lower
      return true;  // Still allow, but with lower benefit estimation
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      // BLOBs - not good for Storage Index (no meaningful min/max)
      return false;
    default:
      return false;
  }

  // 2. Check if column has statistics available
  // (TODO: check if min/max are tracked)

  // 3. Check for monotonicity hints (if available in metadata)
  // Monotonic columns (like auto-increment ID, timestamp) benefit most
  // if (col_meta.is_monotonic) {
  //   return true;  // Excellent candidate
  // }

  // 4. Check NDV (Number of Distinct Values)
  // High NDV with selective predicates -> good pruning
  // Low NDV (e.g., boolean) -> less benefit
  uint64_t ndv = col_meta.distinct_count;
  uint64_t total_rows = rpd_table->meta().total_rows.load();
  if (ndv > 0 && total_rows > 0) {
    double cardinality_ratio = static_cast<double>(ndv) / total_rows;
    if (cardinality_ratio < 0.01) {  // Very low cardinality
      return false;                  // Not much benefit from IMCU pruning
    }
  }
  return true;
}

/**
 * Estimate the benefit of Storage Index pruning and update scan cost
 *
 * This function calculates:
 * 1. Expected IMCU skip ratio based on predicates
 * 2. Updated cardinality (rows after pruning)
 * 3. Updated cost (reduced I/O and processing)
 *
 * @param scan Scan node to update
 * @param predicates Predicates that will be used for pruning
 */
void StorageIndexPrune::estimate_pruning_benefit(ScanTable *scan, const std::vector<Item *> &predicates) {
  if (!scan || !scan->rpd_table || predicates.empty()) return;

  auto &table_meta = scan->rpd_table->meta();
  size_t total_imcus = table_meta.total_imcus.load(std::memory_order_acquire);

  if (total_imcus == 0) return;  // No IMCUs to prune

  // Calculate combined selectivity of all predicates
  double combined_selectivity = 1.0;
  double imcu_skip_ratio = 0.0;  // Fraction of IMCUs we can skip
  for (auto *pred : predicates) {
    // Row-level selectivity (how many rows pass the predicate)
    double row_selectivity = estimate_predicate_selectivity(pred, scan->rpd_table);
    combined_selectivity *= row_selectivity;

    // IMCU-level skip ratio (how many IMCUs we can skip entirely)
    double imcu_selectivity = estimate_imcu_skip_ratio(pred, scan->rpd_table);
    imcu_skip_ratio = std::max(imcu_skip_ratio, imcu_selectivity);
  }

  // For multiple predicates with AND, combine IMCU skip ratios
  // Conservative estimate: use maximum skip ratio among predicates
  // (In practice, each IMCU needs min/max check for all predicates)

  // Update cardinality based on row-level selectivity
  ha_rows original_rows = scan->estimated_rows;
  scan->estimated_rows = static_cast<ha_rows>(original_rows * combined_selectivity);

  // Update cost based on IMCU-level pruning
  // Cost model:
  // 1. IMCU header scan cost (read all IMCU headers to check min/max)
  // 2. IMCU data read cost (only for non-skipped IMCUs)
  // 3. Row processing cost (only for rows that pass predicates)

  double original_cost = scan->cost;

  // Cost breakdown assumptions:
  constexpr double HEADER_SCAN_COST_RATIO = 0.02;  // 2% - scan IMCU metadata
  constexpr double IMCU_READ_COST_RATIO = 0.68;    // 68% - read IMCU data from disk/memory
  constexpr double ROW_PROCESS_COST_RATIO = 0.30;  // 30% - decompress and process rows

  double header_scan_cost = original_cost * HEADER_SCAN_COST_RATIO;
  double imcu_read_cost = original_cost * IMCU_READ_COST_RATIO * (1.0 - imcu_skip_ratio);
  double row_process_cost = original_cost * ROW_PROCESS_COST_RATIO * combined_selectivity;

  double new_cost = header_scan_cost + imcu_read_cost + row_process_cost;
  scan->cost = new_cost;

  // Log the pruning benefit (for debugging)
  DBUG_PRINT("rapid_optimizer", ("StorageIndexPrune: table=%s, IMCUs=%zu, skip_ratio=%.2f, "
                                 "row_selectivity=%.4f, cost: %.2f -> %.2f (%.1f%% reduction)",
                                 table_meta.table_name.c_str(), total_imcus, imcu_skip_ratio, combined_selectivity,
                                 original_cost, new_cost, (original_cost - new_cost) / original_cost * 100.0));
}

/**
 * Estimate row-level selectivity of a predicate
 *
 * @param pred Predicate to estimate
 * @param rpd_table Table for statistics
 * @return Selectivity estimate [0.0, 1.0]
 */
double StorageIndexPrune::estimate_predicate_selectivity(Item *pred, Imcs::RpdTable *rpd_table) {
  if (!pred) return 1.0;

  switch (pred->type()) {
    case Item::FUNC_ITEM: {
      auto *func = static_cast<Item_func *>(pred);
      switch (func->functype()) {
        case Item_func::EQ_FUNC: {
          // col = value, Selectivity ≈ 1 / NDV
          Item **args = func->arguments();
          if (args[0]->type() == Item::FIELD_ITEM) {
            auto *field = static_cast<Item_field *>(args[0]);
            uint32_t col_idx = field->field->field_index();
            uint64_t ndv = rpd_table->meta().fields[col_idx].distinct_count;
            return (ndv > 0) ? 1.0 / ndv : 0.01;
          }
          return 0.1;
        } break;
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC:
          // Range predicates
          // Without histograms, assume 33% selectivity
          return 0.33;
        case Item_func::BETWEEN:
          // BETWEEN typically more selective than single-ended range
          return 0.15;
        case Item_func::IN_FUNC: {
          auto *in_func = static_cast<Item_func_in *>(func);
          uint32_t list_size = in_func->argument_count() - 1;

          // Selectivity ≈ list_size / NDV
          // Without NDV, estimate based on list size
          if (list_size == 1) return 0.01;
          if (list_size <= 5) return 0.05;
          if (list_size <= 20) return 0.15;
          return 0.30;
        } break;
        case Item_func::ISNULL_FUNC:
          // Depends on null percentage in column
          // Default: assume 5% nulls
          return 0.05;
        case Item_func::ISNOTNULL_FUNC:
          return 0.95;
        default:
          return 0.5;  // Unknown predicate type
      }
    } break;
    case Item::COND_ITEM: {
      auto *cond = static_cast<Item_cond *>(pred);
      if (cond->functype() == Item_func::COND_AND_FUNC) {
        // AND: multiply selectivities (independence assumption)
        double sel = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          sel *= estimate_predicate_selectivity(item, rpd_table);
        }
        return sel;
      } else if (cond->functype() == Item_func::COND_OR_FUNC) {
        // OR: 1 - ∏(1 - sel_i)
        double complement = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          complement *= (1.0 - estimate_predicate_selectivity(item, rpd_table));
        }
        return 1.0 - complement;
      }
    } break;
    default:
      return 0.5;
  }
  return 0.5;
}

/**
 * Estimate IMCU-level skip ratio for a predicate
 *
 * This estimates what fraction of IMCUs can be entirely skipped
 * based on min/max metadata without reading IMCU data.
 *
 * @param pred Predicate to analyze
 * @param rpd_table Table for statistics
 * @return Fraction of IMCUs that can be skipped [0.0, 1.0]
 */
double StorageIndexPrune::estimate_imcu_skip_ratio(Item *pred, Imcs::RpdTable *rpd_table) {
  if (!pred) return 0.0;

  switch (pred->type()) {
    case Item::FUNC_ITEM: {
      auto *func = static_cast<Item_func *>(pred);
      switch (func->functype()) {
        case Item_func::EQ_FUNC: {
          // col = value
          // Can skip IMCUs where: value < min OR value > max
          //
          // Estimation: If data is uniformly distributed across IMCUs,
          // we can skip (NDV - 1) / NDV of IMCUs
          //
          // For clustered data (e.g., sorted by this column), skip ratio is higher
          // For random data, skip ratio is lower
          // Conservative estimate for random data
          return 0.30;  // Can skip ~30% of IMCUs
        } break;
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC: {
          // col > value (or <, <=, >=)
          // Can skip IMCUs where: max < value (for col > value)
          //                       min > value (for col < value)
          // For sorted/clustered data, this is very effective
          // Estimate depends on data distribution
          return 0.40;  // Conservative estimate, Can skip ~40% of IMCUs
        } break;
        case Item_func::BETWEEN: {
          // col BETWEEN low AND high
          // Can skip IMCUs where: max < low OR min > high
          // BETWEEN on clustered columns is excellent for pruning
          return 0.50;  // Can skip ~50% of IMCUs
        } break;
        case Item_func::IN_FUNC: {
          // col IN (v1, v2, ..., vN)
          // Can skip IMCUs where min/max range doesn't overlap with any values
          auto *in_func = static_cast<Item_func_in *>(func);
          uint32_t list_size = in_func->argument_count() - 1;
          return (list_size <= 5) ? 0.35 /**Small IN list, good pruning */ : 0.20 /** // Large IN list, less pruning*/;
        } break;
        case Item_func::ISNULL_FUNC: {
          // col IS NULL
          // Can skip IMCUs with null_count = 0
          // Estimate: If nulls are rare and clustered
          return 0.70;  // Can skip most IMCUs without nulls
        } break;
        case Item_func::ISNOTNULL_FUNC: {
          // col IS NOT NULL
          // Can skip IMCUs where all values are null (rare)
          return 0.05;  // Can only skip all-null IMCUs
        } break;
        default:
          return 0.10;  // Conservative default
      }
    } break;
    case Item::COND_ITEM: {
      auto *cond = static_cast<Item_cond *>(pred);
      if (cond->functype() == Item_func::COND_AND_FUNC) {
        // AND: Use maximum skip ratio (any predicate can prune)
        double max_skip = 0.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          double skip = estimate_imcu_skip_ratio(item, rpd_table);
          max_skip = std::max(max_skip, skip);
        }
        return max_skip;
      } else if (cond->functype() == Item_func::COND_OR_FUNC) {
        // OR: Use minimum skip ratio (all predicates must agree to skip)
        double min_skip = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          double skip = estimate_imcu_skip_ratio(item, rpd_table);
          min_skip = std::min(min_skip, skip);
        }
        return min_skip;
      }
    } break;
    default:
      return 0.0;
  }
  return 0.0;
}

/**
 * Split a condition into conjunctive predicates (AND)
 */
void StorageIndexPrune::split_conjunctions(Item *condition, std::vector<Item *> &predicates) {
  if (!condition) return;

  if (condition->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(condition);
    if (cond->functype() == Item_func::COND_AND_FUNC) {
      // Recursively split AND conditions
      List_iterator<Item> li(*cond->argument_list());
      Item *item;
      while ((item = li++)) {
        split_conjunctions(item, predicates);
      }
      return;
    }
  }
  // Base case: not an AND condition, add as-is
  predicates.push_back(condition);
}

/**
 * Get all tables referenced by an item
 */
std::unordered_set<std::string> StorageIndexPrune::get_referenced_tables(Item *item) {
  std::unordered_set<std::string> tables;
  if (!item) return tables;

  std::function<void(Item *)> collect_tables = [&](Item *it) {
    if (!it) return;
    if (it->type() == Item::FIELD_ITEM) {
      auto *field_item = static_cast<Item_field *>(it);
      if (field_item->field && field_item->field->table) {
        TABLE *table = field_item->field->table;
        std::string key = std::string(table->s->db.str) + "." + std::string(table->s->table_name.str);
        tables.insert(key);
      }
      return;
    }

    if (it->type() == Item::FUNC_ITEM) {
      auto *func = static_cast<Item_func *>(it);
      for (uint i = 0; i < func->argument_count(); i++) {
        collect_tables(func->arguments()[i]);
      }
      return;
    }

    if (it->type() == Item::COND_ITEM) {
      auto *cond = static_cast<Item_cond *>(it);
      List_iterator<Item> li(*cond->argument_list());
      Item *child;
      while ((child = li++)) {
        collect_tables(child);
      }
      return;
    }

    if (it->type() == Item::REF_ITEM) {
      auto *ref = static_cast<Item_ref *>(it);
      if (ref->ref_item()) {
        collect_tables(ref->ref_item());
      }
    }
  };

  collect_tables(item);
  return tables;
}

/**
 * Get available tables in a plan subtree
 */
std::unordered_set<std::string> StorageIndexPrune::get_available_tables(const Plan &node) {
  std::unordered_set<std::string> tables;
  if (!node) return tables;

  if (node->type() == PlanNode::Type::SCAN) {
    auto *scan = static_cast<ScanTable *>(node.get());
    if (scan->source_table) {
      std::string key =
          std::string(scan->source_table->s->db.str) + "." + std::string(scan->source_table->s->table_name.str);
      tables.insert(key);
    }
  } else {
    for (const auto &child : node->children) {
      auto child_tables = get_available_tables(child);
      tables.insert(child_tables.begin(), child_tables.end());
    }
  }
  return tables;
}

/**
 * Check if item contains aggregate function references
 */
bool StorageIndexPrune::contains_aggregate_reference(Item *item) {
  if (!item) return false;

  if (item->type() == Item::SUM_FUNC_ITEM) return true;

  if (item->type() == Item::FUNC_ITEM || item->type() == Item::COND_ITEM) {
    auto *func = static_cast<Item_func *>(item);
    for (uint i = 0; i < func->argument_count(); i++) {
      if (contains_aggregate_reference(func->arguments()[i])) return true;
    }
  }

  if (item->type() == Item::REF_ITEM) {
    auto *ref = static_cast<Item_ref *>(item);
    if (ref->ref_item()) {
      return contains_aggregate_reference(ref->ref_item());
    }
  }
  return false;
}

void ProjectionPruning::apply(Plan &root) {
  if (!root) return;

  // Step 1: Collect all referenced columns from the entire plan tree
  auto referenced_columns = collect_referenced_columns(root);
  // Step 2: Push the pruned column set down to each ScanTable node
  auto projection_prun_func = [&](PlanNode *node) {
    if (node->type() != PlanNode::Type::SCAN) return;
    auto *scan = static_cast<ScanTable *>(node);
    if (!scan->rpd_table) return;

    // Build table key: "db.table"
    std::string table_key =
        std::string(scan->rpd_table->meta().db_name) + "." + std::string(scan->rpd_table->meta().table_name);
    // Check if we have any referenced columns for this table
    auto it = referenced_columns.find(table_key);
    if (it != referenced_columns.end()) {
      const auto &required_cols = it->second;

      // For now, we can estimate the cost reduction
      size_t total_columns = scan->rpd_table->meta().num_columns;
      size_t required_columns = required_cols.size();

      if (required_columns < total_columns && required_columns > 0) {
        double pruning_ratio = static_cast<double>(required_columns) / total_columns;  // Calculate pruning ratio
        scan->cost *= pruning_ratio;  // Adjust cost based on fewer columns to read
        scan->projected_columns.assign(required_cols.begin(), required_cols.end());
      }
    } else {
      // Edge case: No columns explicitly referenced
      // This can happen with COUNT(*) queries
      // In this case, we only need to read the row count metadata
      // Most efficient: just scan the IMCU headers without reading any CU data
      // Extreme optimization for COUNT(*)
      scan->cost *= 0.1;  // Very cheap, just count rows
    }
  };

  WalkPlan(root.get(), projection_prun_func);
}

std::map<std::string, std::set<uint32_t>> ProjectionPruning::collect_referenced_columns(Plan &root) {
  std::map<std::string, std::set<uint32_t>> columns;

  if (!root) return columns;

  auto collect_ref_columns_func = [&](PlanNode *node) {
    switch (node->type()) {
      case PlanNode::Type::HASH_JOIN:
      case PlanNode::Type::NESTED_LOOP_JOIN: {  // Collect columns from join conditions
        if (node->type() == PlanNode::Type::HASH_JOIN) {
          auto *join = static_cast<HashJoin *>(node);
          collect_from_join_conditions(join->join_conditions, columns);
        } else {
          auto *join = static_cast<NestLoopJoin *>(node);
          collect_from_join_conditions(join->join_conditions, columns);
        }
      } break;
      case PlanNode::Type::LOCAL_AGGREGATE: {
        auto *agg = static_cast<LocalAgg *>(node);
        collect_from_aggregation(agg, columns);
      } break;
      case PlanNode::Type::FILTER: {
        auto *filter = static_cast<Filter *>(node);
        collect_from_filter(filter, columns);
      } break;
      case PlanNode::Type::SORT: {  // Collect columns from `ORDER BY`
        auto *topn = static_cast<Sort *>(node);
        for (ORDER *ord = topn->order; ord; ord = ord->next) {
          if (ord->item) collect_from_item(*ord->item, columns);
        }
      } break;
      case PlanNode::Type::TOP_N: {  // Collect columns from `ORDER BY... LIMIT`
        auto *topn = static_cast<TopN *>(node);
        for (ORDER *ord = topn->order; ord; ord = ord->next) {
          if (ord->item) collect_from_item(*ord->item, columns);
        }
      } break;
      default:
        break;
    }
  };

  // Walk the entire plan tree and collect column references
  WalkPlan(root.get(), collect_ref_columns_func);
  return columns;
}

void ProjectionPruning::collect_from_join_conditions(const std::vector<Item *> &conditions,
                                                     std::map<std::string, std::set<uint32_t>> &columns) {
  for (auto *item : conditions) collect_from_item(item, columns);
}

void ProjectionPruning::collect_from_aggregation(const LocalAgg *agg,
                                                 std::map<std::string, std::set<uint32_t>> &columns) {
  // Collect from GROUP BY
  for (auto *item : agg->group_by) collect_from_item(item, columns);
  // Collect from ORDER BY
  for (auto *item : agg->order_by) collect_from_item(item, columns);
  // Collect from aggregate functions (SUM, AVG, MIN, MAX, etc.)
  for (auto *func : agg->aggregates) collect_from_item(func, columns);
}

void ProjectionPruning::collect_from_filter(const Filter *filter, std::map<std::string, std::set<uint32_t>> &columns) {
  // If filter->condition (Item*) is available
  if (filter->condition) {
    collect_from_item(filter->condition, columns);
  }
}

void ProjectionPruning::collect_from_item(Item *item, std::map<std::string, std::set<uint32_t>> &columns) {
  if (!item) return;

  // Handle Field Items (the leaf nodes we're looking for)
  if (item->type() == Item::FIELD_ITEM) {
    auto *field_item = static_cast<Item_field *>(item);

    // Ensure field and table are valid
    if (field_item->field && field_item->field->table) {
      Field *field = field_item->field;
      TABLE *table = field->table;

      // Construct unique key: "db.table"
      std::string key = std::string(table->s->db.str) + "." + std::string(table->s->table_name.str);
      // Insert the field index (0-based)
      columns[key].insert(field->field_index());
    }
    return;
  }

  // Handle Function Items (e.g., ABS(col), col1 + col2)
  if (item->type() == Item::FUNC_ITEM || item->type() == Item::SUM_FUNC_ITEM) {
    auto *func = static_cast<Item_func *>(item);
    for (uint i = 0; i < func->argument_count(); i++) {
      collect_from_item(func->arguments()[i], columns);
    }
    return;
  }

  // Handle Condition Items (e.g., AND, OR, XOR)
  if (item->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(item);
    List_iterator<Item> li(*cond->argument_list());
    Item *it;
    while ((it = li++)) {
      collect_from_item(it, columns);
    }
    return;
  }

  // Handle Reference Items (e.g., references in HAVING or ORDER BY)
  if (item->type() == Item::REF_ITEM) {
    auto *ref = static_cast<Item_ref *>(item);
    auto *ref_item = ref->ref_item();
    if (ref_item) collect_from_item(ref_item, columns);
    return;
  }
  // Handle Subselect Items (if needed)
  // Usually for projection pruning, we focus on the current query level
}
}  // namespace Optimizer
}  // namespace ShannonBase