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

#include "include/my_dbug.h"  //DBUG_PRINT
#include "sql/field.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_iterator.h"  //HashJoinIterator
#include "sql/iterators/timing_iterator.h"

#include "sql/join_optimizer/cost_model.h"
//#include "sql/range_optimizer/range_optimizer.h"  //KEY_PART,QUICK_RANGE
#include "sql/range_optimizer/tree.h"  //SEL_ARG
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

void ProjectionExtractor::Extract(Item *item, table_map state_map, std::vector<Item *> &proj_items,
                                  bool include_constants) {
  if (!item) return;
  table_map used = item->used_tables();

  // case 1：const var
  if (used == 0) {
    if (include_constants) proj_items.push_back(item);
    return;
  }

  // case 2：all in state_map
  if ((used & ~state_map) == 0) {
    proj_items.push_back(item);
    return;
  }

  // case 3：complex expr
  if (item->type() == Item::FUNC_ITEM || item->type() == Item::COND_ITEM) {
    Item_func *func_item = static_cast<Item_func *>(item);
    for (uint i = 0; i < func_item->argument_count(); ++i) {
      Extract(func_item->arguments()[i], state_map, proj_items, include_constants);
    }
  }
}

void ProjectionExtractor::ExtractRequired(Item *condition, table_map state_map, std::vector<Item *> &required) {
  if (!condition) return;
  WalkItem(condition, enum_walk::POSTFIX, [&](Item *item) -> bool {
    if (item->type() == Item::FIELD_ITEM) {
      auto *f = static_cast<Item_field *>(item);
      if (f->table_ref && (f->table_ref->map() & state_map)) required.push_back(item);
    }
    return false;
  });
}

void Optimizer::AddDefaultRules() {
  // becareful the order of rules. The rules be applied in the order of added.
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
  // Final reordering with all optimizations, maybe we should not using the rules to change the optimized order
  // caution: we should use this rules with cares.
  m_optimize_rules.emplace_back(std::make_unique<JoinReOrder>());
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
    DBUG_PRINT("optimizer", ("Rule %s took %s", rule->name().c_str(), rule_timer.lap_formatted().c_str()));
  }
  return std::move(plan.root);
}

Plan Optimizer::get_query_plan(OptimizeContext *context, THD *thd, const JOIN *join) {
  ut_a(context && thd);
  if (!join || !join->query_expression()->root_access_path()) return std::make_unique<ZeroRows>();

  TranslateState root_state;
  if (translate_access_path(&root_state, thd, join->query_expression()->root_access_path(), join)) return nullptr;

  if (!root_state.plan_node) return std::make_unique<ZeroRows>();
  return std::move(root_state.plan_node);
}

bool Optimizer::translate_access_path(TranslateState *state, THD *thd, AccessPath *path, const JOIN *join) {
  if (!path) return true;

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
        if (irs.ranges != nullptr && irs.num_ranges > 0) {
          std::vector<std::unique_ptr<Imcs::Predicate>> all_predicates;
          for (unsigned i = 0; i < irs.num_ranges; ++i) {
            QUICK_RANGE *qr = irs.ranges[i];
            if (!qr) continue;
            auto range_pred = Optimizer::convert_range_to_predicate(qr, table, irs.index);
            if (range_pred) all_predicates.push_back(std::move(range_pred));
          }

          if (!all_predicates.empty())
            scan->prune_predicate = (all_predicates.size() == 1)
                                        ? std::move(all_predicates[0])
                                        : Imcs::Predicate_Builder::create_or(std::move(all_predicates));
        }
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
      scan->cost = path->cost();
      scan->estimated_rows = static_cast<ha_rows>(path->num_output_rows());
      scan->source_table = table;

      state->state_map = table->pos_in_table_list->map();
      // ProjectionPruning will prune.
      for (Field **field_ptr = table->field; *field_ptr; ++field_ptr) {
        Field *field = *field_ptr;
        Item_field *item = new (thd->mem_root) Item_field(field);
        if (item) {
          state->projection_items.push_back(item);
        }
      }

      auto *rapid_ctx = down_cast<Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());
      if (rapid_ctx) {
        auto *cost_info = rapid_ctx->GetTableCost(table);
        if (cost_info && cost_info->can_use_si) {
          scan->use_storage_index = true;
        }
      }
      state->plan_node = std::move(scan);
      return false;
    } break;
    case AccessPath::HASH_JOIN: {
      auto &hj = path->hash_join();

      TranslateState outer_state, inner_state;
      if (translate_access_path(&outer_state, thd, hj.outer, join)) return true;
      if (translate_access_path(&inner_state, thd, hj.inner, join)) return true;

      auto node = std::make_unique<HashJoin>();
      node->original_path = path;
      // 1: extra join condition.
      if (hj.join_predicate) {
        extract_join_conditions(hj.join_predicate->expr, node->join_conditions);
      }

      // 2: extra post-join filter（extra predicates）
      std::vector<Item *> post_join_filters;
      if (hj.join_predicate) {
        extract_post_join_filters(hj.join_predicate, Utils::get_tablescovered(path), post_join_filters);
      }

      if (ShannonBase::Optimizer::Utils::is_outerjoin(hj.join_predicate) &&
          ShannonBase::Optimizer::Utils::is_zerorows(hj.inner)) {
        return hanle_outerjoin_zerorows(state, thd, path, join, inner_state);
      }

      node->children.push_back(std::move(outer_state.plan_node));
      node->children.push_back(std::move(inner_state.plan_node));

      if (!post_join_filters.empty()) {
        for (auto *filter_item : post_join_filters) {
          ProjectionExtractor::Extract(filter_item, Utils::get_tablescovered(path), state->projection_items);
        }

        auto filter = std::make_unique<Filter>();
        filter->condition = ShannonBase::Optimizer::Utils::combine_with_and(post_join_filters);
        filter->children.push_back(std::move(node));
        state->plan_node = std::move(filter);
      } else {
        state->plan_node = std::move(node);
      }

      state->state_map = Utils::get_tablescovered(path);
      return false;
    } break;
    case AccessPath::LIMIT_OFFSET: {
      auto &limit_ap = path->limit_offset();

      TranslateState child_state;
      if (translate_access_path(&child_state, thd, limit_ap.child, join)) {
        return true;
      }

      double child_rows = child_state.plan_node->estimated_rows;

      auto node = std::make_unique<Limit>();
      node->original_path = path;
      node->limit = limit_ap.limit;
      node->offset = limit_ap.offset;
      node->count_all_rows = limit_ap.count_all_rows;
      node->reject_multiple_rows = limit_ap.reject_multiple_rows;
      node->children.push_back(std::move(child_state.plan_node));
      node->cost = path->cost();

      if (limit_ap.offset > 0) {
        child_rows = std::max(0.0, child_rows - limit_ap.offset);
      }
      if (limit_ap.limit != HA_POS_ERROR) {
        child_rows = std::min(child_rows, static_cast<double>(limit_ap.limit));
      }
      node->estimated_rows = static_cast<ha_rows>(child_rows);

      state->projection_items = child_state.projection_items;
      state->plan_node = std::move(node);
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::FILTER: {
      auto &f = path->filter();
      if (ShannonBase::Optimizer::Utils::contains_correlated_subquery(f.condition)) {
        // if filter contains subquery cannot offload
        auto native = std::make_unique<MySQLNative>();
        native->original_path = path;
        state->plan_node = std::move(native);
        return false;
      }
      TranslateState child_state;
      if (translate_access_path(&child_state, thd, f.child, join)) return true;

      std::vector<Item *> required;
      ProjectionExtractor::ExtractRequired(f.condition, child_state.state_map, required);
      state->required_items.insert(state->required_items.end(), required.begin(), required.end());

      auto node = std::make_unique<Filter>();
      node->condition = f.condition;
      node->original_path = path;
      node->children.push_back(std::move(child_state.plan_node));

      state->plan_node = std::move(node);
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::AGGREGATE: {
      auto &agg_ap = path->aggregate();
      TranslateState child_state;
      if (translate_access_path(&child_state, thd, agg_ap.child, join)) return true;

      bool is_rollup = (agg_ap.olap == ROLLUP_TYPE);
      bool has_grouping = join && join->grouped;

      auto node = std::make_unique<LocalAgg>();
      node->original_path = path;
      node->olap = agg_ap.olap;
      node->is_global = !(has_grouping || is_rollup);

      if (join && !join->group_list.empty()) {
        for (ORDER *group = join->group_list.order; group; group = group->next) {
          if (!group->item || !*group->item) continue;
          Item *item = *group->item;

          Item *unwrapped = item;
          if (is_rollup) {
            if (auto *rgi = dynamic_cast<Item_rollup_group_item *>(item)) unwrapped = rgi->inner_item();
          }

          node->group_by.push_back(item);
          ProjectionExtractor::ExtractRequired(unwrapped, child_state.state_map, child_state.projection_items);
          ProjectionExtractor::Extract(unwrapped, child_state.state_map, state->projection_items,
                                       /*include_constants=*/true);
        }
      }

      if (join && join->sum_funcs) {
        for (Item_sum **func_ptr = join->sum_funcs; *func_ptr; ++func_ptr) {
          Item_sum *sum_func = *func_ptr;
          if (!sum_func) continue;

          node->aggregates.push_back(sum_func);
          state->projection_items.push_back(sum_func);

          for (uint i = 0; i < sum_func->argument_count(); ++i) {
            Item *arg = sum_func->get_arg(i);
            if (!arg || arg->const_item()) continue;
            ProjectionExtractor::Extract(arg, child_state.state_map, child_state.projection_items,
                                         /*include_constants=*/false);
          }
        }
      }

      // is_global == true: return 1 rows , is_global == false: by MySQL optimizer
      node->estimated_rows = node->is_global ? 1 : static_cast<ha_rows>(path->num_output_rows());
      node->children.push_back(std::move(child_state.plan_node));
      node->cost = path->cost();

      // HAVING（greedy optimization
      if (!thd->lex->using_hypergraph_optimizer() && join && join->having_cond) {
        auto having_filter = std::make_unique<Filter>();
        having_filter->condition = join->having_cond;
        having_filter->cost = path->cost();
        having_filter->estimated_rows = static_cast<ha_rows>(path->num_output_rows());
        having_filter->children.push_back(std::move(node));
        state->plan_node = std::move(having_filter);
      } else {
        state->plan_node = std::move(node);
      }

      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::SORT: {
      auto &sort_ap = path->sort();

      TranslateState child_state;
      if (translate_access_path(&child_state, thd, sort_ap.child, join)) {
        return true;
      }
      ha_rows limit = sort_ap.limit;
      bool is_topn = (limit != HA_POS_ERROR);
      if (is_topn) {
        auto node = std::make_unique<TopN>();
        node->original_path = path;
        node->filesort = sort_ap.filesort;
        node->order = sort_ap.order;
        node->limit = limit;
        if (sort_ap.order) {
          for (ORDER *ord = sort_ap.order; ord; ord = ord->next) {
            Item *item = *ord->item;
            ProjectionExtractor::Extract(item, child_state.state_map, state->projection_items);
          }
        }

        node->children.push_back(std::move(child_state.plan_node));
        node->cost = path->cost();
        node->estimated_rows = std::min(static_cast<ha_rows>(path->num_output_rows()), limit);

        state->plan_node = std::move(node);
      } else {
        auto node = std::make_unique<Sort>();
        node->original_path = path;
        node->filesort = sort_ap.filesort;
        node->order = sort_ap.order;
        node->limit = HA_POS_ERROR;
        node->remove_duplicates = sort_ap.remove_duplicates;
        node->unwrap_rollup = sort_ap.unwrap_rollup;
        node->force_sort_rowids = sort_ap.force_sort_rowids;
        node->tables_to_get_rowid_for = sort_ap.tables_to_get_rowid_for;
        if (sort_ap.order) {
          for (ORDER *ord = sort_ap.order; ord; ord = ord->next) {
            Item *item = *ord->item;
            ProjectionExtractor::Extract(item, child_state.state_map, state->projection_items);
          }
        }

        node->children.push_back(std::move(child_state.plan_node));
        node->cost = path->cost();
        node->estimated_rows = static_cast<ha_rows>(path->num_output_rows());
        state->plan_node = std::move(node);
      }
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::EQ_REF: {
      return false;  // not reach forever.
    } break;
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::FAKE_SINGLE_ROW: {
      auto node = std::make_unique<ZeroRows>();
      node->original_path = path;
      node->rows_returned = (path->type == AccessPath::FAKE_SINGLE_ROW) ? 1 : 0;

      state->filter.zero_row_state_map = Utils::get_tablescovered(path);
      state->plan_node = std::move(node);
      state->state_map = Utils::get_tablescovered(path);
    } break;

    case AccessPath::MATERIALIZE: {
      // Materialize can be：
      // 1. Derived table / CTE
      // 2. Subquery materialization
      // 3. Window function materialization
      auto &mat = path->materialize();
      bool is_cte = (mat.param->cte != nullptr);
      bool is_derived = (!is_cte && mat.param->unit != nullptr);
      if (is_cte) {
        Common_table_expr *cte = mat.param->cte;
        TABLE *tmp_table = mat.param->table;
        if (!tmp_table) {
          make_native_plan(state, path);
          return false;
        }
        for (Field **field_ptr = tmp_table->field; *field_ptr; ++field_ptr) {
          Field *field = *field_ptr;
          if (field->is_hidden_by_system() || field->is_hidden_by_user()) continue;
        }

        std::vector<std::unique_ptr<PlanNode>> inner_plans;
        for (size_t i = 0; i < mat.param->m_operands.size(); ++i) {
          const auto &operand = mat.param->m_operands[i];
          if (!operand.subquery_path) continue;

          TranslateState operand_state;
          if (translate_access_path(&operand_state, thd, operand.subquery_path, operand.join)) continue;
          inner_plans.push_back(std::move(operand_state.plan_node));
        }

        auto cte_node = std::make_unique<MaterializeCTE>();
        cte_node->original_path = path;
        cte_node->cte_name = cte ? std::string(cte->name.str, cte->name.length) : "unnamed_cte";
        cte_node->tmp_table = tmp_table;
        for (auto &plan : inner_plans) {
          cte_node->inner_plans.push_back(std::move(plan));
        }

        cte_node->cost = path->cost();
        cte_node->estimated_rows = static_cast<ha_rows>(path->num_output_rows());
        cte_node->is_recursive = (cte && cte->recursive);

        if (mat.param->limit_rows != HA_POS_ERROR) cte_node->limit = mat.param->limit_rows;

        state->plan_node = std::move(cte_node);
        state->state_map = Utils::get_tablescovered(path);

        for (Field **field_ptr = tmp_table->field; *field_ptr; ++field_ptr) {
          Field *field = *field_ptr;
          if (field->is_hidden_by_system() || field->is_hidden_by_user()) continue;
          Item_field *item = new (thd->mem_root) Item_field(field);
          if (item) state->projection_items.push_back(item);
        }
        return false;
      } else if (is_derived) {
        Query_expression *unit = mat.param->unit;
        TABLE *tmp_table = mat.param->table;
        if (!unit || !tmp_table) {
          make_native_plan(state, path);
          return false;
        }

        std::vector<std::unique_ptr<PlanNode>> inner_plans;
        for (size_t i = 0; i < mat.param->m_operands.size(); ++i) {
          const auto &operand = mat.param->m_operands[i];
          if (!operand.subquery_path) continue;

          TranslateState operand_state;
          if (translate_access_path(&operand_state, thd, operand.subquery_path, operand.join)) continue;
          inner_plans.push_back(std::move(operand_state.plan_node));
        }

        auto derived_node = std::make_unique<MaterializeDerived>();
        derived_node->original_path = path;
        derived_node->tmp_table = tmp_table;

        for (auto &plan : inner_plans) {
          derived_node->inner_plans.push_back(std::move(plan));
        }

        derived_node->cost = path->cost();
        derived_node->estimated_rows = static_cast<ha_rows>(path->num_output_rows());

        // UNION
        derived_node->has_union = (mat.param->m_operands.size() > 1);
        derived_node->is_union_distinct =
            (mat.param->m_operands.size() > 0 && !mat.param->m_operands[0].disable_deduplication_by_hash_field);

        state->plan_node = std::move(derived_node);
        state->state_map = Utils::get_tablescovered(path);

        // proj set.
        for (Field **field_ptr = tmp_table->field; *field_ptr; ++field_ptr) {
          Field *field = *field_ptr;
          if (field->is_hidden_by_system() || field->is_hidden_by_user()) continue;

          Item_field *item = new (thd->mem_root) Item_field(field);
          if (item) state->projection_items.push_back(item);
        }
        return false;
      } else {
        auto native = std::make_unique<MySQLNative>();
        native->original_path = path;
        native->cost = path->cost();
        native->estimated_rows = static_cast<ha_rows>(path->num_output_rows());

        state->plan_node = std::move(native);
        state->state_map = Utils::get_tablescovered(path);
        return false;
      }
      return false;
    } break;
    default: {
      // if Rapid can not handle, then re-encapsulate to a Fallback node
      auto original = std::make_unique<MySQLNative>();
      original->original_path = path;
      original->estimated_rows = path->num_output_rows();
      state->plan_node = std::move(original);
      // no need to transalte anymore, because it's a MySQL AccessPath.
      return false;
    }
  }

  return false;
}

void Optimizer::extract_join_conditions(const RelationalExpression *expr, std::vector<Item *> &out_conditions) {
  if (!expr) return;

  switch (expr->type) {
    case RelationalExpression::TABLE:
      break;

    case RelationalExpression::INNER_JOIN:
    case RelationalExpression::LEFT_JOIN:
    case RelationalExpression::SEMIJOIN:
    case RelationalExpression::ANTIJOIN:
    case RelationalExpression::MULTI_INNER_JOIN: {
      if (expr->type != RelationalExpression::MULTI_INNER_JOIN) {
        extract_join_conditions(expr->left, out_conditions);
        extract_join_conditions(expr->right, out_conditions);
      } else {
        for (const RelationalExpression *child : expr->multi_children) {
          extract_join_conditions(child, out_conditions);
        }
      }

      for (Item *item : expr->join_conditions) {
        if (item && !item->has_subquery()) {
          out_conditions.push_back(item);
        }
      }
    } break;

    default:
      break;
  }
}

void Optimizer::extract_post_join_filters(const JoinPredicate *pred, table_map covered_tables,
                                          std::vector<Item *> &out_filters) {
  if (!pred || !pred->expr) return;
  walk_relational_expression(pred->expr, [&](const RelationalExpression *expr) {
    for (Item *item : expr->join_conditions) {
      if (!item) continue;

      table_map used = item->used_tables();
      if ((used & covered_tables) == used) {
        if (!ShannonBase::Optimizer::Utils::is_simple_equijoin(item)) out_filters.push_back(item);
      }
    }
    return false;
  });
}

void Optimizer::walk_relational_expression(const RelationalExpression *expr,
                                           std::function<bool(const RelationalExpression *)> func) {
  if (!expr) return;
  if (func(expr)) return;

  switch (expr->type) {
    case RelationalExpression::TABLE:
      break;
    case RelationalExpression::INNER_JOIN:
    case RelationalExpression::LEFT_JOIN:
    case RelationalExpression::SEMIJOIN:
    case RelationalExpression::ANTIJOIN:
      walk_relational_expression(expr->left, func);
      walk_relational_expression(expr->right, func);
      break;
    case RelationalExpression::MULTI_INNER_JOIN:
      for (const RelationalExpression *child : expr->multi_children) {
        walk_relational_expression(child, func);
      }
      break;
    default:
      break;
  }
}

bool Optimizer::hanle_outerjoin_zerorows(TranslateState *parent_state, THD *thd, AccessPath *path, const JOIN *join,
                                         TranslateState &inner_state) {
  // When the inner side of an Outer Join produces ZERO_ROWS:
  // Special handling required to preserve the outer side rows
  auto &hj = path->hash_join();

  auto filter = std::make_unique<Filter>();
  filter->condition = new (thd->mem_root) Item_func_false();

  parent_state->filter.zero_row_state_map = inner_state.filter.zero_row_state_map;

  TranslateState outer_state;
  if (translate_access_path(&outer_state, thd, hj.outer, join)) return true;

  // Build Join (structure preserved even though inner side is empty)
  auto node = std::make_unique<HashJoin>();
  node->original_path = path;
  node->children.push_back(std::move(outer_state.plan_node));
  node->children.push_back(std::move(inner_state.plan_node));

  filter->children.push_back(std::move(node));
  parent_state->plan_node = std::move(filter);
  parent_state->state_map = Utils::get_tablescovered(path);
  return false;
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

std::unique_ptr<Imcs::Predicate> Optimizer::convert_item_to_predicate(THD *thd, Index_lookup *lookup, TABLE *table) {
  if (!lookup || !lookup->key_err || lookup->key == -1 || lookup->key_parts == 0) return nullptr;

  // Check for impossible NULL references
  if (lookup->impossible_null_ref()) return nullptr;  // This will never match
  KEY *key_info = nullptr;
  if (table && lookup->key >= 0 && lookup->key < (int)table->s->keys) {
    key_info = &table->key_info[lookup->key];
  }

  // If there's only one key part, create a simple equality predicate
  if (lookup->key_parts == 1) {
    Item *item = lookup->items[0];
    if (!item) return nullptr;
    assert(item->const_item());

    // Check if this key part has a guard condition
    if (lookup->cond_guards && lookup->cond_guards[0] && !(*lookup->cond_guards[0])) return nullptr;

    // Get target field information from the key
    Field *target_field = nullptr;
    enum_field_types field_type = MYSQL_TYPE_NULL;
    uint32 col_idx = 0;
    if (!key_info || !key_info->key_part) return nullptr;
    target_field = key_info->key_part[0].field;
    if (!target_field) return nullptr;
    field_type = target_field->type();
    col_idx = target_field->field_index();
    // Extract the value with target field type for proper conversion
    // This handles type conversion: string -> datetime, int -> datetime, string -> int, etc.
    Imcs::PredicateValue value = extract_value_from_item(thd, item, field_type, target_field);

    // Check if this is a NULL-rejecting equality
    bool is_null_rejecting = (lookup->null_rejecting & 1);
    if (is_null_rejecting && value.is_null()) return nullptr;
    return std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
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

    // Get target field information from the key
    Field *target_field = nullptr;
    enum_field_types field_type = MYSQL_TYPE_NULL;
    uint32 col_idx = 0;
    if (key_info && i < key_info->user_defined_key_parts) {
      target_field = key_info->key_part[i].field;
      if (!target_field) continue;
      field_type = target_field->type();
      col_idx = target_field->field_index();
    } else {
      // No key info - cannot determine target type, skip this part
      continue;
    }

    // Extract value with proper type conversion
    // item is the lookup value, target_field provides the type information
    Imcs::PredicateValue value = extract_value_from_item(thd, item, field_type, target_field);

    // Check for NULL-rejecting
    bool is_null_rejecting = (lookup->null_rejecting & (1 << i));
    if (is_null_rejecting && value.is_null()) return nullptr;  // This key part rejects NULL

    // Create equality predicate for this key part
    auto pred = std::make_unique<Imcs::Simple_Predicate>(col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
    compound->add_child(std::move(pred));
    has_predicates = true;
  }

  if (!has_predicates) return nullptr;
  // If only one predicate was created, return it directly
  if (compound->children.size() == 1) {
    return std::move(compound->children[0]);  // Transfer ownership of the single child
  }
  return compound;
}

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
    } break;
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
  Imcs::PredicateValue value = extract_value_from_item(thd, value_item, field_type, field);

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
  Imcs::PredicateValue min_val = extract_value_from_item(thd, min_arg, field_type, field);
  Imcs::PredicateValue max_val = extract_value_from_item(thd, max_arg, field_type, field);

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
    Imcs::PredicateValue val = extract_value_from_item(thd, value_item, field_type, field);
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
  Imcs::PredicateValue pattern = extract_value_from_item(thd, pattern_arg, field_type, field);
  Imcs::PredicateOperator op = is_negated ? Imcs::PredicateOperator::NOT_LIKE : Imcs::PredicateOperator::LIKE;
  return std::make_unique<Imcs::Simple_Predicate>(col_idx, op, pattern, field_type);
}

std::unique_ptr<Imcs::Predicate> Optimizer::convert_range_to_predicate(QUICK_RANGE *qr, TABLE *table, int index_no) {
  KEY *key_info = &table->key_info[index_no];
  std::vector<std::unique_ptr<Imcs::Predicate>> predicates;

  const uchar *min_ptr = qr->min_key;
  const uchar *max_ptr = qr->max_key;
  uint current_offset = 0;

  for (unsigned part_idx = 0; part_idx < key_info->user_defined_key_parts; ++part_idx) {
    KEY_PART_INFO *key_part = &key_info->key_part[part_idx];
    Field *field = key_part->field;
    uint store_length = key_part->store_length;

    bool has_min = (!(qr->flag & NO_MIN_RANGE) && (current_offset < qr->min_length));
    bool has_max = (!(qr->flag & NO_MAX_RANGE) && (current_offset < qr->max_length));
    if (!has_min && !has_max) break;

    bool is_eq = (has_min && has_max && ((qr->flag & EQ_RANGE) || memcmp(min_ptr, max_ptr, store_length) == 0));
    if (is_eq) {
      const uchar *ptr = min_ptr;
      if (field->is_nullable()) {
        if (*ptr) goto next_part;
        ptr++;
      }
      Imcs::PredicateValue val;
      if (decode_key_value(ptr, field, val)) {
        predicates.push_back(Imcs::Predicate_Builder::create_simple(
            field->field_index(), Imcs::PredicateOperator::EQUAL, val, field->type()));
      }
    } else {
      if (has_min) {
        const uchar *ptr = min_ptr;
        if (field->is_nullable()) {
          if (*ptr) goto skip_min;
          ptr++;
        }
        Imcs::PredicateValue min_val;
        if (decode_key_value(ptr, field, min_val)) {
          // NEAR_MIN is only valid if this column is the "first range column"
          Imcs::PredicateOperator op =
              (qr->flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL;
          predicates.push_back(
              Imcs::Predicate_Builder::create_simple(field->field_index(), op, min_val, field->type()));
        }
      }
    skip_min:
      if (has_max) {
        const uchar *ptr = max_ptr;
        if (field->is_nullable()) {
          if (*ptr) goto skip_max;
          ptr++;
        }
        Imcs::PredicateValue max_val;
        if (decode_key_value(ptr, field, max_val)) {
          Imcs::PredicateOperator op =
              (qr->flag & NEAR_MAX) ? Imcs::PredicateOperator::LESS_THAN : Imcs::PredicateOperator::LESS_EQUAL;
          predicates.push_back(
              Imcs::Predicate_Builder::create_simple(field->field_index(), op, max_val, field->type()));
        }
      }
    skip_max:
      break;
    }
  next_part:
    min_ptr += store_length;
    max_ptr += store_length;
    current_offset += store_length;
  }
  if (predicates.empty()) return nullptr;
  return (predicates.size() == 1) ? std::move(predicates[0])
                                  : Imcs::Predicate_Builder::create_and(std::move(predicates));
}

bool Optimizer::decode_key_value(const uchar *key_ptr, Field *field, Imcs::PredicateValue &out_value) {
  auto field_type = field->type();

  if (field_type == MYSQL_TYPE_NEWDECIMAL) {
    auto new_field = down_cast<Field_new_decimal *>(field);
    my_decimal dec_val;
    if (binary2my_decimal(E_DEC_FATAL_ERROR & ~E_DEC_OVERFLOW, key_ptr, &dec_val, new_field->precision,
                          new_field->decimals(), true) != E_DEC_OK) {
      return false;
    }

    double d_val;
    if (decimal2double(&dec_val, &d_val) != E_DEC_OK) return false;
    out_value = Imcs::PredicateValue(d_val);
    return true;
  }

  uchar *old_ptr = field->field_ptr();
  field->set_field_ptr(const_cast<uchar *>(key_ptr));

  if (is_integer_type(field_type) || is_temporal_type(field_type)) {
    out_value = Imcs::PredicateValue(static_cast<int64_t>(field->val_int()));
  } else if (is_numeric_type(field_type)) {
    out_value = Imcs::PredicateValue(static_cast<double>(field->val_real()));
  } else if (is_string_type(field_type)) {
    String str_val;
    String *str = field->val_str(&str_val);
    if (str) out_value = Imcs::PredicateValue(std::string(str->ptr(), str->length()));
  }

  field->set_field_ptr(old_ptr);
  return true;
}

/**
 * Extract value from Item
 */
Imcs::PredicateValue Optimizer::extract_value_from_item(THD *thd, Item *item, enum_field_types target_type,
                                                        Field *target_field) {
  if (!item->const_item()) assert(false);
  if (!item || target_type == MYSQL_TYPE_NULL) return Imcs::PredicateValue::null_value();

  if (target_type != MYSQL_TYPE_NULL && target_field) {
    Item_result item_result_type = item->result_type();
    Item_result target_result_type;
    switch (target_type) {
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        target_result_type = REAL_RESULT;
        break;
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VAR_STRING:
        target_result_type = STRING_RESULT;
        break;
      default:
        target_result_type = INT_RESULT;
    }

    if (item_result_type !=
        target_result_type) {  // convert item_type to target_field type. such as datetime op '2022-12-12'
      type_conversion_status store_result = TYPE_OK;
      switch (item_result_type) {
        case INT_RESULT: {
          longlong int_val = item->val_int();
          bool is_unsigned = item->unsigned_flag;
          ShannonBase::Utils::ColumnMapGuard cg(target_field->table, ShannonBase::Utils::ColumnMapGuard::TYPE::WRITE);
          store_result = target_field->store(int_val, is_unsigned);
        } break;
        case REAL_RESULT: {
          double real_val = item->val_real();
          ShannonBase::Utils::ColumnMapGuard cg(target_field->table, ShannonBase::Utils::ColumnMapGuard::TYPE::WRITE);
          store_result = target_field->store(real_val);
        } break;
        case STRING_RESULT: {
          String str_buf;
          String *str = item->val_str(&str_buf);
          ShannonBase::Utils::ColumnMapGuard cg(target_field->table, ShannonBase::Utils::ColumnMapGuard::TYPE::WRITE);
          if (str) store_result = target_field->store(str->ptr(), str->length(), str->charset());
        } break;
        case DECIMAL_RESULT: {
          my_decimal decimal_buf;
          my_decimal *dec = item->val_decimal(&decimal_buf);
          ShannonBase::Utils::ColumnMapGuard cg(target_field->table, ShannonBase::Utils::ColumnMapGuard::TYPE::WRITE);
          if (dec) store_result = target_field->store_decimal(dec);
        } break;
        default:
          assert(false);
          break;
      }
      if (store_result == TYPE_OK && !target_field->is_null()) {
        if (target_result_type == INT_RESULT) {
          int64 int_value = target_field->val_int();
          return Imcs::PredicateValue(int_value);
        }
        if (target_result_type == REAL_RESULT) {
          double real_value = target_field->val_real();
          return Imcs::PredicateValue(real_value);
        }
        if (target_result_type == STRING_RESULT) {
          String str_buf;
          String *str = target_field->val_str(&str_buf);
          std::string string_value = str ? std::string(str->ptr(), str->length()) : "";
          return Imcs::PredicateValue(string_value);
        }
      }
    }
  }

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
      return str ? Imcs::PredicateValue(std::string(str->ptr(), str->length())) : Imcs::PredicateValue::null_value();
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