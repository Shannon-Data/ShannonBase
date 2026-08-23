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
#include "storage/rapid_engine/optimizer/utils.h"

#include "sql/field.h"
#include "sql/join_optimizer/join_optimizer.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/range_optimizer/range_optimizer.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

namespace ShannonBase {
namespace Optimizer {
namespace Utils {
RpdTableLookup rpd_lookup_func() {
  return [](TABLE *table) -> Imcs::RpdTable * {
    if (!table || !table->s) return nullptr;

    auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
    if (!share) return nullptr;

    return share->is_partitioned ? Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                 : Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  };
}

// Returns table_map for TABLE *t, or 0 if t or its pos_in_table_list is null.
// Materialized temp tables have no pos_in_table_list — they don't correspond
// to any bit in the outer query's table_map.
inline table_map safe_map(TABLE *t) { return (t && t->pos_in_table_list) ? t->pos_in_table_list->map() : 0; }

table_map get_tablescovered(const AccessPath *path) {
  if (!path) return 0;

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      return safe_map(path->table_scan().table);
    case AccessPath::INDEX_SCAN:
      return safe_map(path->index_scan().table);
    case AccessPath::REF:
      return safe_map(path->ref().table);
    case AccessPath::EQ_REF:
      return safe_map(path->eq_ref().table);
    case AccessPath::INDEX_RANGE_SCAN:
      return (path->index_range_scan().used_key_part != nullptr &&
              path->index_range_scan().used_key_part[0].field != nullptr)
                 ? safe_map(path->index_range_scan().used_key_part[0].field->table)
                 : 0;
    case AccessPath::CONST_TABLE:
      return safe_map(path->const_table().table);
    case AccessPath::HASH_JOIN:
      return get_tablescovered(path->hash_join().outer) | get_tablescovered(path->hash_join().inner);
    case AccessPath::NESTED_LOOP_JOIN:
      return get_tablescovered(path->nested_loop_join().outer) | get_tablescovered(path->nested_loop_join().inner);
    case AccessPath::BKA_JOIN:
      return get_tablescovered(path->bka_join().outer) | get_tablescovered(path->bka_join().inner);
    case AccessPath::FILTER:
      return get_tablescovered(path->filter().child);
    case AccessPath::SORT:
      return get_tablescovered(path->sort().child);
    case AccessPath::AGGREGATE:
      return get_tablescovered(path->aggregate().child);
    case AccessPath::LIMIT_OFFSET:
      return get_tablescovered(path->limit_offset().child);
    case AccessPath::STREAM:
      return get_tablescovered(path->stream().child);
    case AccessPath::WINDOW:
      return get_tablescovered(path->window().child);
    case AccessPath::REMOVE_DUPLICATES:
      return get_tablescovered(path->remove_duplicates().child);
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      return get_tablescovered(path->remove_duplicates_on_index().child);
    case AccessPath::MATERIALIZE:
      // Treat MATERIALIZE as opaque, like core MySQL's WalkTablesUnderAccessPath
      // (WalkAccessPathPolicy::STOP_AT_MATERIALIZATION) does: each operand's subquery_path is
      // resolved in its own query block's locally-numbered table_map domain, so a subquery's
      // bit N and the outer block's bit N can refer to unrelated tables. Only mat.table_path -
      // the reader of the materialized result, whose TABLE lives in the outer numbering domain -
      // is safe to fold into the result here.
      return get_tablescovered(path->materialize().table_path);
    case AccessPath::APPEND: {
      table_map result = 0;
      for (const AppendPathParameters &child : *path->append().children) {
        result |= get_tablescovered(child.path);
      }
      return result;
    } break;
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
      return 0;
    case AccessPath::FOLLOW_TAIL:
      return safe_map(path->follow_tail().table);
    case AccessPath::REF_OR_NULL:
      return safe_map(path->ref_or_null().table);
    case AccessPath::PUSHED_JOIN_REF:
      return safe_map(path->pushed_join_ref().table);
    case AccessPath::MRR:
      return safe_map(path->mrr().table);
    case AccessPath::INDEX_SKIP_SCAN:
      return safe_map(path->index_skip_scan().table);
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return safe_map(path->group_index_skip_scan().table);
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return safe_map(path->dynamic_index_range_scan().table);
    case AccessPath::INDEX_MERGE: {
      table_map result = 0;
      for (AccessPath *child : *path->index_merge().children) result |= get_tablescovered(child);
      return result;
    } break;
    case AccessPath::ROWID_INTERSECTION: {
      table_map result = get_tablescovered(path->rowid_intersection().cpk_child);
      for (AccessPath *child : *path->rowid_intersection().children) result |= get_tablescovered(child);
      return result;
    } break;
    case AccessPath::ROWID_UNION: {
      table_map result = 0;
      for (AccessPath *child : *path->rowid_union().children) result |= get_tablescovered(child);
      return result;
    } break;
    case AccessPath::WEEDOUT:
      return get_tablescovered(path->weedout().child);
    case AccessPath::ALTERNATIVE:
      // Mirrors core MySQL's WalkTablesUnderAccessPath: table_scan_path is guaranteed to be a
      // TABLE_SCAN over the same table as the ref lookup on the other branch (.child), so either
      // side resolves to the same table_map bit.
      return safe_map(path->alternative().table_scan_path->table_scan().table);
    case AccessPath::TEMPTABLE_AGGREGATE:
      return get_tablescovered(path->temptable_aggregate().subquery_path) |
             get_tablescovered(path->temptable_aggregate().table_path);
    default:
      return 0;
  }
}

table_map get_tablescovered_from_hypergraph(const AccessPath *path, const JoinHypergraph &graph) {
  if (path->type == AccessPath::TABLE_SCAN) {
    TABLE *table = path->table_scan().table;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
      if (graph.nodes[i].table() == table) {
        return table->pos_in_table_list->map();
      }
    }
  }
  return get_tablescovered(path);
}

bool is_provably_inner_join(const PlanNode *join_node) {
  if (join_node == nullptr) return false;

  const bool is_hash_join = (join_node->type() == PlanNode::Type::HASH_JOIN);
  if (!is_hash_join && join_node->type() != PlanNode::Type::NESTED_LOOP_JOIN) return false;

  const JoinPredicate *predicate = nullptr;
  const AccessPath *original_path = nullptr;
  if (is_hash_join) {
    const auto *hash_join = static_cast<const HashJoin *>(join_node);
    original_path = hash_join->original_path;
    if (original_path != nullptr) {
      if (original_path->type == AccessPath::HASH_JOIN)
        predicate = original_path->hash_join().join_predicate;
      else if (original_path->type == AccessPath::NESTED_LOOP_JOIN)
        predicate = original_path->nested_loop_join().join_predicate;
    }
  } else {
    const auto *nested_loop = static_cast<const NestLoopJoin *>(join_node);
    original_path = nested_loop->original_path;
    predicate = nested_loop->source_join_predicate;
  }

  if (predicate != nullptr && predicate->expr != nullptr) {
    return predicate->expr->type == RelationalExpression::INNER_JOIN ||
           predicate->expr->type == RelationalExpression::STRAIGHT_INNER_JOIN;
  }
  if (original_path != nullptr && original_path->type == AccessPath::NESTED_LOOP_JOIN) {
    // The legacy optimizer may leave JoinPredicate null. JoinType is still authoritative.
    return original_path->nested_loop_join().join_type == JoinType::INNER;
  }
  // Missing hash-join predicate metadata (or no original_path at all) is not proof of
  // innerness — stay conservative, same as PredicatePushDown's own default.
  return false;
}

bool is_storage_index_predicate_safe(const Imcs::Predicate *predicate) {
  if (predicate == nullptr || predicate->is_compound()) return false;
  const auto *simple = static_cast<const Imcs::Simple_Predicate *>(predicate);
  const auto type = simple->column_type.load(std::memory_order_acquire);
  const Field *field = simple->field_meta.load(std::memory_order_acquire);

  // Signed INT/BIGINT comparisons use exact integer evaluation. BIGINT is excluded from
  // StorageIndex zone-map pruning separately because current min/max storage uses double.
  if (type != MYSQL_TYPE_LONG && type != MYSQL_TYPE_LONGLONG) return false;
  if (field != nullptr && field->is_unsigned()) return false;

  switch (simple->op) {
    case Imcs::PredicateOperator::EQUAL:
    case Imcs::PredicateOperator::NOT_EQUAL:
    case Imcs::PredicateOperator::LESS_THAN:
    case Imcs::PredicateOperator::LESS_EQUAL:
    case Imcs::PredicateOperator::GREATER_THAN:
    case Imcs::PredicateOperator::GREATER_EQUAL:
    case Imcs::PredicateOperator::BETWEEN:
    case Imcs::PredicateOperator::IS_NULL:
    case Imcs::PredicateOperator::IS_NOT_NULL:
      return true;
    default:
      return false;
  }
}
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase