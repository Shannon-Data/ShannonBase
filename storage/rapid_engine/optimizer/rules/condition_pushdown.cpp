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

#include <algorithm>
#include <utility>

#include "sql/field.h"
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

namespace {
const Imcs::Simple_Predicate *as_simple(const Imcs::Predicate *pred) {
  return (pred && !pred->is_compound()) ? static_cast<const Imcs::Simple_Predicate *>(pred) : nullptr;
}

/**
 * `col BETWEEN lo AND hi` and the `col >= lo AND col <= hi` pair that an
 * INDEX_RANGE_SCAN translation leaves in prune_predicate select the same rows,
 * but they are not structurally identical, so Predicate::equals cannot see it.
 */
bool is_between_range_pair(const Imcs::Predicate &between_pred, const Imcs::Predicate &range_pred) {
  const auto *between = as_simple(&between_pred);
  if (!between || between->op != Imcs::PredicateOperator::BETWEEN) return false;
  if (!range_pred.is_compound() || range_pred.op != Imcs::PredicateOperator::AND) return false;

  const auto &bounds = static_cast<const Imcs::Compound_Predicate &>(range_pred).children;
  if (bounds.size() != 2) return false;

  const auto *lower = as_simple(bounds[0].get());
  const auto *upper = as_simple(bounds[1].get());
  if (!lower || !upper) return false;
  if (lower->op == Imcs::PredicateOperator::LESS_EQUAL) std::swap(lower, upper);

  return lower->op == Imcs::PredicateOperator::GREATER_EQUAL && upper->op == Imcs::PredicateOperator::LESS_EQUAL &&
         lower->column_id == between->column_id && upper->column_id == between->column_id &&
         lower->value == between->value && upper->value == between->value2;
}

/** Two predicates that qualify the same rows, however they were spelled. */
bool same_condition(const Imcs::Predicate &lhs, const Imcs::Predicate &rhs) {
  return lhs.equals(rhs) || is_between_range_pair(lhs, rhs) || is_between_range_pair(rhs, lhs);
}

/**
 * Narrow a row estimate by a selectivity, keeping a non-empty input non-empty.
 * A selective predicate can round the product below one, and
 * PropagateCostAndRows() reads a zero estimate as "no estimate at all" and
 * publishes kUnknownRowCount -- strictly less information than "one row".
 */
ha_rows apply_selectivity(ha_rows rows, double selectivity) {
  const auto scaled = static_cast<ha_rows>(rows * selectivity);
  return (rows > 0) ? std::max<ha_rows>(1, scaled) : scaled;
}
}  // namespace

void PredicatePushDown::apply(Plan &root) {
  if (!root) return;

  std::vector<Item *> pending_filters;
  root = push_down_recursive(root, pending_filters);

  // If there are still pending filters at the top, wrap in a Filter node
  if (root && !pending_filters.empty()) {
    Item *combined = ShannonBase::Optimizer::Utils::combine_with_and(pending_filters);
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
      // Aggregation is a semantic barrier. A predicate can only cross it after
      // proving it references group keys exclusively; Item shape alone is not
      // sufficient (HAVING may reference a non-group representative value).
      std::vector<Item *> child_filters;
      agg->children[0] = push_down_recursive(agg->children[0], child_filters);
      agg->children[0] = wrap_if_pending(std::move(agg->children[0]), child_filters);
      return wrap_if_pending(std::move(node), pending_filters);
    } break;
    case PlanNode::Type::LIMIT: {
      auto *limit = static_cast<Limit *>(node.get());
      std::vector<Item *> child_filters;
      limit->children[0] = push_down_recursive(limit->children[0], child_filters);
      limit->children[0] = wrap_if_pending(std::move(limit->children[0]), child_filters);
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
      std::vector<Item *> child_filters;
      topn->children[0] = push_down_recursive(topn->children[0], child_filters);
      topn->children[0] = wrap_if_pending(std::move(topn->children[0]), child_filters);
      return wrap_if_pending(std::move(node), pending_filters);
    } break;
    default:  // For other node types, don't push down
      return std::move(node);
  }
}

