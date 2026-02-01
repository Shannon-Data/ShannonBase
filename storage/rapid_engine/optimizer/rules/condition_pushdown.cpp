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
#include "storage/rapid_engine/optimizer/rules/condition_pushdown.h"

#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/sql_class.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
void PredicatePushDown::apply(Plan &root) {
  if (!root) return;

  std::vector<Item *> pending_filters;
  root = push_down_recursive(root, pending_filters);

  // If there are still pending filters at the top, wrap in a Filter node
  if (root && !pending_filters.empty()) {
    Item *combined = combine_with_and(pending_filters);
    root = create_filter_node(std::move(root), combined);
  }
}

Plan PredicatePushDown::push_down_recursive(Plan &node, std::vector<Item *> &pending_filters) {
  if (!node) return nullptr;

  switch (node->type()) {
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node.get());

      // Split the filter condition into conjunctions
      std::vector<Item *> new_filters;
      split_conjunctions(filter->condition, new_filters);
      pending_filters.insert(pending_filters.end(), new_filters.begin(), new_filters.end());

      // Remove the filter node and continue with its child
      Plan child = std::move(filter->children[0]);
      auto optimized_child = push_down_recursive(child, pending_filters);
      return wrap_if_pending(std::move(optimized_child), pending_filters);
    } break;
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      return push_below_join(node, pending_filters);
    } break;
    case PlanNode::Type::SCAN: {
      return push_into_scan(node, pending_filters);
    } break;
    case PlanNode::Type::LOCAL_AGGREGATE: {
      auto *agg = static_cast<LocalAgg *>(node.get());

      std::vector<Item *> pushable_to_child;
      std::vector<Item *> remaining_at_agg;
      for (auto *filter : pending_filters) {
        // Core check: Does the predicate contain aggregate function references?
        // Filter conditions after aggregation (similar to HAVING) cannot be pushed down below the aggregate operator
        if (contains_aggregate_reference(filter)) {
          remaining_at_agg.push_back(filter);
        } else {
          // Predicates that only reference regular columns or grouping columns can continue to be pushed down
          pushable_to_child.push_back(filter);
        }
      }

      // We pass down pushable_to_child; child nodes (e.g., Scan) will try to absorb them
      agg->children[0] = push_down_recursive(agg->children[0], pushable_to_child);

      // Collect predicates that were "rejected by child nodes and returned" and predicates that couldn't be pushed
      // down at this level
      pending_filters = remaining_at_agg;
      if (!pushable_to_child.empty()) {
        pending_filters.insert(pending_filters.end(), pushable_to_child.begin(), pushable_to_child.end());
      }

      // Use wrap_if_pending to intercept all predicates that cannot be pushed further down directly above the Agg
      // node This ensures that post-aggregation filtering (HAVING logic) is executed right after the Agg node
      return wrap_if_pending(std::move(node), pending_filters);
    } break;
    case PlanNode::Type::LIMIT: {
      auto *limit = static_cast<Limit *>(node.get());
      // Push filters below LIMIT
      limit->children[0] = push_down_recursive(limit->children[0], pending_filters);
      return wrap_if_pending(std::move(node), pending_filters);
    } break;
    case PlanNode::Type::SORT: {
      auto *sort = static_cast<Sort *>(node.get());
      // Push filters below SORT/TOP_N
      sort->children[0] = push_down_recursive(sort->children[0], pending_filters);
      return std::move(node);
    } break;
    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());
      // Push filters below SORT/TOP_N
      topn->children[0] = push_down_recursive(topn->children[0], pending_filters);
      return std::move(node);
    } break;
    default:  // For other node types, don't push down
      return std::move(node);
  }
}

Plan PredicatePushDown::push_below_join(Plan &join, std::vector<Item *> &pending_filters) {
  bool is_hash_join = (join->type() == PlanNode::Type::HASH_JOIN);

  // Get references to children (don't move them yet)
  auto &left_child = join->children[0];
  auto &right_child = join->children[1];

  // Get available tables in each subtree
  auto left_tables = get_available_tables(left_child);
  auto right_tables = get_available_tables(right_child);
  auto all_tables = left_tables;
  all_tables.insert(right_tables.begin(), right_tables.end());

  // Partition filters into three categories
  std::vector<Item *> left_filters;       // Only references left tables
  std::vector<Item *> right_filters;      // Only references right tables
  std::vector<Item *> join_filters;       // References both sides
  std::vector<Item *> remaining_filters;  // Can't be pushed (e.g., subqueries)

  for (const auto &filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);
    bool all_in_left{true}, all_in_right{true}, has_external{false};

    if (referenced.empty()) {  // const expr.
      left_filters.push_back(filter);
      continue;
    }

    for (const auto &table : referenced) {
      if (left_tables.find(table) == left_tables.end()) all_in_left = false;
      if (right_tables.find(table) == right_tables.end()) all_in_right = false;
      if (all_tables.find(table) == all_tables.end()) has_external = true;
    }

    if (has_external) {
      remaining_filters.push_back(filter);
    } else if (all_in_left) {
      left_filters.push_back(filter);
    } else if (all_in_right) {
      right_filters.push_back(filter);
    } else {  // both sides predicate
      join_filters.push_back(filter);
    }
  }

  // in-place processing the remaining filters
  join->children[0] = push_down_recursive(join->children[0], left_filters);
  join->children[0] = wrap_if_pending(std::move(join->children[0]), left_filters);
  join->children[1] = push_down_recursive(join->children[1], right_filters);
  join->children[1] = wrap_if_pending(std::move(join->children[1]), right_filters);

  if (!join_filters.empty()) {
    // only simple cond can be pushed into join conditions (for hash join build)
    // the other complex predicates are kept as remaining filters to be handled
    // at upper apply level or wrapped above the Join
    std::vector<Item *> pushable_to_join;
    for (const auto &jf : join_filters) {
      if (is_simple_predicate(jf))
        pushable_to_join.push_back(jf);
      else
        remaining_filters.push_back(jf);
    }

    if (!pushable_to_join.empty()) {
      if (is_hash_join) {
        auto *hj = static_cast<HashJoin *>(join.get());
        hj->join_conditions.insert(hj->join_conditions.end(), pushable_to_join.begin(), pushable_to_join.end());
      } else {
        auto *nj = static_cast<NestLoopJoin *>(join.get());
        nj->join_conditions.insert(nj->join_conditions.end(), pushable_to_join.begin(), pushable_to_join.end());
      }
    }
  }
  pending_filters = remaining_filters;
  return std::move(join);
}

