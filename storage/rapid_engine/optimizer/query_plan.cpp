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

#include <limits>
#include "sql/dd/cache/dictionary_client.h"    // Dictionary_client
#include "storage/innobase/include/dict0dd.h"  //dd_table_is_partitioned
#include "storage/rapid_engine/cost/planning_statistics.h"
#include "storage/rapid_engine/executor/iterators/materialize_iterator.h"
#include "storage/rapid_engine/executor/iterators/nested_loop_iterator.h"
#include "storage/rapid_engine/executor/iterators/sort_iterator.h"
#include "storage/rapid_engine/executor/iterators/stream_iterator.h"
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "sql/filesort.h"                                    // Filesort
#include "sql/join_optimizer/access_path.h"                  // AccessPath
#include "sql/join_optimizer/materialize_path_parameters.h"  // MaterializePathParameters
#include "sql/join_optimizer/relational_expression.h"        // RelationalExpression
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/mem_root_array.h"  // Mem_root_array
#include "sql/sql_executor.h"
#include "sql/sql_lex.h"  // query_expression
#include "sql/sql_optimizer.h"
#include "storage/rapid_engine/executor/iterators/aggregate_iterator.h"  // HashGroupFixedBytes
#include "storage/rapid_engine/imcs/table.h"                             // RpdTable
#include "storage/rapid_engine/optimizer/writable_access_path.inc"       // Rapid Params
#include "storage/rapid_engine/utils/utils.h"                            // hash_aggregate_memory_budget

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

/*
 * Produces a MATERIALIZE AccessPath carrying the Rapid-translated operand
 * subplans, leaving `original` and its MaterializePathParameters untouched.
 */