Plan PredicatePushDown::push_below_join(Plan &join, std::vector<Item *> &pending_filters) {
  // Pushing a WHERE predicate below OUTER/SEMI/ANTI would change NULL-complementation or match
  // semantics; only a provably INNER join is safe to push through.
  const bool can_push_through_join = Utils::is_provably_inner_join(join.get());

  if (!can_push_through_join) {
    // Filters above outer/semi/anti joins cannot in general be pushed into
    // either input without changing NULL-complementation or match semantics.
    std::vector<Item *> left_pending;
    std::vector<Item *> right_pending;
    join->children[0] = push_down_recursive(join->children[0], left_pending);
    join->children[0] = wrap_if_pending(std::move(join->children[0]), left_pending);
    join->children[1] = push_down_recursive(join->children[1], right_pending);
    join->children[1] = wrap_if_pending(std::move(join->children[1]), right_pending);
    return wrap_if_pending(std::move(join), pending_filters);
  }

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

    for (TABLE *table : referenced) {
      if (left_tables.count(table) == 0) all_in_left = false;
      if (right_tables.count(table) == 0) all_in_right = false;
      if (all_tables.count(table) == 0) has_external = true;
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
    // Do not transfer a WHERE residual into PlanNode::join_conditions unless the
    // AccessPath JoinPredicate is rebuilt from that vector as part of the same
    // transformation.  Today ToAccessPath() preserves the original
    // JoinPredicate, so moving the predicate here would make it disappear from
    // execution. Keep cross-input residuals above the join instead.
    remaining_filters.insert(remaining_filters.end(), join_filters.begin(), join_filters.end());
  }
  pending_filters = remaining_filters;
  return std::move(join);
}

