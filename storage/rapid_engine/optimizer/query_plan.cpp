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
#include "storage/rapid_engine/optimizer/query_plan.h"
#include "sql/dd/cache/dictionary_client.h"    // Dictionary_client
#include "storage/innobase/include/dict0dd.h"  //dd_table_is_partitioned
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "sql/filesort.h"                              // Filesort
#include "sql/join_optimizer/access_path.h"            // AccessPath
#include "sql/join_optimizer/relational_expression.h"  // RelationalExpression
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"  // query_expression
#include "sql/sql_optimizer.h"
#include "storage/rapid_engine/imcs/table.h"                        // RpdTable
#include "storage/rapid_engine/optimizer/writable_access_path.inc"  // Rapid Params

namespace ShannonBase {
namespace Optimizer {
namespace {

void BindUnboundItemFields(Item *root) {
  if (root == nullptr) return;
  WalkItem(root, enum_walk::POSTFIX, [](Item *item) {
    if (item->type() == Item::FIELD_ITEM) {
      auto *field_item = down_cast<Item_field *>(item);
      if (field_item->field == nullptr) field_item->bind_fields();
    }
    return false;
  });
}

bool HasUnboundItemField(Item *root) {
  return root != nullptr && WalkItem(root, enum_walk::POSTFIX, [](Item *item) {
           return item->type() == Item::FIELD_ITEM && down_cast<Item_field *>(item)->field == nullptr;
         });
}

bool PrepareAggregateFields(THD *thd, LocalAgg *aggregate) {
  JOIN *join = aggregate->join;
  if (join == nullptr) return true;

  for (Item_func *item : aggregate->aggregates) BindUnboundItemFields(item);

  /*
   * Prepare into a detached List first. LocalAgg::ToAccessPath() may fall back
   * to original_path when binding/allocation fails; mutating JOIN::group_fields
   * before that point would corrupt the native fallback plan. List assignment
   * is the same cheap list-header copy already used for group_fields_cache.
   */
  List<Cached_item> prepared_group_fields;
  for (Item *item : aggregate->group_by) {
    BindUnboundItemFields(item);
    if (HasUnboundItemField(item)) return true;

    Cached_item *cached = new_Cached_item(thd, item);
    if (cached == nullptr || prepared_group_fields.push_front(cached)) return true;
  }

  join->group_fields = prepared_group_fields;
  join->group_fields_cache = prepared_group_fields;
  join->streaming_aggregation = !aggregate->group_by.empty();
  return false;
}

// Carries the PlanNode's own cost/row estimate onto the AccessPath it produces, so EXPLAIN and
// downstream sizing (e.g. hash-join build estimation) see Rapid's numbers instead of falling back
// to AccessPath's unknown-cost/unknown-row defaults. Prefers the node's own estimate (set by the
// cost model or narrowed by a rewrite rule such as PredicatePushDown/StorageIndexPrune); falls back
// to the original MySQL-computed AccessPath estimate when the node never got one of its own.
void PropagateCostAndRows(const PlanNode *node, AccessPath *path) {
  double rows = node->estimated_rows > 0 ? static_cast<double>(node->estimated_rows)
                : node->original_path    ? node->original_path->num_output_rows()
                                         : kUnknownRowCount;
  double node_cost = node->cost > 0.0 ? node->cost : node->original_path ? node->original_path->cost() : kUnknownCost;
  path->set_num_output_rows(rows);
  path->set_cost(node_cost);
}

}  // namespace

// Returns true if all provided child AccessPaths are vectorized.
// Use this to propagate vectorized flag from children.
bool PlanNode::AllChildrenVectorized(std::initializer_list<const AccessPath *> paths) {
  for (const auto *p : paths) {
    if (!p || !p->vectorized) return false;
  }
  return true;
}

LocalAgg::LocalAgg() : strategy(AggregateStrategy::STREAMING) {}

AccessPath *ScanTable::ToAccessPath(THD *thd) {
  assert(this->source_table);
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::TABLE_SCAN;
  path->table_scan().table = this->source_table;
  path->vectorized = this->vectorized;

  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Table *table_def = nullptr;
  if (thd->dd_client()->acquire(this->source_table->s->db.str, this->source_table->s->table_name.str, &table_def)) {
    // Error is reported by the dictionary subsystem.
    path->vectorized = false;
  }
  // if table is partition table, we dont use `VectorizedTableScanIterator`
  if (table_def && dd_table_is_partitioned(*table_def)) {
    path->vectorized = false;
  }

  auto rapid_scan_params = new (thd->mem_root) ShannonBase::Optimizer::RapidScanParameters{};
  /*
   * RapidScanParameters itself lives on MEM_ROOT, so it must not own heap
   * allocations. Expose short-lived views into the Plan IR instead. The
   * iterator factory consumes them synchronously while this Plan is alive.
   */
  rapid_scan_params->prune_predicate = this->prune_predicate.get();
  rapid_scan_params->rpd_table = this->rpd_table;
  rapid_scan_params->use_storage_index = this->use_storage_index;
  rapid_scan_params->projected_columns = &this->projected_columns;
  // ProjectionPruning is part of Rapid's Plan IR, whereas MySQL's
  // TableCollection (used to construct parent batch iterators) derives its
  // column layout from TABLE::read_set. Keep both representations in sync so
  // every column produced by the scan has a matching parent ColumnChunk.
  if (this->source_table->read_set != nullptr) {
    for (uint32_t field_index : this->projected_columns) {
      if (field_index < this->source_table->s->fields) bitmap_set_bit(this->source_table->read_set, field_index);
    }
  }
  rapid_scan_params->limit = this->limit;
  rapid_scan_params->offset = this->offset;
  path->secondary_engine_data = rapid_scan_params;
  PropagateCostAndRows(this, path);
  return path;
}

std::string ScanTable::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Vectorized Scan " + (rpd_table ? rpd_table->meta().table_name : "UNKNOWN") +
         (use_storage_index ? " [Pruned]" : "");
}

AccessPath *Filter::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::FILTER;
  path->filter().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->filter().condition = this->condition;
  path->filter().materialize_subqueries =
      this->original_path ? this->original_path->filter().materialize_subqueries : false;

  path->vectorized = AllChildrenVectorized({path->filter().child});
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  return path;
}

std::string Filter::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Filter (vectorized)";
}

AccessPath *NestLoopJoin::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::NESTED_LOOP_JOIN;
  path->nested_loop_join().join_type =
      (this->original_path != nullptr && this->original_path->type == AccessPath::NESTED_LOOP_JOIN)
          ? this->original_path->nested_loop_join().join_type
          : JoinType::INNER;
  path->nested_loop_join().outer = (children.size() >= 1 && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().inner = (children.size() >= 2 && children[1]) ? children[1]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().pfs_batch_mode = this->pfs_batch_mode;
  path->nested_loop_join().already_expanded_predicates = this->already_expanded_predicates;
  path->nested_loop_join().join_predicate = this->source_join_predicate;
  path->nested_loop_join().equijoin_predicates = this->equijoin_predicates;

  path->vectorized = AllChildrenVectorized({path->nested_loop_join().outer, path->nested_loop_join().inner});
  path->secondary_engine_data = nullptr;
  return path;
}

std::string NestLoopJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Nested Loop Join (vectorized)";
}

AccessPath *HashJoin::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::HASH_JOIN;

  if (children.size() >= 2) {
    path->hash_join().outer = children[0]->ToAccessPath(thd);
    path->hash_join().inner = children[1]->ToAccessPath(thd);
  } else {
    path->hash_join().outer = nullptr;
    path->hash_join().inner = nullptr;
  }

  if (this->original_path && this->original_path->type == AccessPath::NESTED_LOOP_JOIN) {
    const auto &nlj = this->original_path->nested_loop_join();
    path->hash_join().join_predicate = nlj.join_predicate;
    // Old optimizer: join_predicate is nullptr.  Create a minimal
    // JoinPredicate so that EXPLAIN and CreateIteratorFromAccessPath
    // can handle the HASH_JOIN path safely.
    if (!path->hash_join().join_predicate) {
      auto *expr = new (thd->mem_root) RelationalExpression(thd);
      switch (nlj.join_type) {
        case JoinType::OUTER:
          expr->type = RelationalExpression::LEFT_JOIN;
          break;
        case JoinType::SEMI:
          expr->type = RelationalExpression::SEMIJOIN;
          break;
        case JoinType::ANTI:
          expr->type = RelationalExpression::ANTIJOIN;
          break;
        case JoinType::INNER:
        default:
          expr->type = RelationalExpression::INNER_JOIN;
          break;
      }
      // Populate equijoin_conditions from this->join_conditions so that
      // CreateIteratorFromAccessPath can build HashJoinCondition objects.
      for (Item *cond : this->join_conditions) {
        if (cond->type() == Item::FUNC_ITEM) {
          auto *func = static_cast<Item_func *>(cond);
          if (func->functype() == Item_func::EQ_FUNC) {
            expr->equijoin_conditions.push_back(static_cast<Item_eq_base *>(cond));
          }
        }
      }
      auto *jp = new (thd->mem_root) JoinPredicate;
      jp->expr = expr;
      path->hash_join().join_predicate = jp;
    }
    path->hash_join().allow_spill_to_disk = this->allow_spill;
    path->hash_join().store_rowids = false;
    path->hash_join().rewrite_semi_to_inner = false;
    path->hash_join().tables_to_get_rowid_for = 0;
  } else if (this->original_path && this->original_path->type == AccessPath::HASH_JOIN) {
    path->hash_join().allow_spill_to_disk =
        this->preserves_probe_order ? false : this->original_path->hash_join().allow_spill_to_disk;
    path->hash_join().join_predicate = this->original_path->hash_join().join_predicate;
    path->hash_join().store_rowids = this->original_path->hash_join().store_rowids;
    path->hash_join().rewrite_semi_to_inner = this->original_path->hash_join().rewrite_semi_to_inner;
    path->hash_join().tables_to_get_rowid_for =
        path->hash_join().store_rowids ? this->original_path->hash_join().tables_to_get_rowid_for : 0;
  } else {
    path->hash_join().allow_spill_to_disk = this->allow_spill;
    path->hash_join().join_predicate = nullptr;
    path->hash_join().store_rowids = false;
    path->hash_join().rewrite_semi_to_inner = false;
    path->hash_join().tables_to_get_rowid_for = 0;
  }

  path->vectorized = !path->hash_join().allow_spill_to_disk && !path->hash_join().store_rowids &&
                     path->hash_join().join_predicate != nullptr && path->hash_join().join_predicate->expr != nullptr;
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  return path;
}

std::string HashJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  if (allow_spill) return pad + "→ Hash Join (native, spill-capable)";
  return pad + (preserves_probe_order ? "→ Sorted Hash Join (vectorized)" : "→ Hash Join (vectorized)");
}

AccessPath *LocalAgg::ToAccessPath(THD *thd) {
  // Build the child before committing JOIN grouping metadata. If child
  // conversion fails, native fallback must see the untouched original JOIN.
  AccessPath *child_path = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  if (child_path == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid aggregate requires a valid child plan");
    return original_path;
  }

  if (PrepareAggregateFields(thd, this)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid could not bind the aggregate GROUP BY expressions");
    return original_path;
  }

  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::AGGREGATE;
  path->aggregate().child = child_path;
  path->aggregate().olap = this->olap;

  path->vectorized = true;
  auto *params = new (thd->mem_root) RapidAggregateParameters{};
  params->strategy = strategy;
  params->hash_output_order = hash_output_order;
  path->secondary_engine_data = params;
  PropagateCostAndRows(this, path);
  return path;
}

std::string LocalAgg::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + (strategy == AggregateStrategy::HASH ? "→ Hash Aggregate" : "→ Streaming Aggregate");
}

AccessPath *TopN::ToAccessPath(THD *thd) {
  auto *sort_path = new (thd->mem_root) AccessPath();
  sort_path->type = AccessPath::SORT;
  sort_path->sort().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  sort_path->sort().order = this->order;
  // Filesort must retain enough rows for the outer OFFSET operation.
  sort_path->sort().limit = (offset > HA_POS_ERROR - limit) ? HA_POS_ERROR : static_cast<ha_rows>(limit + offset);
  sort_path->sort().filesort = this->filesort;
  sort_path->sort().remove_duplicates = this->original_path ? this->original_path->sort().remove_duplicates : false;
  sort_path->sort().unwrap_rollup = this->original_path ? this->original_path->sort().unwrap_rollup : false;
  sort_path->sort().force_sort_rowids = this->original_path ? this->original_path->sort().force_sort_rowids : false;

  // TopN itself is not SIMD-vectorized, but mark based on child so upstream nodes still see the flag correctly
  sort_path->vectorized = AllChildrenVectorized({sort_path->sort().child});
  sort_path->secondary_engine_data = nullptr;

  if (offset == 0) return sort_path;

  auto *limit_path = new (thd->mem_root) AccessPath();
  limit_path->type = AccessPath::LIMIT_OFFSET;
  limit_path->limit_offset().child = sort_path;
  limit_path->limit_offset().limit = limit;
  limit_path->limit_offset().offset = offset;
  limit_path->limit_offset().count_all_rows = false;
  limit_path->limit_offset().reject_multiple_rows = false;
  limit_path->limit_offset().send_records_override = nullptr;
  limit_path->vectorized = sort_path->vectorized;
  limit_path->secondary_engine_data = nullptr;
  return limit_path;
}

std::string TopN::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Top-N (limit=" + std::to_string(limit) + ")";
}

AccessPath *Sort::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::SORT;
  path->sort().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->sort().order = this->order;
  path->sort().limit = this->limit;
  path->sort().filesort = this->filesort;
  path->sort().remove_duplicates = this->remove_duplicates;
  path->sort().unwrap_rollup = this->unwrap_rollup;
  path->sort().force_sort_rowids = this->force_sort_rowids;
  path->sort().tables_to_get_rowid_for = this->tables_to_get_rowid_for;

  // Synthetic Sort nodes introduced by the Rapid optimizer (e.g. the grouping
  // sort inserted above an unordered hash join) carry only an ORDER list and
  // no Filesort object. The iterator constructor requires a valid Filesort, so
  // build one here, exactly like FinalizePlanForQueryBlock does for MySQL sort
  // paths.
  if (path->sort().filesort == nullptr && path->sort().order != nullptr && path->sort().child != nullptr) {
    path->sort().filesort = new (thd->mem_root)
        Filesort(thd, CollectTables(thd, path), /*keep_buffers=*/false, path->sort().order, path->sort().limit,
                 path->sort().remove_duplicates, path->sort().force_sort_rowids, path->sort().unwrap_rollup);
    if (!path->sort().filesort->using_addon_fields()) {
      FindTablesToGetRowidFor(path);
    }
  }

  path->vectorized = AllChildrenVectorized({path->sort().child});
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  return path;
}

std::string Sort::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Sort";
}

AccessPath *Limit::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::LIMIT_OFFSET;
  path->limit_offset().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->limit_offset().limit = this->limit;
  path->limit_offset().offset = this->offset;
  path->limit_offset().count_all_rows = this->count_all_rows;
  path->limit_offset().reject_multiple_rows = this->reject_multiple_rows;
  path->limit_offset().send_records_override =
      this->original_path ? this->original_path->limit_offset().send_records_override : nullptr;

  path->vectorized = AllChildrenVectorized({path->limit_offset().child});
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  return path;
}

std::string Limit::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Limit (limit=" + std::to_string(limit) + ", offset=" + std::to_string(offset) + ")";
}