AccessPath *RebuildMaterializeWithOperands(THD *thd, AccessPath *original,
                                           const std::vector<std::unique_ptr<PlanNode>> &inner_plans,
                                           bool batch_requested = false) {
  if (original == nullptr || original->type != AccessPath::MATERIALIZE) return original;
  const MaterializePathParameters *source = original->materialize().param;
  if (source == nullptr) return original;

  auto *param = new (thd->mem_root)
      MaterializePathParameters{Mem_root_array<MaterializePathParameters::Operand>(thd->mem_root, source->m_operands),
                                source->invalidators,
                                source->table,
                                source->cte,
                                source->unit,
                                source->ref_slice,
                                source->rematerialize,
                                source->limit_rows,
                                source->reject_multiple_rows};

  const size_t count = std::min(param->m_operands.size(), inner_plans.size());
  for (size_t i = 0; i < count; ++i) {
    if (!inner_plans[i]) continue;
    AccessPath *rewritten = inner_plans[i]->ToAccessPath(thd);
    if (rewritten != nullptr) param->m_operands[i].subquery_path = rewritten;
  }

  auto *path = new (thd->mem_root) AccessPath(*original);
  path->materialize().param = param;
  path->iterator = nullptr;
  path->vectorized = batch_requested && Executor::VectorizedMaterializeIterator::CanVectorize(path);
  return path;
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
  // An ordered scan is driven by the ART index, not by rowid order, so emit the
  // path type that says so: the iterator calls ha_index_init()/index_next_batch()
  // and EXPLAIN names the index it walks.
  const bool index_driven = this->has_required_order && this->index_no >= 0;
  if (index_driven) {
    path->type = AccessPath::INDEX_SCAN;
    path->index_scan().table = this->source_table;
    path->index_scan().idx = static_cast<unsigned>(this->index_no);
    path->index_scan().use_order = true;
    path->index_scan().reverse = this->reverse;
  } else {
    path->type = AccessPath::TABLE_SCAN;
    path->table_scan().table = this->source_table;
  }
  path->vectorized = this->vectorized && !this->row_mode;

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

  // Without the vectorized iterator nothing reads index_next_batch(), so an
  // INDEX_SCAN path would be served by the native iterator against a cursor
  // this plan never positioned. Fall back to the table scan shape.
  if (index_driven && !path->vectorized) {
    path->type = AccessPath::TABLE_SCAN;
    path->table_scan().table = this->source_table;
  }

  auto rapid_scan_params = new (thd->mem_root) ShannonBase::Optimizer::RapidScanParameters{};

  rapid_scan_params->prune_predicate = this->prune_predicate.get();
  rapid_scan_params->rpd_table = this->rpd_table;
  rapid_scan_params->use_storage_index = this->use_storage_index;
  rapid_scan_params->projected_columns = &this->projected_columns;

  if (this->source_table->read_set != nullptr) {
    for (uint32_t field_index : this->projected_columns) {
      if (field_index < this->source_table->s->fields) bitmap_set_bit(this->source_table->read_set, field_index);
    }
  }
  rapid_scan_params->limit = this->limit;
  rapid_scan_params->offset = this->offset;
  if (this->has_required_order) {
    rapid_scan_params->index_no = this->index_no;
    rapid_scan_params->reverse = this->reverse;
  }
  path->secondary_engine_data = rapid_scan_params;
  PropagateCostAndRows(this, path);
  // A vectorized scan feeds ColumnChunks to whoever reads it.
  this->produces_batches = path->vectorized;
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
  if (this->row_mode && !children.empty() && children[0]) children[0]->row_mode = true;
  path->filter().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->filter().condition = this->condition;
  path->filter().materialize_subqueries =
      this->original_path ? this->original_path->filter().materialize_subqueries : false;

  path->vectorized = !this->row_mode && AllChildrenVectorized({path->filter().child});
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  // A residual filter forwards its child's transport unless a subquery forces row re-entry.
  const bool child_batches = !children.empty() && children[0] != nullptr && children[0]->produces_batches;
  this->produces_batches =
      path->vectorized && child_batches && (this->condition == nullptr || !this->condition->has_subquery());
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
  // Read() on the existing vectorized children already supplies TABLE buffers.
  // A row-consuming join must not disable their internal batch scans/filters.
  path->nested_loop_join().outer = (children.size() >= 1 && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().inner = (children.size() >= 2 && children[1]) ? children[1]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().pfs_batch_mode = this->pfs_batch_mode;
  path->nested_loop_join().already_expanded_predicates = this->already_expanded_predicates;
  path->nested_loop_join().join_predicate = this->source_join_predicate;
  path->nested_loop_join().equijoin_predicates = this->equijoin_predicates;

  PropagateCostAndRows(this, path);
  path->vectorized = !row_mode && Executor::SelectVectorizedNestedLoop(thd, path, batch_output_requested);
  if (!path->vectorized) {
    // A later aggregate decision can put this join back in row mode. Direct
    // lookups then have no batch consumer; keep their native row entry point.
    // Scans and filters may still benefit from batching internally.
    for (AccessPath *input : {path->nested_loop_join().outer, path->nested_loop_join().inner}) {
      if (input != nullptr && (input->type == AccessPath::REF || input->type == AccessPath::REF_OR_NULL ||
                               input->type == AccessPath::EQ_REF))
        input->vectorized = false;
    }
  }
  path->secondary_engine_data = nullptr;
  if (path->vectorized) {
    auto *params = new (thd->mem_root) RapidNestedLoopParameters;
    params->memory_budget = std::min<size_t>(ShannonBase::Utils::Util::hash_join_memory_budget(thd), 2 * 1024 * 1024);
    DBUG_EXECUTE_IF("rapid_nlj_tiny_budget", { params->memory_budget = 64; });
    path->secondary_engine_data = params;
  }
  produces_batches = path->vectorized;
  PropagateCostAndRows(this, path);
  return path;
}

std::string NestLoopJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Nested Loop Join (vectorized)";
}

