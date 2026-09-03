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

#include <unordered_set>

#include "sql/field.h"
#include "sql/join_optimizer/join_optimizer.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
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

namespace {
/**
 * double holds an integer exactly only up to 2^53 -- 15 full decimal digits --
 * and both sides of a DECIMAL comparison travel through double
 * (Simple_Predicate::extract_value() decodes the column with
 * get_field_numeric<double>(), and extract_value_from_item() stores the
 * constant into the field and reads val_real()). Beyond 15 digits two distinct
 * DECIMALs can land on the same double, which for a row filter means
 * qualifying the wrong rows -- so only a precision that round-trips exactly may
 * be pushed. The scalar evaluation path is no more exact than the SIMD one; it
 * decodes through the same double.
 */
bool decimal_fits_double_exactly(const Field *field) {
  constexpr uint kMaxExactDecimalDigitsInDouble = 15;
  if (field == nullptr || field->type() != MYSQL_TYPE_NEWDECIMAL) return false;
  return down_cast<const Field_new_decimal *>(field)->precision <= kMaxExactDecimalDigitsInDouble;
}

/** Comparisons the storage layer can evaluate, given a type it compares exactly. */
bool is_pushable_comparison(Imcs::PredicateOperator op) {
  switch (op) {
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

/** The subset that needs equality only, never an ordering. */
bool is_equality_comparison(Imcs::PredicateOperator op) {
  switch (op) {
    case Imcs::PredicateOperator::EQUAL:
    case Imcs::PredicateOperator::NOT_EQUAL:
    case Imcs::PredicateOperator::IS_NULL:
    case Imcs::PredicateOperator::IS_NOT_NULL:
      return true;
    default:
      return false;
  }
}
}  // namespace

double like_pattern_selectivity(const Item_func *like_func) {
  // A leading wildcard forces a substring search over the whole column, which
  // in practice matches far less than a prefix does; an anchored pattern is
  // closer to an equality. Neither is derived from statistics -- these are the
  // conventional defaults, and they exist to be less wrong than a flat 0.5.
  constexpr double kContainsSelectivity = 0.05;
  constexpr double kAnchoredSelectivity = 0.1;

  if (like_func == nullptr || like_func->argument_count() < 2) return kAnchoredSelectivity;
  Item *pattern_item = const_cast<Item_func *>(like_func)->arguments()[1];
  if (pattern_item == nullptr || !pattern_item->const_item()) return kAnchoredSelectivity;

  String buffer;
  const String *pattern = pattern_item->val_str(&buffer);
  if (pattern == nullptr || pattern->length() == 0) return kAnchoredSelectivity;

  const std::string pat(pattern->ptr(), pattern->length());
  const bool leading_wildcard = pat.front() == '%';
  const bool trailing_wildcard = pat.back() == '%';
  return (leading_wildcard && trailing_wildcard) ? kContainsSelectivity : kAnchoredSelectivity;
}

/**
 * A pushed predicate becomes the final row qualification, so the stored column
 * value and the constant must reach PredicateValue by routes that agree
 * exactly. Zone-map pruning has the weaker requirement -- never skipping is
 * always legal -- and StorageIndex::can_skip_simple_predicate() applies its own,
 * stricter type filter, so widening this gate cannot make pruning unsafe.
 */
bool is_storage_index_predicate_safe(const Imcs::Predicate *predicate) {
  if (predicate == nullptr || predicate->is_compound()) return false;
  const auto *simple = static_cast<const Imcs::Simple_Predicate *>(predicate);
  const auto type = simple->column_type.load(std::memory_order_acquire);
  const Field *field = simple->field_meta.load(std::memory_order_acquire);

  if (!is_pushable_comparison(simple->op)) return false;

  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      // Signed integers widen losslessly to int64, and so do the unsigned types
      // narrower than 64 bits. BIGINT UNSIGNED is the one exclusion: the column
      // side tags its PredicateValue unsigned and the constant side does not,
      // so values at or above 2^63 would compare in different domains.
      if (type == MYSQL_TYPE_LONGLONG && field != nullptr && field->is_unsigned()) return false;
      return true;

    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      // The packed 3-byte date decodes to the same YYYYMMDD integer that
      // Field_newdate::val_int() hands the constant side. Any other real_type()
      // falls into get_field_numeric()'s zero branch, which would qualify every
      // row against 0.
      return field != nullptr && field->real_type() == MYSQL_TYPE_NEWDATE;

    case MYSQL_TYPE_YEAR:
      return true;

    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      // Both sides land on the same YYYYMMDDHHMMSS (or HHMMSS) integer, but the
      // column side casts the decoded value to int64 and so drops the
      // fractional seconds the constant keeps -- a fractional-precision column
      // would qualify rows whose sub-second part differs.
      return field != nullptr && field->decimals() == 0;

      // TIMESTAMP is deliberately absent. get_field_numeric() decodes it to Unix
      // epoch seconds while the constant side reaches PredicateValue through
      // Field_timestampf::val_int(), which yields YYYYMMDDHHMMSS. The two never
      // compare equal, so a pushed TIMESTAMP predicate qualifies no rows at all.

    case MYSQL_TYPE_NEWDECIMAL:
      return decimal_fits_double_exactly(field);

    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
      // Equality goes through the collation (PredicateValue::operator== calls
      // strnncollsp), which is what makes CHAR padding and case-insensitive
      // collations come out right. Ordering comparisons stay out until the
      // string range filter is proven PAD SPACE correct.
      return is_equality_comparison(simple->op);

    default:
      // FLOAT/DOUBLE are comparable but excluded on purpose: a literal that is
      // not representable in binary floating point would qualify by rounding.
      return false;
  }
}

JoinMultiplicity prove_at_most_one(const PlanNode *child, const std::vector<Item *> &join_conditions) {
  if (child == nullptr || child->type() != PlanNode::Type::SCAN) return JoinMultiplicity::kUnknown;

  const auto *scan = static_cast<const ScanTable *>(child);
  TABLE *table = scan->source_table;
  if (table == nullptr || table->s == nullptr) return JoinMultiplicity::kUnknown;

  // Columns of `table` that a simple equi-join predicate (t.col = other.col) binds to the other
  // side. Only a key fully covered by these columns can be proven unique for this join.
  std::unordered_set<uint> equi_key_fields;
  for (Item *cond : join_conditions) {
    if (!is_simple_equijoin(cond)) continue;
    auto *func = down_cast<Item_func *>(cond);
    for (Item *side : {func->arguments()[0], func->arguments()[1]}) {
      auto *field_item = down_cast<Item_field *>(side);
      if (field_item->field != nullptr && field_item->field->table == table) {
        equi_key_fields.insert(field_item->field->field_index());
      }
    }
  }
  if (equi_key_fields.empty()) return JoinMultiplicity::kUnknown;

  for (uint k = 0; k < table->s->keys; ++k) {
    const KEY &key = table->key_info[k];
    if (!(key.flags & HA_NOSAME)) continue;      // not a unique key
    if (key.flags & HA_NULL_PART_KEY) continue;  // SQL allows duplicate NULLs; no true uniqueness guarantee

    bool fully_bound = true;
    for (uint p = 0; p < key.user_defined_key_parts; ++p) {
      const Field *part_field = key.key_part[p].field;
      if (part_field == nullptr || !equi_key_fields.count(part_field->field_index())) {
        fully_bound = false;
        break;
      }
    }
    if (fully_bound) return JoinMultiplicity::kAtMostOne;
  }
  return JoinMultiplicity::kUnknown;
}

bool has_correlation(const AccessPath *path, const JoinHypergraph &graph, table_map outer_tables) {
  if (!path) return false;

  switch (path->type) {
    case AccessPath::FILTER: {
      auto &f = path->filter();
      if (f.condition) {
        table_map used = f.condition->used_tables();
        if (used & outer_tables) return true;  // ref outer table.
      }
      return has_correlation(f.child, graph, outer_tables);
    } break;
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN: {  // check filter_predicates
      if (IsEmpty(path->filter_predicates)) return false;
      for (size_t i = 0; i < graph.predicates.size(); ++i) {
        if (IsBitSet(i, path->filter_predicates)) {
          const auto &pred = graph.predicates[i];
          if (pred.condition) {
            table_map used = pred.condition->used_tables();
            if (used & outer_tables) return true;  // LATERAL：filter ref outer table.
          }
        }
      }
      return false;
    } break;
    case AccessPath::HASH_JOIN:
    case AccessPath::NESTED_LOOP_JOIN: {
      auto get_join_paths = [&](const AccessPath *p) -> std::pair<const AccessPath *, const AccessPath *> {
        if (p->type == AccessPath::HASH_JOIN) {
          auto &hj = p->hash_join();
          return {hj.outer, hj.inner};
        } else {
          auto &nlj = p->nested_loop_join();
          return {nlj.outer, nlj.inner};
        }
      };

      auto [outer, inner] = get_join_paths(path);
      return has_correlation(outer, graph, outer_tables) || has_correlation(inner, graph, outer_tables);
    } break;
    case AccessPath::MATERIALIZE: {  // MATERIALIZE maybe contains subquery.
      auto &mat = path->materialize();
      for (size_t i = 0; i < mat.param->m_operands.size(); ++i) {
        const auto &operand = mat.param->m_operands[i];
        if (!operand.subquery_path) continue;
        if (has_correlation(operand.subquery_path, graph, outer_tables)) return true;
      }
      return false;
    } break;
    default:
      return false;
  }
}

bool has_parameterization(const AccessPath *root) {
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

void collect_join_conditions(const RelationalExpression *expr, std::vector<Item *> &out_conditions) {
  if (expr == nullptr) return;
  switch (expr->type) {
    case RelationalExpression::TABLE:
      break;

    case RelationalExpression::INNER_JOIN:
    case RelationalExpression::LEFT_JOIN:
    case RelationalExpression::SEMIJOIN:
    case RelationalExpression::ANTIJOIN:
    case RelationalExpression::MULTI_INNER_JOIN:
      for (Item_eq_base *item : expr->equijoin_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
      }
      for (Item *item : expr->join_conditions) {
        if (item && !item->has_subquery()) out_conditions.push_back(item);
      }
      break;

    default:
      break;
  }
}

bool can_convert_to_hash_join(const AccessPath *path, const JoinHypergraph &graph) {
  if (path->type != AccessPath::NESTED_LOOP_JOIN) return false;

  const auto &nlj = path->nested_loop_join();
  if (!nlj.join_predicate) return false;

  // A REF/EQ_REF *directly* on the inner is not disqualifying: the translator
  // widens it to a full INDEX_SCAN build side, which drops the parameterization.
  // Only look deeper when the inner is something else.
  if (nlj.inner->type == AccessPath::STREAM) return false;
  if (nlj.inner->type != AccessPath::REF && nlj.inner->type != AccessPath::EQ_REF && has_parameterization(nlj.inner))
    return false;

  std::vector<Item *> conditions;
  collect_join_conditions(nlj.join_predicate->expr, conditions);
  if (conditions.empty()) return false;

  const table_map outer_tables = get_tablescovered(nlj.outer);
  for (Item *cond : conditions) {
    if (cond == nullptr || !is_simple_equijoin(cond)) continue;
    if (!has_correlation(nlj.inner, graph, outer_tables)) return true;
  }
  return false;
}
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase