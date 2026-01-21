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
#ifndef __SHANNONBASE_WRITABLE_ACCESS_PATH_H__
#define __SHANNONBASE_WRITABLE_ACCESS_PATH_H__

#include "sql/join_optimizer/access_path.h"

namespace ShannonBase {
namespace Optimizer {

/**
 * AccessPath *&root_path = lex->unit->root_access_path();
 * JOIN *join = lex->unit->first_query_block()->join;
 *
 * WalkAndRewriteAccessPaths(root_path, join, WalkAccessPathPolicy::ENTIRE_TREE,
 *  [](AccessPath *path, const JOIN *join) -> AccessPath * {
 *    return ShannonBase::Optimizer::RewriteTableScan(path, join);
 *    });
 */

template <class AccessPathPtr, class JoinPtr, class RewriteFunc>
AccessPathPtr WalkAndRewriteAccessPaths(AccessPathPtr path, JoinPtr join, WalkAccessPathPolicy policy,
                                        RewriteFunc &&rewrite, bool post_order_traversal = false) {
  static_assert(std::is_convertible<AccessPathPtr, const AccessPath *>::value,
                "The “path” argument must be AccessPath * or const AccessPath *.");
  static_assert(std::is_convertible<JoinPtr, const JOIN *>::value,
                "The “join” argument must be JOIN * or const JOIN * (or nullptr).");
  if (policy == WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK) {
    assert(join != nullptr);
  }
  if (!post_order_traversal) {
    if (AccessPath *new_path = rewrite(path, join)) {
      path = new_path;
    }
  }

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::SAMPLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_DISTANCE_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::CONST_TABLE:
    case AccessPath::MRR:
    case AccessPath::FOLLOW_TAIL:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
    case AccessPath::UNQUALIFIED_COUNT:
      // No children.
      break;
    case AccessPath::NESTED_LOOP_JOIN:
      path->nested_loop_join().outer =
          WalkAndRewriteAccessPaths(path->nested_loop_join().outer, join, policy, rewrite, post_order_traversal);

      path->nested_loop_join().inner = WalkAndRewriteAccessPaths(
          path->nested_loop_join().inner, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      path->nested_loop_semijoin_with_duplicate_removal().outer =
          WalkAndRewriteAccessPaths(path->nested_loop_semijoin_with_duplicate_removal().outer, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      path->nested_loop_semijoin_with_duplicate_removal().inner =
          WalkAndRewriteAccessPaths(path->nested_loop_semijoin_with_duplicate_removal().inner, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::BKA_JOIN:
      path->bka_join().outer = WalkAndRewriteAccessPaths(path->bka_join().outer, join, policy,
                                                         std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      path->bka_join().inner = WalkAndRewriteAccessPaths(path->bka_join().inner, join, policy,
                                                         std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::HASH_JOIN:
      path->hash_join().outer = WalkAndRewriteAccessPaths(path->hash_join().outer, join, policy,
                                                          std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      path->hash_join().inner = WalkAndRewriteAccessPaths(path->hash_join().inner, join, policy,
                                                          std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::FILTER:
      path->filter().child = WalkAndRewriteAccessPaths(path->filter().child, join, policy,
                                                       std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::SORT:
      path->sort().child = path->sort().child = WalkAndRewriteAccessPaths(
          path->sort().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::AGGREGATE:
      path->aggregate().child = WalkAndRewriteAccessPaths(path->aggregate().child, join, policy,
                                                          std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::TEMPTABLE_AGGREGATE:
      path->temptable_aggregate().subquery_path =
          WalkAndRewriteAccessPaths(path->temptable_aggregate().subquery_path, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      path->temptable_aggregate().table_path =
          WalkAndRewriteAccessPaths(path->temptable_aggregate().table_path, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::LIMIT_OFFSET:
      path->limit_offset().child = WalkAndRewriteAccessPaths(
          path->limit_offset().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::STREAM:
      if (policy == WalkAccessPathPolicy::ENTIRE_TREE ||
          (policy == WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK && path->stream().join == join)) {
        path->stream().child = WalkAndRewriteAccessPaths(path->stream().child, path->stream().join, policy,
                                                         std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::MATERIALIZE:
      path->materialize().table_path = WalkAndRewriteAccessPaths(
          path->materialize().table_path, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      for (MaterializePathParameters::Operand &operand : path->materialize().param->m_operands) {
        if (policy == WalkAccessPathPolicy::ENTIRE_TREE ||
            (policy == WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK && operand.join == join)) {
          operand.subquery_path = WalkAndRewriteAccessPaths(
              operand.subquery_path, operand.join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
        }
      }
      break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      path->materialize_information_schema_table().table_path =
          WalkAndRewriteAccessPaths(path->materialize_information_schema_table().table_path, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::APPEND:
      if (policy == WalkAccessPathPolicy::ENTIRE_TREE) {
        for (AppendPathParameters &child : *path->append().children) {
          child.path = WalkAndRewriteAccessPaths(child.path, child.join, policy, std::forward<RewriteFunc &&>(rewrite),
                                                 post_order_traversal);
        }
      }
      break;
    case AccessPath::WINDOW:
      path->window().child = path->window().child = WalkAndRewriteAccessPaths(
          path->window().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::WEEDOUT:
      path->weedout().child = WalkAndRewriteAccessPaths(path->weedout().child, join, policy,
                                                        std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::REMOVE_DUPLICATES:
      path->remove_duplicates().child = WalkAndRewriteAccessPaths(
          path->remove_duplicates().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      path->remove_duplicates_on_index().child =
          WalkAndRewriteAccessPaths(path->remove_duplicates_on_index().child, join, policy,
                                    std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::ALTERNATIVE:
      path->alternative().child = WalkAndRewriteAccessPaths(
          path->alternative().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::CACHE_INVALIDATOR:
      path->cache_invalidator().child = WalkAndRewriteAccessPaths(
          path->cache_invalidator().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::INDEX_MERGE:
      for (AccessPath *child : *path->index_merge().children) {
        child =
            WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::ROWID_INTERSECTION:
      for (AccessPath *child : *path->rowid_intersection().children) {
        child =
            WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::ROWID_UNION:
      for (AccessPath *child : *path->rowid_union().children) {
        child =
            WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::DELETE_ROWS:
      path->delete_rows().child = WalkAndRewriteAccessPaths(
          path->delete_rows().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::UPDATE_ROWS:
      path->update_rows().child = WalkAndRewriteAccessPaths(
          path->update_rows().child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
  }

  if (post_order_traversal) {
    if (AccessPath *new_path = rewrite(path, join)) {
      path = new_path;
    }
  }

  return path;
}

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_WRITABLE_ACCESS_PATH_H__