AccessPath *HashJoin::ToAccessPath(THD *thd) {
  // A missing child cannot produce an executable hash join; fall back to the
  // original (native) AccessPath rather than dereferencing a null child.
  if (children.size() < 2 || !children[0] || !children[1]) {
    return original_path;
  }

  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::HASH_JOIN;

  {
    path->hash_join().outer = children[0]->ToAccessPath(thd);
    path->hash_join().inner = children[1]->ToAccessPath(thd);
  }

  if (this->original_path && this->original_path->type == AccessPath::NESTED_LOOP_JOIN) {
    const auto &nlj = this->original_path->nested_loop_join();
    path->hash_join().join_predicate = nlj.join_predicate;

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
    // Take the node's own protocol decision, as the other two branches here
    // already do. Reading the original path's flag discarded that choice for
    // every join MySQL had already planned as a hash join -- most of them --
    // pinning them to the native iterator whatever the node asked for.
    path->hash_join().allow_spill_to_disk = this->allow_spill;
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

  /*
    This is where a hash join is accepted or refused for the vectorized
    iterator; access_path.cpp re-tests the same three conditions when it builds
    the iterator.

    allow_spill_to_disk is the planner asking for a join that is *prepared* to
    spill, not a prediction that this one will.
  */
  const bool materialized_input =
      (path->hash_join().outer != nullptr && path->hash_join().outer->type == AccessPath::MATERIALIZE &&
       !path->hash_join().outer->vectorized) ||
      (path->hash_join().inner != nullptr && path->hash_join().inner->type == AccessPath::MATERIALIZE &&
       !path->hash_join().inner->vectorized);
  /*
    A materialized derived table (e.g. the deduplicated derived table MySQL
    builds for NOT EXISTS) is a MySQL temp table, read through
    MaterializeIterator. Rapid reads its build side only after initializing the
    probe, and the first row read from an on-disk (InnoDB) temp table then
    crashes inside InnoDB. MySQL's own iterator reads the build before the
    probe, so leave this join to it.
  */
  path->vectorized = !this->row_mode && !materialized_input && !path->hash_join().allow_spill_to_disk &&
                     !path->hash_join().store_rowids && path->hash_join().join_predicate != nullptr &&
                     path->hash_join().join_predicate->expr != nullptr;
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  // The join materializes chunks even over row-at-a-time inputs; Init() re-checks.
  this->produces_batches = path->vectorized;
  return path;
}

std::string HashJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  if (allow_spill) return pad + "→ Hash Join (native, spill-capable)";
  return pad + (preserves_probe_order ? "→ Sorted Hash Join (vectorized)" : "→ Hash Join (vectorized)");
}

