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

#include <iomanip>
#include <sstream>

#include "include/my_dbug.h"  //DBUG_PRINT
#include "sql/field.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_iterator.h"  //HashJoinIterator
#include "sql/iterators/timing_iterator.h"

#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/walk_access_paths.h"
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

namespace {

bool HasGrouping(const JOIN *join) {
  return join != nullptr && (join->grouped || !join->group_fields.is_empty() ||
                             (join->query_block != nullptr && join->query_block->group_list.first != nullptr));
}

table_map GroupingTableMap(const JOIN *join) {
  if (join == nullptr) return 0;
  table_map tables = 0;
  ORDER *group = join->group_list.order;
  if (group == nullptr && join->query_block != nullptr) group = join->query_block->group_list.first;
  if (group != nullptr) {
    for (; group != nullptr; group = group->next) {
      if (group->item != nullptr && *group->item != nullptr)
        tables |= (*group->item)->used_tables() & ~PSEUDO_TABLE_BITS;
    }
  } else {
    for (const Cached_item &cached : join->group_fields) {
      if (cached.get_item() != nullptr) tables |= cached.get_item()->used_tables() & ~PSEUDO_TABLE_BITS;
    }
  }
  return tables;
}

void SplitConjunctions(Item *condition, std::vector<Item *> *out) {
  if (condition == nullptr || out == nullptr) return;
  if (condition->type() == Item::COND_ITEM) {
    auto *cond = down_cast<Item_cond *>(condition);
    if (cond->functype() == Item_func::COND_AND_FUNC) {
      List_iterator<Item> it(*cond->argument_list());
      Item *child;
      while ((child = it++)) SplitConjunctions(child, out);
      return;
    }
  }
  out->push_back(condition);
}

// Helper: create a Simple_Predicate and set column_name from a Field pointer.
template <typename... Args>
std::unique_ptr<Imcs::Simple_Predicate> make_predicate(const Field *field, Args &&...args) {
  auto p = std::make_unique<Imcs::Simple_Predicate>(std::forward<Args>(args)...);
  p->set_column_name_from_field(field);
  return p;
}

bool IsHashAggregateGroupField(const Field *field) {
  if (field == nullptr) return false;
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

bool IsHashAggregateValueField(const Field *field) {
  if (field == nullptr || field->is_flag_set(UNSIGNED_FLAG)) return false;
  switch (field->type()) {
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_NEWDECIMAL:
      return true;
    default:
      return false;
  }
}

bool CanUseHashAggregate(const JOIN *join) {
  if (!HasGrouping(join) || join->sum_funcs == nullptr || join->rollup_state != JOIN::RollupState::NONE) return false;

  const auto valid_group_item = [](Item *item) {
    if (item == nullptr) return false;
    item = item->real_item();
    if (item->type() != Item::FIELD_ITEM || !IsHashAggregateGroupField(down_cast<Item_field *>(item)->field))
      return false;
    return true;
  };

  ORDER *group = join->group_list.order;
  if (group == nullptr && join->query_block != nullptr) group = join->query_block->group_list.first;
  if (group != nullptr) {
    for (; group != nullptr; group = group->next) {
      if (group->item == nullptr || !valid_group_item(*group->item)) return false;
    }
  } else {
    // The legacy optimizer cleans group_list after creating its temporary
    // table path, but retains the resolved grouping expressions here.
    if (join->group_fields.is_empty()) return false;
    for (const Cached_item &cached : join->group_fields) {
      if (!valid_group_item(cached.get_item())) return false;
    }
  }

  for (Item_sum **sum = join->sum_funcs; *sum != nullptr; ++sum) {
    Item_sum *agg = *sum;
    if (agg->has_with_distinct()) return false;

    switch (agg->sum_func()) {
      case Item_sum::COUNT_FUNC:
        if (agg->arg_count == 0) continue;  // COUNT(*)
        if (agg->get_arg(0) == nullptr) return false;
        if (agg->get_arg(0)->const_item()) {
          // COUNT(NULL) has different state semantics from COUNT(non-NULL
          // constant); keep it on the native/scalar path for now.
          if (agg->get_arg(0)->is_null()) return false;
          continue;
        }
        if (agg->get_arg(0)->real_item()->type() != Item::FIELD_ITEM) return false;
        if (!IsHashAggregateGroupField(down_cast<Item_field *>(agg->get_arg(0)->real_item())->field)) return false;
        continue;
      case Item_sum::SUM_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        break;
      case Item_sum::AVG_FUNC: {
        if (agg->arg_count == 0 || agg->get_arg(0) == nullptr) return false;
        Item *argument = agg->get_arg(0)->real_item();
        if (argument->type() != Item::FIELD_ITEM) return false;
        const enum_field_types type = down_cast<Item_field *>(argument)->field->type();
        // Hash aggregate currently materializes an AVG state by feeding the
        // finalized average through the source Field once. That is lossless for
        // DOUBLE, but FLOAT narrows the finalized value before Item_sum_avg sees
        // it. Keep FLOAT on streaming/native aggregation until Rapid has a
        // direct sum+count state handoff.
        if (type != MYSQL_TYPE_DOUBLE) return false;
      } break;
      default:
        return false;
    }
    if (agg->arg_count == 0 || agg->get_arg(0) == nullptr) return false;
    Item *argument = agg->get_arg(0)->real_item();
    if (argument->type() != Item::FIELD_ITEM) return false;
    if (!IsHashAggregateValueField(down_cast<Item_field *>(argument)->field)) return false;
  }
  return true;
}

// A sort directly below a grouped AggregateIterator is only needed to make
// equal keys adjacent. Hash aggregation does not need that full-row ordering;
// when it also satisfies ORDER BY, the final (much smaller) group set is sorted
// instead. Keep the test strict: the sort must contain exactly the group keys,
// with no DISTINCT or LIMIT semantics attached.
bool IsGroupingSort(const AccessPath *path, const JOIN *join) {
  if (path == nullptr || path->type != AccessPath::SORT || !HasGrouping(join)) return false;
  const auto &sort = path->sort();
  if (sort.child == nullptr || sort.order == nullptr || sort.remove_duplicates || sort.limit != HA_POS_ERROR)
    return false;

  ORDER *group_order = join->group_list.order;
  if (group_order == nullptr && join->query_block != nullptr) group_order = join->query_block->group_list.first;
  size_t sort_item_count = 0;
  for (ORDER *order = sort.order; order != nullptr; order = order->next) {
    if (order->item == nullptr || *order->item == nullptr) return false;
    ++sort_item_count;
  }

  size_t group_item_count = 0;
  const auto appears_in_sort = [&sort, &group_item_count](Item *group_item) {
    if (group_item == nullptr) return false;
    ++group_item_count;
    bool found = false;
    for (ORDER *order = sort.order; order != nullptr; order = order->next) {
      if (order->item != nullptr && *order->item != nullptr &&
          group_item->real_item()->eq((*order->item)->real_item(), /*binary_cmp=*/true)) {
        found = true;
        break;
      }
    }
    return found;
  };

  if (group_order != nullptr) {
    for (ORDER *group = group_order; group != nullptr; group = group->next) {
      if (group->item == nullptr || !appears_in_sort(*group->item)) return false;
    }
  } else {
    if (join->group_fields.is_empty()) return false;
    for (const Cached_item &cached : join->group_fields) {
      if (!appears_in_sort(cached.get_item())) return false;
    }
  }
  return group_item_count == sort_item_count;
}

// Like IsGroupingSort(), but additionally accepts a sort key that is
// equivalent to a GROUP BY key through one of the join's equijoin conditions.
// This matters when the NLJ outer side is sorted on the join key of the other
// table (e.g. t2.grp) while GROUP BY references t1.grp and t1.grp = t2.grp:
// the join output is then ordered by the group key as well.
bool IsGroupingSortOfJoin(const AccessPath *path, const JOIN *join, const JoinPredicate *join_predicate) {
  if (path == nullptr || path->type != AccessPath::SORT || !HasGrouping(join)) return false;
  const auto &sort = path->sort();
  if (sort.child == nullptr || sort.order == nullptr || sort.remove_duplicates || sort.limit != HA_POS_ERROR)
    return false;

  const auto items_equivalent = [join_predicate](const Item *a, const Item *b) {
    if (a == nullptr || b == nullptr) return false;
    const Item *ra = a->real_item();
    const Item *rb = b->real_item();
    if (ra->eq(rb, /*binary_cmp=*/true)) return true;
    if (join_predicate == nullptr || join_predicate->expr == nullptr) return false;
    for (Item_eq_base *cond : join_predicate->expr->equijoin_conditions) {
      if (cond == nullptr) continue;
      const Item *left = cond->get_arg(0)->real_item();
      const Item *right = cond->get_arg(1)->real_item();
      if ((ra->eq(left, /*binary_cmp=*/true) && rb->eq(right, /*binary_cmp=*/true)) ||
          (ra->eq(right, /*binary_cmp=*/true) && rb->eq(left, /*binary_cmp=*/true))) {
        return true;
      }
    }
    return false;
  };

  size_t sort_item_count = 0;
  for (ORDER *order = sort.order; order != nullptr; order = order->next) {
    if (order->item == nullptr || *order->item == nullptr) return false;
    ++sort_item_count;
  }

  size_t group_item_count = 0;
  ORDER *group_order = join->group_list.order;
  if (group_order == nullptr && join->query_block != nullptr) group_order = join->query_block->group_list.first;
  if (group_order != nullptr) {
    for (ORDER *group = group_order; group != nullptr; group = group->next) {
      if (group->item == nullptr) return false;
      ++group_item_count;
      bool found = false;
      for (ORDER *order = sort.order; order != nullptr && !found; order = order->next) {
        if (order->item != nullptr && *order->item != nullptr && items_equivalent(*group->item, *order->item)) {
          found = true;
        }
      }
      if (!found) return false;
    }
  } else {
    if (join->group_fields.is_empty()) return false;
    for (const Cached_item &cached : join->group_fields) {
      Item *item = cached.get_item();
      if (item == nullptr) return false;
      ++group_item_count;
      bool found = false;
      for (ORDER *order = sort.order; order != nullptr && !found; order = order->next) {
        if (order->item != nullptr && *order->item != nullptr && items_equivalent(item, *order->item)) {
          found = true;
        }
      }
      if (!found) return false;
    }
  }
  return group_item_count == sort_item_count;
}

bool AccessPathContainsJoin(const AccessPath *root) {
  bool found = false;
  WalkAccessPaths(const_cast<AccessPath *>(root), /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                  [&found](AccessPath *path, const JOIN *) {
                    if (path->type == AccessPath::HASH_JOIN || path->type == AccessPath::NESTED_LOOP_JOIN ||
                        path->type == AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL ||
                        path->type == AccessPath::BKA_JOIN) {
                      found = true;
                      return true;
                    }
                    return false;
                  });
  return found;
}

bool AccessPathHasParameterization(const AccessPath *root) {
  bool found = false;
  WalkAccessPaths(const_cast<AccessPath *>(root), /*join=*/nullptr, WalkAccessPathPolicy::ENTIRE_TREE,
                  [&found](AccessPath *path, const JOIN *) {
                    if (path->parameter_tables != 0) {
                      found = true;
                      return true;
                    }
                    return false;
                  });
  return found;
}

bool HasUnorderedHashOutput(const PlanNode *node) {
  if (node == nullptr) return false;
  if (node->type() == PlanNode::Type::HASH_JOIN) {
    return !static_cast<const HashJoin *>(node)->preserves_probe_order;
  }
  // Sort/TopN/Limit and projection-like wrappers may sit between the aggregate
  // and join in the logical plan.  They do not make the join->aggregate field
  // bindings safe, so inspect the complete subtree rather than only Filter.
  for (const auto &child : node->children) {
    if (HasUnorderedHashOutput(child.get())) return true;
  }
  return false;
}

// AccessPath::num_output_rows() uses kUnknownRowCount (-1.0) as a sentinel for "not yet
// estimated". PlanNode::estimated_rows is unsigned (ha_rows); a naive static_cast<ha_rows> of
// that negative sentinel wraps to a value near UINT64_MAX instead of clamping to 0.
ha_rows SafeRowEstimate(double rows) { return rows > 0.0 ? static_cast<ha_rows>(rows) : 0; }

JoinType ToJoinType(RelationalExpression::Type type) {
  switch (type) {
    case RelationalExpression::INNER_JOIN:
    case RelationalExpression::STRAIGHT_INNER_JOIN:
    case RelationalExpression::MULTI_INNER_JOIN:
      return JoinType::INNER;
    case RelationalExpression::LEFT_JOIN:
      return JoinType::OUTER;
    case RelationalExpression::SEMIJOIN:
      return JoinType::SEMI;
    case RelationalExpression::ANTIJOIN:
      return JoinType::ANTI;
    case RelationalExpression::FULL_OUTER_JOIN:
      return JoinType::FULL_OUTER;
    default:
      return JoinType::INNER;
  }
}

}  // namespace

Timer::Timer() { m_begin = std::chrono::steady_clock::now(); }
std::chrono::nanoseconds Timer::lap() {
  const auto now = std::chrono::steady_clock::now();
  const auto lap_duration = std::chrono::nanoseconds{now - m_begin};
  m_begin = now;
  return lap_duration;
}

std::string Timer::lap_formatted() {
  // Previously returned the empty string from an untouched stringstream, which
  // made every rule-timing DBUG_PRINT log nothing.
  const auto elapsed = lap();
  auto stream = std::stringstream{};
  stream << std::fixed << std::setprecision(3) << (static_cast<double>(elapsed.count()) / 1e6) << " ms";
  return stream.str();
}

void Optimizer::AddDefaultRules() {
  // becareful the order of rules. The rules be applied in the order of added.
  // Make predicates available
  m_optimize_rules.emplace_back(std::make_unique<PredicatePushDown>());
  // Use predicates for IMCU pruning
  m_optimize_rules.emplace_back(std::make_unique<StorageIndexPrune>());
  // After predicates clarify needed columns
  m_optimize_rules.emplace_back(std::make_unique<ProjectionPruning>());
  // Push a plain LIMIT/OFFSET into a scan only after predicate pushdown has
  // removed every Filter that the scan can evaluate itself. Sorts, joins and
  // aggregates remain barriers in the conservative implementation.
  m_optimize_rules.emplace_back(std::make_unique<TopNPushDown>());
  // Runs last: relocates a LocalAgg below its HashJoin child only where HashJoin::join_type and
  // HashJoin::child_multiplicity prove it changes nothing (see AggregationPushDown::apply).
  m_optimize_rules.emplace_back(std::make_unique<AggregationPushDown>());
  // JoinReOrder remains experimental: reconstruction does not yet preserve wrapper operators,
  // original AccessPath metadata, join types and STRAIGHT_JOIN constraints (see JoinReOrder::apply).
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

  // The legacy optimizer can replace the inner side of a transformed IN
  // semijoin with FAKE_SINGLE_ROW after it has used the primary handler to
  // optimize the subquery. That placeholder is not executable by a secondary
  // engine: it has neither the inner rows nor the projected join key. Rebuild
  // this narrow shape from the still-resolved inner query block so Rapid can
  // execute it as scan/filter -> hash semijoin.
  if (!thd->lex->using_hypergraph_optimizer()) {
    WalkAccessPaths(
        join->query_expression()->root_access_path(), join, WalkAccessPathPolicy::ENTIRE_TREE,
        [&](AccessPath *path, const JOIN *) {
          if (path->type != AccessPath::HASH_JOIN || path->hash_join().inner == nullptr ||
              path->hash_join().inner->type != AccessPath::MATERIALIZE ||
              path->hash_join().inner->materialize().param == nullptr ||
              (path->hash_join().inner->materialize().param->unit != nullptr &&
               path->hash_join().inner->materialize().param->unit->derived_table != nullptr) ||
              path->hash_join().inner->materialize().param->reject_multiple_rows)
            return false;

          MaterializePathParameters *param = path->hash_join().inner->materialize().param;
          for (MaterializePathParameters::Operand &operand : param->m_operands) {
            if (operand.subquery_path == nullptr || operand.join == nullptr || operand.join->query_block == nullptr)
              continue;

            AccessPath **fake_child = nullptr;
            if (operand.subquery_path->type == AccessPath::FAKE_SINGLE_ROW) {
              fake_child = &operand.subquery_path;
            } else if (operand.subquery_path->type == AccessPath::FILTER &&
                       operand.subquery_path->filter().child != nullptr &&
                       operand.subquery_path->filter().child->type == AccessPath::FAKE_SINGLE_ROW) {
              fake_child = &operand.subquery_path->filter().child;
            }
            if (fake_child == nullptr) continue;

            Query_block *inner_qb =
                param->unit == nullptr ? operand.join->query_block : param->unit->first_query_block();
            JOIN *inner_join = inner_qb == nullptr ? nullptr : inner_qb->join;
            if (inner_qb == nullptr || inner_join == nullptr) continue;
            TABLE *inner_table = nullptr;
            if (param->unit != nullptr) {
              Table_ref *inner_ref = inner_qb->leaf_tables;
              inner_table = inner_ref == nullptr ? nullptr : inner_ref->table;
            } else if (operand.subquery_path->type == AccessPath::FILTER &&
                       operand.subquery_path->filter().condition != nullptr) {
              WalkItem(operand.subquery_path->filter().condition, enum_walk::POSTFIX, [&inner_table](Item *item) {
                if (inner_table == nullptr && item->type() == Item::FIELD_ITEM) {
                  Field *field = down_cast<Item_field *>(item)->field;
                  if (field != nullptr) inner_table = field->table;
                }
                return inner_table != nullptr;
              });
            }
            if (inner_table == nullptr) {
              Table_ref *inner_ref = inner_qb->leaf_tables;
              inner_table = inner_ref == nullptr ? nullptr : inner_ref->table;
            }
            if (inner_table == nullptr) continue;

            // The const-table optimization that produced
            // FAKE_SINGLE_ROW no longer needs a scan and may have
            // narrowed read_set accordingly. A restored scan must
            // populate both the predicate and materialized-key
            // fields; keep this conservative because the operand's
            // copy_items list can reference hidden expressions.
            if (inner_table->read_set != nullptr) bitmap_set_all(inner_table->read_set);
            AccessPath *inner_path = NewTableScanAccessPath(thd, inner_table, /*count_examined_rows=*/true);
            // JOIN::where_cond is the optimized condition and can
            // already be folded against the const-table row that
            // was replaced above. Reuse the query block's original
            // predicate so the restored scan evaluates every row.
            Item *inner_condition = inner_qb->where_cond();
            if (inner_condition != nullptr) inner_path = NewFilterAccessPath(thd, inner_path, inner_condition);
            operand.subquery_path = inner_path;
            operand.join = inner_join;
          }
          return false;
        });
  }

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
        scan->has_required_order = path->index_scan().use_order;
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
      ut_a(table);

      // Temporary / materialized tables (e.g. LATERAL derived tables) are not
      // loaded in Rapid; fall back to MySQL native scan so that the materialize
      // → invalidate cycle works correctly for each outer row.
      if (table->s->tmp_table != NO_TMP_TABLE) {
        auto native = std::make_unique<MySQLNative>();
        native->original_path = path;
        native->estimated_rows = SafeRowEstimate(path->num_output_rows());
        state->plan_node = std::move(native);
        state->state_map = table->pos_in_table_list->map();
        return false;
      }

      auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
      if (!share) {
        make_native_plan(state, path);
        return false;
      }
      auto table_id = share->m_tableid;
      scan->rpd_table = (share->is_partitioned) ? Imcs::Imcs::instance()->get_rpd_parttable(table_id)
                                                : Imcs::Imcs::instance()->get_rpd_table(table_id);
      if (!scan->rpd_table) {
        make_native_plan(state, path);
        return false;
      }
      scan->cost = path->cost();
      scan->estimated_rows = SafeRowEstimate(path->num_output_rows());
      scan->source_table = table;

      state->state_map = table->pos_in_table_list->map();

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
    case AccessPath::NESTED_LOOP_JOIN: {
      /*
       * nested_loop_join().join_predicate is written only by the hypergraph
       * optimizer (sql/join_optimizer/join_optimizer.cc). The legacy executor
       * builds its nested loops through CreateNestedLoopAccessPath(), which
       * assigns outer/inner/join_type/pfs_batch_mode and nothing else -- and
       * AccessPath's default constructor initializes only a few bitfields,
       * leaving the union that holds join_predicate indeterminate.
       *
       * Testing that field on a legacy plan is therefore a read of an
       * uninitialized union member, not a real null check. Measured on a fresh
       * MEM_ROOT block it does come back null -- so the previous
       * `HasGrouping(join) && nlj_legacy.join_predicate` guard did take the
       * native path -- but that is allocator behaviour, not a guarantee: a
       * recycled block holding non-null bytes would instead fall through and
       * dereference join_predicate->expr below. Keep legacy nested loops native
       * without inspecting the field at all.
       */
      if (!thd->lex->using_hypergraph_optimizer()) {
        make_native_plan(state, path);
        return false;
      }
      auto &nlj = path->nested_loop_join();
      // Hash Join requires a full scan of the build (inner) side.
      // Point lookups (REF / EQ_REF) return only matching rows and must be
      // widened to an index or table scan.
      AccessPath &inner_scan_storage = *new (thd->mem_root) AccessPath();
      AccessPath *inner_child = nlj.inner;
      switch (inner_child->type) {
        case AccessPath::REF: {
          inner_scan_storage.type = AccessPath::INDEX_SCAN;
          inner_scan_storage.index_scan().table = inner_child->ref().table;
          inner_scan_storage.index_scan().idx = inner_child->ref().ref->key;
          inner_scan_storage.index_scan().use_order = inner_child->ref().use_order;
          inner_scan_storage.index_scan().reverse = inner_child->ref().reverse;
          inner_scan_storage.set_cost(inner_child->cost());
          const double full_scan_rows =
              (inner_child->ref().table != nullptr && inner_child->ref().table->file != nullptr &&
               inner_child->ref().table->file->stats.records != HA_POS_ERROR)
                  ? static_cast<double>(inner_child->ref().table->file->stats.records)
                  : inner_child->num_output_rows();
          inner_scan_storage.set_num_output_rows(full_scan_rows);
          inner_scan_storage.vectorized = true;
          inner_child = &inner_scan_storage;
        } break;
        case AccessPath::EQ_REF: {
          inner_scan_storage.type = AccessPath::INDEX_SCAN;
          inner_scan_storage.index_scan().table = inner_child->eq_ref().table;
          inner_scan_storage.index_scan().idx = inner_child->eq_ref().ref->key;
          inner_scan_storage.index_scan().use_order = false;
          inner_scan_storage.index_scan().reverse = false;
          inner_scan_storage.set_cost(inner_child->cost());
          const double full_scan_rows =
              (inner_child->eq_ref().table != nullptr && inner_child->eq_ref().table->file != nullptr &&
               inner_child->eq_ref().table->file->stats.records != HA_POS_ERROR)
                  ? static_cast<double>(inner_child->eq_ref().table->file->stats.records)
                  : inner_child->num_output_rows();
          inner_scan_storage.set_num_output_rows(full_scan_rows);
          inner_scan_storage.vectorized = true;
          inner_child = &inner_scan_storage;
        } break;
        case AccessPath::INDEX_SCAN:
          // Already an index scan — keep as-is, no conversion needed.
          break;
        default:
          break;  // Use inner child as-is (e.g. TABLE_SCAN, FILTER, etc.).
      }

      TranslateState outer_state, inner_state;
      if (translate_access_path(&outer_state, thd, nlj.outer, join)) return true;
      if (translate_access_path(&inner_state, thd, inner_child, join)) return true;

      // Either side may translate to "supported but no plan node"; a null child
      // would later crash HashJoin/NestLoopJoin::ToAccessPath. Fall back to the
      // native plan for this join island instead.
      if (outer_state.plan_node == nullptr || inner_state.plan_node == nullptr) {
        make_native_plan(state, path);
        return false;
      }

      // Correlated LATERAL subqueries must stay as nested-loop joins — a hash
      // join would build the inner side once and probe all outer rows, but the
      // inner depends on the current outer correlation value and must be
      // re-executed per outer row. Streamed lateral derived tables show up as
      // STREAM, while materialized ones carry a non-zero parameter_tables on
      // the inner subtree (e.g. the MATERIALIZE access path for the lateral
      // derived table).
      if (inner_child->type == AccessPath::STREAM || AccessPathHasParameterization(inner_child)) {
        auto nl_node = std::make_unique<NestLoopJoin>();
        nl_node->original_path = path;
        nl_node->source_join_predicate = nlj.join_predicate;
        nl_node->pfs_batch_mode = false;

        nl_node->children.push_back(std::move(outer_state.plan_node));
        nl_node->children.push_back(std::move(inner_state.plan_node));

        state->plan_node = std::move(nl_node);
        state->state_map = Utils::get_tablescovered(path);
        return false;
      }

      // If MySQL sorted the NLJ outer side for streaming GROUP BY, convert to
      // a sorted hash join. The vectorized iterator is probe-major and thus
      // retains the probe-side ordering. Keep the sorted outer side as the
      // probe so the join output stays in GROUP BY / ORDER BY order.
      auto node = std::make_unique<HashJoin>();
      node->original_path = path;

      const bool using_hypergraph = thd->lex->using_hypergraph_optimizer();
      node->preserves_probe_order = IsGroupingSortOfJoin(nlj.outer, join, nlj.join_predicate);

      /*
       * VectorizedHashJoinIterator does not implement external spill. For an
       * ordinary synthetic NLJ->HASH_JOIN conversion, keep allow_spill=true so
       * HashJoin::ToAccessPath() deliberately selects MySQL's native
       * spill-capable iterator.
       *
       * When MySQL already sorted the probe side for GROUP BY, however, probe
       * order is part of the physical contract consumed by the streaming Rapid
       * aggregate. Use the Rapid probe-major hash join for that shape and make
       * the no-spill limitation explicit instead of falling all the way back to
       * the original nested loop.
       */
      node->allow_spill = !node->preserves_probe_order;

      if (using_hypergraph) {
        if (nlj.join_predicate && nlj.join_predicate->expr)
          extract_join_conditions(nlj.join_predicate->expr, node->join_conditions);
      } else {
        if (inner_child->type == AccessPath::INDEX_SCAN && inner_child == &inner_scan_storage) {
          if (nlj.inner->type == AccessPath::REF || nlj.inner->type == AccessPath::EQ_REF) {
            TABLE *ref_table = nlj.inner->type == AccessPath::REF ? nlj.inner->ref().table : nlj.inner->eq_ref().table;
            Index_lookup *ref = nlj.inner->type == AccessPath::REF ? nlj.inner->ref().ref : nlj.inner->eq_ref().ref;
            if (ref_table && ref) {
              uint key_idx = ref->key;
              if (key_idx < ref_table->s->keys) {
                KEY *key_info = &ref_table->key_info[key_idx];
                for (uint i = 0; i < ref->key_parts; i++) {
                  Item *outer_expr = ref->items[i];
                  if (outer_expr == nullptr) continue;

                  Item *real_outer = outer_expr->real_item();
                  Field *inner_field = key_info->key_part[i].field;
                  Item_field *right = new (thd->mem_root) Item_field(inner_field);
                  auto *eq = new (thd->mem_root) Item_func_eq(real_outer, right);
                  node->join_conditions.push_back(eq);
                }
              }
            }
          }
        }
      }

      if (node->join_conditions.empty()) {
        make_native_plan(state, path);
        return false;
      }

      // Extra post-join filters.
      std::vector<Item *> post_join_filters;
      if (nlj.join_predicate) {
        extract_post_join_filters(nlj.join_predicate, Utils::get_tablescovered(path), post_join_filters);
      }

      node->children.push_back(std::move(outer_state.plan_node));
      // A legacy NLJ inner Filter may contain both the cross-table lookup
      // predicate and genuine inner-table predicates. Dropping the entire
      // Filter loses the latter. Keep local predicates on the build side and
      // only remove simple cross-table equijoins represented by the hash key.
      if (inner_state.plan_node && inner_state.plan_node->type() == PlanNode::Type::FILTER) {
        auto *filter_node = static_cast<Filter *>(inner_state.plan_node.get());
        std::vector<Item *> conjuncts;
        SplitConjunctions(filter_node->condition, &conjuncts);
        std::vector<Item *> local_predicates;
        for (Item *condition : conjuncts) {
          const table_map used = condition ? condition->used_tables() : 0;
          if ((used & ~inner_state.state_map) == 0) {
            local_predicates.push_back(condition);
          } else if (!condition || !Utils::is_simple_equijoin(condition)) {
            // A cross-table residual cannot run during hash build and is not
            // proven to be represented by the hash key. Preserve the native
            // plan rather than risk changing semantics.
            make_native_plan(state, path);
            return false;
          }
        }

        if (filter_node->children.empty()) {
          make_native_plan(state, path);
          return false;
        }
        Plan inner_child_plan = std::move(filter_node->children[0]);
        if (!local_predicates.empty()) {
          auto local_filter = std::make_unique<Filter>();
          local_filter->condition = Utils::combine_with_and(local_predicates, thd);
          local_filter->children.push_back(std::move(inner_child_plan));
          node->children.push_back(std::move(local_filter));
        } else {
          node->children.push_back(std::move(inner_child_plan));
        }
      } else {
        node->children.push_back(std::move(inner_state.plan_node));
      }

      // Hash join emits probe rows in input order. The sorted outer side stays
      // the probe (children[0]), so its ordering is preserved for a streaming
      // aggregate above us.
      if (!post_join_filters.empty()) {
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
    case AccessPath::HASH_JOIN: {
      auto &hj = path->hash_join();
      if (hj.join_predicate == nullptr || hj.join_predicate->expr == nullptr) {
        make_native_plan(state, path);
        return false;
      }

      TranslateState outer_state, inner_state;
      if (translate_access_path(&outer_state, thd, hj.outer, join)) return true;
      if (translate_access_path(&inner_state, thd, hj.inner, join)) return true;

      // A child translation may legitimately succeed without yielding a plan
      // node (unsupported shape handled by returning empty). Pushing a null
      // child would crash HashJoin::ToAccessPath; keep this join island native.
      if (outer_state.plan_node == nullptr || inner_state.plan_node == nullptr) {
        make_native_plan(state, path);
        return false;
      }

      auto node = std::make_unique<HashJoin>();
      node->original_path = path;
      const table_map grouping_tables = GroupingTableMap(join);
      const bool can_swap_join_inputs = hj.join_predicate != nullptr && hj.join_predicate->expr != nullptr &&
                                        hj.join_predicate->expr->type == RelationalExpression::INNER_JOIN;
      const bool group_side_is_inner = can_swap_join_inputs && grouping_tables != 0 &&
                                       (grouping_tables & inner_state.state_map) != 0 &&
                                       (grouping_tables & outer_state.state_map) == 0;
      node->preserves_probe_order = IsGroupingSort(group_side_is_inner ? hj.inner : hj.outer, join);
      node->allow_spill = hj.allow_spill_to_disk && !node->preserves_probe_order;
      // 1: extra join condition.
      if (hj.join_predicate) {
        extract_join_conditions(hj.join_predicate->expr, node->join_conditions);
      } else
        ut_ad(false);

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
      if (group_side_is_inner) std::swap(node->children[0], node->children[1]);

      // Groundwork for rules that must not change join result cardinality (eager aggregation
      // pushdown, join reordering): record the join's SQL type and, per child, whether it's
      // proven to match at most one row of the other side. Computed after the swap above so the
      // indices always correspond to the final children[0]/children[1].
      node->join_type = ToJoinType(hj.join_predicate->expr->type);
      node->child_multiplicity[0] = Utils::prove_at_most_one(node->children[0].get(), node->join_conditions);
      node->child_multiplicity[1] = Utils::prove_at_most_one(node->children[1].get(), node->join_conditions);

      if (!post_join_filters.empty()) {
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
      node->send_records_override = limit_ap.send_records_override;
      node->children.push_back(std::move(child_state.plan_node));
      node->cost = path->cost();

      if (limit_ap.offset > 0) {
        child_rows = std::max(0.0, child_rows - limit_ap.offset);
      }
      if (limit_ap.limit != HA_POS_ERROR) {
        child_rows = std::min(child_rows, static_cast<double>(limit_ap.limit));
      }
      node->estimated_rows = SafeRowEstimate(child_rows);

      state->plan_node = std::move(node);
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::FILTER: {
      auto &f = path->filter();
      // Legacy IN/EXISTS transformations carry materialization and predicate
      // state outside Item itself. Keep that filter as one native island; its
      // parents and join siblings can still be translated independently.
      if (!thd->lex->using_hypergraph_optimizer() &&
          (ShannonBase::Optimizer::Utils::contains_subquery(f.condition) ||
           (join != nullptr && join->query_block != nullptr && join->query_block->has_subquery_transforms()))) {
        make_native_plan(state, path);
        return false;
      }
      // Fall back to MySQL native if the filter contains a correlated subquery
      // or references tables from an outer query block (LATERAL).
      if (ShannonBase::Optimizer::Utils::contains_correlated_subquery(f.condition) ||
          (f.condition->used_tables() & OUTER_REF_TABLE_BIT)) {
        auto native = std::make_unique<MySQLNative>();
        native->original_path = path;
        native->estimated_rows = SafeRowEstimate(path->num_output_rows());
        state->plan_node = std::move(native);
        state->state_map = Utils::get_tablescovered(path);
        return false;
      }
      TranslateState child_state;
      if (translate_access_path(&child_state, thd, f.child, join)) return true;

      auto node = std::make_unique<Filter>();
      node->condition = f.condition;
      node->original_path = path;
      node->children.push_back(std::move(child_state.plan_node));

      state->plan_node = std::move(node);
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::TEMPTABLE_AGGREGATE: {
      bool has_grouping = HasGrouping(join);
      const bool legacy_optimizer = !thd->lex->using_hypergraph_optimizer();

      // Temp table aggregate wraps the real query in subquery_path. Bypass the temp table and translate the underlying
      // path directly, then wrap in a LocalAgg to perform the aggregation in Rapid.
      auto &tta = path->temptable_aggregate();
      if (!tta.subquery_path) return true;

      TranslateState child_state;
      if (translate_access_path(&child_state, thd, tta.subquery_path, join)) return true;

      // Native temporary-table bindings are an indivisible physical unit.
      // Limit fallback to this aggregate island instead of the entire query.
      if (child_state.plan_node && child_state.plan_node->type() == PlanNode::Type::MYSQL_NATIVE) {
        make_native_plan(state, path);
        return false;
      }

      const bool use_sorted_hash_join = child_state.plan_node != nullptr &&
                                        child_state.plan_node->type() == PlanNode::Type::HASH_JOIN &&
                                        !HasUnorderedHashOutput(child_state.plan_node.get());
      const bool use_hash_aggregate = CanUseHashAggregate(join) && !use_sorted_hash_join;
      if (has_grouping && !use_hash_aggregate && !use_sorted_hash_join) {
        make_native_plan(state, path);
        return false;
      }

      // Under the legacy optimizer the SELECT list is rebound to the temp
      // table's Fields, so the temp table must stay in the plan. Only the plain
      // single-input hash-aggregate shape is wired for the vectorized
      // temp-table path (VectorizedTemptableAggregateIterator fills that same
      // temp table); anything else keeps core MySQL's iterator.
      const bool legacy_temptable = has_grouping && legacy_optimizer;
      if (legacy_temptable) {
        const bool simple_child =
            child_state.plan_node != nullptr && (child_state.plan_node->type() == PlanNode::Type::SCAN ||
                                                 child_state.plan_node->type() == PlanNode::Type::FILTER);
        // AVG's argument Item is rebound to a temp-table column by the legacy
        // temp-table setup, so the vectorized finalizer (which reads args[0])
        // sees an unpopulated field. Keep any AVG group on the native iterator.
        bool has_avg = false;
        if (join->sum_funcs != nullptr) {
          for (Item_sum **s = join->sum_funcs; *s != nullptr; ++s) {
            if ((*s)->sum_func() == Item_sum::AVG_FUNC) has_avg = true;
          }
        }
        if (!use_hash_aggregate || use_sorted_hash_join || !simple_child || has_avg ||
            join->rollup_state != JOIN::RollupState::NONE || join->having_cond != nullptr) {
          make_native_plan(state, path);
          return false;
        }
      }

      auto node = std::make_unique<LocalAgg>();
      node->original_path = path;
      node->join = const_cast<JOIN *>(join);
      node->olap = UNSPECIFIED_OLAP_TYPE;
      node->is_global = !has_grouping;
      node->strategy = use_hash_aggregate ? AggregateStrategy::HASH : AggregateStrategy::STREAMING;
      node->streaming_over_sorted_hash = !use_hash_aggregate && use_sorted_hash_join;
      if (legacy_temptable) node->temptable_src = path;

      ORDER *group_order = join ? join->group_list.order : nullptr;
      if (group_order == nullptr && join && join->query_block) group_order = join->query_block->group_list.first;
      if (group_order != nullptr) {
        for (ORDER *group = group_order; group; group = group->next) {
          if (!group->item || !*group->item) continue;
          node->group_by.push_back(*group->item);
        }
      } else if (join != nullptr) {
        // Legacy TEMPTABLE_AGGREGATE has already cleaned group_list. Copy the
        // resolved cached fields before LocalAgg::ToAccessPath rebuilds it.
        for (const Cached_item &cached : join->group_fields) {
          Item *item = cached.get_item();
          if (item == nullptr) continue;
          node->group_by.push_back(item);
        }
      }

      if (join && join->sum_funcs) {
        for (Item_sum **func_ptr = join->sum_funcs; *func_ptr; ++func_ptr) {
          Item_sum *sum_func = *func_ptr;
          if (!sum_func) continue;
          node->aggregates.push_back(sum_func);
        }
      }

      node->estimated_rows = node->is_global ? 1 : SafeRowEstimate(path->num_output_rows());

      // When the child is a HashJoin and there is GROUP BY, insert a Sort
      // on the GROUP BY columns so the streaming aggregate gets ordered input.
      if (has_grouping && !use_hash_aggregate && HasUnorderedHashOutput(child_state.plan_node.get()) &&
          group_order != nullptr) {
        auto sort_node = std::make_unique<Sort>();
        sort_node->order = group_order;
        sort_node->children.push_back(std::move(child_state.plan_node));
        sort_node->estimated_rows = node->estimated_rows;
        node->children.push_back(std::move(sort_node));
      } else {
        node->children.push_back(std::move(child_state.plan_node));
      }
      node->cost = path->cost();

      if (!thd->lex->using_hypergraph_optimizer() && join && join->having_cond) {
        auto having_filter = std::make_unique<Filter>();
        having_filter->condition = join->having_cond;
        having_filter->cost = path->cost();
        having_filter->estimated_rows = SafeRowEstimate(path->num_output_rows());
        having_filter->children.push_back(std::move(node));
        state->plan_node = std::move(having_filter);
      } else {
        state->plan_node = std::move(node);
      }

      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::AGGREGATE: {
      bool has_grouping = HasGrouping(join);
      auto &agg_ap = path->aggregate();
      AccessPath *aggregate_child = agg_ap.child;
      ORDER *hash_output_order = nullptr;
      const bool has_grouping_sort = IsGroupingSort(aggregate_child, join);
      const bool grouping_sort_contains_join =
          has_grouping_sort && AccessPathContainsJoin(aggregate_child->sort().child);

      if (has_grouping && CanUseHashAggregate(join) && has_grouping_sort) {
        hash_output_order = aggregate_child->sort().order;
        aggregate_child = aggregate_child->sort().child;
      }

      TranslateState child_state;
      if (translate_access_path(&child_state, thd, aggregate_child, join)) return true;

      if (child_state.plan_node && child_state.plan_node->type() == PlanNode::Type::MYSQL_NATIVE) {
        make_native_plan(state, path);
        return false;
      }

      const bool use_sorted_hash_join = child_state.plan_node != nullptr &&
                                        child_state.plan_node->type() == PlanNode::Type::HASH_JOIN &&
                                        !HasUnorderedHashOutput(child_state.plan_node.get());
      const bool use_hash_aggregate = CanUseHashAggregate(join) && !use_sorted_hash_join;
      const bool use_grouping_sort = has_grouping_sort && grouping_sort_contains_join;
      if (has_grouping && !use_hash_aggregate && !use_sorted_hash_join && !use_grouping_sort) {
        make_native_plan(state, path);
        return false;
      }

      bool is_rollup = (agg_ap.olap == ROLLUP_TYPE);

      auto node = std::make_unique<LocalAgg>();
      node->original_path = path;
      node->join = const_cast<JOIN *>(join);
      node->olap = agg_ap.olap;
      node->is_global = !(has_grouping || is_rollup);
      node->strategy = use_hash_aggregate ? AggregateStrategy::HASH : AggregateStrategy::STREAMING;
      node->hash_output_order = use_hash_aggregate ? hash_output_order : nullptr;
      node->streaming_over_sorted_hash = !use_hash_aggregate && use_sorted_hash_join;

      ORDER *group_order = join ? join->group_list.order : nullptr;
      if (group_order == nullptr && join && join->query_block) group_order = join->query_block->group_list.first;
      if (group_order != nullptr) {
        for (ORDER *group = group_order; group; group = group->next) {
          if (!group->item || !*group->item) continue;
          // Keep the ROLLUP wrapper: LocalAgg::ToAccessPath()/PrepareAggregateFields()
          // rebuild JOIN::group_fields from these Items, and the rollup switcher
          // needs the wrapped form.
          node->group_by.push_back(*group->item);
        }
      } else if (join != nullptr) {
        // group_list can be consumed by either MySQL optimizer before Rapid
        // translates the final AccessPath. group_fields is the resolved
        // physical grouping property shared by both optimizer frontends.
        for (const Cached_item &cached : join->group_fields) {
          Item *item = cached.get_item();
          if (item == nullptr) continue;
          node->group_by.push_back(item);
        }
      }

      if (join && join->sum_funcs) {
        for (Item_sum **func_ptr = join->sum_funcs; *func_ptr; ++func_ptr) {
          Item_sum *sum_func = *func_ptr;
          if (!sum_func) continue;

          node->aggregates.push_back(sum_func);
        }
      }

      // is_global == true: return 1 rows , is_global == false: by MySQL optimizer
      node->estimated_rows = node->is_global ? 1 : SafeRowEstimate(path->num_output_rows());

      // Unsupported aggregate shapes keep the ordered streaming fallback.
      if (has_grouping && !use_hash_aggregate && HasUnorderedHashOutput(child_state.plan_node.get()) &&
          group_order != nullptr) {
        auto sort_node = std::make_unique<Sort>();
        sort_node->order = group_order;
        sort_node->children.push_back(std::move(child_state.plan_node));
        sort_node->estimated_rows = node->estimated_rows;
        node->children.push_back(std::move(sort_node));
      } else {
        node->children.push_back(std::move(child_state.plan_node));
      }
      node->cost = path->cost();

      // HAVING（greedy optimization
      if (!thd->lex->using_hypergraph_optimizer() && join && join->having_cond) {
        auto having_filter = std::make_unique<Filter>();
        having_filter->condition = join->having_cond;
        having_filter->cost = path->cost();
        having_filter->estimated_rows = SafeRowEstimate(path->num_output_rows());
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
        node->children.push_back(std::move(child_state.plan_node));
        node->cost = path->cost();
        node->estimated_rows = std::min(SafeRowEstimate(path->num_output_rows()), limit);

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
        node->children.push_back(std::move(child_state.plan_node));
        node->cost = path->cost();
        node->estimated_rows = SafeRowEstimate(path->num_output_rows());
        state->plan_node = std::move(node);
      }
      state->state_map = child_state.state_map;
      return false;
    } break;
    case AccessPath::EQ_REF: {
      // Normally an EQ_REF inner is rewritten to an INDEX_SCAN by the enclosing
      // join before it reaches here. It can still arrive directly (e.g. as a
      // join's outer/probe input, or a wrapper's child), and returning success
      // without producing a plan node leaves the parent holding a null child
      // (crash in HashJoin::ToAccessPath). Emit a native node instead.
      make_native_plan(state, path);
      return false;
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
          Plan operand_plan;
          if (operand.subquery_path) {
            TranslateState operand_state;
            if (!translate_access_path(&operand_state, thd, operand.subquery_path, operand.join))
              operand_plan = std::move(operand_state.plan_node);
          }
          // Preserve one-to-one indexing with MaterializePathParameters::m_operands.
          inner_plans.push_back(std::move(operand_plan));
        }

        auto cte_node = std::make_unique<MaterializeCTE>();
        cte_node->original_path = path;
        cte_node->cte_name = cte ? std::string(cte->name.str, cte->name.length) : "unnamed_cte";
        cte_node->tmp_table = tmp_table;
        for (auto &plan : inner_plans) {
          cte_node->inner_plans.push_back(std::move(plan));
        }

        cte_node->cost = path->cost();
        cte_node->estimated_rows = SafeRowEstimate(path->num_output_rows());
        cte_node->is_recursive = (cte && cte->recursive);

        if (mat.param->limit_rows != HA_POS_ERROR) cte_node->limit = mat.param->limit_rows;

        state->plan_node = std::move(cte_node);
        state->state_map = Utils::get_tablescovered(path);

        return false;
      } else if (is_derived) {
        // Legacy materialization retains temporary-table Item/Field bindings
        // that MaterializeDerived does not currently reproduce.  Executing
        // the rewritten node can collapse a grouped derived table to one row.
        if (!thd->lex->using_hypergraph_optimizer()) {
          make_native_plan(state, path);
          return false;
        }
        Query_expression *unit = mat.param->unit;
        TABLE *tmp_table = mat.param->table;
        if (!unit || !tmp_table) {
          make_native_plan(state, path);
          return false;
        }

        std::vector<std::unique_ptr<PlanNode>> inner_plans;
        for (size_t i = 0; i < mat.param->m_operands.size(); ++i) {
          const auto &operand = mat.param->m_operands[i];
          Plan operand_plan;
          if (operand.subquery_path) {
            TranslateState operand_state;
            if (!translate_access_path(&operand_state, thd, operand.subquery_path, operand.join))
              operand_plan = std::move(operand_state.plan_node);
          }
          // Preserve one-to-one indexing with MaterializePathParameters::m_operands.
          inner_plans.push_back(std::move(operand_plan));
        }

        auto derived_node = std::make_unique<MaterializeDerived>();
        derived_node->original_path = path;
        derived_node->tmp_table = tmp_table;

        for (auto &plan : inner_plans) {
          derived_node->inner_plans.push_back(std::move(plan));
        }

        derived_node->cost = path->cost();
        derived_node->estimated_rows = SafeRowEstimate(path->num_output_rows());

        // UNION
        derived_node->has_union = (mat.param->m_operands.size() > 1);
        derived_node->is_union_distinct =
            (mat.param->m_operands.size() > 0 && !mat.param->m_operands[0].disable_deduplication_by_hash_field);

        state->plan_node = std::move(derived_node);
        state->state_map = Utils::get_tablescovered(path);

        return false;
      } else {
        auto native = std::make_unique<MySQLNative>();
        native->original_path = path;
        native->cost = path->cost();
        native->estimated_rows = SafeRowEstimate(path->num_output_rows());

        state->plan_node = std::move(native);
        state->state_map = Utils::get_tablescovered(path);
        return false;
      }
      return false;
    } break;
    case AccessPath::STREAM: {
      // LATERAL correlated subquery under the hypergraph optimizer.
      // Recursively translate the inner subquery so that Plan-IR passes
      // (ProjectionPruning, PredicatePushDown, etc.) can see the
      // correlated predicate (e.g. t_med.grp = a.grp) and mark the
      // outer correlation column as referenced.
      const auto &stream = path->stream();
      TranslateState child_state;
      if (translate_access_path(&child_state, thd, stream.child, stream.join)) return true;

      // Wrap the translated child in MySQLNative (so ToAccessPath returns
      // the original STREAM AccessPath), but store the child in `children`
      // so WalkPlan can descend into the inner subquery.
      auto native = std::make_unique<MySQLNative>();
      native->original_path = path;
      native->estimated_rows = SafeRowEstimate(path->num_output_rows());
      if (child_state.plan_node) native->children.push_back(std::move(child_state.plan_node));

      state->plan_node = std::move(native);
      state->state_map = Utils::get_tablescovered(path);
      return false;
    } break;
    default: {
      // if Rapid can not handle, then re-encapsulate to a Fallback node
      auto original = std::make_unique<MySQLNative>();
      original->original_path = path;
      original->estimated_rows = SafeRowEstimate(path->num_output_rows());
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
      for (Item_eq_base *item : expr->equijoin_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
      }
      for (Item *item : expr->join_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
      }
      break;

    case RelationalExpression::MULTI_INNER_JOIN: {
      for (Item_eq_base *item : expr->equijoin_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
      }
      for (Item *item : expr->join_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
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

  // Do NOT recurse into left/right or multi_children.
  //
  // Semantic: each hypergraph edge's RelationalExpression carries its
  // complete predicate set in the top-level join_conditions /
  // equijoin_conditions. Children represent different edges (processed
  // by their own AccessPath nodes).
  //
  // Memory safety: MoveFilterPredicatesIntoHashJoinCondition creates
  // RelationalExpression nodes that only set type / join_conditions /
  // equijoin_conditions — left / right are uninitialized (the struct
  // has no default member initializers for these raw pointers), so
  // reading them yields garbage bytes that may be non-null.
  (void)expr->type;
}

bool Optimizer::hanle_outerjoin_zerorows(TranslateState *parent_state, THD *thd, AccessPath *path, const JOIN *join,
                                         TranslateState &inner_state) {
  // The native iterator already implements NULL-complementation for an empty
  // build side. A false Filter above the join would incorrectly remove every
  // outer row, so keep the original physical path until Rapid has a dedicated
  // zero-build outer-join representation.
  (void)thd;
  (void)join;
  parent_state->filter.zero_row_state_map = inner_state.filter.zero_row_state_map;
  make_native_plan(parent_state, path);
  parent_state->state_map = Utils::get_tablescovered(path);
  return false;
}

std::unique_ptr<Imcs::Predicate> Optimizer::convert_item_to_predicate(const THD *thd, const Item *item) {
  if (!item) return nullptr;

  // Handle different Item types
  switch (item->type()) {
    case Item::FUNC_ITEM:
      return convert_func_item_to_predicate(thd, static_cast<Item_func *>(const_cast<Item *>(item)));
    case Item::COND_ITEM:
      return convert_cond_item_to_predicate(thd, static_cast<Item_cond *>(const_cast<Item *>(item)));
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

std::unique_ptr<Imcs::Predicate> Optimizer::convert_item_to_predicate(const THD *thd, const Index_lookup *lookup,
                                                                      const TABLE *table) {
  // Note: Index_lookup::key_err is an *execution-time* flag (set by
  // construct_lookup(); true means the key could not be built and the lookup
  // matches nothing). It carries no meaning during optimization, and the old
  // `!lookup->key_err` guard inverted it -- declining exactly the usable
  // lookups. Gate on the structural preconditions only.
  if (!lookup || lookup->key == -1 || lookup->key_parts == 0) return nullptr;

  // Check for impossible NULL references
  if (lookup->impossible_null_ref()) return nullptr;  // This will never match
  KEY *key_info = nullptr;
  if (table && lookup->key >= 0 && lookup->key < (int)table->s->keys) {
    key_info = &table->key_info[lookup->key];
  }

  // If there's only one key part, create a simple equality predicate
  if (lookup->key_parts == 1) {
    Item *item = lookup->items[0];
    // A non-constant item is the documented "dynamic lookup" case (a join
    // condition such as t1.id = t2.id): there is no constant to store, so no
    // simple predicate can be built.
    if (!item || !item->const_item()) return nullptr;

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
    return make_predicate(target_field, col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
  }

  // Multiple key parts - create compound predicate with AND
  auto compound = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
  bool has_predicates = false;

  for (uint i = 0; i < lookup->key_parts; i++) {
    Item *item = lookup->items[i];
    // Skip dynamic (non-constant) key parts; see the single-key-part case above.
    if (!item || !item->const_item()) continue;

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
    auto pred = make_predicate(target_field, col_idx, Imcs::PredicateOperator::EQUAL, value, field_type);
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

std::unique_ptr<Imcs::Predicate> Optimizer::convert_sel_arg_to_predicate(const THD *thd, const SEL_ARG *sel_arg,
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
      current_node_pred = make_predicate(field, col_idx, Imcs::PredicateOperator::IS_NULL,
                                         Imcs::PredicateValue::null_value(), field_type);
    } else if (arg->is_singlepoint()) {
      // case2：col = 10
      Imcs::PredicateValue val = extract_value_from_sel_arg_min(thd, arg, field_type);
      current_node_pred = make_predicate(field, col_idx, Imcs::PredicateOperator::EQUAL, val, field_type);
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
          current_node_pred = make_predicate(field, col_idx, min_val, max_val, field_type);
        } else {
          auto range_and = std::make_unique<Imcs::Compound_Predicate>(Imcs::PredicateOperator::AND);
          range_and->add_child(make_predicate(
              field, col_idx,
              (min_flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL,
              min_val, field_type));
          range_and->add_child(make_predicate(
              field, col_idx,
              (max_flag & NEAR_MAX) ? Imcs::PredicateOperator::LESS_THAN : Imcs::PredicateOperator::LESS_EQUAL, max_val,
              field_type));
          current_node_pred = std::move(range_and);
        }
      } else if (has_min) {
        Imcs::PredicateValue min_val = extract_value_from_sel_arg_min(thd, arg, field_type);
        auto op =
            (min_flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL;
        current_node_pred = make_predicate(field, col_idx, op, min_val, field_type);
      } else if (has_max) {
        Imcs::PredicateValue max_val = extract_value_from_sel_arg_max(thd, arg, field_type);
        auto op = (max_flag & NEAR_MAX) ? Imcs::PredicateOperator::LESS_THAN : Imcs::PredicateOperator::LESS_EQUAL;
        current_node_pred = make_predicate(field, col_idx, op, max_val, field_type);
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

std::unique_ptr<Imcs::Predicate> Optimizer::convert_quick_range_to_predicate(const THD *thd, const QUICK_RANGE *range,
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
        auto min_pred = make_predicate(field, col_idx, min_op, min_val, field_type);
        compound->add_child(std::move(min_pred));
      }

      if (has_max) {
        auto max_op = max_inclusive ? Imcs::PredicateOperator::LESS_EQUAL : Imcs::PredicateOperator::LESS_THAN;
        auto max_pred = make_predicate(field, col_idx, max_op, max_val, field_type);
        compound->add_child(std::move(max_pred));
      }
      return compound;
    } else {
      // Both inclusive - create BETWEEN predicate
      return make_predicate(field, col_idx, min_val, max_val, field_type);
    }
  }

  // Only min bound
  if (has_min) {
    Imcs::PredicateValue min_val = Optimizer::extract_value_from_key_part(thd, range->min_key, key_part, field_type);
    auto op = min_inclusive ? Imcs::PredicateOperator::GREATER_EQUAL : Imcs::PredicateOperator::GREATER_THAN;
    return make_predicate(field, col_idx, op, min_val, field_type);
  }

  // Only max bound
  if (has_max) {
    Imcs::PredicateValue max_val = Optimizer::extract_value_from_key_part(thd, range->max_key, key_part, field_type);
    auto op = max_inclusive ? Imcs::PredicateOperator::LESS_EQUAL : Imcs::PredicateOperator::LESS_THAN;
    return make_predicate(field, col_idx, op, max_val, field_type);
  }

  // No bounds - this shouldn't happen
  return nullptr;
}

/**
 * Extract value from key part buffer
 */
Imcs::PredicateValue Optimizer::extract_value_from_key_part(const THD *thd, const uchar *key_ptr,
                                                            const KEY_PART_INFO *key_part,
                                                            enum_field_types field_type) {
  (void)thd;
  (void)field_type;  // Taken from key_part->field by the shared decoder.
  if (!key_ptr || !key_part || !key_part->field) return Imcs::PredicateValue::null_value();

  if (key_part->null_bit) {
    if (*key_ptr) return Imcs::PredicateValue::null_value();  // NULL flag is set
    ++key_ptr;                                                // Skip NULL byte
  }

  Imcs::PredicateValue value;
  if (!decode_key_value(key_ptr, key_part->field, value)) return Imcs::PredicateValue::null_value();
  return value;
}

/**
 * Extract value from SEL_ARG (single point)
 */
Imcs::PredicateValue Optimizer::extract_value_from_sel_arg(const THD *thd, const SEL_ARG *sel_arg,
                                                           enum_field_types field_type) {
  if (!sel_arg) return Imcs::PredicateValue::null_value();
  // For single point, min and max are the same
  return extract_value_from_sel_arg_min(thd, sel_arg, field_type);
}

/**
 * Extract a bound value from SEL_ARG
 *
 * SEL_ARG::min_value / max_value point at a handler *key image* for the field,
 * prefixed by a one-byte NULL flag when the field is nullable (see
 * SEL_ARG::store_min_value() and is_null_interval() in sql/range_optimizer/tree.h)
 * -- the same layout QUICK_RANGE::min_key/max_key use. Decode it with the same
 * routine instead of reinterpret_cast-ing the buffer.
 */
Imcs::PredicateValue Optimizer::extract_sel_arg_bound(const SEL_ARG *sel_arg, const uchar *bound) {
  if (!sel_arg || !sel_arg->field || !bound) return Imcs::PredicateValue::null_value();

  Field *field = sel_arg->field;
  if (field->is_nullable()) {
    if (*bound) return Imcs::PredicateValue::null_value();  // NULL flag byte set
    ++bound;
  }

  Imcs::PredicateValue value;
  if (!decode_key_value(bound, field, value)) return Imcs::PredicateValue::null_value();
  return value;
}

Imcs::PredicateValue Optimizer::extract_value_from_sel_arg_min(const THD *thd, const SEL_ARG *sel_arg,
                                                               enum_field_types field_type) {
  (void)thd;
  (void)field_type;  // Derived from SEL_ARG::field by the shared decoder.
  return extract_sel_arg_bound(sel_arg, sel_arg ? sel_arg->min_value : nullptr);
}

Imcs::PredicateValue Optimizer::extract_value_from_sel_arg_max(const THD *thd, const SEL_ARG *sel_arg,
                                                               enum_field_types field_type) {
  (void)thd;
  (void)field_type;
  return extract_sel_arg_bound(sel_arg, sel_arg ? sel_arg->max_value : nullptr);
}

/**
 * Convert function Item to predicate (comparison operations)
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_func_item_to_predicate(const THD *thd, const Item_func *func) {
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
      return convert_between_to_predicate(thd, static_cast<const Item_func_between *>(func));
    case Item_func::IN_FUNC:
      return convert_in_to_predicate(thd, static_cast<const Item_func_in *>(func));
    case Item_func::ISNULL_FUNC:
      return convert_isnull_to_predicate(thd, func);
    case Item_func::ISNOTNULL_FUNC:
      return convert_isnotnull_to_predicate(thd, func);
    case Item_func::LIKE_FUNC:
      return convert_like_to_predicate(thd, static_cast<const Item_func_like *>(func), false);
    case Item_func::COND_AND_FUNC:
    case Item_func::COND_OR_FUNC:
      // These should have been handled by convert_cond_item_to_predicate
      return convert_cond_item_to_predicate(thd, static_cast<const Item_cond *>(func));
    default:
      // Unsupported function type
      return nullptr;
  }
}

/**
 * Convert condition Item to compound predicate (AND/OR/NOT)
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_cond_item_to_predicate(const THD *thd, const Item_cond *cond) {
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
  List_iterator<Item> li(*(const_cast<Item_cond *>(cond)->argument_list()));
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
std::unique_ptr<Imcs::Predicate> Optimizer::convert_comparison_to_predicate(const THD *thd, const Item_func *func,
                                                                            Imcs::PredicateOperator op) {
  if (func->argument_count() != 2) return nullptr;

  Item *left = func->arguments()[0];
  Item *right = func->arguments()[1];

  // We need one side to be a field reference
  Item_field *field_item = nullptr;
  Item *value_item = nullptr;

  // Only `field OP constant` maps onto a Simple_Predicate. A column-to-column
  // comparison (t.a = t.b, or a join residual) has no constant to store, so it
  // must stay an ordinary Item evaluated by a Filter above the scan. Checking
  // const_item() here is what keeps extract_value_from_item() from being handed
  // a Field it cannot evaluate.
  if (left->type() == Item::FIELD_ITEM && right->const_item()) {
    field_item = static_cast<Item_field *>(left);
    value_item = right;
  } else if (right->type() == Item::FIELD_ITEM && left->const_item()) {
    field_item = static_cast<Item_field *>(right);
    value_item = left;

    // Swap operator direction if field is on right
    op = swap_operator(op);
  } else {
    // Not a simple `column OP constant` pattern - can't create a simple predicate
    return nullptr;
  }

  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();
  Imcs::PredicateValue value = extract_value_from_item(thd, value_item, field_type, field);
  return make_predicate(field, col_idx, op, value, field_type);
}

/**
 * Convert BETWEEN to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_between_to_predicate(const THD *thd,
                                                                         const Item_func_between *between) {
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
    auto less_pred = make_predicate(field, col_idx, Imcs::PredicateOperator::LESS_THAN, min_val, field_type);
    auto greater_pred = make_predicate(field, col_idx, Imcs::PredicateOperator::GREATER_THAN, max_val, field_type);

    compound->add_child(std::move(less_pred));
    compound->add_child(std::move(greater_pred));

    return compound;
  } else {
    // BETWEEN: create simple predicate
    return make_predicate(field, col_idx, min_val, max_val, field_type);
  }
}

/**
 * Convert IN to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_in_to_predicate(const THD *thd, const Item_func_in *in_func) {
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
  return make_predicate(field, col_idx, values, is_negated, field_type);
}

/**
 * Convert IS NULL to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_isnull_to_predicate(const THD *thd, const Item_func *func) {
  if (func->argument_count() != 1) return nullptr;

  Item *arg = func->arguments()[0];
  if (arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  return make_predicate(field, col_idx, Imcs::PredicateOperator::IS_NULL, Imcs::PredicateValue::null_value(),
                        field_type);
}

/**
 * Convert IS NOT NULL to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_isnotnull_to_predicate(const THD *thd, const Item_func *func) {
  if (func->argument_count() != 1) return nullptr;

  Item *arg = func->arguments()[0];
  if (arg->type() != Item::FIELD_ITEM) return nullptr;

  Item_field *field_item = static_cast<Item_field *>(arg);
  if (!field_item->field) return nullptr;

  Field *field = field_item->field;
  uint32 col_idx = field->field_index();
  enum_field_types field_type = field->type();

  return make_predicate(field, col_idx, Imcs::PredicateOperator::IS_NOT_NULL, Imcs::PredicateValue::null_value(),
                        field_type);
}

/**
 * Convert LIKE to predicate
 */
std::unique_ptr<Imcs::Predicate> Optimizer::convert_like_to_predicate(const THD *thd, const Item_func_like *like_func,
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
  return make_predicate(field, col_idx, op, pattern, field_type);
}

std::unique_ptr<Imcs::Predicate> Optimizer::convert_range_to_predicate(const QUICK_RANGE *qr, const TABLE *table,
                                                                       int index_no) {
  const KEY *key_info = &table->key_info[index_no];
  std::vector<std::unique_ptr<Imcs::Predicate>> predicates;

  const uchar *min_ptr = qr->min_key;
  const uchar *max_ptr = qr->max_key;
  uint current_offset = 0;

  for (unsigned part_idx = 0; part_idx < key_info->user_defined_key_parts; ++part_idx) {
    const KEY_PART_INFO *key_part = &key_info->key_part[part_idx];
    Field *field = key_part->field;
    uint store_length = key_part->store_length;

    bool has_min = (!(qr->flag & NO_MIN_RANGE) && (current_offset < qr->min_length));
    bool has_max = (!(qr->flag & NO_MAX_RANGE) && (current_offset < qr->max_length));
    if (!has_min && !has_max) break;

    bool is_eq = (has_min && has_max && ((qr->flag & EQ_RANGE) || memcmp(min_ptr, max_ptr, store_length) == 0));
    if (is_eq) {
      const uchar *ptr = min_ptr;
      if (field->is_nullable()) {
        if (*ptr) {
          // NULL flag set: this key part is `col IS NULL`. Emit it rather than
          // dropping the restriction and leaving a broader AND behind.
          predicates.push_back(
              Imcs::Predicate_Builder::create_simple(field->field_index(), Imcs::PredicateOperator::IS_NULL,
                                                     Imcs::PredicateValue::null_value(), field->type(), field));
          goto next_part;
        }
        ptr++;
      }
      Imcs::PredicateValue val;
      if (decode_key_value(ptr, field, val)) {
        predicates.push_back(Imcs::Predicate_Builder::create_simple(
            field->field_index(), Imcs::PredicateOperator::EQUAL, val, field->type(), field));
      }
    } else {
      if (has_min) {
        const uchar *ptr = min_ptr;
        if (field->is_nullable()) {
          if (*ptr) {
            if (qr->flag & NEAR_MIN) {
              predicates.push_back(
                  Imcs::Predicate_Builder::create_simple(field->field_index(), Imcs::PredicateOperator::IS_NOT_NULL,
                                                         Imcs::PredicateValue::null_value(), field->type(), field));
            }
            goto skip_min;
          }
          ptr++;
        }
        Imcs::PredicateValue min_val;
        if (decode_key_value(ptr, field, min_val)) {
          // NEAR_MIN is only valid if this column is the "first range column"
          Imcs::PredicateOperator op =
              (qr->flag & NEAR_MIN) ? Imcs::PredicateOperator::GREATER_THAN : Imcs::PredicateOperator::GREATER_EQUAL;
          predicates.push_back(
              Imcs::Predicate_Builder::create_simple(field->field_index(), op, min_val, field->type(), field));
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
              Imcs::Predicate_Builder::create_simple(field->field_index(), op, max_val, field->type(), field));
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

bool Optimizer::decode_key_value(const uchar *key_ptr, const Field *field, Imcs::PredicateValue &out_value) {
  Field *mutable_field = const_cast<Field *>(field);
  auto field_type = mutable_field->type();

  if (field_type == MYSQL_TYPE_NEWDECIMAL) {
    auto new_field = down_cast<Field_new_decimal *>(mutable_field);
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

  if (field_type == MYSQL_TYPE_VARCHAR || field_type == MYSQL_TYPE_VAR_STRING) {
    // QUICK_RANGE/min_key/max_key use the handler key-image format, where a
    // variable-length key part always starts with HA_KEY_BLOB_LENGTH (2) bytes.
    // A TABLE::record Field_varstring may instead use a one-byte length prefix
    // (e.g. VARCHAR(32) utf8mb4). Pointing Field::val_str() directly at the
    // handler image therefore shifts the payload and creates a bogus residual
    // range predicate. Decode the handler image explicitly.
    const uint data_len = static_cast<uint>(uint2korr(key_ptr));
    if (data_len > field->field_length) return false;
    out_value =
        Imcs::PredicateValue(std::string(reinterpret_cast<const char *>(key_ptr + HA_KEY_BLOB_LENGTH), data_len));
    return true;
  }

  uchar *old_ptr = mutable_field->field_ptr();
  mutable_field->set_field_ptr(const_cast<uchar *>(key_ptr));

  bool decoded = true;
  if (is_integer_type(field_type) || is_temporal_type(field_type) || field->real_type() == MYSQL_TYPE_ENUM ||
      field->real_type() == MYSQL_TYPE_SET) {
    out_value = Imcs::PredicateValue(static_cast<int64_t>(field->val_int()));
  } else if (is_numeric_type(field_type)) {
    out_value = Imcs::PredicateValue(static_cast<double>(field->val_real()));
  } else if (is_string_type(field_type)) {
    String str_val;
    String *str = field->val_str(&str_val);
    if (str)
      out_value = Imcs::PredicateValue(std::string(str->ptr(), str->length()));
    else
      decoded = false;
  } else {
    // No branch handled this type. Reporting success would hand the caller a
    // default-constructed value and produce a predicate that does not describe
    // the range at all.
    decoded = false;
  }

  mutable_field->set_field_ptr(old_ptr);
  return decoded;
}

/**
 * Extract value from Item
 */
Imcs::PredicateValue Optimizer::extract_value_from_item(const THD *thd, const Item *item, enum_field_types target_type,
                                                        const Field *target_field) {
  if (!item || target_type == MYSQL_TYPE_NULL || !item->const_item()) return Imcs::PredicateValue::null_value();

  if (target_type != MYSQL_TYPE_NULL && target_field) {
    Field *mutable_target_field = const_cast<Field *>(target_field);
    Item_result item_result_type = item->result_type();
    Item_result target_result_type;

    if (target_field->real_type() == MYSQL_TYPE_ENUM || target_field->real_type() == MYSQL_TYPE_SET) {
      target_result_type = INT_RESULT;
    } else {
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
    }

    if (item_result_type !=
        target_result_type) {  // convert item_type to target_field type. such as datetime op '2022-12-12'
      type_conversion_status store_result = TYPE_OK;
      Item *mutable_item = const_cast<Item *>(item);
      ShannonBase::Utils::ColumnMapGuard write_guard(mutable_target_field->table,
                                                     ShannonBase::Utils::ColumnMapGuard::TYPE::WRITE);
      switch (item_result_type) {
        case INT_RESULT:
          store_result = mutable_target_field->store(mutable_item->val_int(), item->unsigned_flag);
          break;
        case REAL_RESULT:
          store_result = mutable_target_field->store(mutable_item->val_real());
          break;
        case STRING_RESULT: {
          String str_buf;
          if (String *str = mutable_item->val_str(&str_buf))
            store_result = mutable_target_field->store(str->ptr(), str->length(), str->charset());
        } break;
        case DECIMAL_RESULT: {
          my_decimal decimal_buf;
          if (my_decimal *dec = mutable_item->val_decimal(&decimal_buf))
            store_result = mutable_target_field->store_decimal(dec);
        } break;
        default:
          // ROW_RESULT / INVALID_RESULT have no scalar Field representation.
          // Decline the conversion rather than aborting the server.
          return Imcs::PredicateValue::null_value();
      }
      if (store_result == TYPE_OK && !mutable_target_field->is_null()) {
        if (target_result_type == INT_RESULT) {
          int64 int_value = mutable_target_field->val_int();
          return Imcs::PredicateValue(int_value);
        }
        if (target_result_type == REAL_RESULT) {
          double real_value = mutable_target_field->val_real();
          return Imcs::PredicateValue(real_value);
        }
        if (target_result_type == STRING_RESULT) {
          String str_buf;
          String *str = mutable_target_field->val_str(&str_buf);
          std::string string_value = str ? std::string(str->ptr(), str->length()) : "";
          return Imcs::PredicateValue(string_value);
        }
      }
    }
  }

  switch (item->type()) {
    case Item::INT_ITEM: {
      auto *int_item = const_cast<Item_int *>(static_cast<const Item_int *>(item));
      return Imcs::PredicateValue(static_cast<int64>(int_item->val_int()));
    } break;
    case Item::REAL_ITEM: {
      auto *float_item = const_cast<Item_float *>(static_cast<const Item_float *>(item));
      return Imcs::PredicateValue(float_item->val_real());
    } break;
    case Item::DECIMAL_ITEM: {
      auto *decimal_item = const_cast<Item_decimal *>(static_cast<const Item_decimal *>(item));
      my_decimal dv;
      if (decimal_item->val_decimal(&dv)) {
        String str_buf;
        my_decimal2string(E_DEC_FATAL_ERROR, &dv, &str_buf);
        return Imcs::PredicateValue(std::string(str_buf.ptr(), str_buf.length()),
                                    ShannonBase::Imcs::PredicateValueType::DECIMAL);
      }
      return Imcs::PredicateValue::null_value();
    } break;
    case Item::STRING_ITEM: {
      auto *string_item = const_cast<Item_string *>(static_cast<const Item_string *>(item));
      String *str = string_item->val_str(nullptr);
      return str ? Imcs::PredicateValue(std::string(str->ptr(), str->length())) : Imcs::PredicateValue::null_value();
    } break;
    case Item::NULL_ITEM:
      return Imcs::PredicateValue::null_value();
      break;
    case Item::FUNC_ITEM: {
      // Try to evaluate the function
      auto *func = const_cast<Item_func *>(static_cast<const Item_func *>(item));
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
        return Imcs::PredicateValue(static_cast<int64>(const_cast<Item *>(item)->val_int()));
      } else if (item->result_type() == REAL_RESULT) {
        return Imcs::PredicateValue(const_cast<Item *>(item)->val_real());
      } else if (item->result_type() == STRING_RESULT) {
        String str_buf;
        String *str = const_cast<Item *>(item)->val_str(&str_buf);
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

bool Optimizer::CanPathBeVectorized(const AccessPath *path) {
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
      bool is_loaded = shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str) != nullptr;
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
      return hash_join.join_predicate != nullptr && hash_join.join_predicate->expr != nullptr;
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

bool Optimizer::CheckChildVectorization(const AccessPath *child_path) {
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
      context->can_vectorized = Optimizer::CanPathBeVectorized(path);
      if (path->vectorized == context->can_vectorized) return nullptr;

      auto rapid_path = new (current_thd->mem_root) AccessPath();
      rapid_path->vectorized = context->can_vectorized;
      rapid_path->type = AccessPath::INDEX_SCAN;
      rapid_path->count_examined_rows = true;
      rapid_path->index_scan().table = path->index_scan().table;
      rapid_path->index_scan().idx = path->index_scan().idx;
      rapid_path->index_scan().use_order = path->index_scan().use_order;
      rapid_path->index_scan().reverse = path->index_scan().reverse;
      rapid_path->iterator = nullptr;
      return rapid_path;
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
      /*
       * Do not perform the old low-level NLJ->RapidHashJoin rewrite here. The
       * Rapid hash join currently retains the complete build side and has no
       * external spill implementation, so forcing allow_spill_to_disk=false
       * turns join_buffer_size into a sizing hint rather than a memory bound.
       * The Plan-IR translation path above can either preserve the original NLJ
       * (when probe order matters) or generate a native spill-capable hash join.
       */
      return nullptr;
    } break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
    } break;
    case AccessPath::BKA_JOIN: {
    } break;
    case AccessPath::HASH_JOIN: {
      auto hash_join = path->hash_join();
      if (hash_join.join_predicate != nullptr && hash_join.join_predicate->expr != nullptr &&
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
      rapid_path->hash_join().join_predicate = hash_join.join_predicate;
      rapid_path->hash_join().store_rowids = hash_join.store_rowids;
      rapid_path->hash_join().allow_spill_to_disk = hash_join.allow_spill_to_disk;
      rapid_path->hash_join().rewrite_semi_to_inner = hash_join.rewrite_semi_to_inner;
      rapid_path->hash_join().tables_to_get_rowid_for = hash_join.store_rowids ? hash_join.tables_to_get_rowid_for : 0;
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
      aggregate_evaluated outcome = AGGR_COMPLETE;
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
        rapid_path->iterator = nullptr;
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
