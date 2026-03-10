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

class TABLE;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
}
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
inline Item *combine_with_and(const std::vector<Item *> &predicates, THD *thd = current_thd) {
  if (predicates.empty()) return nullptr;
  if (predicates.size() == 1) return predicates[0];

  // Create an AND condition
  Item_cond_and *and_cond = new (thd->mem_root) Item_cond_and();
  for (auto *pred : predicates) {
    and_cond->add(pred);
  }
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
table_map get_tablescovered_from_hypergraph(const AccessPath *path, const JoinHypergraph &graph);
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_QUERY_PLAN_UTILS_H__