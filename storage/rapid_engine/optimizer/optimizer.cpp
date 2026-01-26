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

   Copyright (c) 2023, 2026, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_iterator.h"  //HashJoinIterator
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/range_optimizer/range_optimizer.h"  //KEY_PART,QUICK_RANGE
#include "sql/range_optimizer/tree.h"             //SEL_ARG
#include "sql/sql_class.h"
#include "sql/sql_lex.h"                      //Query_expression
#include "sql/sql_opt_exec_shared.h"          //Index_lookup
#include "sql/sql_optimizer.h"                //JOIN
#include "sql/table.h"                        //Table
#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/include/rapid_column_info.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/optimizer/rules/condition_pushdown.h"
#include "storage/rapid_engine/optimizer/rules/join_reorder.h"
#include "storage/rapid_engine/optimizer/rules/prune.h"
#include "storage/rapid_engine/utils/utils.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/optimizer/path/access_path.h"
#include "storage/rapid_engine/populate/log_commons.h"

namespace ShannonBase {
namespace Optimizer {
Timer::Timer() { m_begin = std::chrono::steady_clock::now(); }
std::chrono::nanoseconds Timer::lap() {
  const auto now = std::chrono::steady_clock::now();
  const auto lap_duration = std::chrono::nanoseconds{now - m_begin};
  m_begin = now;
  return lap_duration;
}

std::string Timer::lap_formatted() {
  auto stream = std::stringstream{};
  return stream.str();
}

void Optimizer::AddDefaultRules() {
// becareful the order of rules. The rules be applied in the order of added.
#if 0
  // Make predicates available
  m_optimize_rules.emplace_back(std::make_unique<PredicatePushDown>());
  // Use predicates for IMCU pruning
  m_optimize_rules.emplace_back(std::make_unique<StorageIndexPrune>());
  // After predicates clarify needed columns
  m_optimize_rules.emplace_back(std::make_unique<ProjectionPruning>());
  // Before aggregation changes structure
  m_optimize_rules.emplace_back(std::make_unique<TopNPushDown>());
  // aggregation push down to lower level operators
  m_optimize_rules.emplace_back(std::make_unique<AggregationPushDown>());
  // Re-run after structure changes
  m_optimize_rules.emplace_back(std::make_unique<ProjectionPruning>());
  // Final reordering with all optimizations
  m_optimize_rules.emplace_back(std::make_unique<JoinReOrder>());
#endif
  m_registered.store(true, std::memory_order_relaxed);
}

Plan Optimizer::Optimize(const OptimizeContext *context, const THD *thd, const JOIN *join) {
  if (!m_registered.load()) AddDefaultRules();
  if (m_optimize_rules.empty()) return nullptr;

  QueryPlan plan;
  plan.root = get_query_plan(const_cast<OptimizeContext *>(context), const_cast<THD *>(thd), const_cast<JOIN *>(join));
  for (auto &rule : m_optimize_rules) {
    Timer rule_timer;
    rule->apply(plan.root);
  }
  return std::move(plan.root);
}

Plan Optimizer::get_query_plan(OptimizeContext *context, THD *thd, const JOIN *join) {
  if (!join || !join->query_expression()->root_access_path()) return std::make_unique<ZeroRows>();

  Plan plan_root = translate_access_path(context, thd, join->query_expression()->root_access_path(), join);
  if (!plan_root) return std::make_unique<ZeroRows>();
  return plan_root;
}

Plan Optimizer::translate_access_path(OptimizeContext *ctx, THD *thd, AccessPath *path, const JOIN *join) {
  if (!path) return nullptr;

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_RANGE_SCAN: {
      auto scan = std::make_unique<ScanTable>();
      scan->original_path = path;
      TABLE *table{nullptr};
      if (path->type == AccessPath::INDEX_SCAN) {
        table = path->index_scan().table;
        scan->scan_type = ScanTable::ScanType::INDEX_SCAN;
      } else if (path->type == AccessPath::INDEX_RANGE_SCAN) {
        const auto &irs = path->index_range_scan();
        if (irs.used_key_part != nullptr && irs.num_used_key_parts > 0 && irs.used_key_part[0].field != nullptr)
          table = irs.used_key_part[0].field->table;
        scan->scan_type = ScanTable::ScanType::INDEX_SCAN;
      } else {
        table = path->table_scan().table;
        scan->scan_type = ScanTable::ScanType::FULL_TABLE_SCAN;
      }
      assert(table);

      auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
      auto table_id = share ? share->m_tableid : 0;
      scan->rpd_table = (share->is_partitioned) ? Imcs::Imcs::instance()->get_rpd_parttable(table_id)
                                                : Imcs::Imcs::instance()->get_rpd_table(table_id);
      assert(scan->rpd_table);
      scan->estimated_rows = path->num_output_rows();
      scan->source_table = table;
      return scan;
    } break;
    case AccessPath::HASH_JOIN: {
      auto hashjoin_node = std::make_unique<HashJoin>();
      hashjoin_node->original_path = path;
      auto &param = path->hash_join();

      // Recursively convert children
      hashjoin_node->children.push_back(translate_access_path(ctx, thd, param.outer, join));
      hashjoin_node->children.push_back(translate_access_path(ctx, thd, param.inner, join));
      // Extract Join Conditions
      if (param.join_predicate) {
        for (auto *cond : param.join_predicate->expr->equijoin_conditions) {
          hashjoin_node->join_conditions.push_back(cond);
        }
        // Handle other conditions...
      }

      hashjoin_node->allow_spill = param.allow_spill_to_disk;
      hashjoin_node->estimated_rows = path->num_output_rows();
      return hashjoin_node;
    } break;
    case AccessPath::NESTED_LOOP_JOIN: {
      auto nestloop_node = std::make_unique<NestLoopJoin>();
      nestloop_node->original_path = path;
      auto &param = path->nested_loop_join();

      // Recursively convert children
      nestloop_node->children.push_back(translate_access_path(ctx, thd, param.outer, join));
      nestloop_node->children.push_back(translate_access_path(ctx, thd, param.inner, join));
      nestloop_node->pfs_batch_mode = param.pfs_batch_mode;
      nestloop_node->already_expanded_predicates = param.already_expanded_predicates;
      // Extract Join Conditions
      nestloop_node->source_join_predicate = param.join_predicate;
      if (param.join_predicate) {
        for (auto *cond : param.join_predicate->expr->equijoin_conditions) {
          nestloop_node->join_conditions.push_back(cond);
        }
        // Handle other conditions...
      }
      nestloop_node->equijoin_predicates = param.equijoin_predicates;
      return nestloop_node;
    } break;
    case AccessPath::AGGREGATE: {
      auto agg = std::make_unique<LocalAgg>();
      agg->original_path = path;
      auto param = path->aggregate();

      agg->olap = param.olap;
      agg->children.push_back(translate_access_path(ctx, thd, param.child, join));
      fill_aggregate_info(agg.get(), join);
      return agg;
    } break;
    case AccessPath::LIMIT_OFFSET: {
      auto limit = std::make_unique<Limit>();
      limit->original_path = path;
      auto param = path->limit_offset();

      limit->limit = param.limit;
      limit->offset = param.offset;
      limit->children.push_back(translate_access_path(ctx, thd, param.child, join));
      return limit;
    } break;
    case AccessPath::FILTER: {
      auto filter = std::make_unique<Filter>();
      filter->original_path = path;
      auto param = path->filter();

      filter->condition = param.condition;
      filter->children.push_back(translate_access_path(ctx, thd, param.child, join));
      filter->predict = Optimizer::convert_item_to_predicate(thd, param.condition);
      return filter;
    } break;
    case AccessPath::SORT: {
      auto param = path->sort();
      // only it has `limit` clause, can be converted to `TopN`
      if (param.limit > 0 && param.limit != HA_POS_ERROR) {
        auto topn = std::make_unique<TopN>();
        topn->original_path = path;
        topn->order = param.order;
        topn->limit = param.limit;
        topn->filesort = param.filesort;
        topn->children.push_back(translate_access_path(ctx, thd, param.child, join));
        topn->estimated_rows = std::min(topn->children[0]->estimated_rows, (ha_rows)param.limit);
        return topn;
      } else {
        // without `LIMIT` clause, keep it as `order by`
        auto sort = std::make_unique<Sort>();
        sort->original_path = path;
        sort->order = param.order;
        sort->filesort = param.filesort;
        sort->limit = HA_POS_ERROR;
        sort->children.push_back(translate_access_path(ctx, thd, param.child, join));
        sort->estimated_rows = sort->children[0]->estimated_rows;
        sort->remove_duplicates = param.remove_duplicates;
        sort->unwrap_rollup = param.unwrap_rollup;
        sort->force_sort_rowids = param.force_sort_rowids;
        return sort;
      }
      assert(false);
      return nullptr;
    } break;
    case AccessPath::EQ_REF: {
      auto param = path->eq_ref();
      // Check if this is a dynamic join condition (not a constant lookup)
      bool dynamic_lookup{false};
      for (uint i = 0; i < param.ref->key_parts; i++) {
        Item *item = param.ref->items[i];
        if (!item) continue;
        // Check if this item references fields from other tables or is a non-constant expression
        if (!item->const_item()) {
          dynamic_lookup = true;
          break;
        }
      }

      if (!dynamic_lookup) {
        auto scan = std::make_unique<ScanTable>();
        scan->original_path = path;
        scan->source_table = param.table;
        scan->scan_type = ScanTable::ScanType::EQ_REF_SCAN;
        auto share = ShannonBase::shannon_loaded_tables->get(scan->source_table->s->db.str,
                                                             scan->source_table->s->table_name.str);
        auto table_id = share ? share->m_tableid : 0;
        scan->rpd_table = (share->is_partitioned) ? Imcs::Imcs::instance()->get_rpd_parttable(table_id)
                                                  : Imcs::Imcs::instance()->get_rpd_table(table_id);
        assert(scan->rpd_table);

        // If this is a dynamic join condition, we cannot convert it to a static Predicate. Return nullptr
        // to indicate this should be handled as a join condition during execution.
        // This is a JOIN condition like "c.customer_id = o.customer_id"
        // It should be handled by the join executor, not converted to a static filter predicate
        scan->prune_predicate = Optimizer::convert_item_to_predicate(thd, param.ref);
        return scan;
      } else {  // if it's dynamic join condition(such as a.id = b.id), the join condition cannot be pushed down.
        auto eq_ref = std::make_unique<MySQLNative>();
        eq_ref->original_path = path;
        return eq_ref;
      }
      assert(false);
      return nullptr;  // not reach forever.
    } break;
    default: {
      // if Rapid can not handle, then re-encapsulate to a Fallback node
      auto original = std::make_unique<MySQLNative>();
      original->original_path = path;
      original->estimated_rows = path->num_output_rows();
      // no need to transalte anymore, because it's a MySQL AccessPath.
      return original;
    }
  }
}

void Optimizer::fill_aggregate_info(LocalAgg *node, const JOIN *join) {
  Query_block *query_block = join->query_block;
  for (ORDER *ord = query_block->order_list.first; ord; ord = ord->next) {
    if (ord->item && *ord->item) node->order_by.push_back(*ord->item);
  }

  for (ORDER *ord = query_block->group_list.first; ord; ord = ord->next) {
    if (ord->item && *ord->item) node->group_by.push_back(*ord->item);
  }

  auto fields = query_block->get_fields_list();
  for (auto it = fields->begin(); it != fields->end(); ++it) {
    Item *item = *it;
    switch (item->type()) {
      case Item::SUM_FUNC_ITEM:
        node->aggregates.push_back(static_cast<Item_func *>(item));
        break;
      case Item::AGGR_FIELD_ITEM:
        node->aggregates.push_back(static_cast<Item_func *>(item));
        break;
      case Item::FUNC_ITEM: {
        Item_func *func_item = static_cast<Item_func *>(item);
        switch (func_item->functype()) {
          case Item_func::LEAST_FUNC: {
          } break;
          case Item_func::GREATEST_FUNC: {
          } break;
          default:
            break;
        }
      } break;
      default:
        break;
    }
  }
}

/**
 * Convert MySQL Item to Rapid Predicate
 *
 * This function translates MySQL's Item expression tree into our custom
 * Predicate representation for optimized execution in IMCS.
 *
 * @param thd Thread handler
 * @param item MySQL Item to convert
 * @return Converted Predicate, or nullptr if not convertible
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_item_to_predicate(THD *thd, Item *item) {
  if (!item) return nullptr;

  // Handle different Item types
  switch (item->type()) {
    case Item::FUNC_ITEM:
      return convert_func_item_to_predicate(thd, static_cast<Item_func *>(item));
    case Item::COND_ITEM:
      return convert_cond_item_to_predicate(thd, static_cast<Item_cond *>(item));
    case Item::FIELD_ITEM:
    case Item::INT_ITEM:
    case Item::STRING_ITEM:
    case Item::REAL_ITEM:
    case Item::DECIMAL_ITEM:
      // These are leaf nodes, typically part of a comparison
      // They don't form predicates by themselves
      return nullptr;
    default:  // Unsupported Item type for predicate conversion
      return nullptr;
  }
}

/**
 * Convert Index_lookup to Predicate
 *
 * Index_lookup contains range conditions for index scans.
 * This function extracts the range conditions and converts them to predicates.
 *
 * The items[] array may contain:
 * 1. Constants lookup(for simple WHERE conditions like "id = 5")
 * 2. Dynamic lookup Field references from other tables (for JOIN conditions like "t1.id = t2.id")
 *
 * For case 2, we CANNOT convert to a simple predicate with a constant value.
 * Instead, we need to recognize this as a JOIN condition that will be evaluated dynamically during execution.
 *
 * @param thd Thread handler
 * @param lookup Index lookup information
 * @return Converted Predicate representing the index range conditions
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_item_to_predicate(THD *thd, Index_lookup *lookup) {
  if (!lookup || !lookup->key_err || lookup->key == -1 || lookup->key_parts == 0) return nullptr;

  // Check for impossible NULL references
  if (lookup->impossible_null_ref()) return nullptr;  // This will never match

  // If there's only one key part, create a simple equality predicate
  if (lookup->key_parts == 1) {
    Item *item = lookup->items[0];
    if (!item) return nullptr;
    assert(item->const_item());

    // Check if this key part has a guard condition
    if (lookup->cond_guards && lookup->cond_guards[0] && !(*lookup->cond_guards[0])) return nullptr;

    // Extract field from the item
    if (item->type() == Item::FIELD_ITEM) {
      Item_field *field_item = static_cast<Item_field *>(item);
      if (field_item->field) {
        uint32 col_idx = field_item->field->field_index();
        enum_field_types field_type = field_item->field->type();

        // Get the value to compare against
        Imcs::PredicateValue value = extract_value_from_item(thd, item);

        // Check if this is a NULL-rejecting equality
        bool is_null_rejecting = (lookup->null_rejecting & 1);
        if (is_null_rejecting && value.is_null()) return nullptr;
        return std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
      }
    }

    // Try to evaluate the item directly
    Imcs::PredicateValue value = extract_value_from_item(thd, item);
    if (value.is_null()) return nullptr;

    // We couldn't extract field info - return nullptr
    return nullptr;
  }

  // Multiple key parts - create compound predicate with AND
  auto compound = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
  bool has_predicates = false;

  for (uint i = 0; i < lookup->key_parts; i++) {
    Item *item = lookup->items[i];
    if (!item) continue;

    // Check if this key part has a guard condition
    if (lookup->cond_guards && lookup->cond_guards[i]) {
      if (!(*lookup->cond_guards[i])) continue;  // Guard is off - skip this condition
    }

    // Extract field information
    uint32 col_idx = 0;
    enum_field_types field_type = MYSQL_TYPE_NULL;

    if (item->type() == Item::FIELD_ITEM) {
      Item_field *field_item = static_cast<Item_field *>(item);
      if (field_item->field) {
        col_idx = field_item->field->field_index();
        field_type = field_item->field->type();
      } else
        continue;
    } else
      continue;  // Not a field item - skip

    Imcs::PredicateValue value = extract_value_from_item(thd, item);  // Get the value from the item
    bool is_null_rejecting = (lookup->null_rejecting & (1 << i));     // Check for NULL-rejecting
    if (is_null_rejecting && value.is_null()) {
      // This key part rejects NULL - entire lookup is impossible
      return nullptr;  // compound will be automatically destroyed
    }

    // Create equality predicate for this key part
    auto pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
    compound->add_child(std::move(pred));
    has_predicates = true;
  }

  if (!has_predicates) return nullptr;  // compound will be automatically destroyed

  // If only one predicate was created, return it directly
  if (compound->children.size() == 1)
    return std::move(compound->children[0]);  // Transfer ownership of the single child
  return compound;
}

/**
 * Convert SEL_ARG tree to Predicate
 *
 * SEL_ARG represents the range tree for index optimization.
 * It's a red-black tree structure containing range intervals.
 *
 * @param thd Thread handler
 * @param sel_arg Range tree root
 * @param key_part Key part information
 * @return Converted Predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_sel_arg_to_predicate(THD *thd, const SEL_ARG *sel_arg,
                                                                         const KEY_PART_INFO *key_part) {
  if (!sel_arg || sel_arg == opt_range::null_element) return nullptr;

  // create root OR container, to build all discrete intervals
  auto or_predicate = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::OR);

  std::function<void(SEL_ARG *)> traverse_rb_tree = [&](SEL_ARG *arg) {
    if (!arg || arg == opt_range::null_element) return;

    // traverse left subtree (discrete intervals in red-black tree are OR relations)
    traverse_rb_tree(arg->left);

    // process current node's interval conditions
    std::unique_ptr<Imcs::Predicate> current_node_pred;
    Field *field = arg->field;
    uint32 col_idx = field->field_index();
    enum_field_types field_type = field->type();

    if (arg->is_null_interval()) {
      // case1: col IS NULL
      current_node_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::IS_NULL,
                                                                   Imcs::PredicateValue::null_value(), field_type);
    } else if (arg->is_singlepoint()) {
      // case2：col = 10
      Imcs::PredicateValue val = extract_value_from_sel_arg_min(thd, arg, field_type);
      current_node_pred =
          std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::EQUAL, val, field_type);
    } else {
      // case：range: col > 10 AND col <= 20
      uint min_flag = arg->get_min_flag();
      uint max_flag = arg->get_max_flag();
      bool has_min = !(min_flag & NO_MIN_RANGE);
      bool has_max = !(max_flag & NO_MAX_RANGE);

      if (has_min && has_max) {
        Imcs::PredicateValue min_val = extract_value_from_sel_arg_min(thd, arg, field_type);
        Imcs::PredicateValue max_val = extract_value_from_sel_arg_max(thd, arg, field_type);

        // using between if both bounds are inclusive
        if (!(min_flag & NEAR_MIN) && !(max_flag & NEAR_MAX)) {
          current_node_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, min_val, max_val, field_type);
        } else {
          auto range_and = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
          range_and->add_child(std::make_unique<Imcs::Simple_Predicate>(
              col_idx,
              (min_flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL,
              min_val, field_type));
          range_and->add_child(std::make_unique<Imcs::Simple_Predicate>(
              col_idx, (max_flag & NEAR_MAX) ? Imcs::PredicateOperator::LESS_THAN : Imcs::PredicateOperator::LESS_EQUAL,
              max_val, field_type));
          current_node_pred = std::move(range_and);
        }
      } else if (has_min) {
        Imcs::PredicateValue min_val = extract_value_from_sel_arg_min(thd, arg, field_type);
        auto op =
            (min_flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL;
        current_node_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, op, min_val, field_type);
      } else if (has_max) {
        Imcs::PredicateValue max_val = extract_value_from_sel_arg_max(thd, arg, field_type);
        auto op = (max_flag & NEAR_MAX) ? Imcs::PredicateOperator::LESS_THAN : Imcs::PredicateOperator::LESS_EQUAL;
        current_node_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, op, max_val, field_type);
      }
    }

    // processing And link to next key part if exists
    // if current interval is followed by a restriction on the next column, recursively link them
    if (current_node_pred && arg->next_key_part && arg->next_key_part->root) {
      std::unique_ptr<Imcs::Predicate> next_part_pred =
          convert_sel_arg_to_predicate(thd, arg->next_key_part->root, key_part + 1);

      if (next_part_pred) {
        auto multi_col_and = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
        multi_col_and->add_child(std::move(current_node_pred));
        multi_col_and->add_child(std::move(next_part_pred));
        current_node_pred = std::move(multi_col_and);
      }
    }

    // processing whole predicate and add to OR list
    if (current_node_pred) {
      or_predicate->add_child(std::move(current_node_pred));
    }

    traverse_rb_tree(arg->right);
  };

  traverse_rb_tree(const_cast<SEL_ARG *>(sel_arg));

  // clean up empty OR
  if (or_predicate->children.empty()) return nullptr;  // or_predicate will be automatically destroyed

  // simplify single-node OR
  if (or_predicate->children.size() == 1)
    return std::move(or_predicate->children[0]);  // Transfer ownership of the single child

  return or_predicate;
}

/**
 * Convert QUICK_RANGE to Predicate
 *
 * QUICK_RANGE represents a range scan on an index.
 * It contains min/max values and flags for each key part.
 *
 * @param thd Thread handler
 * @param range Quick range information
 * @param key_part Key part information
 * @return Converted Predicate representing the range
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_quick_range_to_predicate(THD *thd, const QUICK_RANGE *range,
                                                                             const KEY_PART_INFO *key_part) {
  if (!range || !key_part) return nullptr;

  // Get field information
  Field *field = key_part->field;
  if (!field) return nullptr;

  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  // Extract min and max values from range
  bool has_min = !(range->flag & NO_MIN_RANGE);
  bool has_max = !(range->flag & NO_MAX_RANGE);
  bool min_inclusive = !(range->flag & NEAR_MIN);
  bool max_inclusive = !(range->flag & NEAR_MAX);

  // If we have both min and max, create BETWEEN predicate
  if (has_min && has_max) {
    // Extract values
    Imcs::PredicateValue min_val = Optimizer::extract_value_from_key_part(thd, range->min_key, key_part, field_type);
    Imcs::PredicateValue max_val = Optimizer::extract_value_from_key_part(thd, range->max_key, key_part, field_type);

    // Adjust for inclusive/exclusive bounds
    if (!min_inclusive || !max_inclusive) {
      // If not fully inclusive, create compound predicate
      auto compound = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
      if (has_min) {
        auto min_op = min_inclusive ? Imcs::PredicateOperator::GREATER_EQUAL : Imcs::PredicateOperator::GREATER_THAN;
        auto min_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, min_op, min_val, field_type);
        compound->add_child(std::move(min_pred));
      }

      if (has_max) {
        auto max_op = max_inclusive ? Imcs::PredicateOperator::LESS_EQUAL : Imcs::PredicateOperator::LESS_THAN;
        auto max_pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, max_op, max_val, field_type);
        compound->add_child(std::move(max_pred));
      }
      return compound;
    } else {
      // Both inclusive - create BETWEEN predicate
      return std::make_unique<Imcs::Simple_Predicate>(col_idx, min_val, max_val, field_type);
    }
  }

  // Only min bound
  if (has_min) {
    Imcs::PredicateValue min_val = Optimizer::extract_value_from_key_part(thd, range->min_key, key_part, field_type);
    auto op = min_inclusive ? Imcs::PredicateOperator::GREATER_EQUAL : Imcs::PredicateOperator::GREATER_THAN;
    return std::make_unique<Imcs::Simple_Predicate>(col_idx, op, min_val, field_type);
  }

  // Only max bound
  if (has_max) {
    Imcs::PredicateValue max_val = Optimizer::extract_value_from_key_part(thd, range->max_key, key_part, field_type);
    auto op = max_inclusive ? Imcs::PredicateOperator::LESS_EQUAL : Imcs::PredicateOperator::LESS_THAN;
    return std::make_unique<Imcs::Simple_Predicate>(col_idx, op, max_val, field_type);
  }

  // No bounds - this shouldn't happen
  return nullptr;
}

/**
 * Extract value from key part buffer
 */
Imcs::PredicateValue Optimizer::extract_value_from_key_part(THD *thd, const uchar *key_ptr,
                                                            const KEY_PART_INFO *key_part,
                                                            enum_field_types field_type) {
  if (!key_ptr || !key_part || !key_part->field) return Imcs::PredicateValue::null_value();

  Field *field = key_part->field;
  // Handle NULL flag if present
  if (key_part->null_bit) {
    if (*key_ptr) return Imcs::PredicateValue::null_value();  // NULL flag is set
    key_ptr++;                                                // Skip NULL byte
  }

  // Extract value based on field type
  switch (field_type) {
    case MYSQL_TYPE_TINY: {
      int8 val = static_cast<int8>(*key_ptr);
      return Imcs::PredicateValue(static_cast<int64>(val));
    } break;
    case MYSQL_TYPE_SHORT: {
      int16 val = sint2korr(key_ptr);
      return Imcs::PredicateValue(static_cast<int64>(val));
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: {
      int32 val = sint4korr(key_ptr);
      return Imcs::PredicateValue(static_cast<int64>(val));
    } break;
    case MYSQL_TYPE_LONGLONG: {
      int64 val = sint8korr(key_ptr);
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_FLOAT: {
      float val;
      memcpy(&val, key_ptr, sizeof(float));
      return Imcs::PredicateValue(static_cast<double>(val));
    }
    case MYSQL_TYPE_DOUBLE: {
      double val;
      memcpy(&val, key_ptr, sizeof(double));
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: {
      // String types - extract length and data
      uint length;
      const uchar *data_ptr = key_ptr;
      if (key_part->length > 255) {
        // 2-byte length prefix
        length = uint2korr(data_ptr);
        data_ptr += 2;
      } else {
        // 1-byte length prefix
        length = *data_ptr;
        data_ptr += 1;
      }

      // Limit to actual key part length
      length = std::min(length, static_cast<uint>(key_part->length));
      std::string str(reinterpret_cast<const char *>(data_ptr), length);
      return Imcs::PredicateValue(str);
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: {
      // For date/time types, convert to int64 representation
      int64 val = sint8korr(key_ptr);
      return Imcs::PredicateValue(val);
    } break;
    default:
      // For other types, try to use the field's conversion
      String str_buf;
      field->set_key_image(key_ptr, key_part->length);
      if (field->is_null()) return Imcs::PredicateValue::null_value();
      switch (field->result_type()) {
        case INT_RESULT:
          return Imcs::PredicateValue(static_cast<int64>(field->val_int()));
        case REAL_RESULT:
          return Imcs::PredicateValue(static_cast<double>(field->val_real()));
        case STRING_RESULT: {
          String *str = field->val_str(&str_buf);
          if (str) return Imcs::PredicateValue(std::string(str->ptr(), str->length()));
          return Imcs::PredicateValue::null_value();
        }
        default:
          return Imcs::PredicateValue::null_value();
      }
  }
}

/**
 * Extract value from SEL_ARG (single point)
 */
Imcs::PredicateValue Optimizer::extract_value_from_sel_arg(THD *thd, const SEL_ARG *sel_arg,
                                                           enum_field_types field_type) {
  if (!sel_arg) return Imcs::PredicateValue::null_value();
  // For single point, min and max are the same
  return extract_value_from_sel_arg_min(thd, sel_arg, field_type);
}

/**
 * Extract minimum value from SEL_ARG
 */
Imcs::PredicateValue Optimizer::extract_value_from_sel_arg_min(THD *thd, const SEL_ARG *sel_arg,
                                                               enum_field_types field_type) {
  if (!sel_arg || !sel_arg->field) return Imcs::PredicateValue::null_value();

  // Get the field and extract value
  Field *field = sel_arg->field;
  // SEL_ARG stores values in field's native format
  // We need to extract based on field type
  switch (field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT: {
      auto val = *reinterpret_cast<short *>(sel_arg->min_value);
      return Imcs::PredicateValue(static_cast<int64>(val));
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      // For integer types, use min_value
      auto val = *reinterpret_cast<long int *>(sel_arg->min_value);
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      // For floating point, interpret min_value as double
      double val;
      memcpy(&val, &sel_arg->min_value, sizeof(double));
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: {
      // Try to get string from field
      String str_buf;
      // field->store(sel_arg->min_value, false);
      String *str = field->val_str(&str_buf);
      if (str) return Imcs::PredicateValue(std::string(str->ptr(), str->length()));
      return Imcs::PredicateValue::null_value();
    } break;
    default:
      return Imcs::PredicateValue::null_value();
  }
}

/**
 * Extract maximum value from SEL_ARG
 */
Imcs::PredicateValue Optimizer::extract_value_from_sel_arg_max(THD *thd, const SEL_ARG *sel_arg,
                                                               enum_field_types field_type) {
  if (!sel_arg || !sel_arg->field) return Imcs::PredicateValue::null_value();

  // Get the field and extract value
  Field *field = sel_arg->field;
  switch (field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT: {
      auto val = *reinterpret_cast<short *>(sel_arg->max_value);
      return Imcs::PredicateValue(static_cast<int64>(val));
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      auto val = *reinterpret_cast<long int *>(sel_arg->max_value);
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      double val;
      memcpy(&val, &sel_arg->max_value, sizeof(double));
      return Imcs::PredicateValue(val);
    } break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: {
      String str_buf;
      // field->store(sel_arg->max_value, false);
      String *str = field->val_str(&str_buf);
      if (str) return Imcs::PredicateValue(std::string(str->ptr(), str->length()));

      return Imcs::PredicateValue::null_value();
    } break;
    default:
      return Imcs::PredicateValue::null_value();
  }
}

/**
 * Convert function Item to predicate (comparison operations)
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_func_item_to_predicate(THD *thd, Item_func *func) {
  if (!func) return nullptr;

  switch (func->functype()) {
    case Item_func::EQ_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::EQUAL);
    case Item_func::NE_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::NOT_EQUAL);
    case Item_func::LT_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::LESS_THAN);
    case Item_func::LE_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::LESS_EQUAL);
    case Item_func::GT_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::GREATER_THAN);
    case Item_func::GE_FUNC:
      return convert_comparison_to_predicate(thd, func, Imcs::PredicateOperator::GREATER_EQUAL);
    case Item_func::BETWEEN:
      return convert_between_to_predicate(thd, static_cast<Item_func_between *>(func));
    case Item_func::IN_FUNC:
      return convert_in_to_predicate(thd, static_cast<Item_func_in *>(func));
    case Item_func::ISNULL_FUNC:
      return convert_isnull_to_predicate(thd, func);
    case Item_func::ISNOTNULL_FUNC:
      return convert_isnotnull_to_predicate(thd, func);
    case Item_func::LIKE_FUNC:
      return convert_like_to_predicate(thd, static_cast<Item_func_like *>(func), false);
    case Item_func::COND_AND_FUNC:
    case Item_func::COND_OR_FUNC:
      // These should have been handled by convert_cond_item_to_predicate
      return convert_cond_item_to_predicate(thd, static_cast<Item_cond *>(func));
    default:
      // Unsupported function type
      return nullptr;
  }
}

/**
 * Convert condition Item to compound predicate (AND/OR/NOT)
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_cond_item_to_predicate(THD *thd, Item_cond *cond) {
  if (!cond) return nullptr;

  Imcs::PredicateOperator op;

  switch (cond->functype()) {
    case Item_func::COND_AND_FUNC:
      op = Imcs::PredicateOperator::AND;
      break;
    case Item_func::COND_OR_FUNC:
      op = Imcs::PredicateOperator::OR;
      break;
    default:
      return nullptr;
  }

  // Create compound predicate
  auto compound = std::make_unique<Imcs::Compound_Predicate>(op);

  // Convert each child
  List_iterator<Item> li(*cond->argument_list());
  Item *item;
  while ((item = li++)) {
    std::unique_ptr<Imcs::Predicate> child_pred = Optimizer::convert_item_to_predicate(thd, item);
    if (child_pred) compound->add_child(std::move(child_pred));
  }

  // If no children were converted, return nullptr
  if (compound->children.empty()) return nullptr;

  return compound;
}

/**
 * Convert comparison operation to Simple_Predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_comparison_to_predicate(THD *thd, Item_func *func,
                                                                            Imcs::PredicateOperator op) {
  if (func->argument_count() != 2) return nullptr;

  Item *left = func->arguments()[0];
  Item *right = func->arguments()[1];

  // We need one side to be a field reference
  Item_field *field_item = nullptr;
  Item *value_item = nullptr;

  if (left->type() == Item::FIELD_ITEM) {
    field_item = static_cast<Item_field *>(left);
    value_item = right;
  } else if (right->type() == Item::FIELD_ITEM) {
    field_item = static_cast<Item_field *>(right);
    value_item = left;

    // Swap operator direction if field is on right
    op = swap_operator(op);
  } else {
    // Neither side is a field - can't create a simple predicate
    return nullptr;
  }

  // Extract field information
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  // Extract comparison value
  Imcs::PredicateValue value = extract_value_from_item(thd, value_item);

  // Create Simple_Predicate
  return std::make_unique<Imcs::Simple_Predicate>(col_idx, op, value, field_type);
}

/**
 * Convert BETWEEN to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_between_to_predicate(THD *thd, Item_func_between *between) {
  if (between->argument_count() != 3) return nullptr;

  Item *field_arg = between->arguments()[0];
  Item *min_arg = between->arguments()[1];
  Item *max_arg = between->arguments()[2];

  if (field_arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(field_arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  // Extract min and max values
  Imcs::PredicateValue min_val = extract_value_from_item(thd, min_arg);
  Imcs::PredicateValue max_val = extract_value_from_item(thd, max_arg);

  // Check if this is NOT BETWEEN
  bool is_negated = between->negated;
  if (is_negated) {
    // NOT BETWEEN: create compound predicate (val < min OR val > max)
    auto compound = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::OR);
    auto less_pred =
        std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::LESS_THAN, min_val, field_type);
    auto greater_pred =
        std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::GREATER_THAN, max_val, field_type);

    compound->add_child(std::move(less_pred));
    compound->add_child(std::move(greater_pred));

    return compound;
  } else {
    // BETWEEN: create simple predicate
    return std::make_unique<Imcs::Simple_Predicate>(col_idx, min_val, max_val, field_type);
  }
}

/**
 * Convert IN to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_in_to_predicate(THD *thd, Item_func_in *in_func) {
  if (in_func->argument_count() < 2) return nullptr;

  Item *field_arg = in_func->arguments()[0];
  if (field_arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(field_arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  // Extract values from IN list
  std::vector<Imcs::PredicateValue> values;
  for (uint i = 1; i < in_func->argument_count(); i++) {
    Item *value_item = in_func->arguments()[i];
    Imcs::PredicateValue val = extract_value_from_item(thd, value_item);
    values.push_back(val);
  }

  bool is_negated = in_func->negated;
  return std::make_unique<Imcs::Simple_Predicate>(col_idx, values, is_negated, field_type);
}

/**
 * Convert IS NULL to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_isnull_to_predicate(THD *thd, Item_func *func) {
  if (func->argument_count() != 1) return nullptr;

  Item *arg = func->arguments()[0];
  if (arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  return std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::IS_NULL,
                                                  Imcs::PredicateValue::null_value(), field_type);
}

/**
 * Convert IS NOT NULL to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_isnotnull_to_predicate(THD *thd, Item_func *func) {
  if (func->argument_count() != 1) return nullptr;

  Item *arg = func->arguments()[0];
  if (arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  return std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::IS_NOT_NULL,
                                                  Imcs::PredicateValue::null_value(), field_type);
}

/**
 * Convert LIKE to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_like_to_predicate(THD *thd, Item_func_like *like_func,
                                                                      bool is_negated) {
  if (like_func->argument_count() < 2) return nullptr;

  Item *field_arg = like_func->arguments()[0];
  Item *pattern_arg = like_func->arguments()[1];

  if (field_arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(field_arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  // Extract pattern
  Imcs::PredicateValue pattern = extract_value_from_item(thd, pattern_arg);
  Imcs::PredicateOperator op = is_negated ? Imcs::PredicateOperator::NOT_LIKE : Imcs::PredicateOperator::LIKE;
  return std::make_unique<Imcs::Simple_Predicate>(col_idx, op, pattern, field_type);
}

/**
 * Extract value from Item
 */
Imcs::PredicateValue Optimizer::extract_value_from_item(THD *thd, Item *item) {
  if (!item) return Imcs::PredicateValue::null_value();

  // Handle constant folding if needed
  if (!item->const_item()) assert(false);

  switch (item->type()) {
    case Item::INT_ITEM: {
      Item_int *int_item = static_cast<Item_int *>(item);
      return Imcs::PredicateValue(static_cast<int64>(int_item->val_int()));
    } break;
    case Item::REAL_ITEM: {
      Item_float *float_item = static_cast<Item_float *>(item);
      return Imcs::PredicateValue(float_item->val_real());
    } break;
    case Item::DECIMAL_ITEM: {
      Item_decimal *decimal_item = static_cast<Item_decimal *>(item);
      return Imcs::PredicateValue(decimal_item->val_real());
    } break;
    case Item::STRING_ITEM: {
      Item_string *string_item = static_cast<Item_string *>(item);
      String *str = string_item->val_str(nullptr);
      if (str) {
        return Imcs::PredicateValue(std::string(str->ptr(), str->length()));
      }
      return Imcs::PredicateValue::null_value();
    } break;
    case Item::NULL_ITEM:
      return Imcs::PredicateValue::null_value();
      break;
    case Item::FUNC_ITEM: {
      // Try to evaluate the function
      Item_func *func = static_cast<Item_func *>(item);

      if (func->result_type() == INT_RESULT) {
        return Imcs::PredicateValue(static_cast<int64>(func->val_int()));
      } else if (func->result_type() == REAL_RESULT) {
        return Imcs::PredicateValue(func->val_real());
      } else if (func->result_type() == STRING_RESULT) {
        String str_buf;
        String *str = func->val_str(&str_buf);
        if (str) {
          return Imcs::PredicateValue(std::string(str->ptr(), str->length()));
        }
      }
      return Imcs::PredicateValue::null_value();
    } break;
    default:
      // For other types, try generic evaluation
      if (item->result_type() == INT_RESULT) {
        return Imcs::PredicateValue(static_cast<int64>(item->val_int()));
      } else if (item->result_type() == REAL_RESULT) {
        return Imcs::PredicateValue(item->val_real());
      } else if (item->result_type() == STRING_RESULT) {
        String str_buf;
        String *str = item->val_str(&str_buf);
        if (str) {
          return Imcs::PredicateValue(std::string(str->ptr(), str->length()));
        }
      }
      return Imcs::PredicateValue::null_value();
  }
}

/**
 * Swap comparison operator when operands are reversed
 */
Imcs::PredicateOperator Optimizer::swap_operator(Imcs::PredicateOperator op) {
  switch (op) {
    case Imcs::PredicateOperator::LESS_THAN:
      return Imcs::PredicateOperator::GREATER_THAN;
    case Imcs::PredicateOperator::LESS_EQUAL:
      return Imcs::PredicateOperator::GREATER_EQUAL;
    case Imcs::PredicateOperator::GREATER_THAN:
      return Imcs::PredicateOperator::LESS_THAN;
    case Imcs::PredicateOperator::GREATER_EQUAL:
      return Imcs::PredicateOperator::LESS_EQUAL;
    case Imcs::PredicateOperator::EQUAL:
    case Imcs::PredicateOperator::NOT_EQUAL:
      // These are symmetric
      return op;
    default:
      return op;
  }
}

bool Optimizer::CanPathBeVectorized(AccessPath *path) {
  if (path == nullptr) return true;

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN: {
      TABLE *table{nullptr};
      if (path->type == AccessPath::TABLE_SCAN)
        table = path->table_scan().table;
      else if (path->type == AccessPath::INDEX_SCAN)
        table = path->index_scan().table;
      bool is_secondary_engine = table->s->is_secondary_engine();
      bool is_loaded = shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
      bool has_sufficient_data = (table->file->stats.records >= SHANNON_VECTOR_WIDTH);
      return is_secondary_engine && is_loaded && has_sufficient_data;
    }
    case AccessPath::INDEX_RANGE_SCAN:  // INDEX_RANGE_SCAN: Can be vectorized with sufficient data
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:  // FILTER: Vectorizable if the child can be vectorized
      return true;
    case AccessPath::SORT:  // SORT: Can be vectorized for in-memory sorts
      return true;
    case AccessPath::AGGREGATE: {  // AGGREGATE: Vectorizable with sufficient output rows and child support
      return true;
    }
    case AccessPath::HASH_JOIN: {  // HASH_JOIN: Vectorizable under specific conditions
      auto &hash_join = path->hash_join();
      // Cannot vectorize if storing rowids or allowing disk spilling
      if (hash_join.store_rowids) return false;
      if (hash_join.allow_spill_to_disk) return false;
      return hash_join.join_predicate != nullptr;
    }
    case AccessPath::NESTED_LOOP_JOIN:  // NESTED_LOOP_JOIN: Generally not suitable for vectorization
      return false;
    case AccessPath::FOLLOW_TAIL:  // These access methods are inherently non-vectorizable
    case AccessPath::MRR:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::INDEX_MERGE:
      return false;
    default:  // Default conservative approach: assume vectorizable
      return true;
  }
}

bool Optimizer::CheckChildVectorization(AccessPath *child_path) {
  if (child_path == nullptr) return true;
  // Check current path.
  if (!CanPathBeVectorized(child_path)) return false;

  // recursive all children path.
  switch (child_path->type) {
    case AccessPath::HASH_JOIN:
      return CheckChildVectorization(child_path->hash_join().outer) &&
             CheckChildVectorization(child_path->hash_join().inner);
    case AccessPath::NESTED_LOOP_JOIN:
      return CheckChildVectorization(child_path->nested_loop_join().outer) &&
             CheckChildVectorization(child_path->nested_loop_join().inner);
    case AccessPath::FILTER:
      return CheckChildVectorization(child_path->filter().child);
    case AccessPath::SORT:
      return CheckChildVectorization(child_path->sort().child);
    case AccessPath::AGGREGATE:
      return CheckChildVectorization(child_path->aggregate().child);
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
      return true;
    default:
      return true;
  }
}

/**
 * @brief Builds customized access paths for secondary engine optimization
 *
 * Creates specialized AccessPath types (Vectorized Table Scan, GPU Join, etc.)
 * for secondary engine execution. The function examines each AccessPath type
 * and creates optimized versions when possible.
 *
 * Why not use original AccessPath directly?
 * - Future extensions may require custom AccessPath types (e.g., RapidAccessPath)
 * - Original AccessPath tree will be freed and replaced with rapid_path
 * - Enables specialized iterators in PathGenerator::CreateIteratorFromAccessPath
 *
 * @param context Optimization context with vectorization capabilities
 * @param path Input AccessPath to optimize
 * @param join JOIN structure for query context
 * @return AccessPath* Optimized AccessPath, or nullptr if no optimization applied
 */
AccessPath *Optimizer::OptimizeAndRewriteAccessPath(OptimizeContext *context, AccessPath *path, const JOIN *join) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN: {
      // create vectorized table scan if it can.
      context->can_vectorized = Optimizer::CanPathBeVectorized(path);
      if (path->vectorized == context->can_vectorized) return nullptr;  // has vectorized, not need the new AP.

      // create vectorized table scan if it can.
      auto rapid_path = new (current_thd->mem_root) AccessPath();
      rapid_path->vectorized = context->can_vectorized;
      rapid_path->type = AccessPath::TABLE_SCAN;
      rapid_path->count_examined_rows = true;
      rapid_path->table_scan().table = path->table_scan().table;
      rapid_path->iterator = nullptr;
      rapid_path->secondary_engine_data = path->secondary_engine_data;

      return rapid_path;
    } break;
    case AccessPath::INDEX_SCAN: {
    } break;
    case AccessPath::SAMPLE_SCAN: {
    } break;
    case AccessPath::REF: {
    } break;
    case AccessPath::REF_OR_NULL: {
    } break;
    case AccessPath::EQ_REF: {
    } break;
    case AccessPath::PUSHED_JOIN_REF: {
    } break;
    case AccessPath::FULL_TEXT_SEARCH: {
    } break;
    case AccessPath::CONST_TABLE: {
    } break;
    case AccessPath::MRR: {
    } break;
    case AccessPath::FOLLOW_TAIL: {
    } break;
    case AccessPath::INDEX_RANGE_SCAN: {
    } break;
    case AccessPath::INDEX_MERGE: {
    } break;
    case AccessPath::ROWID_INTERSECTION: {
    } break;
    case AccessPath::ROWID_UNION: {
    } break;
    case AccessPath::INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::GROUP_INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
    } break;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
    } break;
    case AccessPath::FAKE_SINGLE_ROW: {
    } break;
    case AccessPath::ZERO_ROWS: {
    } break;
    case AccessPath::ZERO_ROWS_AGGREGATED: {
    } break;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
    } break;
    case AccessPath::UNQUALIFIED_COUNT: {
    } break;

    // Joins.
    case AccessPath::NESTED_LOOP_JOIN: {
    } break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
    } break;
    case AccessPath::BKA_JOIN: {
    } break;
    case AccessPath::HASH_JOIN: {
      auto hash_join = path->hash_join();
      if (hash_join.join_predicate != nullptr &&
          (hash_join.rewrite_semi_to_inner || hash_join.allow_spill_to_disk == false)) {
        context->can_vectorized = true;
      }
      if (hash_join.allow_spill_to_disk || hash_join.store_rowids) context->can_vectorized = false;
      if (path->vectorized == context->can_vectorized) return nullptr;

      auto rapid_path = new (current_thd->mem_root) AccessPath();
      rapid_path->vectorized = context->can_vectorized;
      rapid_path->type = AccessPath::HASH_JOIN;
      rapid_path->hash_join().outer = hash_join.outer;
      rapid_path->hash_join().inner = hash_join.inner;
      rapid_path->iterator = nullptr;

      return rapid_path;
    } break;

