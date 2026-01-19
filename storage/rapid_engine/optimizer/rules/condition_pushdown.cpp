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
#include "storage/rapid_engine/optimizer/rules/condition_pushdown.h"

#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"

#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
void PredicatePushDown::apply(Plan &root) {
  if (!root) return;

  // Start with empty pending filters
  std::vector<Item *> pending_filters;

  // Recursively push down predicates
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

      // Add to pending filters
      pending_filters.insert(pending_filters.end(), new_filters.begin(), new_filters.end());

      // Remove the filter node and continue with its child
      Plan child = std::move(filter->children[0]);
      return push_down_recursive(child, pending_filters);
    }

    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      return push_below_join(node, pending_filters);
    }

    case PlanNode::Type::SCAN: {
      return push_into_scan(node, pending_filters);
    }

    case PlanNode::Type::LOCAL_AGGREGATE: {
      auto *agg = static_cast<LocalAgg *>(node.get());

      // Recursively process child
      std::vector<Item *> child_filters;
      agg->children[0] = push_down_recursive(agg->children[0], child_filters);

      // Filters can be pushed below aggregation if they don't reference aggregate functions
      std::vector<Item *> pushable_filters;
      std::vector<Item *> non_pushable_filters;

      for (auto *filter : pending_filters) {
        // Check if filter references any aggregate function
        bool references_agg = false;
        // Simple check: if it's an aggregate function itself
        if (filter->type() == Item::SUM_FUNC_ITEM) {
          references_agg = true;
        }
        // TODO: More thorough check for nested aggregate references

        if (references_agg) {
          non_pushable_filters.push_back(filter);
        } else {
          pushable_filters.push_back(filter);
        }
      }

      // Push down the pushable filters
      if (!pushable_filters.empty()) {
        agg->children[0] = push_down_recursive(agg->children[0], pushable_filters);
      }

      // Keep non-pushable filters pending
      pending_filters = non_pushable_filters;

      return std::move(node);
    }

    case PlanNode::Type::LIMIT: {
      auto *limit = static_cast<Limit *>(node.get());

      // Push filters below LIMIT
      limit->children[0] = push_down_recursive(limit->children[0], pending_filters);

      return std::move(node);
    }

    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());

      // Push filters below TOP_N
      topn->children[0] = push_down_recursive(topn->children[0], pending_filters);

      return std::move(node);
    }

    default:
      // For other node types, don't push down
      return std::move(node);
  }
}

Plan PredicatePushDown::push_below_join(Plan &join, std::vector<Item *> &pending_filters) {
  bool is_hash_join = (join->type() == PlanNode::Type::HASH_JOIN);

  // Get references to children (don't move them yet)
  Plan *left_child_ptr = &join->children[0];
  Plan *right_child_ptr = &join->children[1];

  // Get available tables in each subtree
  auto left_tables = get_available_tables(*left_child_ptr);
  auto right_tables = get_available_tables(*right_child_ptr);
  auto all_tables = left_tables;
  all_tables.insert(right_tables.begin(), right_tables.end());

  // Partition filters into three categories
  std::vector<Item *> left_filters;       // Only references left tables
  std::vector<Item *> right_filters;      // Only references right tables
  std::vector<Item *> join_filters;       // References both sides
  std::vector<Item *> remaining_filters;  // Can't be pushed (e.g., subqueries)

  for (auto *filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);

    // Check if all referenced tables are in left subtree
    bool all_in_left = true;
    bool all_in_right = true;
    bool has_external = false;

    for (const auto &table : referenced) {
      if (left_tables.find(table) == left_tables.end()) {
        all_in_left = false;
      }
      if (right_tables.find(table) == right_tables.end()) {
        all_in_right = false;
      }
      if (all_tables.find(table) == all_tables.end()) {
        has_external = true;
      }
    }

    if (has_external) {
      // References tables outside this join - can't push
      remaining_filters.push_back(filter);
    } else if (all_in_left && is_simple_predicate(filter)) {
      // Can push to left side
      left_filters.push_back(filter);
    } else if (all_in_right && is_simple_predicate(filter)) {
      // Can push to right side
      right_filters.push_back(filter);
    } else if (!referenced.empty()) {
      // References both sides or complex predicate - becomes join condition
      join_filters.push_back(filter);
    } else {
      // No table references (constant expression) - push to both sides
      left_filters.push_back(filter);
    }
  }

  // Recursively push filters to children
  *left_child_ptr = push_down_recursive(*left_child_ptr, left_filters);
  *right_child_ptr = push_down_recursive(*right_child_ptr, right_filters);

  // Add join filters to the join condition
  if (!join_filters.empty()) {
    if (is_hash_join) {
      auto *hash_join = static_cast<HashJoin *>(join.get());
      hash_join->join_conditions.insert(hash_join->join_conditions.end(), join_filters.begin(), join_filters.end());
    } else {
      auto *nest_join = static_cast<NestLoopJoin *>(join.get());
      nest_join->join_conditions.insert(nest_join->join_conditions.end(), join_filters.begin(), join_filters.end());
    }
  }

  // Update pending filters with remaining ones
  pending_filters = remaining_filters;

  // Update cost estimate based on pushed filters
  if (!left_filters.empty() || !right_filters.empty()) {
    double selectivity = 1.0;
    for (auto *f : left_filters) {
      selectivity *= estimate_selectivity(f);
    }
    for (auto *f : right_filters) {
      selectivity *= estimate_selectivity(f);
    }

    // Reduce join cost due to smaller input
    join->cost *= selectivity;
  }

  return std::move(join);
}