Plan PredicatePushDown::push_into_scan(Plan &scan, std::vector<Item *> &pending_filters) {
  auto *scan_node = static_cast<ScanTable *>(scan.get());
  if (pending_filters.empty()) return std::move(scan);
  std::string table_key;
  assert(scan_node->source_table);
  table_key =
      std::string(scan_node->source_table->s->db.str).append(".").append(scan_node->source_table->s->table_name.str);

  std::vector<Item *> to_storage_engine;  // simple predicate：push down to (Storage Index)
  std::vector<Item *> to_filter_node;     // complex predicate：to be Filter operator
  std::vector<Item *> remaining_others;   // not belonging to this table

  for (const auto &filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);
    bool belongs_here =
        referenced.empty() || (referenced.size() == 1 && referenced.find(table_key) != referenced.end());
    if (belongs_here) {
      if (is_simple_predicate(filter)) {
        // simple predicate,（such a=1, b>10）：push to storage engine
        to_storage_engine.push_back(filter);
      } else {
        // complex predicate（such ABS(id)=5）：can not pushdown，to be Filter in-place.
        to_filter_node.push_back(filter);
      }
    } else {
      // not belonging to this table (e.g., Join another side of the table), keep in pending_filters
      remaining_others.push_back(filter);
    }
  }

  pending_filters = remaining_others;
  if (!to_storage_engine.empty()) {
    // stores predicate into ScanNode，perform IMCU Skipping.
    auto predicates = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
    for (const auto &f : to_storage_engine) {
      auto child_pre = Optimizer::convert_item_to_predicate(current_thd, f);
      if (!child_pre) {
        to_filter_node.push_back(f);
        continue;
      }
      predicates->add_child(std::move(child_pre));
    }

    if (predicates->children.empty()) {
      // all predicates failed to convert, then degrade to filter node
      to_filter_node.insert(to_filter_node.end(), to_storage_engine.begin(), to_storage_engine.end());
    } else {
      scan_node->prune_predicate = std::move(predicates);
      scan_node->use_storage_index = true;

      // update Cost：due to pushdown.
      double total_selectivity = 1.0;
      for (const auto &f : to_storage_engine) {
        total_selectivity *= estimate_selectivity(f);
      }
      scan_node->estimated_rows = static_cast<ha_rows>(scan_node->estimated_rows * total_selectivity);
      scan_node->cost *= (0.1 + 0.9 * total_selectivity);
    }
  }

  if (!to_filter_node.empty()) {
    Item *combined_cond = combine_with_and(to_filter_node);
    double filter_selectivity = 1.0;
    for (const auto &f : to_filter_node) {
      filter_selectivity *= estimate_selectivity(f);
    }
    auto filter_node = create_filter_node(std::move(scan), combined_cond);
    filter_node->estimated_rows = static_cast<ha_rows>(filter_node->estimated_rows * filter_selectivity);
    return filter_node;
  }
  return std::move(scan);
}

void PredicatePushDown::split_conjunctions(Item *condition, std::vector<Item *> &predicates) {
  if (!condition) return;

  // Check if this is an AND condition
  if (condition->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(condition);
    if (cond->functype() == Item_func::COND_AND_FUNC) {
      // Recursively split each argument
      List_iterator<Item> li(*(cond->argument_list()));
      Item *item;
      while ((item = li++)) {
        split_conjunctions(item, predicates);
      }
      return;
    }
  }
  // Not an AND - add as-is
  predicates.push_back(condition);
}

std::unordered_set<std::string> PredicatePushDown::get_referenced_tables(Item *item) {
  std::unordered_set<std::string> tables;
  if (!item) return tables;

  // Use MySQL's built-in used_tables() bitmap
  table_map used = item->used_tables();
  if (used == 0) return tables;  // Constant expression

  // Walk the item tree to find table references
  std::function<void(Item *)> find_table_refs_func = [&](Item *it) {
    if (!it) return;
    if (it->type() == Item::FIELD_ITEM) {
      auto *field_item = static_cast<Item_field *>(it);
      if (field_item->field && field_item->field->table) {
        TABLE *table = field_item->field->table;
        std::string key = std::string(table->s->db.str).append(".").append(table->s->table_name.str);
        tables.insert(key);
      }
    } else if (it->type() == Item::FUNC_ITEM || it->type() == Item::SUM_FUNC_ITEM) {
      auto *func = static_cast<Item_func *>(it);
      for (uint i = 0; i < func->argument_count(); i++) {
        find_table_refs_func(func->arguments()[i]);
      }
    } else if (it->type() == Item::COND_ITEM) {
      auto *cond = static_cast<Item_cond *>(it);
      List_iterator<Item> li(*(cond->argument_list()));
      Item *arg;
      while ((arg = li++)) {
        find_table_refs_func(arg);
      }
    } else if (it->type() == Item::REF_ITEM) {
      auto ref = static_cast<Item_ref *>(it);
      if (ref->ref_item()) find_table_refs_func(ref->ref_item());
    }
  };

  find_table_refs_func(item);
  return tables;
}

bool PredicatePushDown::can_push_to_subtree(Item *predicate, const std::unordered_set<std::string> &available_tables) {
  auto referenced = get_referenced_tables(predicate);

  // Check if all referenced tables are available
  for (const auto &table : referenced) {
    if (available_tables.find(table) == available_tables.end()) return false;
  }
  return true;
}

std::unordered_set<std::string> PredicatePushDown::get_available_tables(const Plan &node) {
  std::unordered_set<std::string> tables;
  if (!node) return tables;

  if (node->type() == PlanNode::Type::SCAN) {
    auto *scan = static_cast<ScanTable *>(node.get());
    assert(scan->source_table);
    std::string key =
        std::string(scan->source_table->s->db.str).append(".").append(scan->source_table->s->table_name.str);
    tables.insert(key);
  } else {
    // Recursively collect from children
    for (const auto &child : node->children) {
      auto child_tables = get_available_tables(child);
      tables.insert(child_tables.begin(), child_tables.end());
    }
  }
  return tables;
}

