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

namespace ShannonBase {
namespace Optimizer {
namespace Utils {
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
  if (!item) return false;
  return (item->used_tables() & OUTER_REF_TABLE_BIT) ? true : false;
}

table_map get_tablescovered(const AccessPath *path);
table_map get_tablescovered_from_hypergraph(const AccessPath *path, const JoinHypergraph &graph);
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_QUERY_PLAN_UTILS_H__