Plan PredicatePushDown::push_into_scan(Plan &scan, std::vector<Item *> &pending_filters) {
  auto *scan_node = static_cast<ScanTable *>(scan.get());

  if (pending_filters.empty()) {
    return std::move(scan);
  }

  // Get this table's identifier
  std::string table_key;
  if (scan_node->source_table) {
    table_key =
        std::string(scan_node->source_table->s->db.str) + "." + std::string(scan_node->source_table->s->table_name.str);
  }

  // Partition filters: those that apply to this table vs. others
  std::vector<Item *> applicable_filters;
  std::vector<Item *> other_filters;

  for (auto *filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);

    // Check if this filter only references this table
    bool applies_here = false;
    if (referenced.empty()) {
      // Constant expression - applies everywhere
      applies_here = true;
    } else if (referenced.size() == 1 && referenced.find(table_key) != referenced.end()) {
      applies_here = true;
    }

    if (applies_here) {
      applicable_filters.push_back(filter);
    } else {
      other_filters.push_back(filter);
    }
  }

  // Update pending filters
  pending_filters = other_filters;

  if (applicable_filters.empty()) {
    return std::move(scan);
  }

  // Create a Filter node above the scan
  Item *combined_condition = combine_with_and(applicable_filters);

  auto filter = std::make_unique<Filter>();
  filter->condition = combined_condition;
  filter->children.push_back(std::move(scan));

  // Update cost estimate
  double selectivity = 1.0;
  for (auto *f : applicable_filters) {
    selectivity *= estimate_selectivity(f);
  }

  filter->estimated_rows = static_cast<ha_rows>(filter->children[0]->estimated_rows * selectivity);
  filter->cost = filter->children[0]->cost * 1.1;  // Small overhead for filtering

  return filter;
}

void PredicatePushDown::split_conjunctions(Item *condition, std::vector<Item *> &predicates) {
  if (!condition) return;

  // Check if this is an AND condition
  if (condition->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(condition);

    if (cond->functype() == Item_func::COND_AND_FUNC) {
      // Recursively split each argument
      List_iterator<Item> li(*cond->argument_list());
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

  // Special cases
  if (used == 0) {
    // Constant expression
    return tables;
  }

  // Walk the item tree to find table references
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
    } else if (it->type() == Item::COND_ITEM) {
      auto *cond = static_cast<Item_cond *>(it);
      List_iterator<Item> li(*cond->argument_list());
      Item *arg;
      while ((arg = li++)) {
        visit(arg);
      }
    } else if (it->type() == Item::REF_ITEM) {
      auto *ref = static_cast<Item_ref *>(it);
      if (ref->ref_item()) {
        visit(ref->ref_item());
      }
    }
  };

  visit(item);
  return tables;
}

bool PredicatePushDown::can_push_to_subtree(Item *predicate, const std::unordered_set<std::string> &available_tables) {
  auto referenced = get_referenced_tables(predicate);

  // Check if all referenced tables are available
  for (const auto &table : referenced) {
    if (available_tables.find(table) == available_tables.end()) {
      return false;
    }
  }

  return true;
}

std::unordered_set<std::string> PredicatePushDown::get_available_tables(const Plan &node) {
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
  Item_cond_and *and_cond = new Item_cond_and();
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

double PredicatePushDown::estimate_selectivity(Item *predicate) {
  if (!predicate) return 1.0;

  // Simplified selectivity estimation
  if (predicate->type() == Item::FUNC_ITEM) {
    auto *func = static_cast<Item_func *>(predicate);

    switch (func->functype()) {
      case Item_func::EQ_FUNC:
        return 0.1;  // Equality is very selective
      case Item_func::NE_FUNC:
        return 0.9;
      case Item_func::LT_FUNC:
      case Item_func::LE_FUNC:
      case Item_func::GT_FUNC:
      case Item_func::GE_FUNC:
        return 0.33;  // Range predicates
      case Item_func::BETWEEN:
        return 0.25;
      default:
        return 0.5;
    }
  }

  return 0.5;  // Default: 50% selectivity
}

void AggregationPushDown::apply(Plan &root) {}

void TopNPushDown::apply(Plan &root) {}
}  // namespace Optimizer
}  // namespace ShannonBase