Item *PredicatePushDown::combine_with_and(const std::vector<Item *> &predicates) {
  if (predicates.empty()) return nullptr;
  if (predicates.size() == 1) return predicates[0];

  // Create an AND condition
  Item_cond_and *and_cond = new (current_thd->mem_root) Item_cond_and();
  for (auto *pred : predicates) {
    and_cond->add(pred);
  }
  return and_cond;
}

Plan PredicatePushDown::create_filter_node(Plan child, Item *condition) {
  auto filter = std::make_unique<Filter>();
  filter->condition = condition;
  filter->estimated_rows = child->estimated_rows;
  filter->cost = child->cost;
  filter->children.push_back(std::move(child));
  return filter;
}

bool PredicatePushDown::is_simple_predicate(Item *item) {
  if (!item) return false;

  // Simple predicates are comparison operations on columns
  if (item->type() == Item::FUNC_ITEM) {
    auto *func = static_cast<Item_func *>(item);
    switch (func->functype()) {
      case Item_func::EQ_FUNC:
      case Item_func::NE_FUNC:
      case Item_func::LT_FUNC:
      case Item_func::LE_FUNC:
      case Item_func::GT_FUNC:
      case Item_func::GE_FUNC:
      case Item_func::BETWEEN:
        return true;
      default:
        return false;
    }
  }
  return false;
}

bool PredicatePushDown::contains_aggregate_reference(Item *item) {
  if (!item) return false;

  // if item is one of (SUM, AVG, COUNT, etc.)
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
  // for `Item_field` or `Item_cache` etc.，does not contain aggregate reference.
  return false;
}

double PredicatePushDown::estimate_selectivity(Item *predicate) {
  if (!predicate) return 1.0;

  switch (predicate->type()) {
    case Item::FUNC_ITEM: {
      auto *func = static_cast<Item_func *>(predicate);
      return estimate_function_selectivity(func);
    } break;
    case Item::COND_ITEM: {
      auto *cond = static_cast<Item_cond *>(predicate);
      if (cond->functype() == Item_func::COND_AND_FUNC) {
        // AND: Under the assumption of independence, the selection rates are multiplied
        double sel = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          sel *= estimate_selectivity(item);
        }
        return sel;
      } else if (cond->functype() == Item_func::COND_OR_FUNC) {
        // OR: 1 - the probability that none of the items will occur
        double complement = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *item;
        while ((item = li++)) {
          complement *= (1.0 - estimate_selectivity(item));
        }
        return 1.0 - complement;
      }
    } break;
    case Item::REF_ITEM: {
      auto *ref = static_cast<Item_ref *>(predicate);
      return estimate_selectivity(ref->ref_item());
    } break;
    default:
      break;
  }
  return 0.5;  // other conditions, using default selectivity.
}

double PredicatePushDown::estimate_function_selectivity(Item_func *func) {
  switch (func->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:  // <=> operator
      return estimate_equality_selectivity(func);
    case Item_func::NE_FUNC:
      return 1.0 - estimate_equality_selectivity(func);
    case Item_func::LT_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
      return 0.3333;  // Experience value: range query
    case Item_func::BETWEEN:
      return 0.1;  // Experience value: BETWEEN is typically stricter than a single range
    case Item_func::IN_FUNC: {
      // Rough estimate: (1/NDV) * number of arguments, but capped at 0.5
      double eq_sel = estimate_equality_selectivity(func);
      // The first parameter is a column, followed by a list of constants.
      return std::min(0.5, eq_sel * (func->argument_count() - 1));
    } break;
    case Item_func::ISNULL_FUNC:
      return 0.01;  // Assume the data is compact and has fewer NULLs
    case Item_func::ISNOTNULL_FUNC:
      return 0.99;
    case Item_func::LIKE_FUNC:
      return 0.2;  // fuzzy query
    default:
      return 0.5;
  }
}

double PredicatePushDown::estimate_equality_selectivity(Item_func *eq_func) {
  Item **args = eq_func->arguments();
  Item_field *field_item = nullptr;

  // 1. Try to find the field (col = const or const = col)
  if (args[0]->type() == Item::FIELD_ITEM) {
    field_item = static_cast<Item_field *>(args[0]);
  } else if (args[1]->type() == Item::FIELD_ITEM) {
    field_item = static_cast<Item_field *>(args[1]);
  }

  // 2. try to get NDV (unique values count) from the rapid imcs (column statistics)
  if (field_item && field_item->field) {
    // Assume you have an interface to get storage engine statistics
    uint32_t col_idx = field_item->field->field_index();
    auto share = ShannonBase::shannon_loaded_tables->get(field_item->field->table->s->db.str,
                                                         field_item->field->table->s->table_name.str);
    auto rpd_table = Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
    if (rpd_table) {
      auto ndv = rpd_table->meta().fields[col_idx].distinct_count;
      if (ndv > 0) return 1.0 / static_cast<double>(ndv);
    }

    // If there is no statistics, make heuristic guesses based on whether it's a primary key or unique index
    if (field_item->field->all_flags() & PRI_KEY_FLAG) {
      return 0.0001;  // Very high filtering rate
    }
  }

  // 3. Fallback default value
  // For equality queries, 0.01 (1%) is a better default than 0.1 for modern big data distributions
  return 0.01;
}

/**
 * AggregationPushDown::apply - Push aggregation operations closer to data sources
 *
 * Optimization Strategy:
 * 1. Two-phase aggregation: Local (partial) aggregation near scans + Global (final) aggregation
 * 2. Push aggregation below LIMIT when safe (e.g., no GROUP BY or with complete groups)
 * 3. Decompose complex aggregations into simpler operations when possible
 *
 * Benefits:
 * - Reduces network/memory transfer by aggregating early
 * - Enables parallel partial aggregation across partitions
 * - Decreases memory footprint for subsequent operators
 *
 * Example Transformations:
 *
 * BEFORE:
 *   GlobalAgg (SUM(value))
 *     └─ HashJoin
 *         ├─ Scan(orders)     -- 1M rows
 *         └─ Scan(customers)  -- 100K rows
 *
 * AFTER:
 *   GlobalAgg (SUM(partial_sum))
 *     └─ HashJoin
 *         ├─ LocalAgg (SUM(value) as partial_sum)  -- Reduces to ~1000 rows
 *         │   └─ Scan(orders)
 *         └─ Scan(customers)
 */
void AggregationPushDown::apply(Plan &root) {
  if (!root) return;

  // Start recursive transformation from root
  root = push_aggregation_recursive(root);
}

/**
 * Recursively push aggregation operations down the plan tree
 *
 * @param node Current plan node being processed
 * @return Transformed plan node (may be wrapped or restructured)
 */
Plan AggregationPushDown::push_aggregation_recursive(Plan &node) {
  if (!node) return nullptr;

  switch (node->type()) {
    case PlanNode::Type::LOCAL_AGGREGATE: {
      return handle_aggregation_node(node);
    } break;
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      return handle_join_with_aggregation(node);
    } break;
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node.get());
      filter->children[0] = push_aggregation_recursive(filter->children[0]);
      return std::move(node);
    } break;
    case PlanNode::Type::LIMIT:
    case PlanNode::Type::TOP_N: {
      // Recursively process child first
      auto *limit = static_cast<Limit *>(node.get());
      limit->children[0] = push_aggregation_recursive(limit->children[0]);
      return std::move(node);
    } break;
    case PlanNode::Type::SCAN: {
      // Leaf node - no further pushdown possible
      return std::move(node);
    } break;
    default:
      // For other node types, recursively process children
      for (auto &child : node->children) {
        child = push_aggregation_recursive(child);
      }
      return std::move(node);
  }
}

/**
 * Handle aggregation node optimization
 *
 * Strategies:
 * 1. If child is a Join, consider pushing partial aggregation to join inputs
 * 2. If aggregation is decomposable (SUM, COUNT), create two-phase aggregation
 * 3. Optimize GROUP BY with small cardinality
 */
Plan AggregationPushDown::handle_aggregation_node(Plan &agg_node) {
  auto *agg = static_cast<LocalAgg *>(agg_node.get());

  // Recursively process child first
  agg->children[0] = push_aggregation_recursive(agg->children[0]);

  // Check if we can apply two-phase aggregation
  if (can_apply_two_phase_aggregation(agg)) {
    return create_two_phase_aggregation(std::move(agg_node));
  }

  // Check if we can push partial aggregation below join
  if (agg->children[0]->type() == PlanNode::Type::HASH_JOIN ||
      agg->children[0]->type() == PlanNode::Type::NESTED_LOOP_JOIN) {
    return try_push_below_join(std::move(agg_node));
  }
  return std::move(agg_node);
}

/**
 * Handle joins that have aggregation above them
 *
 * Strategy: If there's no aggregation directly above, just recurse.
 * This function is called when we encounter a join during traversal.
 */
Plan AggregationPushDown::handle_join_with_aggregation(Plan &join_node) {
  bool is_hash_join = (join_node->type() == PlanNode::Type::HASH_JOIN);

  if (is_hash_join) {
    auto *hash_join = static_cast<HashJoin *>(join_node.get());
    hash_join->children[0] = push_aggregation_recursive(hash_join->children[0]);
    hash_join->children[1] = push_aggregation_recursive(hash_join->children[1]);
  } else {
    auto *nest_join = static_cast<NestLoopJoin *>(join_node.get());
    nest_join->children[0] = push_aggregation_recursive(nest_join->children[0]);
    nest_join->children[1] = push_aggregation_recursive(nest_join->children[1]);
  }
  return std::move(join_node);
}

/**
 * Check if aggregation can use two-phase optimization
 *
 * Requirements:
 * 1. All aggregate functions must be decomposable (SUM, COUNT, MIN, MAX)
 * 2. Non-decomposable functions like AVG need special handling (AVG = SUM/COUNT)
 * 3. Must have significant data reduction potential
 */
bool AggregationPushDown::can_apply_two_phase_aggregation(const LocalAgg *agg) {
  // Check if all aggregate functions are decomposable
  for (auto *agg_func : agg->aggregates) {
    if (!is_decomposable_aggregate(agg_func)) return false;
  }

  // Estimate benefit: only worthwhile if we reduce data significantly
  // Heuristic: input rows >> output rows (e.g., 100x reduction)
  ha_rows input_rows = agg->children[0]->estimated_rows;
  ha_rows output_rows = agg->estimated_rows;

  if (output_rows == 0) output_rows = 1;
  double reduction_ratio = static_cast<double>(input_rows) / output_rows;

  // Only apply if reduction is significant (at least 10x)
  return reduction_ratio >= 10.0;
}

/**
 * Check if an aggregate function is decomposable
 *
 * Decomposable: Can be split into local partial + global final phases
 * - SUM: local_sum + global_sum
 * - COUNT: local_count + global_sum
 * - MIN/MAX: local_min/max + global_min/max
 * - AVG: (local_sum, local_count) + global_sum/global_sum
 *
 * Non-decomposable: STDDEV, VARIANCE (need all raw values)
 */
bool AggregationPushDown::is_decomposable_aggregate(const Item_func *agg_func) {
  if (!agg_func || agg_func->type() != Item::SUM_FUNC_ITEM) {
    return false;
  }

  auto *sum_func = static_cast<const Item_sum *>(agg_func);

  switch (sum_func->sum_func()) {
    case Item_sum::SUM_FUNC:
    case Item_sum::COUNT_FUNC:
    case Item_sum::COUNT_DISTINCT_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
    case Item_sum::AVG_FUNC:  // Can decompose as SUM/COUNT
      return true;

    case Item_sum::STD_FUNC:
    case Item_sum::VARIANCE_FUNC:
    case Item_sum::SUM_DISTINCT_FUNC:
    case Item_sum::GROUP_CONCAT_FUNC:
      // These need all raw values or special handling
      return false;

    default:
      return false;
  }
}

/**
 * Create two-phase aggregation plan
 *
 * Transform:
 *   GlobalAgg(SUM(x), AVG(y))
 *     └─ Scan
 *
 * Into:
 *   GlobalAgg(SUM(partial_sum_x), SUM(partial_sum_y)/SUM(partial_count_y))
 *     └─ LocalAgg(SUM(x) as partial_sum_x, SUM(y) as partial_sum_y, COUNT(y) as partial_count_y)
 *         └─ Scan
 */
Plan AggregationPushDown::create_two_phase_aggregation(Plan global_agg_node) {
  auto *global_agg = static_cast<LocalAgg *>(global_agg_node.get());

  // Create local (partial) aggregation node
  auto local_agg = std::make_unique<LocalAgg>();

  // Copy GROUP BY columns (same for both phases)
  local_agg->group_by = global_agg->group_by;

  // Transform aggregate functions for local phase
  for (auto *global_func : global_agg->aggregates) {
    auto *sum_func = static_cast<Item_sum *>(global_func);
    switch (sum_func->sum_func()) {
      case Item_sum::SUM_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        // Direct mapping: local_sum -> global_sum
        local_agg->aggregates.push_back(global_func);
        break;
      case Item_sum::COUNT_FUNC:
        // local_count -> global_sum(local_count)
        local_agg->aggregates.push_back(global_func);
        break;
      case Item_sum::AVG_FUNC:
        // AVG needs decomposition into SUM and COUNT
        // local: SUM(x) as partial_sum, COUNT(x) as partial_count
        // global: SUM(partial_sum) / SUM(partial_count)
        // For now, keep as-is (simplified - production code would transform)
        local_agg->aggregates.push_back(global_func);
        break;
      default:
        // Non-decomposable - keep in global only
        break;
    }
  }

  // Connect: LocalAgg -> original child
  local_agg->children.push_back(std::move(global_agg->children[0]));
  // Estimate costs
  local_agg->estimated_rows = global_agg->estimated_rows;
  local_agg->cost = local_agg->children[0]->cost * 1.5;  // Local agg overhead
  // Connect: GlobalAgg -> LocalAgg
  global_agg->children[0] = std::move(local_agg);
  global_agg->cost = global_agg->children[0]->cost * 1.2;  // Global agg overhead
  return global_agg_node;
}

/**
 * Try to push partial aggregation below join
 *
 * Only safe if:
 * 1. GROUP BY columns are from one side of the join
 * 2. Aggregate columns are also from the same side
 * 3. Join doesn't duplicate rows (e.g., many-to-one relationship)
 *
 * Example:
 * BEFORE:
 *   Agg(SUM(orders.amount) GROUP BY orders.customer_id)
 *     └─ Join(orders.customer_id = customers.id)
 *         ├─ Scan(orders)
 *         └─ Scan(customers)
 *
 * AFTER:
 *   Join(partial_agg.customer_id = customers.id)
 *     ├─ LocalAgg(SUM(amount) GROUP BY customer_id)
 *     │   └─ Scan(orders)
 *     └─ Scan(customers)
 */
Plan AggregationPushDown::try_push_below_join(Plan agg_node) {
  auto *agg = static_cast<LocalAgg *>(agg_node.get());
  auto &join = agg->children[0];

  // Get tables from left and right sides
  auto left_tables = get_available_tables(join->children[0]);
  auto right_tables = get_available_tables(join->children[1]);

  // Check if all GROUP BY and aggregate columns are from one side
  bool all_from_left = true;
  bool all_from_right = true;

  // Check GROUP BY columns
  for (auto *group_item : agg->group_by) {
    auto tables = get_item_tables(group_item);
    for (const auto &table : tables) {
      if (left_tables.find(table) == left_tables.end()) all_from_left = false;
      if (right_tables.find(table) == right_tables.end()) all_from_right = false;
    }
  }

  // Check aggregate columns
  for (auto *agg_func : agg->aggregates) {
    auto tables = get_item_tables(agg_func);
    for (const auto &table : tables) {
      if (left_tables.find(table) == left_tables.end()) all_from_left = false;
      if (right_tables.find(table) == right_tables.end()) all_from_right = false;
    }
  }

  // If all columns are from one side, we can push down
  if (all_from_left && !all_from_right) {
    return push_aggregation_to_join_side(std::move(agg_node), join, true);
  } else if (all_from_right && !all_from_left) {
    return push_aggregation_to_join_side(std::move(agg_node), join, false);
  }
  // Cannot push - columns from both sides
  return agg_node;
}

/**
 * Push aggregation to one side of the join
 */
Plan AggregationPushDown::push_aggregation_to_join_side(Plan agg_node, Plan &join, bool push_to_left) {
  auto *agg = static_cast<LocalAgg *>(agg_node.get());

  // Create new aggregation node for the join side
  auto side_agg = std::make_unique<LocalAgg>();
  side_agg->group_by = agg->group_by;
  side_agg->aggregates = agg->aggregates;
  side_agg->order_by = agg->order_by;

  if (push_to_left) {
    // Insert LocalAgg above left child
    side_agg->children.push_back(std::move(join->children[0]));
    side_agg->estimated_rows = agg->estimated_rows;
    side_agg->cost = side_agg->children[0]->cost * 1.5;
    join->children[0] = std::move(side_agg);
  } else {
    // Insert LocalAgg above right child
    side_agg->children.push_back(std::move(join->children[1]));
    side_agg->estimated_rows = agg->estimated_rows;
    side_agg->cost = side_agg->children[0]->cost * 1.5;
    join->children[1] = std::move(side_agg);
  }

  // Update join cost
  join->cost = join->children[0]->cost + join->children[1]->cost;
  join->estimated_rows = agg->estimated_rows;
  // Remove the top-level aggregation and return just the join
  return std::move(join);
}

/**
 * Get tables referenced by an Item
 */
std::unordered_set<std::string> AggregationPushDown::get_item_tables(Item *item) {
  std::unordered_set<std::string> tables;
  if (!item) return tables;

  std::function<void(Item *)> visit = [&](Item *it) {
    if (!it) return;

    if (it->type() == Item::FIELD_ITEM) {
      auto *field_item = static_cast<Item_field *>(it);
      if (field_item->field && field_item->field->table) {
        TABLE *table = field_item->field->table;
        std::string key = std::string(table->s->db.str) + "." + std::string(table->s->table_name.str);
        tables.insert(key);
      }
    } else if (it->type() == Item::FUNC_ITEM || it->type() == Item::SUM_FUNC_ITEM) {
      auto *func = static_cast<Item_func *>(it);
      for (uint i = 0; i < func->argument_count(); i++) {
        visit(func->arguments()[i]);
      }
    } else if (it->type() == Item::REF_ITEM) {
      auto *ref = static_cast<Item_ref *>(it);
      if (ref->ref_item()) visit(ref->ref_item());
    }
  };

  visit(item);
  return tables;
}

/**
 * Get available tables in a plan subtree
 * (Reuses PredicatePushDown's implementation)
 */
std::unordered_set<std::string> AggregationPushDown::get_available_tables(const Plan &node) {
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

void TopNPushDown::apply(Plan &root) {
  if (!root) return;
  // Start recursion with no pending limit
  root = push_limit_recursive(root, 0, 0, nullptr);
}

/**
 * Recursively push down LIMIT/TOP N
 * @param node Current plan node
 * @param pending_limit Pending limit to push down
 * @param pending_offset Pending offset
 * @param pending_order ORDER BY for TopN (nullptr if just LIMIT)
 * @return Modified plan node
 */
Plan TopNPushDown::push_limit_recursive(Plan &node, ha_rows pending_limit, ha_rows pending_offset, ORDER *pending_order,
                                        Filesort *pending_filesort) {
  if (!node) return nullptr;
  switch (node->type()) {
    case PlanNode::Type::LIMIT: {
      auto *limit_node = static_cast<Limit *>(node.get());
      // Merge with pending limit if exists
      ha_rows new_limit = limit_node->limit;
      ha_rows new_offset = limit_node->offset;

      if (pending_limit > 0 && pending_limit != HA_POS_ERROR)
        merge_limits(pending_limit, pending_offset, limit_node->limit, limit_node->offset, new_limit, new_offset);

      // Remove current limit node and continue with child
      Plan child = std::move(limit_node->children[0]);
      return push_limit_recursive(child, new_limit, new_offset, pending_order, pending_filesort);
    } break;
    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());
      ha_rows new_limit = topn->limit;
      ha_rows new_offset = pending_offset;
      ORDER *new_order = topn->order;
      Filesort *filesort_to_use = topn->filesort ? topn->filesort : pending_filesort;

      // If we have a pending limit, merge it
      if (pending_limit > 0 && pending_limit != HA_POS_ERROR)
        merge_limits(pending_limit, pending_offset, topn->limit, 0, new_limit, new_offset);

      Plan child = std::move(topn->children[0]);
      return push_limit_recursive(child, new_limit, new_offset, new_order, filesort_to_use);
    } break;
    case PlanNode::Type::SORT: {
      auto *sort = static_cast<Sort *>(node.get());
      ORDER *new_order = sort->order;
      Filesort *filesort_to_use = sort->filesort ? sort->filesort : pending_filesort;

      // if pending limit，SORT to TOP_N
      if (pending_limit > 0 && pending_limit != HA_POS_ERROR) {
        Plan child = std::move(sort->children[0]);
        child = push_limit_recursive(child, 0, 0, nullptr, filesort_to_use);
        return create_topn_node(std::move(child), pending_limit, pending_offset, new_order, filesort_to_use);
      }

      sort->children[0] = push_limit_recursive(sort->children[0], 0, 0, nullptr, filesort_to_use);
      return std::move(node);
    } break;
    case PlanNode::Type::LOCAL_AGGREGATE: {
      auto *agg = static_cast<LocalAgg *>(node.get());

      // LIMIT can be pushed below GROUP BY in some cases
      // For now, apply limit after aggregation
      agg->children[0] = push_limit_recursive(agg->children[0], 0, 0, nullptr);
      if (pending_limit > 0 && pending_limit != HA_POS_ERROR) {
        // Wrap with limit node
        return pending_order
                   ? create_topn_node(std::move(node), pending_limit, pending_offset, pending_order, pending_filesort)
                   : create_limit_node(std::move(node), pending_limit, pending_offset);
      }
      return std::move(node);
    } break;
    case PlanNode::Type::NESTED_LOOP_JOIN:
    case PlanNode::Type::HASH_JOIN: {
      /** some notes on pushing LIMIT/TOP N through JOINs:
       * 1: Repeated Triggers for Inner Tables:
       *
       * Hash Join:
       * The inner table (Build Side) is typically scanned only once to build the hash table. Adding a LIMIT to the
       * inner table may result in an incomplete hash table and produce incorrect query results.
       *
       *  Nested Loop Join (NLJ):
       * The inner table performs a scan for each row of the outer table (probe side). If you add LIMIT 10 to the inner
       * table, it will stop after finding only 10 matching rows for the first row of the outer table. This can cause
       * the join to miss valid matching rows (unless the business logic guarantees that each key in the inner table has
       * at most 10 rows).
       *
       * 2: Order Preservation (Pipeline Property):
       *
       * NLJ preserves order in a streaming manner: If the driving table (left/outer table) is ordered, the output of
       * the NLJ also follows that order. Therefore, pushing a TopN operation down to the left table can directly
       * accelerate the entire join execution path.
       *
       * Hash Join, however, completely disrupts order (it's a blocking or hash-distributing operation). Even if
       * sorting is pushed down to either side, the join output often requires re-sorting afterward. An exception is
       * when the cost model determines that pre-sorting can reduce the size of hash buckets.
       *
       * 3: Prohibited Pushdown for Inner Tables:
       * In NLJ, you must never push pending_limit down to children[1] (the inner table subplan). This would break the
       * join predicate's semantics and produce incorrect query results.
       *
       * Overall Guideline:
       * In TopNPushDown, the handling of NESTED_LOOP_JOIN should be more conservative:
       * Left table (driving table): It is acceptable to proactively push down Limit or qualified TopN operations.
       * Right table (driven table): Pushing down Limit operations is strictly prohibited except for specific semi-join
       * optimizations.
       */
      bool has_order_by = (pending_order != nullptr);
      // Check if we can push limit below join
      if (pending_limit > 0 && pending_limit != HA_POS_ERROR && can_push_below_join(node, has_order_by)) {
        // For joins without ORDER BY, we can sometimes push limit to both sides
        if (!has_order_by) {
          // Push to both children with increased limit (safety margin)
          ha_rows child_limit = pending_limit * 10;  // Safety factor
          if (node->type() == PlanNode::Type::HASH_JOIN) {
            auto *hash_join = static_cast<HashJoin *>(node.get());
            hash_join->children[0] = push_limit_recursive(hash_join->children[0], child_limit, 0, nullptr);
            hash_join->children[1] = push_limit_recursive(hash_join->children[1], child_limit, 0, nullptr);
          } else {
            auto *nest_join = static_cast<NestLoopJoin *>(node.get());
            nest_join->children[0] = push_limit_recursive(nest_join->children[0], child_limit, 0, nullptr);
            // For nested loop, don't push to inner side as it's executed multiple times
            // An internal table (Right Side) cannot have a LIMIT clause because it needs to provide complete matching
            // results for each row of the outer table.
            nest_join->children[1] = push_limit_recursive(nest_join->children[1], 0, 0, nullptr);
          }
          // Wrap join with limit
          return create_limit_node(std::move(node), pending_limit, pending_offset);
        } else {
          // Has ORDER BY - check if we can push to one side
          auto left_tables = get_available_tables(node->children[0]);
          auto right_tables = get_available_tables(node->children[1]);

          bool order_uses_left_only = order_by_uses_only_tables(pending_order, left_tables);
          bool order_uses_right_only = order_by_uses_only_tables(pending_order, right_tables);

          if (order_uses_left_only) {
            // Push TopN to left side
            if (node->type() == PlanNode::Type::HASH_JOIN) {
              auto *hash_join = static_cast<HashJoin *>(node.get());
              hash_join->children[0] =
                  push_limit_recursive(hash_join->children[0], pending_limit, pending_offset, pending_order);
              hash_join->children[1] = push_limit_recursive(hash_join->children[1], 0, 0, nullptr);
            } else {
              auto *nest_join = static_cast<NestLoopJoin *>(node.get());
              nest_join->children[0] =
                  push_limit_recursive(nest_join->children[0], pending_limit, pending_offset, pending_order);
              nest_join->children[1] = push_limit_recursive(nest_join->children[1], 0, 0, nullptr);
            }
            return std::move(node);
          } else if (order_uses_right_only) {
            // Push TopN to right side
            if (node->type() == PlanNode::Type::HASH_JOIN) {
              auto *hash_join = static_cast<HashJoin *>(node.get());
              hash_join->children[0] = push_limit_recursive(hash_join->children[0], 0, 0, nullptr);
              hash_join->children[1] =
                  push_limit_recursive(hash_join->children[1], pending_limit, pending_offset, pending_order);
            } else {
              auto *nest_join = static_cast<NestLoopJoin *>(node.get());
              nest_join->children[0] = push_limit_recursive(nest_join->children[0], 0, 0, nullptr);
              // Don't push to inner of nested loop
            }
            return std::move(node);
          } else {
            // ORDER BY uses columns from both sides - cannot push below join
            // Process children without limit
            if (node->type() == PlanNode::Type::HASH_JOIN) {
              auto *hash_join = static_cast<HashJoin *>(node.get());
              hash_join->children[0] = push_limit_recursive(hash_join->children[0], 0, 0, nullptr);
              hash_join->children[1] = push_limit_recursive(hash_join->children[1], 0, 0, nullptr);
            } else {
              auto *nest_join = static_cast<NestLoopJoin *>(node.get());
              nest_join->children[0] = push_limit_recursive(nest_join->children[0], 0, 0, nullptr);
              nest_join->children[1] = push_limit_recursive(nest_join->children[1], 0, 0, nullptr);
            }
            // Wrap join with TopN
            return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order, pending_filesort);
          }
        }
      } else {
        // Cannot push - process children and wrap
        if (node->type() == PlanNode::Type::HASH_JOIN) {
          auto *hash_join = static_cast<HashJoin *>(node.get());
          hash_join->children[0] = push_limit_recursive(hash_join->children[0], 0, 0, nullptr);
          hash_join->children[1] = push_limit_recursive(hash_join->children[1], 0, 0, nullptr);
        } else {
          auto *nest_join = static_cast<NestLoopJoin *>(node.get());
          nest_join->children[0] = push_limit_recursive(nest_join->children[0], 0, 0, nullptr);
          nest_join->children[1] = push_limit_recursive(nest_join->children[1], 0, 0, nullptr);
        }

        if (pending_limit > 0 && pending_limit != HA_POS_ERROR) {
          return pending_order
                     ? create_topn_node(std::move(node), pending_limit, pending_offset, pending_order, pending_filesort)
                     : create_limit_node(std::move(node), pending_limit, pending_offset);
        }
        return std::move(node);
      }
    } break;
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node.get());
      // Push limit through filter
      filter->children[0] =
          push_limit_recursive(filter->children[0], pending_limit, pending_offset, pending_order, pending_filesort);
      filter->estimated_rows = std::min(filter->estimated_rows, filter->children[0]->estimated_rows);
      return std::move(node);
    } break;
    case PlanNode::Type::SCAN: {
      auto scan = static_cast<ScanTable *>(node.get());
      if (pending_limit > 0 && pending_limit != HA_POS_ERROR) {
        if (pending_order) {
          // Scan can't sort - wrap in TopN
          scan->limit = pending_limit + pending_offset;  // Scan more to account for offset
          return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order, pending_filesort);
        } else {
          // Just limit, no sort
          scan->limit = pending_limit;
          scan->offset = pending_offset;
          scan->estimated_rows = std::min(scan->estimated_rows, pending_limit + pending_offset);
          return std::move(node);
        }
      }
      return std::move(node);
    } break;
    default: {
      // For other node types, process children and apply limit if pending
      for (auto &child : node->children) {
        child = push_limit_recursive(child, 0, 0, nullptr);
      }

      if (pending_limit > 0 && pending_limit != HA_POS_ERROR) {
        return pending_order ? create_topn_node(std::move(node), pending_limit, pending_offset, pending_order)
                             : create_limit_node(std::move(node), pending_limit, pending_offset);
      }
      return std::move(node);
    }
  }
}

/**
 * Check if we can push limit below join
 * @param join Join plan node
 * @param has_order_by Whether there's an ORDER BY clause
 * @return true if safe to push limit below join
 */
bool TopNPushDown::can_push_below_join(const Plan &join, bool has_order_by) const {
  // Never push if there's an ORDER BY that uses columns from both sides
  // For hash join, we can push limit in some cases
  if (join->type() == PlanNode::Type::HASH_JOIN) {
    // Safe to push if no ORDER BY
    if (!has_order_by) return true;
    // With ORDER BY, only safe if ORDER BY uses one side only
    return true;
  }

  // For nested loop join, more conservative
  if (join->type() == PlanNode::Type::NESTED_LOOP_JOIN) {
    // Can push to outer side if no ORDER BY
    if (!has_order_by) return true;
    // With ORDER BY, only if it uses outer side columns only
    return true;
  }
  return false;
}

/**
 * Create a TopN node
 * @param child Child plan
 * @param limit Number of rows to limit
 * @param order ORDER BY structure
 * @return TopN plan node
 */
Plan TopNPushDown::create_topn_node(Plan child, ha_rows limit, ha_rows offset, ORDER *order, Filesort *filesort) {
  auto topn = std::make_unique<TopN>();
  topn->limit = limit;
  topn->order = order;
  if (filesort) {
    topn->filesort = filesort;
  } else if (child) {
    if (child->type() == PlanNode::Type::SORT) {
      auto *sort_child = static_cast<Sort *>(child.get());
      topn->filesort = sort_child->filesort;
    } else if (child->type() == PlanNode::Type::TOP_N) {
      auto *topn_child = static_cast<TopN *>(child.get());
      topn->filesort = topn_child->filesort;
    }
  }

  // Estimate cost
  topn->estimated_rows = std::min(child->estimated_rows, limit);
  topn->cost = child->cost * 1.2;  // TopN has overhead for sorting
  topn->children.push_back(std::move(child));
  return topn;
}