AccessPath *LocalAgg::ToAccessPath(THD *thd) {
  // Predict batch transport before recursively building the child AccessPath.
  const auto can_batches = [thd](const auto &self, const PlanNode *node) -> bool {
    if (node == nullptr || !node->vectorized || node->row_mode) return false;
    switch (node->type()) {
      case PlanNode::Type::SCAN: {
        const auto *scan = static_cast<const ScanTable *>(node);
        return scan->source_table != nullptr && scan->source_table->part_info == nullptr &&
               Executor::CanBatchRecords(scan->source_table);
      }
      case PlanNode::Type::HASH_JOIN: {
        if (node->original_path == nullptr) return false;
        bool supported = true;
        WalkTablesUnderAccessPath(
            node->original_path,
            [&](TABLE *table) {
              supported = supported && Executor::CanBatchRecords(table);
              return false;
            },
            true);
        return supported;
      }
      case PlanNode::Type::NESTED_LOOP_JOIN:
        return Executor::SelectVectorizedNestedLoop(thd, node->original_path,
                                                    static_cast<const NestLoopJoin *>(node)->batch_output_requested);
      case PlanNode::Type::REF_LOOKUP:
        return Executor::SelectVectorizedLookup(thd, node->original_path);
      case PlanNode::Type::STREAM: {
        if (node->original_path == nullptr || node->children.size() != 1 || !self(self, node->children[0].get()))
          return false;
        const auto &stream = node->original_path->stream();
        return Executor::VectorizedStreamIterator::CanVectorize(stream.temp_table_param, stream.table,
                                                                stream.provide_rowid);
      }
      case PlanNode::Type::MATERIALIZE_DERIVED:
        return static_cast<const MaterializeDerived *>(node)->batch_output_requested &&
               Executor::VectorizedMaterializeIterator::CanVectorize(node->original_path);
      case PlanNode::Type::SORT: {
        const auto *sort = static_cast<const Sort *>(node);
        return node->children.size() == 1 && self(self, node->children[0].get()) &&
               Executor::VectorizedSortIterator::CanVectorize(sort->filesort, sort->unwrap_rollup,
                                                              sort->tables_to_get_rowid_for) &&
               Executor::VectorizedSortIterator::CanVectorizeBatches(sort->filesort);
      }
      case PlanNode::Type::TOP_N: {
        const auto *top = static_cast<const TopN *>(node);
        if (top->offset != 0 || top->original_path == nullptr || node->children.size() != 1 ||
            !self(self, node->children[0].get()))
          return false;
        const auto &sort = top->original_path->sort();
        return Executor::VectorizedSortIterator::CanVectorize(top->filesort, sort.unwrap_rollup,
                                                              sort.tables_to_get_rowid_for) &&
               Executor::VectorizedSortIterator::CanVectorizeBatches(top->filesort);
      }
      case PlanNode::Type::FILTER: {
        const auto *filter = static_cast<const Filter *>(node);
        return node->children.size() == 1 && self(self, node->children[0].get()) &&
               (filter->condition == nullptr || !filter->condition->has_subquery());
      }
      case PlanNode::Type::LIMIT: {
        const auto *limit = static_cast<const Limit *>(node);
        return !limit->count_all_rows && !limit->reject_multiple_rows && node->children.size() == 1 &&
               self(self, node->children[0].get());
      }
      default:
        return false;
    }
  };
  // A legacy temp-table aggregate always stays vectorized: MySQL's counterpart groups
  // from metadata this conversion rewrites, so handed the Rapid child it returns the
  // ungrouped input rows.
  const bool temptable = temptable_src != nullptr;
  // A dropped grouping sort leaves no order for MySQL's aggregate to stream over: it
  // would start a new group on every row, so those plans can never hand back either.
  const bool native_fallback_safe = hash_output_order == nullptr;
  // Decide the transport before building the child: a native aggregate needs record
  // images, so the child must not install a batch iterator for it.
  const bool plan_batches = !children.empty() && children[0] != nullptr && can_batches(can_batches, children[0].get());
  // A hash aggregate that cannot keep its groups resident writes and re-reads every
  // row it spills, which costs more than MySQL's sorted-stream aggregate. Hand those
  // back: the child then builds in row mode, so no conversion is paid either side.
  const bool fits_budget = FitsHashMemoryBudget(thd);
  const PlanNode *input = children.empty() ? nullptr : children[0].get();
  const bool expanded_transport =
      input != nullptr &&
      (input->type() == PlanNode::Type::SORT || input->type() == PlanNode::Type::TOP_N ||
       input->type() == PlanNode::Type::STREAM || input->type() == PlanNode::Type::MATERIALIZE_DERIVED ||
       input->type() == PlanNode::Type::LIMIT || input->type() == PlanNode::Type::REF_LOOKUP ||
       input->type() == PlanNode::Type::NESTED_LOOP_JOIN);
  bool consumes_batches = true;
  if (expanded_transport && strategy == AggregateStrategy::STREAMING) {
    // A new batch-capable edge must not route DISTINCT/expression streaming
    // aggregates into an implementation that will immediately request rows.
    for (Item_func *function : aggregates) {
      auto *sum = down_cast<Item_sum *>(function);
      Item *argument = sum->arg_count == 0 ? nullptr : sum->get_arg(0)->real_item();
      Field *field = argument != nullptr && argument->type() == Item::FIELD_ITEM
                         ? down_cast<Item_field *>(argument)->field
                         : nullptr;
      if (sum->has_with_distinct()) consumes_batches = false;
      if (sum->sum_func() == Item_sum::COUNT_FUNC) {
        if (field == nullptr && sum->arg_count != 0 &&
            (argument == nullptr || !argument->const_item() || argument->is_null()))
          consumes_batches = false;
      } else if (field == nullptr || field->is_flag_set(UNSIGNED_FLAG) ||
                 (field->type() != MYSQL_TYPE_LONG && field->type() != MYSQL_TYPE_LONGLONG &&
                  field->type() != MYSQL_TYPE_FLOAT && field->type() != MYSQL_TYPE_DOUBLE &&
                  field->type() != MYSQL_TYPE_NEWDECIMAL) ||
                 (sum->sum_func() != Item_sum::SUM_FUNC && sum->sum_func() != Item_sum::MIN_FUNC &&
                  sum->sum_func() != Item_sum::MAX_FUNC && sum->sum_func() != Item_sum::AVG_FUNC)) {
        consumes_batches = false;
      }
    }
    if (input->original_path != nullptr) {
      for (Item *key : group_by) {
        Item *item = key->real_item();
        Field *field = item->type() == Item::FIELD_ITEM ? down_cast<Item_field *>(item)->field : nullptr;
        bool found = false;
        WalkTablesUnderAccessPath(
            input->original_path,
            [&](TABLE *table) {
              found = found || (field != nullptr && table == field->table &&
                                bitmap_is_set(table->read_set, field->field_index()));
              return false;
            },
            true);
        consumes_batches = consumes_batches && found;
      }
    }
  }
  // Only the edge feeding this aggregate changes transport; a row-mode child
  // propagates the request further itself (see Filter::ToAccessPath).
  const bool vectorize = temptable || !native_fallback_safe || (plan_batches && fits_budget && consumes_batches);
  if (!vectorize && !children.empty() && children[0] != nullptr) children[0]->row_mode = true;

  // Build the child before committing JOIN grouping metadata. If child
  // conversion fails, native fallback must see the untouched original JOIN.
  AccessPath *child_path = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  if (child_path == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid aggregate requires a valid child plan");
    return original_path;
  }

  // MySQL's cached grouping expressions describe the physical input slice.
  // In legacy plans GROUP BY may already refer to the output temporary table;
  // rebuilding the native iterator's cache from those Items merges all rows
  // into one group. Only Rapid's aggregate needs translated grouping metadata.
  if (vectorize && PrepareAggregateFields(thd, this)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid could not bind the aggregate GROUP BY expressions");
    return original_path;
  }

  auto *params = new (thd->mem_root) RapidAggregateParameters{};
  params->strategy = strategy;
  params->hash_output_order = hash_output_order;

  // Legacy unsorted GROUP BY: keep the temp-table plan shape so no Item/Field
  // rebinding is needed; VectorizedTemptableAggregateIterator fills that temp
  // table with vectorized group rows.
  if (temptable_src != nullptr) {
    const auto &tta = temptable_src->temptable_aggregate();
    auto *tpath = new (thd->mem_root) AccessPath();
    tpath->type = AccessPath::TEMPTABLE_AGGREGATE;
    tpath->temptable_aggregate().subquery_path = child_path;
    tpath->temptable_aggregate().temp_table_param = tta.temp_table_param;
    tpath->temptable_aggregate().table = tta.table;
    tpath->temptable_aggregate().table_path = tta.table_path;
    tpath->temptable_aggregate().ref_slice = tta.ref_slice;
    // `vectorize` is always true here because of the `temptable` term above.
    // TODO: relax it once the temp-table aggregate fills the temp table from chunks
    // itself; then the row-image check can apply to this shape too.
    tpath->vectorized = vectorize;
    tpath->secondary_engine_data = params;
    PropagateCostAndRows(this, tpath);
    return tpath;
  }

  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::AGGREGATE;
  path->aggregate().child = child_path;
  path->aggregate().olap = this->olap;

  path->vectorized = vectorize;
  path->secondary_engine_data = params;
  PropagateCostAndRows(this, path);
  return path;
}