Plan PredicatePushDown::push_into_scan(Plan &scan, std::vector<Item *> &pending_filters) {
  auto *scan_node = static_cast<ScanTable *>(scan.get());
  if (pending_filters.empty()) return std::move(scan);
  assert(scan_node->source_table);
  TABLE *scan_table = scan_node->source_table;

  std::vector<Item *> to_storage_engine;  // simple predicate：push down to (Storage Index)
  std::vector<Item *> to_filter_node;     // complex predicate：to be Filter operator
  std::vector<Item *> remaining_others;   // not belonging to this table

  for (const auto &filter : pending_filters) {
    auto referenced = get_referenced_tables(filter);
    bool belongs_here = referenced.empty() || (referenced.size() == 1 && referenced.count(scan_table) > 0);
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
    std::vector<Item *> pushed_storage_items;
    // Seed with what the scan already carries -- typically a range derived from
    // one of these very WHERE items (INDEX_RANGE_SCAN) -- so that an item
    // restating it is recognised below as a duplicate. ANDing it on top instead
    // would print the condition twice in EXPLAIN and square its selectivity.
    if (scan_node->prune_predicate) predicates->add_child(std::move(scan_node->prune_predicate));

    for (const auto &f : to_storage_engine) {
      auto child_pre = Optimizer::convert_item_to_predicate(current_thd, f);
      if (!child_pre || !Utils::is_storage_index_predicate_safe(child_pre.get())) {
        // Conversion success is not enough: pushed predicates participate in
        // final row qualification. Preserve any unproven semantics as a MySQL
        // Filter node instead of risking a wrong result.
        to_filter_node.push_back(f);
        continue;
      }
      const bool already_pushed = std::any_of(
          predicates->children.begin(), predicates->children.end(),
          [&](const std::unique_ptr<Imcs::Predicate> &pushed) { return same_condition(*pushed, *child_pre); });
      // The condition is already enforced by a pushed predicate; dropping this
      // copy keeps both EXPLAIN and the row estimate honest.
      if (already_pushed) continue;

      pushed_storage_items.push_back(f);
      predicates->add_child(std::move(child_pre));
    }

    if (predicates->children.empty()) {
      // Every original Item that was not pushed was already preserved in
      // to_filter_node above.
    } else {
      // A lone conjunct is the predicate itself: no AND wrapper to evaluate, and
      // no parentheses around a single condition in EXPLAIN.
      scan_node->prune_predicate =
          (predicates->children.size() == 1) ? std::move(predicates->children[0]) : std::move(predicates);
      scan_node->use_storage_index = true;

      // update Cost：due to pushdown.
      double total_selectivity = 1.0;
      for (const auto &f : pushed_storage_items) {
        total_selectivity *= estimate_selectivity(f, scan_table);
      }
      scan_node->estimated_rows = apply_selectivity(scan_node->estimated_rows, total_selectivity);
      scan_node->cost *= (0.1 + 0.9 * total_selectivity);
    }
  }

  if (!to_filter_node.empty()) {
    Item *combined_cond = ShannonBase::Optimizer::Utils::combine_with_and(to_filter_node);
    double filter_selectivity = 1.0;
    for (const auto &f : to_filter_node) {
      filter_selectivity *= estimate_selectivity(f, scan_table);
    }
    auto filter_node = create_filter_node(std::move(scan), combined_cond);
    filter_node->estimated_rows = apply_selectivity(filter_node->estimated_rows, filter_selectivity);
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

std::unordered_set<TABLE *> PredicatePushDown::get_referenced_tables(Item *item) {
  std::unordered_set<TABLE *> tables;
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
        tables.insert(field_item->field->table);
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

bool PredicatePushDown::can_push_to_subtree(Item *predicate, const std::unordered_set<TABLE *> &available_tables) {
  auto referenced = get_referenced_tables(predicate);

  // Check if all referenced tables are available
  for (TABLE *table : referenced) {
    if (available_tables.count(table) == 0) return false;
  }
  return true;
}

std::unordered_set<TABLE *> PredicatePushDown::get_available_tables(const Plan &node) {
  std::unordered_set<TABLE *> tables;
  if (!node) return tables;

  if (node->type() == PlanNode::Type::SCAN) {
    auto *scan = static_cast<ScanTable *>(node.get());
    assert(scan->source_table);
    tables.insert(scan->source_table);
  } else {
    // Recursively collect from children
    for (const auto &child : node->children) {
      auto child_tables = get_available_tables(child);
      tables.insert(child_tables.begin(), child_tables.end());
    }
  }
  return tables;
}

Plan PredicatePushDown::create_filter_node(Plan child, Item *condition) {
  auto filter = std::make_unique<Filter>();
  filter->condition = condition;
  filter->estimated_rows = child->estimated_rows;
  filter->cost = child->cost;
  filter->children.push_back(std::move(child));
  // defensive: ensure compound conditions (Item_cond_and / Item_cond_or)
  // are fixed before they reach FilterIterator.  The primary fix is in
  // Utils::combine_with_and(), but this guards any other code path that
  // creates COND_ITEM nodes outside that utility.
  if (condition && (condition->type() == Item::COND_ITEM)) {
    condition->quick_fix_field();
    condition->update_used_tables();
    condition->apply_is_true();
  }
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

  // Item_cond keeps its operands in its own List<Item>, not in Item_func::args.
  if (item->type() == Item::COND_ITEM) {
    auto *cond = down_cast<Item_cond *>(item);
    List_iterator<Item> li(*cond->argument_list());
    Item *child;
    while ((child = li++)) {
      if (contains_aggregate_reference(child)) return true;
    }
    return false;
  }

  if (item->type() == Item::FUNC_ITEM) {
    auto *func = down_cast<Item_func *>(item);
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

double PredicatePushDown::estimate_selectivity(Item *predicate, const TABLE *table) {
  if (!predicate) return 1.0;

  if (table != nullptr) return ShannonBase::Optimizer::SelectivityEstimator::estimate_selectivity(table, predicate);

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
      // Same estimate the hypergraph-side analyzer uses, so the two do not
      // disagree about the same predicate.
      return ShannonBase::Optimizer::Utils::like_pattern_selectivity(func);
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
    auto rpd_table = share ? Imcs::Imcs::instance()->get_rpd_table(share->m_tableid) : nullptr;
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
void AggregationPushDown::apply(Plan &root) { root = push_aggregation_recursive(root); }

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
      // Recursively process child first.
      if (!node->children.empty()) node->children[0] = push_aggregation_recursive(node->children[0]);
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

  // Check if we can push partial aggregation below join. Only HashJoin carries the join-type/
  // child-multiplicity proof (HashJoin::join_type, HashJoin::child_multiplicity) this rewrite
  // requires; NestLoopJoin has no such metadata, so it stays an opaque boundary.
  if (agg->children[0]->type() == PlanNode::Type::HASH_JOIN) {
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
bool AggregationPushDown::can_apply_two_phase_aggregation(const LocalAgg * /*agg*/) {
  // this is used for MPP mode, the storage engine can do partial aggregation, so we can always apply two-phase
  // aggregation In a more advanced implementation, we would check the aggregate functions and their decomposability
  // here.
  return false;
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

  ut_a(!global_agg->children.empty());
  ut_a(global_agg->children[0]->type() == PlanNode::Type::HASH_JOIN ||
       global_agg->children[0]->type() == PlanNode::Type::NESTED_LOOP_JOIN);

  auto local_agg = std::make_unique<LocalAgg>();
  local_agg->original_path = global_agg->original_path;
  local_agg->join = global_agg->join;
  local_agg->group_by = global_agg->group_by;
  local_agg->olap = global_agg->olap;
  local_agg->is_global = false;  // local / partial phase
  local_agg->strategy = global_agg->strategy;

  // Build the local-phase aggregate list with cloned Item_sum objects so that the two nodes are fully independent.
  for (auto *global_func : global_agg->aggregates) {
    auto *sum_func = static_cast<Item_sum *>(global_func);
    switch (sum_func->sum_func()) {
      case Item_sum::SUM_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
      case Item_sum::COUNT_FUNC:
      case Item_sum::AVG_FUNC: {
        Item *copied = sum_func->copy_or_same(current_thd);
        if (copied && copied != static_cast<Item *>(sum_func) && copied->type() == Item::SUM_FUNC_ITEM) {
          local_agg->aggregates.push_back(static_cast<Item_func *>(copied));
        } else {
          // copy_or_same() returned `this` (window func path) or failed;
          // skip local-phase entry for this function — correctness is
          // preserved, just no partial aggregation benefit for it.
        }
        break;
      }
      default:
        // Non-decomposable (STDDEV, VARIANCE, GROUP_CONCAT …):
        // stays in global_agg only, no local counterpart.
        break;
    }
  }

  // Wire: local_agg → original join child
  local_agg->estimated_rows = global_agg->estimated_rows;
  local_agg->cost = global_agg->children[0]->cost * 1.5;
  local_agg->children.push_back(std::move(global_agg->children[0]));

  // Wire: global_agg → local_agg
  global_agg->children[0] = std::move(local_agg);
  global_agg->cost = global_agg->children[0]->cost * 1.2;
  global_agg->is_global = true;

  return global_agg_node;
}

/**
 * Try to push a LocalAgg below its HashJoin child, by relocating the same node (not cloning it)
 * to wrap just one side. See the declaration in condition_pushdown.h for the full list of
 * correctness preconditions this checks -- in short: HashJoin must be provably INNER, GROUP BY
 * and every aggregate argument must resolve to one exclusive side, the other side must be proven
 * to contribute at most one row per join key (HashJoin::child_multiplicity), and every join-key
 * column read from the pushed side must already be a GROUP BY column.
 *
 * Example:
 * BEFORE:
 *   Agg(SUM(orders.amount) GROUP BY orders.customer_id)
 *     └─ HashJoin[INNER] (orders.customer_id = customers.id)
 *         ├─ Scan(orders)
 *         └─ Scan(customers)              -- customers.id is UNIQUE NOT NULL
 *
 * AFTER:
 *   HashJoin[INNER] (customer_id = customers.id)
 *     ├─ Agg(SUM(amount) GROUP BY customer_id)
 *     │   └─ Scan(orders)
 *     └─ Scan(customers)
 */
Plan AggregationPushDown::try_push_below_join(Plan agg_node) {
  auto *agg = static_cast<LocalAgg *>(agg_node.get());

  // No GROUP BY (implicit single-group aggregate), ROLLUP super-aggregate rows, or a hash output
  // order a parent depends on: none of these are provably safe to relocate below a join with the
  // single-phase execution model Rapid's aggregate iterator has today.
  if (agg->group_by.empty() || agg->olap != olap_type::UNSPECIFIED_OLAP_TYPE || agg->hash_output_order != nullptr) {
    return agg_node;
  }

  Plan join = std::move(agg->children[0]);
  auto *hj = static_cast<HashJoin *>(join.get());

  // Only a provably INNER hash join is safe: dropping an unmatched push-side group is identical
  // whether the join runs before or after grouping, which is not true in general for OUTER/SEMI/
  // ANTI/FULL_OUTER joins.
  if (hj->join_type != JoinType::INNER) {
    agg->children[0] = std::move(join);
    return agg_node;
  }

  // Get tables from left and right sides
  auto left_tables = get_available_tables(join->children[0]);
  auto right_tables = get_available_tables(join->children[1]);

  bool all_from_left = false;
  bool all_from_right = false;
  bool any_group = false, any_agg = false;

  // Check GROUP BY columns
  for (auto *group_item : agg->group_by) {
    auto tables = get_item_tables(group_item);
    if (tables.empty()) {
      agg->children[0] = std::move(join);
      return agg_node;  // unresolvable → cannot push
    }
    any_group = true;
    for (const auto &table : tables) {
      if (left_tables.find(table) != left_tables.end()) all_from_left = true;
      if (right_tables.find(table) != right_tables.end()) all_from_right = true;
      if (left_tables.find(table) == left_tables.end() && right_tables.find(table) == right_tables.end()) {
        all_from_left = all_from_right = false;  // unresolvable → cannot push
        break;
      }
    }
    if (!all_from_left && !all_from_right) {
      agg->children[0] = std::move(join);
      return agg_node;
    }
  }

  // Check aggregate columns
  for (auto *agg_func : agg->aggregates) {
    auto tables = get_item_tables(agg_func);
    if (tables.empty()) {
      agg->children[0] = std::move(join);
      return agg_node;  // unresolvable → cannot push
    }
    any_agg = true;
    for (const auto &table : tables) {
      if (left_tables.find(table) != left_tables.end()) all_from_left = true;
      if (right_tables.find(table) != right_tables.end()) all_from_right = true;
      if (left_tables.find(table) == left_tables.end() && right_tables.find(table) == right_tables.end()) {
        all_from_left = all_from_right = false;  // unresolvable → cannot push
        break;
      }
    }
    if (!all_from_left && !all_from_right) {
      agg->children[0] = std::move(join);
      return agg_node;
    }
  }

  // Requires at least one resolved item on each axis and exclusive side.
  if (!any_group || !any_agg || all_from_left == all_from_right) {
    agg->children[0] = std::move(join);
    return agg_node;
  }

  const int push_side = all_from_left ? 0 : 1;
  const int keep_side = 1 - push_side;

  // keep_side must be proven to contribute at most one row per join-key value, or relocating
  // push_side's already-grouped rows below the join could re-duplicate them -- and Rapid's
  // aggregate iterator has no second (global) phase to collapse that back down (LocalAgg::join
  // is the query's one shared JOIN; see HashJoin::child_multiplicity's doc comment).
  if (hj->child_multiplicity[keep_side] != JoinMultiplicity::kAtMostOne) {
    agg->children[0] = std::move(join);
    return agg_node;
  }

  TABLE *push_table = get_pushable_scan_table(join->children[push_side].get());
  if (push_table == nullptr || !group_by_covers_join_keys(agg, hj->join_conditions, push_table)) {
    agg->children[0] = std::move(join);
    return agg_node;
  }

  return push_aggregation_to_join_side(std::move(agg_node), std::move(join), push_side);
}

/**
 * Relocate agg_node (unchanged: same JOIN*, same group_by/aggregates Items) to wrap
 * join->children[push_side], and return join as the new subtree root.
 */
Plan AggregationPushDown::push_aggregation_to_join_side(Plan agg_node, Plan join, int push_side) {
  auto *agg = static_cast<LocalAgg *>(agg_node.get());

  Plan pushed_child = std::move(join->children[push_side]);
  agg->cost = pushed_child->cost * 1.5;
  agg->children[0] = std::move(pushed_child);

  join->children[push_side] = std::move(agg_node);
  join->cost = join->children[0]->cost + join->children[1]->cost;
  // The join can no longer output more rows than the (now pre-grouped) push side has groups, and
  // keep_side is proven to contribute at most one row per match, so agg's own prior estimate
  // remains a valid upper bound for the join's output.
  join->estimated_rows = agg->estimated_rows;

  return join;
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
        // Skip temp tables (legacy optimizer may rewrite GROUP BY items to reference internal tmp tables). Template
        // tables used as materialization targets have a real source table accessible via pos_in_table_list; regular
        // temp tables have NULL.
        if (table->s->tmp_table != NO_TMP_TABLE) {
          if (field_item->table_ref != nullptr && field_item->table_ref->table != nullptr &&
              field_item->table_ref->table->s->tmp_table == NO_TMP_TABLE) {
            table = field_item->table_ref->table;
          }
          // If still a temp table after resolution attempt, give up on this field rather than assigning it to a side.
          if (table->s->tmp_table != NO_TMP_TABLE) return;
        }
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
    } else {
      // Legacy optimizer may wrap GROUP BY items in other Item subtypes.
      Item *real = it->real_item();
      if (real && real != it) visit(real);
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

TABLE *AggregationPushDown::get_pushable_scan_table(const PlanNode *side) {
  if (side == nullptr) return nullptr;
  if (side->type() == PlanNode::Type::SCAN) {
    return static_cast<const ScanTable *>(side)->source_table;
  }
  if (side->type() == PlanNode::Type::FILTER && side->children.size() == 1 && side->children[0]) {
    return get_pushable_scan_table(side->children[0].get());
  }
  // Anything else (a further join, another aggregate, …) would need transitive proof this rule
  // does not attempt -- see HashJoin::child_multiplicity's doc comment for the same limitation.
  return nullptr;
}

bool AggregationPushDown::group_by_covers_join_keys(const LocalAgg *agg, const std::vector<Item *> &join_conditions,
                                                    TABLE *push_table) {
  std::unordered_set<uint> grouped_fields;
  std::function<void(Item *)> collect = [&](Item *item) {
    if (!item) return;
    if (item->type() == Item::FIELD_ITEM) {
      auto *field_item = static_cast<Item_field *>(item);
      if (field_item->field != nullptr && field_item->field->table == push_table) {
        grouped_fields.insert(field_item->field->field_index());
      }
    } else if (item->type() == Item::FUNC_ITEM || item->type() == Item::SUM_FUNC_ITEM) {
      auto *func = static_cast<Item_func *>(item);
      for (uint i = 0; i < func->argument_count(); i++) collect(func->arguments()[i]);
    } else if (item->type() == Item::REF_ITEM) {
      auto *ref = static_cast<Item_ref *>(item);
      if (ref->ref_item()) collect(ref->ref_item());
    } else {
      Item *real = item->real_item();
      if (real && real != item) collect(real);
    }
  };
  for (auto *group_item : agg->group_by) collect(group_item);

  bool any_key_from_push_table = false;
  for (Item *cond : join_conditions) {
    if (!Utils::is_simple_equijoin(cond)) continue;
    auto *func = down_cast<Item_func *>(cond);
    for (Item *side : {func->arguments()[0], func->arguments()[1]}) {
      auto *field_item = down_cast<Item_field *>(side);
      if (field_item->field != nullptr && field_item->field->table == push_table) {
        any_key_from_push_table = true;
        if (!grouped_fields.count(field_item->field->field_index())) {
          return false;  // this join-key column isn't preserved by GROUP BY -- unsafe to push
        }
      }
    }
  }
  return any_key_from_push_table;
}

void TopNPushDown::apply(Plan &root) {
  // Deliberately support only the correctness-obvious base case here. A LIMIT
  // may be absorbed when its direct child is a scan, since no cardinality- or
  // order-changing operator is crossed. PredicatePushDown runs before this
  // rule, so a successfully pushed storage predicate is already part of the
  // Scan node; a residual Filter remains an explicit barrier.
  std::function<void(Plan &)> push_into_direct_scans = [&](Plan &node) {
    if (!node) return;
    for (Plan &child : node->children) push_into_direct_scans(child);

    if (node->type() != PlanNode::Type::LIMIT) return;
    auto *limit_node = static_cast<Limit *>(node.get());
    if (limit_node->children.size() != 1 || !limit_node->children[0] ||
        limit_node->children[0]->type() != PlanNode::Type::SCAN || limit_node->count_all_rows ||
        limit_node->reject_multiple_rows || limit_node->send_records_override != nullptr)
      return;

    auto *scan = static_cast<ScanTable *>(limit_node->children[0].get());
    if (scan->has_required_order) return;
    // LIMIT_OFFSET stores an exclusive row boundary (returned rows +
    // OFFSET), whereas RapidCursor::set_scan_limit() expects the number of
    // rows to return after skipping OFFSET. Normalize the representation at
    // the pushdown boundary.
    scan->limit = limit_node->limit == HA_POS_ERROR
                      ? HA_POS_ERROR
                      : (limit_node->limit > limit_node->offset ? limit_node->limit - limit_node->offset : 0);
    scan->offset = limit_node->offset;
    const ha_rows child_rows = scan->estimated_rows;
    const ha_rows after_offset = child_rows > scan->offset ? child_rows - scan->offset : 0;
    scan->estimated_rows = scan->limit == HA_POS_ERROR ? after_offset : std::min(after_offset, scan->limit);
    node = std::move(limit_node->children[0]);
  };
  push_into_direct_scans(root);
}

/**
 * Recursively push down LIMIT/TOP N
 * @param node Current plan node
 * @param pending_limit Pending limit to push down
 * @param pending_offset Pending offset
 * @param pending_order ORDER BY for TopN (nullptr if just LIMIT)
 * @return Modified plan node
 *
 * Not called from apply() yet — retained intentionally as the planned successor to the
 * direct-scan-only base case above, once it has been re-validated against the current PlanNode
 * shapes. Pushing a LIMIT through Sort/Filter/Join/Aggregate barriers (rather than only into a
 * directly-adjacent Scan) needs the same kind of correctness proof AggregationPushDown/JoinReOrder
 * are waiting on (see AddDefaultRules in optimizer.cpp) before it's safe to wire into apply().
 */
Plan TopNPushDown::push_limit_recursive(Plan &node, ha_rows pending_limit, ha_rows pending_offset, ORDER *pending_order,
                                        Filesort *pending_filesort) {
  if (!node) return nullptr;
  const auto has_limit = [](ha_rows limit) { return limit != HA_POS_ERROR; };
  const auto wrap_pending = [&](Plan child) -> Plan {
    if (!has_limit(pending_limit)) return child;
    return pending_order
               ? create_topn_node(std::move(child), pending_limit, pending_offset, pending_order, pending_filesort)
               : create_limit_node(std::move(child), pending_limit, pending_offset);
  };

  switch (node->type()) {
    case PlanNode::Type::LIMIT: {
      auto *limit_node = static_cast<Limit *>(node.get());
      ha_rows new_limit = limit_node->limit;
      ha_rows new_offset = limit_node->offset;
      if (has_limit(pending_limit))
        merge_limits(pending_limit, pending_offset, limit_node->limit, limit_node->offset, new_limit, new_offset);
      Plan child = std::move(limit_node->children[0]);
      return push_limit_recursive(child, new_limit, new_offset, pending_order, pending_filesort);
    }
    case PlanNode::Type::TOP_N: {
      auto *topn = static_cast<TopN *>(node.get());
      ha_rows new_limit = topn->limit;
      ha_rows new_offset = topn->offset;
      ORDER *new_order = topn->order;
      Filesort *filesort_to_use = topn->filesort ? topn->filesort : pending_filesort;
      if (has_limit(pending_limit))
        merge_limits(pending_limit, pending_offset, topn->limit, topn->offset, new_limit, new_offset);
      Plan child = std::move(topn->children[0]);
      return push_limit_recursive(child, new_limit, new_offset, new_order, filesort_to_use);
    }
    case PlanNode::Type::SORT: {
      auto *sort = static_cast<Sort *>(node.get());
      ORDER *new_order = sort->order;
      Filesort *filesort_to_use = sort->filesort ? sort->filesort : pending_filesort;
      if (has_limit(pending_limit)) {
        Plan child = std::move(sort->children[0]);
        child = push_limit_recursive(child, HA_POS_ERROR, 0, nullptr, filesort_to_use);
        return create_topn_node(std::move(child), pending_limit, pending_offset, new_order, filesort_to_use);
      }
      sort->children[0] = push_limit_recursive(sort->children[0], HA_POS_ERROR, 0, nullptr, filesort_to_use);
      return std::move(node);
    }
    case PlanNode::Type::LOCAL_AGGREGATE: {
      auto *agg = static_cast<LocalAgg *>(node.get());
      agg->children[0] = push_limit_recursive(agg->children[0], HA_POS_ERROR, 0, nullptr);
      return wrap_pending(std::move(node));
    }
    case PlanNode::Type::NESTED_LOOP_JOIN:
    case PlanNode::Type::HASH_JOIN: {
      // LIMIT/TOP-N cannot cross a join without uniqueness, multiplicity and
      // order-preservation proofs. Optimize both children independently and
      // retain the limiting operator above the complete join result.
      node->children[0] = push_limit_recursive(node->children[0], HA_POS_ERROR, 0, nullptr);
      node->children[1] = push_limit_recursive(node->children[1], HA_POS_ERROR, 0, nullptr);
      return wrap_pending(std::move(node));
    }
    case PlanNode::Type::FILTER: {
      auto *filter = static_cast<Filter *>(node.get());
      // LIMIT/TOP-N above a Filter must stay above it; otherwise the number
      // and identity of rows change when early rows fail the predicate.
      filter->children[0] = push_limit_recursive(filter->children[0], HA_POS_ERROR, 0, nullptr);
      return wrap_pending(std::move(node));
    }
    case PlanNode::Type::SCAN: {
      auto scan = static_cast<ScanTable *>(node.get());
      if (has_limit(pending_limit)) {
        if (pending_order) {
          // Do not cap an unordered scan before filesort: the Top-N rows may
          // occur anywhere in the input.
          return create_topn_node(std::move(node), pending_limit, pending_offset, pending_order, pending_filesort);
        } else {
          scan->limit = pending_limit;
          scan->offset = pending_offset;
          scan->estimated_rows = std::min(scan->estimated_rows, pending_limit);
          return std::move(node);
        }
      }
      return std::move(node);
    }
    default: {
      for (auto &child : node->children) {
        child = push_limit_recursive(child, HA_POS_ERROR, 0, nullptr);
      }
      return wrap_pending(std::move(node));
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
  (void)join;
  (void)has_order_by;
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
  topn->offset = offset;
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
  if (topn->filesort == nullptr) {
    ut_a(topn->filesort != nullptr);
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
  result_offset = inner_offset > HA_POS_ERROR - outer_offset ? HA_POS_ERROR : inner_offset + outer_offset;

  // Result limit is minimum of:
  // 1. outer_limit
  // 2. inner_limit - outer_offset (if inner has rows to skip)
  if (inner_limit == HA_POS_ERROR) {
    result_limit = outer_limit;
  } else if (outer_limit == HA_POS_ERROR) {
    result_limit = outer_offset >= inner_limit ? 0 : inner_limit - outer_offset;
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