AccessPath *ZeroRows::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  // ZERO_ROWS_AGGREGATED is not equivalent to ZERO_ROWS or
  // FAKE_SINGLE_ROW: its iterator initializes aggregate expressions and emits
  // the mandatory single row for implicit grouping over an empty input.
  if (original_path && original_path->type == AccessPath::ZERO_ROWS_AGGREGATED)
    path->type = AccessPath::ZERO_ROWS_AGGREGATED;
  else
    path->type = this->rows_returned ? AccessPath::FAKE_SINGLE_ROW : AccessPath::ZERO_ROWS;
  path->vectorized = false;
  path->secondary_engine_data = nullptr;
  return path;
}

std::string ZeroRows::ToString(int indent) const { return "→ Zero Rows"; }

AccessPath *Union::ToAccessPath(THD *thd) { return original_path; }

std::string Union::ToString(int indent) const {
  return std::string(indent, ' ') + (is_distinct ? "→ Union Distinct" : "→ Union All");
}

AccessPath *MaterializeCTE::ToAccessPath(THD *thd) {
  if (!original_path || original_path->type != AccessPath::MATERIALIZE) return original_path;
  auto &operands = original_path->materialize().param->m_operands;
  const size_t count = std::min(operands.size(), inner_plans.size());
  for (size_t i = 0; i < count; ++i) {
    if (inner_plans[i]) operands[i].subquery_path = inner_plans[i]->ToAccessPath(thd);
  }
  return original_path;
}

std::string MaterializeCTE::ToString(int indent) const {
  std::string pad(indent, ' ');
  std::string result = pad + "→ Materialize CTE (" + cte_name + ")";

  if (is_recursive) {
    result += " [RECURSIVE]";
  }
  if (limit != HA_POS_ERROR) {
    result += " LIMIT " + std::to_string(limit);
  }
  result += "\n";

  for (size_t i = 0; i < inner_plans.size(); ++i) {
    result += pad + "  Query Branch " + std::to_string(i) + ":\n";
    if (inner_plans[i]) {
      result += inner_plans[i]->ToString(indent + 4);
    }
  }
  return result;
}

AccessPath *MaterializeDerived::ToAccessPath(THD *thd) {
  if (!original_path || original_path->type != AccessPath::MATERIALIZE) return original_path;
  auto &operands = original_path->materialize().param->m_operands;
  const size_t count = std::min(operands.size(), inner_plans.size());
  for (size_t i = 0; i < count; ++i) {
    if (inner_plans[i]) operands[i].subquery_path = inner_plans[i]->ToAccessPath(thd);
  }
  return original_path;
}

std::string MaterializeDerived::ToString(int indent) const {
  std::string pad(indent, ' ');
  std::string result = pad + "→ Materialize Derived Table";

  if (has_union) {
    result += is_union_distinct ? " [UNION DISTINCT]" : " [UNION ALL]";
  }
  result += "\n";

  for (size_t i = 0; i < inner_plans.size(); ++i) {
    result += pad + "  Branch " + std::to_string(i) + ":\n";
    if (inner_plans[i]) {
      result += inner_plans[i]->ToString(indent + 4);
    }
  }
  return result;
}

AccessPath *MySQLNative::ToAccessPath(THD *) {
  // The path may already have been annotated by the secondary rewrite before
  // it became a native island. Keep the complete physical island native so a
  // streaming MySQL operator never consumes an unordered Rapid child.
  if (original_path != nullptr) {
    WalkAccessPaths(original_path, /*join=*/nullptr, WalkAccessPathPolicy::ENTIRE_TREE,
                    [](AccessPath *path, const JOIN *) {
                      path->vectorized = false;
                      return false;
                    });
  }
  return original_path;
}

std::string MySQLNative::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ MySQL (" + std::to_string((int)original_path->type) + ")";
}

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> cb) {
  if (!node) return;
  cb(node);
  for (auto &child : node->children) {
    WalkPlan(child.get(), cb);
  }
}
}  // namespace Optimizer
}  // namespace ShannonBase