bool LocalAgg::FitsHashMemoryBudget(THD *thd) const {
  // Only a grouping hash table scales with the group count.
  if (strategy != AggregateStrategy::HASH || is_global || group_by.empty()) return true;

  double input_rows = std::numeric_limits<double>::max();
  if (!children.empty() && children[0]) {
    const auto *child = children[0].get();
    const double rows = child->estimated_rows > 0 && child->estimated_rows != HA_POS_ERROR
                            ? static_cast<double>(child->estimated_rows)
                        : child->original_path ? child->original_path->num_output_rows()
                                               : -1.0;
    if (std::isfinite(rows) && rows >= 0) input_rows = rows;
  }
  double groups;
  if (!PlanningStatistics::Groups(thd, group_by, input_rows, &groups)) return true;
  double key_bytes = 0.0;
  for (const Item *item : group_by) key_bytes += static_cast<double>(item->max_length) + 1.0;

  const double group_bytes =
      key_bytes + static_cast<double>(Executor::VectorizedAggregateIterator::HashGroupFixedBytes(aggregates.size()));
  const double budget = static_cast<double>(ShannonBase::Utils::Util::hash_aggregate_memory_budget(
                            thd, Executor::VectorizedAggregateIterator::kDefaultHashMemoryLimit)) /
                        2.0;
  return groups * group_bytes <= budget;
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
  sort_path->sort().tables_to_get_rowid_for =
      this->original_path ? this->original_path->sort().tables_to_get_rowid_for : 0;

  // The flag marks the island, not this node: a sort that cannot be keyed must
  // still not cut the island, or the operators under it fall back to their
  // native form. Whether Rapid runs this sort is decided by CanVectorize()
  // where the iterator is built.
  sort_path->vectorized = AllChildrenVectorized({sort_path->sort().child});
  produces_batches = offset == 0 && sort_path->vectorized && !children.empty() && children[0]->produces_batches &&
                     Executor::VectorizedSortIterator::CanVectorize(filesort, sort_path->sort().unwrap_rollup,
                                                                    sort_path->sort().tables_to_get_rowid_for) &&
                     Executor::VectorizedSortIterator::CanVectorizeBatches(filesort);
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

AccessPath *WindowFunc::ToAccessPath(THD *thd) {
  if (original_path == nullptr) return nullptr;

  AccessPath *child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  // Nothing translated below: hand back MySQL's own path untouched rather than
  // publishing a WINDOW path with a null input.
  if (child == nullptr) return original_path;

  // Copy so every window parameter (the Window itself, the frame buffer temp
  // table, ref_slice, needs_buffering) is carried over verbatim; only the input
  // is rewired.
  auto *path = new (thd->mem_root) AccessPath(*original_path);
  path->window().child = child;
  // The copy inherited the original's iterator; a rebuilt path must construct
  // its own from the new child.
  path->iterator = nullptr;
  // The window itself is not SIMD-vectorized. Mark from the child so operators
  // above still see the input's real capability, as TopN does.
  path->vectorized = AllChildrenVectorized({child});
  path->secondary_engine_data = nullptr;
  return path;
}

std::string WindowFunc::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Window (native, vectorized input)";
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

  // The flag marks the island, not this node: a sort that cannot be keyed must
  // still not cut the island, or the operators under it fall back to their
  // native form. Whether Rapid runs this sort is decided by CanVectorize()
  // where the iterator is built.
  path->vectorized = AllChildrenVectorized({path->sort().child});
  produces_batches =
      path->vectorized && !children.empty() && children[0]->produces_batches &&
      Executor::VectorizedSortIterator::CanVectorize(path->sort().filesort, unwrap_rollup, tables_to_get_rowid_for) &&
      Executor::VectorizedSortIterator::CanVectorizeBatches(path->sort().filesort);
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
  produces_batches = path->vectorized && !count_all_rows && !reject_multiple_rows && !children.empty() &&
                     children[0]->produces_batches;
  path->secondary_engine_data = nullptr;
  PropagateCostAndRows(this, path);
  return path;
}