/**
 * Create a Limit node
 * @param child Child plan
 * @param limit Number of rows to limit
 * @param offset Number of rows to skip
 * @return Limit plan node
 */
Plan TopNPushDown::create_limit_node(Plan child, ha_rows limit, ha_rows offset) {
  auto limit_node = std::make_unique<Limit>();
  limit_node->limit = limit;
  limit_node->offset = offset;
  // Estimate cost
  limit_node->estimated_rows = std::min(child->estimated_rows, limit + offset);
  limit_node->cost = child->cost;  // Limit has minimal overhead
  limit_node->children.push_back(std::move(child));
  return limit_node;
}

void TopNPushDown::merge_limits(ha_rows outer_limit, ha_rows outer_offset, ha_rows inner_limit, ha_rows inner_offset,
                                ha_rows &result_limit, ha_rows &result_offset) {
  // Compute effective limits
  // LIMIT outer_limit OFFSET outer_offset
  // applied to
  // LIMIT inner_limit OFFSET inner_offset
  // Total offset is sum of both offsets
  result_offset = outer_offset + inner_offset;

  // Result limit is minimum of:
  // 1. outer_limit
  // 2. inner_limit - outer_offset (if inner has rows to skip)
  if (inner_limit == 0) {
    // Inner has no limit
    result_limit = outer_limit;
  } else if (outer_offset >= inner_limit) {
    // Outer offset exceeds inner limit - no rows
    result_limit = 0;
  } else {
    // Take minimum
    result_limit = std::min(outer_limit, inner_limit - outer_offset);
  }
}

/**
 * Check if ORDER BY clause only references available tables
 * @param order ORDER BY structure
 * @param available_tables Set of available table identifiers
 * @return true if all ORDER BY columns are from available tables
 */
bool TopNPushDown::order_by_uses_only_tables(ORDER *order,
                                             const std::unordered_set<std::string> &available_tables) const {
  if (!order) return true;

  auto order_tables = get_order_by_tables(order);
  // Check if all ORDER BY tables are in available tables
  for (const auto &table : order_tables) {
    if (available_tables.find(table) == available_tables.end()) return false;
  }
  return true;
}

/**
 * Get tables referenced in ORDER BY clause
 * @param order ORDER BY structure
 * @return Set of table identifiers (e.g., "db.table")
 */
std::unordered_set<std::string> TopNPushDown::get_order_by_tables(ORDER *order) const {
  std::unordered_set<std::string> tables;
  for (ORDER *ord = order; ord; ord = ord->next) {
    if (!ord->item || !*ord->item) continue;

    Item *item = *ord->item;
    // Walk item tree to find field references
    std::function<void(Item *)> get_referenced_tables_func = [&](Item *it) {
      if (!it) return;

      if (it->type() == Item::FIELD_ITEM) {
        auto *field_item = static_cast<Item_field *>(it);
        if (field_item->field && field_item->field->table) {
          TABLE *table = field_item->field->table;
          std::string key = std::string(table->s->db.str).append(".").append(table->s->table_name.str);
          tables.insert(key);
        }
      } else if (it->type() == Item::FUNC_ITEM) {
        auto *func = static_cast<Item_func *>(it);
        for (uint i = 0; i < func->argument_count(); i++) {
          get_referenced_tables_func(func->arguments()[i]);
        }
      } else if (it->type() == Item::REF_ITEM) {
        auto *ref = static_cast<Item_ref *>(it);
        if (ref->ref_item()) {
          get_referenced_tables_func(ref->ref_item());
        }
      }
    };

    get_referenced_tables_func(item);
  }
  return tables;
}

/**
 * Get available tables in a plan subtree
 * Used to determine if predicates/order by can be pushed down
 * @param node Plan node
 * @return Set of table identifiers (e.g., "db.table")
 */
std::unordered_set<std::string> TopNPushDown::get_available_tables(const Plan &node) const {
  std::unordered_set<std::string> tables;
  if (!node) return tables;

  if (node->type() == PlanNode::Type::SCAN) {
    auto *scan = static_cast<ScanTable *>(node.get());
    if (scan->source_table) {
      std::string key =
          std::string(scan->source_table->s->db.str).append(".").append(scan->source_table->s->table_name.str);
      tables.insert(key);
    }
  } else {
    // Recursively collect from children
    for (const auto &child : node->children) {
      auto child_tables = get_available_tables(child);
      tables.insert(child_tables.begin(), child_tables.end());
    }
  }
  return tables;
}
}  // namespace Optimizer
}  // namespace ShannonBase