    // Composite access paths.
    case AccessPath::FILTER: {
    } break;
    case AccessPath::SORT: {
    } break;
    case AccessPath::AGGREGATE:
    case AccessPath::TEMPTABLE_AGGREGATE: {
      // due to OPTION_NO_CONST_TABLES is set, then in first round optimization, it don't do aggregation optimzation.
      // therefore, all sub queries offload to secondary engine, then do optimization in secondary engine.
      aggregate_evaluated outcome;
      if (join->tables_list && join->implicit_grouping &&
          optimize_aggregated_query(join->thd, join->query_block, *join->fields, join->where_cond, &outcome)) {
        DBUG_PRINT("error", ("Error from optimize_aggregated_query"));
        return nullptr;
      }
      if (outcome == AGGR_DELAYED) {
        return NewUnqualifiedCountAccessPath(join->thd);
      }

      // Check both data sufficiency AND child path vectorization support
      if (path->type == AccessPath::AGGREGATE && path->num_output_rows() == kUnknownRowCount) {
        EstimateAggregateCost(join->thd, path, join->query_block);
      }
      auto n_records = path->num_output_rows_before_filter;
      bool data_sufficient = ((size_t)n_records >= SHANNON_VECTOR_WIDTH);

      bool child_support =
          CheckChildVectorization((path->type == AccessPath::AGGREGATE) ? path->aggregate().child : nullptr);
      context->can_vectorized = data_sufficient && child_support;
      if (path->vectorized == context->can_vectorized) return nullptr;

      auto rapid_path = new (current_thd->mem_root) AccessPath();
      if (path->type == AccessPath::AGGREGATE) {
        rapid_path->vectorized = context->can_vectorized;
        rapid_path->type = AccessPath::AGGREGATE;
        rapid_path->aggregate().child = path->aggregate().child;
        rapid_path->aggregate().olap = path->aggregate().olap;
        rapid_path->has_group_skip_scan = path->has_group_skip_scan;
        rapid_path->set_num_output_rows(path->num_output_rows());
        rapid_path->iterator = path->iterator;
      } else if (path->type == AccessPath::TEMPTABLE_AGGREGATE) {
        rapid_path->vectorized = context->can_vectorized;
        rapid_path->type = AccessPath::TEMPTABLE_AGGREGATE;
        rapid_path->temptable_aggregate().subquery_path = path->temptable_aggregate().subquery_path;
        rapid_path->temptable_aggregate().temp_table_param = path->temptable_aggregate().temp_table_param;
        rapid_path->temptable_aggregate().table = path->temptable_aggregate().table;
        rapid_path->temptable_aggregate().table_path = path->temptable_aggregate().table_path;
        rapid_path->temptable_aggregate().ref_slice = path->temptable_aggregate().ref_slice;
      } else
        assert(false);
      return rapid_path;
    } break;
    case AccessPath::LIMIT_OFFSET: {
    } break;
    case AccessPath::STREAM: {
    } break;
    case AccessPath::MATERIALIZE: {
    } break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
    } break;
    case AccessPath::APPEND: {
    } break;
    case AccessPath::WINDOW: {
    } break;
    case AccessPath::WEEDOUT: {
    } break;
    case AccessPath::REMOVE_DUPLICATES: {
    } break;
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
    } break;
    case AccessPath::ALTERNATIVE: {
    } break;
    case AccessPath::CACHE_INVALIDATOR: {
    } break;

    // Access paths that modify tables.
    case AccessPath::DELETE_ROWS: {
    } break;
    case AccessPath::UPDATE_ROWS: {
    } break;
    default:
      break;
  }

  return nullptr;
}
}  // namespace Optimizer
}  // namespace ShannonBase