std::string Limit::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Limit (limit=" + std::to_string(limit) + ", offset=" + std::to_string(offset) + ")";
}

namespace {
/// Said when the path being replaced is not a zero-row path of its own, so EXPLAIN
/// still has a cause to print.
constexpr const char *kNoRowsCause = "no rows can match";

/** The cause the path being replaced already carries, for a node rebuilding one.
  Every core site that makes a zero-row path supplies a literal, so only a rebuild
  whose original was some other kind of path needs the fallback. */
const char *InheritedCause(const AccessPath *original) {
  if (original != nullptr) {
    if (original->type == AccessPath::ZERO_ROWS && original->zero_rows().cause != nullptr)
      return original->zero_rows().cause;
    if (original->type == AccessPath::ZERO_ROWS_AGGREGATED && original->zero_rows_aggregated().cause != nullptr)
      return original->zero_rows_aggregated().cause;
  }
  return kNoRowsCause;
}
}  // namespace

AccessPath *ZeroRows::ToAccessPath(THD *thd) {
  const char *cause = InheritedCause(original_path);
  // ZERO_ROWS_AGGREGATED is not equivalent to ZERO_ROWS or FAKE_SINGLE_ROW: its
  // iterator initializes aggregate expressions and emits the mandatory single row
  // for implicit grouping over an empty input.
  AccessPath *path;
  if (original_path != nullptr && original_path->type == AccessPath::ZERO_ROWS_AGGREGATED) {
    path = NewZeroRowsAggregatedAccessPath(thd, cause);
  } else if (this->rows_returned) {
    // A synthetic row examines none.
    path = NewFakeSingleRowAccessPath(thd, /*count_examined_rows=*/false);
  } else {
    path = NewZeroRowsAccessPath(thd, cause);
  }
  // Built through the core factories because AccessPath keeps its per-type
  // members in a union and the cause EXPLAIN prints unconditionally is one of
  // them: a path built here by hand left it uninitialized and printed nullptr.
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
  return RebuildMaterializeWithOperands(thd, original_path, inner_plans);
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
  AccessPath *path =
      RebuildMaterializeWithOperands(thd, original_path, inner_plans, !row_mode && batch_output_requested);
  produces_batches = path != nullptr && path->vectorized;
  return path;
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

AccessPath *RefLookup::ToAccessPath(THD *thd) {
  if (original_path == nullptr) return nullptr;
  auto *path = new (thd->mem_root) AccessPath(*original_path);
  path->iterator = nullptr;
  path->secondary_engine_data = nullptr;
  path->vectorized = !row_mode && Executor::SelectVectorizedLookup(thd, path);
  produces_batches = path->vectorized;
  PropagateCostAndRows(this, path);
  return path;
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
  if (original_path == nullptr) return pad + "→ MySQL (unbound)";
  return pad + "→ MySQL (" + std::to_string((int)original_path->type) + ")";
}

AccessPath *Stream::ToAccessPath(THD *thd) {
  if (original_path == nullptr || original_path->type != AccessPath::STREAM) return original_path;

  AccessPath *child = (children.size() == 1 && children[0] != nullptr) ? children[0]->ToAccessPath(thd) : nullptr;
  if (child == nullptr || !child->vectorized) {
    // Nothing below translated into a Rapid plan, so the rebuild would only
    // re-shape MySQL's own subtree. Keep the original island, all native.
    WalkAccessPaths(original_path, /*join=*/nullptr, WalkAccessPathPolicy::ENTIRE_TREE,
                    [](AccessPath *path, const JOIN *) {
                      path->vectorized = false;
                      return false;
                    });
    return original_path;
  }

  const auto &source = original_path->stream();
  AccessPath *path =
      NewStreamingAccessPath(thd, child, source.join, source.temp_table_param, source.table, source.ref_slice);
  path->stream().provide_rowid = source.provide_rowid;
  path->has_group_skip_scan = child->has_group_skip_scan;
  path->vectorized =
      !row_mode && children[0]->produces_batches &&
      Executor::VectorizedStreamIterator::CanVectorize(source.temp_table_param, source.table, source.provide_rowid);
  produces_batches = path->vectorized;
  PropagateCostAndRows(this, path);
  return path;
}

std::string Stream::ToString(int indent) const { return std::string(indent, ' ') + "→ Stream"; }

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> cb) {
  if (!node) return;
  cb(node);
  for (auto &child : node->children) {
    WalkPlan(child.get(), cb);
  }
}
}  // namespace Optimizer
}  // namespace ShannonBase
