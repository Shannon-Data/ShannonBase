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
void WalkAndRewriteAccessPaths(AccessPathPtr path, JoinPtr join, WalkAccessPathPolicy policy, RewriteFunc &&rewrite,
                               bool post_order_traversal = false) {
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
      return;
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
      WalkAndRewriteAccessPaths(path->nested_loop_join().outer, join, policy, rewrite, post_order_traversal);

      WalkAndRewriteAccessPaths(path->nested_loop_join().inner, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      WalkAndRewriteAccessPaths(path->nested_loop_semijoin_with_duplicate_removal().outer, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      WalkAndRewriteAccessPaths(path->nested_loop_semijoin_with_duplicate_removal().inner, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::BKA_JOIN:
      WalkAndRewriteAccessPaths(path->bka_join().outer, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      WalkAndRewriteAccessPaths(path->bka_join().inner, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::HASH_JOIN:
      WalkAndRewriteAccessPaths(path->hash_join().outer, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      WalkAndRewriteAccessPaths(path->hash_join().inner, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::FILTER:
      WalkAndRewriteAccessPaths(path->filter().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::SORT:
      WalkAndRewriteAccessPaths(path->sort().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::AGGREGATE:
      WalkAndRewriteAccessPaths(path->aggregate().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::TEMPTABLE_AGGREGATE:
      WalkAndRewriteAccessPaths(path->temptable_aggregate().subquery_path, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      WalkAndRewriteAccessPaths(path->temptable_aggregate().table_path, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::LIMIT_OFFSET:
      WalkAndRewriteAccessPaths(path->limit_offset().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::STREAM:
      if (policy == WalkAccessPathPolicy::ENTIRE_TREE ||
          (policy == WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK && path->stream().join == join)) {
        WalkAndRewriteAccessPaths(path->stream().child, path->stream().join, policy,
                                  std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::MATERIALIZE:
      WalkAndRewriteAccessPaths(path->materialize().table_path, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      for (const MaterializePathParameters::Operand &operand : path->materialize().param->m_operands) {
        if (policy == WalkAccessPathPolicy::ENTIRE_TREE ||
            (policy == WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK && operand.join == join)) {
          WalkAndRewriteAccessPaths(operand.subquery_path, operand.join, policy, std::forward<RewriteFunc &&>(rewrite),
                                    post_order_traversal);
        }
      }
      break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
      WalkAndRewriteAccessPaths(path->materialize_information_schema_table().table_path, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::APPEND:
      if (policy == WalkAccessPathPolicy::ENTIRE_TREE) {
        for (const AppendPathParameters &child : *path->append().children) {
          WalkAndRewriteAccessPaths(child.path, child.join, policy, std::forward<RewriteFunc &&>(rewrite),
                                    post_order_traversal);
        }
      }
      break;
    case AccessPath::WINDOW:
      WalkAndRewriteAccessPaths(path->window().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::WEEDOUT:
      WalkAndRewriteAccessPaths(path->weedout().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::REMOVE_DUPLICATES:
      WalkAndRewriteAccessPaths(path->remove_duplicates().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      WalkAndRewriteAccessPaths(path->remove_duplicates_on_index().child, join, policy,
                                std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      break;
    case AccessPath::ALTERNATIVE:
      WalkAndRewriteAccessPaths(path->alternative().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::CACHE_INVALIDATOR:
      WalkAndRewriteAccessPaths(path->cache_invalidator().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::INDEX_MERGE:
      for (AccessPath *child : *path->index_merge().children) {
        WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::ROWID_INTERSECTION:
      for (AccessPath *child : *path->rowid_intersection().children) {
        WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::ROWID_UNION:
      for (AccessPath *child : *path->rowid_union().children) {
        WalkAndRewriteAccessPaths(child, join, policy, std::forward<RewriteFunc &&>(rewrite), post_order_traversal);
      }
      break;
    case AccessPath::DELETE_ROWS:
      WalkAndRewriteAccessPaths(path->delete_rows().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
    case AccessPath::UPDATE_ROWS:
      WalkAndRewriteAccessPaths(path->update_rows().child, join, policy, std::forward<RewriteFunc &&>(rewrite),
                                post_order_traversal);
      break;
  }

  if (post_order_traversal) {
    if (AccessPath *new_path = rewrite(path, join)) {
      path = new_path;
    }
  }
}

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_WRITABLE_ACCESS_PATH_H__
