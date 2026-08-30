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
#ifndef __SHANNONBASE_QUERY_PLAN_UTILS_H__
#define __SHANNONBASE_QUERY_PLAN_UTILS_H__

#include "include/my_table_map.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/relational_expression.h"

#include "storage/rapid_engine/optimizer/query_plan.h"  // PlanNode, JoinMultiplicity

class TABLE;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Predicate;
}  // namespace Imcs
namespace Optimizer {
namespace Utils {

using RpdTableLookup = std::function<Imcs::RpdTable *(TABLE *)>;
RpdTableLookup rpd_lookup_func();

inline bool is_simple_equijoin(Item *item) {
  if (item->type() != Item::FUNC_ITEM) return false;

  auto *func = down_cast<Item_func *>(item);
  if (func->functype() != Item_func::EQ_FUNC) return false;
  if (func->argument_count() != 2) return false;
  Item *arg0 = func->arguments()[0];
  Item *arg1 = func->arguments()[1];
  return (arg0->type() == Item::FIELD_ITEM && arg1->type() == Item::FIELD_ITEM);
}

inline bool is_outerjoin(const JoinPredicate *pred) {
  if (!pred || !pred->expr) return false;
  return (pred->expr->type == RelationalExpression::LEFT_JOIN ||
          pred->expr->type == RelationalExpression::FULL_OUTER_JOIN);
}

inline bool is_semijoin(const JoinPredicate *pred) {
  if (!pred || !pred->expr) return false;
  return (pred->expr->type == RelationalExpression::SEMIJOIN || pred->expr->type == RelationalExpression::ANTIJOIN);
}

// Proves whether `child` (one side of a HashJoin) contributes at most one row per distinct
// join-key value, by checking whether `join_conditions` bind a UNIQUE NOT NULL index of child's
// table in full. Returns kUnknown (not kAtMostOne) whenever the proof cannot be established --
// including when `child` is anything other than a directly-scanned ScanTable leaf, since a
// further join/aggregate as a child would require transitive proof this function does not
// attempt. See JoinMultiplicity in query_plan.h for why this matters to rules like
// AggregationPushDown/JoinReOrder.
JoinMultiplicity prove_at_most_one(const PlanNode *child, const std::vector<Item *> &join_conditions);

inline bool is_zerorows(const AccessPath *path) {
  if (!path) return false;
  return (path->type == AccessPath::ZERO_ROWS || path->type == AccessPath::ZERO_ROWS_AGGREGATED ||
          path->type == AccessPath::FAKE_SINGLE_ROW);
}

/**
 * @brief Combine multiple predicates with AND
 * @param predicates List of predicates
 * @return Combined AND condition, or single predicate if only one
 */
inline Item *combine_with_and(const std::vector<Item *> &predicates, const THD *thd = current_thd) {
  if (predicates.empty()) return nullptr;
  if (predicates.size() == 1) return predicates[0];

  // Create an AND condition
  Item_cond_and *and_cond = new (thd->mem_root) Item_cond_and();
  for (auto *pred : predicates) {
    and_cond->add(pred);
  }
  and_cond->quick_fix_field();
  and_cond->update_used_tables();
  and_cond->apply_is_true();
  return and_cond;
}

inline bool contains_subquery(Item *item) {
  if (!item) return false;
  return item->has_subquery();
}

inline bool contains_correlated_subquery(Item *item) {
  // return (item->used_tables() & OUTER_REF_TABLE_BIT) ? true : false;

  if (!item || !item->has_subquery()) return false;
  bool found = false;
  WalkItem(item, enum_walk::POSTFIX, [&](Item *it) -> bool {
    if (it->type() == Item::SUBQUERY_ITEM) {
      auto *sub = static_cast<Item_subselect *>(it);
      Query_expression *qexpr = sub->query_expr();
      if (qexpr) {
        for (Query_block *qb = qexpr->first_query_block(); qb; qb = qb->next_query_block()) {
          if (qb->where_cond()) {
            WalkItem(qb->where_cond(), enum_walk::POSTFIX, [&](Item *inner) -> bool {
              if (inner->type() == Item::FIELD_ITEM) {
                if (static_cast<Item_field *>(inner)->depended_from != nullptr) {
                  found = true;
                  return true;
                }
              }
              return false;
            });
          }
          if (found) return true;
        }
      }
    }
    return found;
  });
  return found;
}

table_map get_tablescovered(const AccessPath *path);
/**
 * @brief Hypergraph-aware variant of get_tablescovered().
 *
 * Resolves a TABLE_SCAN through JoinHypergraph::nodes so the table_map is taken
 * from the hypergraph's own node numbering. Used on the hypergraph-optimizer
 * path; falls back to get_tablescovered() for every other AccessPath shape.
 */
table_map get_tablescovered_from_hypergraph(const AccessPath *path, const JoinHypergraph &graph);

/**
 * @brief Returns true only if `join_node` (a HashJoin or NestLoopJoin PlanNode) is provably an
 * INNER join, based on the join-type/JoinPredicate metadata preserved on its original MySQL
 * AccessPath.
 *
 * Outer/semi/anti joins, and a HASH_JOIN whose join_predicate/RelationalExpression metadata is
 * missing, are NOT provably inner and must be treated as an opaque boundary by any rewrite that
 * assumes join commutativity/associativity (join reordering, aggregate pushdown below a join,
 * WHERE-predicate pushdown below a join). Mirrors the check PredicatePushDown::push_below_join
 * has always used for filter pushdown, promoted here so JoinReOrder and AggregationPushDown can
 * share the identical proof instead of re-deriving it.
 */
bool is_provably_inner_join(const PlanNode *join_node);

/**
 * @brief Returns true if `path` (or, transitively, any join/materialize/filter it contains) reads
 * a column of one of `outer_tables`. Used to detect LATERAL / correlated joins: a nested-loop
 * join whose inner side depends on the current outer row cannot be treated as commutative with
 * its outer side, and re-evaluates per outer row rather than once.
 * @param path Candidate inner/subquery AccessPath to inspect
 * @param graph Hypergraph the path was built from (for filter_predicates -> condition lookup)
 * @param outer_tables Table bitmap of the join's outer side
 */
bool has_correlation(const AccessPath *path, const JoinHypergraph &graph, table_map outer_tables);

/**
 * @brief Returns true if any AccessPath in `root`'s subtree is parameterized, i.e. carries a join
 * condition pushed into an index lookup on a table that is not joined in here yet.
 *
 * Such a subtree cannot be pre-built and probed: it has to be re-executed for every outer row, so
 * Optimizer::translate_access_path() keeps it as a NestLoopJoin instead of converting to a
 * (vectorized) HashJoin. Shared with can_convert_to_hash_join() so the cost model and the
 * translator cannot disagree about which joins actually become hash joins.
 */
bool has_parameterization(const AccessPath *root);

/**
 * @brief Collects the conditions a HashJoin can use as its hash key from `expr`.
 *
 * This is the single source of truth for "what will the hash key be": Optimizer::
 * extract_join_conditions() builds the executed join from it, and can_convert_to_hash_join()
 * consults it so the cost model cannot promise a hash join the translator then declines to build.
 */
void collect_join_conditions(const RelationalExpression *expr, std::vector<Item *> &out_conditions);

/**
 * @brief Returns true if `path` is a NESTED_LOOP_JOIN whose join predicate has a field=field
 * equi-join condition and whose inner side is not correlated to its outer side (see
 * has_correlation) -- i.e. it could equivalently run as a HashJoin.
 * @param path Candidate AccessPath, checked to be a NESTED_LOOP_JOIN
 * @param graph Hypergraph the path was built from
 */
bool can_convert_to_hash_join(const AccessPath *path, const JoinHypergraph &graph);

/**
 * @brief Checks whether an already-converted IMCS predicate is safe to hand to the Rapid
 * storage layer for row/IMCU-level qualification (as opposed to only being evaluated by a
 * MySQL Filter node above the scan). Narrow on purpose: only exact-comparison operators over
 * signed LONG/LONGLONG columns are considered proven-safe today.
 */
bool is_storage_index_predicate_safe(const Imcs::Predicate *predicate);
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_QUERY_PLAN_UTILS_H__