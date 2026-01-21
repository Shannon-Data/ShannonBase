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
#include "sql/sql_class.h"

#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table.h"
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
    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());
      // Push filters below TOP_N
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
  if (!left_filters.empty())
    join->children[0] = create_filter_node(std::move(join->children[0]), combine_with_and(left_filters));

  join->children[1] = push_down_recursive(join->children[1], right_filters);
  if (!right_filters.empty()) {
    join->children[1] = create_filter_node(std::move(join->children[1]), combine_with_and(right_filters));
  }

  if (!join_filters.empty()) {
    // only simple cond can be pushed into join conditions (for hash join build)
    // the other complex predicates are kept as remaining filters to be handled
    // at upper apply level or wrapped above the Join
    std::vector<Item *> pushable_to_join;
    for (auto *jf : join_filters) {
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
  if (scan_node->source_table) {
    table_key =
        std::string(scan_node->source_table->s->db.str) + "." + std::string(scan_node->source_table->s->table_name.str);
  }

  std::vector<Item *> to_storage_engine;  // simple predicate：push down to (Storage Index)
  std::vector<Item *> to_filter_node;     // complex predicate：to be Filter operator
  std::vector<Item *> remaining_others;   // not belonging to this table

  for (auto *filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);
    bool belongs_here =
        referenced.empty() || (referenced.size() == 1 && referenced.find(table_key) != referenced.end());
    if (belongs_here) {
      if (is_simple_predicate(filter)) {
        // simnple predicate,（如 a=1, b>10）：push to storage engine
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
    for (auto &f : to_storage_engine) {
      auto child_pre = Optimizer::convert_item_to_predicate(current_thd, f);
      if (!child_pre) continue;
      predicates->add_child(std::move(child_pre));
    }
    scan_node->prune_predicate = std::move(predicates);
    scan_node->use_storage_index = true;

    // update Cost：due to pushdown.
    double total_selectivity = 1.0;
    for (auto &f : to_storage_engine) {
      total_selectivity *= estimate_selectivity(f);
    }
    scan_node->estimated_rows = static_cast<ha_rows>(scan_node->estimated_rows * total_selectivity);
    scan_node->cost *= (0.1 + 0.9 * total_selectivity);
  }

  if (!to_filter_node.empty()) {
    Item *combined_cond = combine_with_and(to_filter_node);
    double filter_selectivity = 1.0;
    for (auto *f : to_filter_node) {
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
  if (used == 0) return tables;  // Constant expression

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
      if (ref->ref_item()) visit(ref->ref_item());
    }
  };

  visit(item);
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

  // for `Item_field` or `Item_cache` etc.，does not contain aggregate reference.
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
Plan TopNPushDown::push_limit_recursive(Plan &node, ha_rows pending_limit, ha_rows pending_offset,
                                        ORDER *pending_order) {
  if (!node) return nullptr;
  switch (node->type()) {
    case PlanNode::Type::LIMIT: {
      auto *limit_node = static_cast<Limit *>(node.get());

      // Merge with pending limit if exists
      ha_rows new_limit = limit_node->limit;
      ha_rows new_offset = limit_node->offset;

      if (pending_limit > 0)
        merge_limits(pending_limit, pending_offset, limit_node->limit, limit_node->offset, new_limit, new_offset);

      // Remove current limit node and continue with child
      Plan child = std::move(limit_node->children[0]);
      return push_limit_recursive(child, new_limit, new_offset, pending_order);
    } break;
    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());

      // If we have a pending limit, merge it
      if (pending_limit > 0) {
        ha_rows new_limit, new_offset;
        merge_limits(pending_limit, pending_offset, topn->limit, 0,  // TopN doesn't have offset
                     new_limit, new_offset);
        topn->limit = new_limit;

        // Update cost estimate
        topn->estimated_rows = std::min(topn->estimated_rows, new_limit);
        topn->cost = topn->children[0]->cost * 1.2;  // Small overhead for TopN
      }

      // Continue pushing down to child
      topn->children[0] = push_limit_recursive(topn->children[0], 0, 0, nullptr);
      return std::move(node);
    } break;
    case PlanNode::Type::LOCAL_AGGREGATE: {
      auto *agg = static_cast<LocalAgg *>(node.get());

      // LIMIT can be pushed below GROUP BY in some cases
      // For now, apply limit after aggregation
      agg->children[0] = push_limit_recursive(agg->children[0], 0, 0, nullptr);
      if (pending_limit > 0) {
        // Wrap with limit node
        if (pending_order) {
          return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order);
        } else {
          return create_limit_node(std::move(node), pending_limit, pending_offset);
        }
      }
      return std::move(node);
    } break;
    case PlanNode::Type::HASH_JOIN:
    case PlanNode::Type::NESTED_LOOP_JOIN: {
      bool has_order_by = (pending_order != nullptr);

      // Check if we can push limit below join
      if (pending_limit > 0 && can_push_below_join(node, has_order_by)) {
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
            return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order);
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

        if (pending_limit > 0) {
          if (pending_order) {
            return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order);
          } else {
            return create_limit_node(std::move(node), pending_limit, pending_offset);
          }
        }
        return std::move(node);
      }
    } break;
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node.get());
      // Push limit through filter
      filter->children[0] = push_limit_recursive(filter->children[0], pending_limit, pending_offset, pending_order);
      return std::move(node);
    } break;
    case PlanNode::Type::SCAN: {
      // Reached leaf node - apply limit here if pending
      if (pending_limit > 0) {
        if (pending_order) {
          return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order);
        } else {
          return create_limit_node(std::move(node), pending_limit, pending_offset);
        }
      }
      return std::move(node);
    } break;
    default:
      // For other node types, process children and apply limit if pending
      for (auto &child : node->children) {
        child = push_limit_recursive(child, 0, 0, nullptr);
      }

      if (pending_limit > 0) {
        if (pending_order) {
          return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order);
        } else {
          return create_limit_node(std::move(node), pending_limit, pending_offset);
        }
      }
      return std::move(node);
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
Plan TopNPushDown::create_topn_node(Plan child, ha_rows limit, ha_rows offset, ORDER *order) {
  auto topn = std::make_unique<TopN>();
  topn->limit = limit;
  topn->order = order;
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
          std::string key = std::string(table->s->db.str) + "." + std::string(table->s->table_name.str);
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
}  // namespace Optimizer
}  // namespace ShannonBase