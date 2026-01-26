/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include "storage/rapid_engine/optimizer/path/access_path.h"

#include <assert.h>
#include <vector>

#include "sql/filesort.h"
#include "sql/pack_rows.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_update.h"

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/bka_iterator.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/delete_rows_iterator.h"
#include "sql/iterators/hash_join_iterator.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/row_iterator.h"
#include "sql/iterators/sorting_iterator.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/iterators/window_iterators.h"

#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/estimate_selectivity.h"
#include "sql/join_optimizer/overflow_bitset.h"
#include "sql/join_optimizer/relational_expression.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/mem_root_array.h"
#include "sql/pack_rows.h"
#include "sql/range_optimizer/geometry_index_range_scan.h"
#include "sql/range_optimizer/group_index_skip_scan.h"
#include "sql/range_optimizer/group_index_skip_scan_plan.h"
#include "sql/range_optimizer/index_merge.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/range_optimizer/index_skip_scan.h"
#include "sql/range_optimizer/index_skip_scan_plan.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/range_optimizer/reverse_index_range_scan.h"
#include "sql/range_optimizer/rowid_ordered_retrieval.h"

#include "storage/rapid_engine/executor/iterators/aggregate_iterator.h"
#include "storage/rapid_engine/executor/iterators/hash_join_iterator.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/executor/iterators/table_scan_iterator.h"
#include "storage/rapid_engine/optimizer/writable_access_path.inc"  //RapidScanParameters
#include "storage/rapid_engine/utils/utils.h"
namespace ShannonBase {
namespace Optimizer {
using pack_rows::TableCollection;
struct IteratorToBeCreated {
  AccessPath *path;
  JOIN *join;
  bool eligible_for_batch_mode;
  unique_ptr_destroy_only<RowIterator> *destination;
  Bounds_checked_array<unique_ptr_destroy_only<RowIterator>> children;

  void AllocChildren(MEM_ROOT *mem_root, int num_children) {
    children = Bounds_checked_array<unique_ptr_destroy_only<RowIterator>>::Alloc(mem_root, num_children);
  }
};

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *child, JOIN *join, bool eligible_for_batch_mode,
                          IteratorToBeCreated *job, Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the child, and we'll return to this job later.
  job->AllocChildren(mem_root, 1);
  todo->push_back(*job);
  todo->push_back({child, join, eligible_for_batch_mode, &job->children[0], {}});
}

void SetupJobsForChildren(MEM_ROOT *mem_root, AccessPath *outer, AccessPath *inner, JOIN *join,
                          bool inner_eligible_for_batch_mode, IteratorToBeCreated *job,
                          Mem_root_array<IteratorToBeCreated> *todo) {
  // Make jobs for the children, and we'll return to this job later.
  // Note that we push the inner before the outer job, so that we get
  // left created before right (invalidators in materialization access paths,
  // used in the old join optimizer, depend on this).
  job->AllocChildren(mem_root, 2);
  todo->push_back(*job);
  todo->push_back({inner, join, inner_eligible_for_batch_mode, &job->children[1], {}});
  todo->push_back({outer, join, false, &job->children[0], {}});
}

bool ShouldEnableBatchMode(AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_DISTANCE_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:
      if (path->filter().condition->has_subquery()) {
        return false;
      } else {
        return ShouldEnableBatchMode(path->filter().child);
      }
    case AccessPath::SORT:
      return ShouldEnableBatchMode(path->sort().child);
    case AccessPath::EQ_REF:
    case AccessPath::CONST_TABLE:
      // These can read only one row per scan, so batch mode will never be a
      // win (fall through).
    default:
      // All others, in particular joins.
      return false;
  }
}

static AccessPath *FindSingleAccessPathOfType(AccessPath *path, AccessPath::Type type) {
  AccessPath *found_path = nullptr;

  auto func = [type, &found_path](AccessPath *subpath, const JOIN *) {
#ifdef NDEBUG
    constexpr bool fast_exit = true;
#else
    constexpr bool fast_exit = false;
#endif
    if (subpath->type == type) {
      assert(found_path == nullptr);
      found_path = subpath;
      // If not in debug mode, stop as soon as we find the first one.
      if (fast_exit) {
        return true;
      }
    }
    return false;
  };
  // Our users generally want to stop at STREAM or MATERIALIZE nodes,
  // since they are table-oriented and those nodes have their own tables.
  WalkAccessPaths(path, /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION, func);
  return found_path;
}

static Prealloced_array<TABLE *, 4> GetUsedTables(AccessPath *child, bool include_pruned_tables) {
  Prealloced_array<TABLE *, 4> tables{PSI_NOT_INSTRUMENTED};
  WalkTablesUnderAccessPath(
      child,
      [&tables](TABLE *table) {
        tables.push_back(table);
        return false;
      },
      include_pruned_tables);
  return tables;
}

/**
  If the path is a FILTER path marked that subqueries are to be materialized,
  do so. If not, do nothing.

  It is important that this is not called until the entire plan is ready;
  not just when planning a single query block. The reason is that a query
  block A with materializable subqueries may itself be part of a materializable
  subquery B, so if one calls this when planning A, the subqueries in A will
  irrevocably be materialized, even if that is not the optimal plan given B.
  Thus, this is done when creating iterators.
 */
bool FinalizeMaterializedSubqueries(THD *thd, JOIN *join, AccessPath *path) {
  if (path->type != AccessPath::FILTER || !path->filter().materialize_subqueries) {
    return false;
  }
  return WalkItem(path->filter().condition, enum_walk::POSTFIX, [thd, join](Item *item) {
    if (!is_quantified_comp_predicate(item)) {
      return false;
    }
    Item_in_subselect *item_subs = down_cast<Item_in_subselect *>(item);
    if (item_subs->strategy == Subquery_strategy::SUBQ_MATERIALIZATION) {
      // This subquery is already set up for materialization.
      return false;
    }
    Query_block *qb = item_subs->query_expr()->first_query_block();
    if (!item_subs->subquery_allows_materialization(thd, qb, join->query_block)) {
      return false;
    }
    if (item_subs->finalize_materialization_transform(thd, qb->join)) {
      return true;
    }
    item_subs->create_iterators(thd);
    return false;
  });
}

/**
  Get the tables that are accessed by EQ_REF and can be on the inner side of an
  outer join. These need some extra care in AggregateIterator when handling
  NULL-complemented rows, so that the cache in EQRefIterator is not disturbed by
  AggregateIterator's switching between groups.
 */
static table_map GetNullableEqRefTables(const AccessPath *root_path) {
  table_map tables = 0;
  WalkAccessPaths(root_path, /*join=*/nullptr, WalkAccessPathPolicy::STOP_AT_MATERIALIZATION,
                  [&tables](const AccessPath *path, const JOIN *) {
                    if (path->type == AccessPath::EQ_REF) {
                      const auto &param = path->eq_ref();
                      if (param.table->is_nullable() && !param.ref->disable_cache) {
                        tables |= param.table->pos_in_table_list->map();
                      }
                    }
                    return false;
                  });
  return tables;
}

static RowIterator *FindSingleIteratorOfType(AccessPath *path, AccessPath::Type type) {
  AccessPath *found_path = FindSingleAccessPathOfType(path, type);
  if (found_path == nullptr) {
    return nullptr;
  } else {
    return found_path->iterator->real_iterator();
  }
}

/***************************************************************************************************************/
/* this function is identical with CreateIteratorFromAccessPath in sql join optimizer
 * but we replace some Iterators with vectorized execution supported iterators. and replace
 * the root iterator tree whith new one, which it is with vectorized iterator, such as vectorized scan table
 * hash join, etc.
 */
unique_ptr_destroy_only<RowIterator> PathGenerator::CreateIteratorFromAccessPath(THD *thd, MEM_ROOT *mem_root,
                                                                                 OptimizeContext *context,
                                                                                 AccessPath *top_path, JOIN *top_join,
                                                                                 bool top_eligible_for_batch_mode) {
  unique_ptr_destroy_only<RowIterator> ret;
  Mem_root_array<IteratorToBeCreated> todo(mem_root);
  todo.push_back({top_path, top_join, top_eligible_for_batch_mode, &ret, {}});

  // The access path trees can be pretty deep, and the stack frames can be big
  // on certain compilers/setups, so instead of explicit recursion, we push jobs
  // onto a MEM_ROOT-backed stack. This uses a little more RAM (the MEM_ROOT
  // typically lives to the end of the query), but reduces the stack usage
  // greatly.
  //
  // The general rule is that if an iterator requires any children, it will push
  // jobs for their access paths at the end of the stack and then re-push
  // itself. When the children are instantiated and we get back to the original
  // iterator, we'll actually instantiate it. (We distinguish between the two
  // cases on basis of whether job.children has been allocated or not; the child
  // iterator's destination will point into this array. The child list needs
  // to be allocated in a way that doesn't move around if the TODO job list
  // is reallocated, which we do by means of allocating it directly on the
  // MEM_ROOT.)
  while (!todo.empty()) {
    IteratorToBeCreated job = todo.back();
    todo.pop_back();

    AccessPath *path = job.path;
    JOIN *join = job.join;
    bool eligible_for_batch_mode = job.eligible_for_batch_mode;

    if (job.join != nullptr) {
      assert(!job.join->needs_finalize);
    }

    unique_ptr_destroy_only<RowIterator> iterator;

    ha_rows *examined_rows = nullptr;
    if (path->count_examined_rows && join != nullptr) {
      examined_rows = &join->examined_rows;
    }

    switch (path->type) {
      case AccessPath::TABLE_SCAN: {
        const auto &param = path->table_scan();
        if (path->secondary_engine_data) {
          auto rapid_scan_param [[maybe_unused]] = static_cast<RapidScanParameters *>(path->secondary_engine_data);
        }
        if (path->vectorized &&
            param.table->s->table_category ==
                enum_table_category::TABLE_CATEGORY_USER)  // Here param.table maybe a temp table/in-memory temp table.)
          iterator = NewIterator<ShannonBase::Executor::VectorizedTableScanIterator>(
              thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        else
          iterator = NewIterator<TableScanIterator>(thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_SCAN: {
        const auto &param = path->index_scan();
        if (param.reverse) {
          iterator = NewIterator<IndexScanIterator<true>>(thd, mem_root, param.table, param.idx, param.use_order,
                                                          path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<IndexScanIterator<false>>(thd, mem_root, param.table, param.idx, param.use_order,
                                                           path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::INDEX_DISTANCE_SCAN: {
        const auto &param = path->index_distance_scan();
        iterator = NewIterator<IndexDistanceScanIterator>(thd, mem_root, param.table, param.idx, param.range,
                                                          path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::REF: {
        const auto &param = path->ref();
        if (param.reverse) {
          iterator = NewIterator<RefIterator<true>>(thd, mem_root, param.table, param.ref, param.use_order,
                                                    path->num_output_rows(), examined_rows);
        } else {
          iterator = NewIterator<RefIterator<false>>(thd, mem_root, param.table, param.ref, param.use_order,
                                                     path->num_output_rows(), examined_rows);
        }
        break;
      }
      case AccessPath::REF_OR_NULL: {
        const auto &param = path->ref_or_null();
        iterator = NewIterator<RefOrNullIterator>(thd, mem_root, param.table, param.ref, param.use_order,
                                                  path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::EQ_REF: {
        const auto &param = path->eq_ref();
        iterator = NewIterator<EQRefIterator>(thd, mem_root, param.table, param.ref, examined_rows);
        break;
      }
      case AccessPath::PUSHED_JOIN_REF: {
        const auto &param = path->pushed_join_ref();
        iterator = NewIterator<PushedJoinRefIterator>(thd, mem_root, param.table, param.ref, param.use_order,
                                                      param.is_unique, examined_rows);
        break;
      }
      case AccessPath::FULL_TEXT_SEARCH: {
        const auto &param = path->full_text_search();
        iterator = NewIterator<FullTextSearchIterator>(thd, mem_root, param.table, param.ref, param.ft_func,
                                                       param.use_order, param.use_limit, examined_rows);
        break;
      }
      case AccessPath::CONST_TABLE: {
        const auto &param = path->const_table();
        iterator = NewIterator<ConstIterator>(thd, mem_root, param.table, param.ref, examined_rows);
        break;
      }
      case AccessPath::MRR: {
        const auto &param = path->mrr();
        const auto &bka_param = param.bka_path->bka_join();
        iterator = NewIterator<MultiRangeRowIterator>(thd, mem_root, param.table, param.ref, param.mrr_flags,
                                                      bka_param.join_type,
                                                      GetUsedTables(bka_param.outer, /*include_pruned_tables=*/true),
                                                      bka_param.store_rowids, bka_param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::FOLLOW_TAIL: {
        const auto &param = path->follow_tail();
        iterator = NewIterator<FollowTailIterator>(thd, mem_root, param.table, path->num_output_rows(), examined_rows);
        break;
      }
      case AccessPath::INDEX_RANGE_SCAN: {
        const auto &param = path->index_range_scan();
        TABLE *table = param.used_key_part[0].field->table;
        if (param.geometry) {
          iterator = NewIterator<GeometryIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
              param.reuse_handler, mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        } else if (param.reverse) {
          iterator = NewIterator<ReverseIndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(), param.index, mem_root, param.mrr_flags,
              Bounds_checked_array{param.ranges, param.num_ranges}, param.using_extended_key_parts);
        } else {
          iterator = NewIterator<IndexRangeScanIterator>(
              thd, mem_root, table, examined_rows, path->num_output_rows(), param.index, param.need_rows_in_rowid_order,
              param.reuse_handler, mem_root, param.mrr_flags, param.mrr_buf_size,
              Bounds_checked_array{param.ranges, param.num_ranges});
        }
        break;
      }
      case AccessPath::INDEX_MERGE: {
        const auto &param = path->index_merge();
        unique_ptr_destroy_only<RowIterator> pk_quick_select;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
          AccessPath *range_scan = (*param.children)[child_idx];
          if (param.allow_clustered_primary_key_scan && param.table->file->primary_key_is_clustered() &&
              range_scan->index_range_scan().index == param.table->s->primary_key) {
            assert(pk_quick_select == nullptr);
            pk_quick_select = std::move(job.children[child_idx]);
          } else {
            children.push_back(std::move(job.children[child_idx]));
          }
        }

        iterator = NewIterator<IndexMergeIterator>(thd, mem_root, mem_root, param.table, std::move(pk_quick_select),
                                                   std::move(children));
        break;
      }
      case AccessPath::ROWID_INTERSECTION: {
        const auto &param = path->rowid_intersection();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size() + (param.cpk_child != nullptr ? 1 : 0));
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          if (param.cpk_child != nullptr) {
            todo.push_back({param.cpk_child,
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[param.children->size()],
                            {}});
          }
          continue;
        }

        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
          children.push_back(std::move(job.children[child_idx]));
        }

        unique_ptr_destroy_only<RowIterator> cpk_child;
        if (param.cpk_child != nullptr) {
          cpk_child = std::move(job.children[param.children->size()]);
        }
        iterator = NewIterator<RowIDIntersectionIterator>(thd, mem_root, mem_root, param.table,
                                                          param.retrieve_full_rows, param.need_rows_in_rowid_order,
                                                          std::move(children), std::move(cpk_child));
        break;
      }
      case AccessPath::ROWID_UNION: {
        const auto &param = path->rowid_union();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
            todo.push_back({(*param.children)[child_idx],
                            join,
                            /*eligible_for_batch_mode=*/false,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        Mem_root_array<unique_ptr_destroy_only<RowIterator>> children(mem_root);
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator = NewIterator<RowIDUnionIterator>(thd, mem_root, mem_root, param.table, std::move(children));
        break;
      }
      case AccessPath::INDEX_SKIP_SCAN: {
        const IndexSkipScanParameters *param = path->index_skip_scan().param;
        iterator = NewIterator<IndexSkipScanIterator>(
            thd, mem_root, path->index_skip_scan().table, param->index_info, path->index_skip_scan().index,
            param->eq_prefix_len, param->eq_prefix_key_parts, param->eq_prefixes,
            path->index_skip_scan().num_used_key_parts, mem_root, param->has_aggregate_function, param->min_range_key,
            param->max_range_key, param->min_search_key, param->max_search_key, param->range_cond_flag,
            param->range_key_len);
        break;
      }
      case AccessPath::GROUP_INDEX_SKIP_SCAN: {
        const GroupIndexSkipScanParameters *param = path->group_index_skip_scan().param;
        iterator = NewIterator<GroupIndexSkipScanIterator>(
            thd, mem_root, path->group_index_skip_scan().table, &param->min_functions, &param->max_functions,
            param->have_agg_distinct, param->min_max_arg_part, param->group_prefix_len, param->group_key_parts,
            param->real_key_parts, param->max_used_key_length, param->index_info, path->group_index_skip_scan().index,
            param->key_infix_len, mem_root, param->is_index_scan, &param->prefix_ranges, &param->key_infix_ranges,
            &param->min_max_ranges);
        break;
      }
      case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
        const auto &param = path->dynamic_index_range_scan();
        iterator = NewIterator<DynamicRangeIterator>(thd, mem_root, param.table, param.qep_tab, examined_rows);
        break;
      }
      case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
        assert(join != nullptr);
        Query_block *query_block = join->query_block;
        iterator = NewIterator<TableValueConstructorIterator>(
            thd, mem_root, examined_rows, *query_block->row_value_list, path->table_value_constructor().output_refs);
        break;
      }
      case AccessPath::FAKE_SINGLE_ROW:
        iterator = NewIterator<FakeSingleRowIterator>(thd, mem_root, examined_rows);
        break;
      case AccessPath::ZERO_ROWS: {
        iterator = NewIterator<ZeroRowsIterator>(thd, mem_root, CollectTables(thd, path));
        break;
      }
      case AccessPath::ZERO_ROWS_AGGREGATED:
        iterator = NewIterator<ZeroRowsAggregatedIterator>(thd, mem_root, join, examined_rows);
        break;
      case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
        const auto &param = path->materialized_table_function();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializedTableFunctionIterator>(thd, mem_root, param.table_function, param.table,
                                                                  std::move(job.children[0]));
        break;
      }
      case AccessPath::UNQUALIFIED_COUNT:
        iterator = NewIterator<UnqualifiedCountIterator>(thd, mem_root, join);
        break;
      case AccessPath::NESTED_LOOP_JOIN: {
        const auto &param = path->nested_loop_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }

        iterator = NewIterator<NestedLoopIterator>(thd, mem_root, std::move(job.children[0]),
                                                   std::move(job.children[1]), param.join_type, param.pfs_batch_mode);
        break;
      }
      case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
        const auto &param = path->nested_loop_semijoin_with_duplicate_removal();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<NestedLoopSemiJoinWithDuplicateRemovalIterator>(
            thd, mem_root, std::move(job.children[0]), std::move(job.children[1]), param.table, param.key,
            param.key_len);
        break;
      }
      case AccessPath::BKA_JOIN: {
        const auto &param = path->bka_join();
        AccessPath *mrr_path = FindSingleAccessPathOfType(param.inner, AccessPath::MRR);
        if (job.children.is_null()) {
          mrr_path->mrr().bka_path = path;
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/false, &job, &todo);
          continue;
        }

        MultiRangeRowIterator *mrr_iterator = down_cast<MultiRangeRowIterator *>(mrr_path->iterator->real_iterator());
        iterator = NewIterator<BKAIterator>(
            thd, mem_root, std::move(job.children[0]), GetUsedTables(param.outer, /*include_pruned_tables=*/true),
            std::move(job.children[1]), thd->variables.join_buff_size, param.mrr_length_per_rec, param.rec_per_key,
            param.store_rowids, param.tables_to_get_rowid_for, mrr_iterator, param.join_type);
        break;
      }
      case AccessPath::HASH_JOIN: {
        const auto &param = path->hash_join();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.outer, param.inner, join,
                               /*inner_eligible_for_batch_mode=*/true, &job, &todo);
          continue;
        }
        const JoinPredicate *join_predicate = param.join_predicate;
        std::vector<HashJoinCondition> conditions;
        conditions.reserve(join_predicate->expr->equijoin_conditions.size());
        for (Item_eq_base *cond : join_predicate->expr->equijoin_conditions) {
          conditions.emplace_back(cond, thd->mem_root);
        }
        const Mem_root_array<Item *> *extra_conditions = GetExtraHashJoinConditions(
            mem_root, thd->lex->using_hypergraph_optimizer(), conditions, join_predicate->expr->join_conditions);
        if (extra_conditions == nullptr) return nullptr;
        const bool probe_input_batch_mode = eligible_for_batch_mode && ShouldEnableBatchMode(param.outer);
        double estimated_build_rows = param.inner->num_output_rows();
        if (param.inner->num_output_rows() < 0.0) {
          // Not all access paths may propagate their costs properly.
          // Choose a fairly safe estimate (it's better to be too large
          // than too small).
          estimated_build_rows = 1048576.0;
        }
        JoinType join_type{JoinType::INNER};
        switch (join_predicate->expr->type) {
          case RelationalExpression::INNER_JOIN:
          case RelationalExpression::STRAIGHT_INNER_JOIN:
            join_type = JoinType::INNER;
            break;
          case RelationalExpression::LEFT_JOIN:
            join_type = JoinType::OUTER;
            break;
          case RelationalExpression::ANTIJOIN:
            join_type = JoinType::ANTI;
            break;
          case RelationalExpression::SEMIJOIN:
            join_type = param.rewrite_semi_to_inner ? JoinType::INNER : JoinType::SEMI;
            break;
          case RelationalExpression::TABLE:
          default:
            assert(false);
        }
        // See if we can allow the hash table to keep its contents across Init()
        // calls.
        //
        // The old optimizer will sometimes push join conditions referring
        // to outer tables (in the same query block) down in under the hash
        // operation, so without analysis of each filter and join condition, we
        // cannot say for sure, and thus have to turn it off. But the hypergraph
        // optimizer sets parameter_tables properly, so we're safe if we just
        // check that.
        //
        // Regardless of optimizer, we can push outer references down in under
        // the hash, but join->hash_table_generation will increase whenever we
        // need to recompute the query block (in JOIN::clear_hash_tables()).
        //
        // TODO(sgunders): The old optimizer had a concept of _when_ to clear
        // derived tables (invalidators), and this is somehow similar. If it
        // becomes a performance issue, consider reintroducing them.
        //
        // TODO(sgunders): Should this perhaps be set as a flag on the access
        // path instead of being computed here? We do make the same checks in
        // the cost model, so perhaps it should set the flag as well.
        uint64_t *hash_table_generation = (thd->lex->using_hypergraph_optimizer() && path->parameter_tables == 0)
                                              ? &join->hash_table_generation
                                              : nullptr;

        const auto first_row_cost = [](const AccessPath &p) {
          return p.init_cost() + p.cost() / std::max(p.num_output_rows(), 1.0);
        };

        // If the probe (outer) input is empty, the join result will be empty,
        // and we do not need to read the build input. For inner join and
        // semijoin, the converse is also true. To benefit from this, we want to
        // start with the input where the cost of reading the first row is
        // lowest. (We only do this for Hypergraph, as the cost data for the
        // traditional optimizer are incomplete, and since we are reluctant to
        // change existing behavior.) Note that we always try the probe input
        // first for left join and antijoin.
        const HashJoinInput first_input =
            (thd->lex->using_hypergraph_optimizer() && first_row_cost(*param.inner) > first_row_cost(*param.outer))
                ? HashJoinInput::kProbe
                : HashJoinInput::kBuild;

        if (path->vectorized)
          iterator = NewIterator<ShannonBase::Executor::VectorizedHashJoinIterator>(
              thd, mem_root, std::move(job.children[1]), GetUsedTables(param.inner, /*include_pruned_tables=*/true),
              estimated_build_rows, std::move(job.children[0]),
              GetUsedTables(param.outer, /*include_pruned_tables=*/true), param.store_rowids,
              param.tables_to_get_rowid_for, thd->variables.join_buff_size, std::move(conditions),
              param.allow_spill_to_disk, join_type, *extra_conditions, first_input, probe_input_batch_mode,
              hash_table_generation);

        else
          iterator = NewIterator<HashJoinIterator>(
              thd, mem_root, std::move(job.children[1]), GetUsedTables(param.inner, /*include_pruned_tables=*/true),
              estimated_build_rows, std::move(job.children[0]),
              GetUsedTables(param.outer, /*include_pruned_tables=*/true), param.store_rowids,
              param.tables_to_get_rowid_for, thd->variables.join_buff_size, std::move(conditions),
              param.allow_spill_to_disk, join_type, *extra_conditions, first_input, probe_input_batch_mode,
              hash_table_generation);
        break;
      }
      case AccessPath::FILTER: {
        const auto &param = path->filter();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (FinalizeMaterializedSubqueries(thd, join, path)) {
          return nullptr;
        }
        iterator = NewIterator<FilterIterator>(thd, mem_root, std::move(job.children[0]), param.condition);
        break;
      }
      case AccessPath::SORT: {
        const auto &param = path->sort();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows num_rows_estimate =
            param.child->num_output_rows() < 0.0 ? HA_POS_ERROR : lrint(param.child->num_output_rows());
        Filesort *filesort = param.filesort;
        iterator = NewIterator<SortingIterator>(thd, mem_root, filesort, std::move(job.children[0]), num_rows_estimate,
                                                param.tables_to_get_rowid_for, examined_rows);
        if (filesort->m_remove_duplicates) {
          filesort->tables[0]->duplicate_removal_iterator = down_cast<SortingIterator *>(iterator->real_iterator());
        } else {
          filesort->tables[0]->sorting_iterator = down_cast<SortingIterator *>(iterator->real_iterator());
        }
        break;
      }
      case AccessPath::AGGREGATE: {
        const auto &param = path->aggregate();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        Prealloced_array<TABLE *, 4> tables = GetUsedTables(param.child, /*include_pruned_tables=*/true);

        if (path->vectorized)
          iterator = NewIterator<ShannonBase::Executor::VectorizedAggregateIterator>(
              thd, mem_root, std::move(job.children[0]), join,
              TableCollection(tables, /*store_rowids=*/false,
                              /*tables_to_get_rowid_for=*/0, GetNullableEqRefTables(param.child)),
              param.olap == ROLLUP_TYPE);
        else
          iterator = NewIterator<AggregateIterator>(
              thd, mem_root, std::move(job.children[0]), join,
              TableCollection(tables, /*store_rowids=*/false,
                              /*tables_to_get_rowid_for=*/0, GetNullableEqRefTables(param.child)),
              param.olap == ROLLUP_TYPE);
        break;
      }
      case AccessPath::TEMPTABLE_AGGREGATE: {
        const auto &param = path->temptable_aggregate();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.subquery_path,
                          join,
                          /*eligible_for_batch_mode=*/true,
                          &job.children[0],
                          {}});
          todo.push_back({param.table_path, join, eligible_for_batch_mode, &job.children[1], {}});
          continue;
        }

        // TODO: using vectorized temptable_aggregate_iterator to replace it.
        if (false)
          iterator = unique_ptr_destroy_only<RowIterator>(temptable_aggregate_iterator::CreateIterator(
              thd, std::move(job.children[0]), param.temp_table_param, param.table, std::move(job.children[1]), join,
              param.ref_slice));
        else
          iterator = unique_ptr_destroy_only<RowIterator>(temptable_aggregate_iterator::CreateIterator(
              thd, std::move(job.children[0]), param.temp_table_param, param.table, std::move(job.children[1]), join,
              param.ref_slice));
        break;
      }
      case AccessPath::LIMIT_OFFSET: {
        const auto &param = path->limit_offset();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        ha_rows *send_records = nullptr;
        if (param.send_records_override != nullptr) {
          send_records = param.send_records_override;
        } else if (join != nullptr) {
          send_records = &join->send_records;
        }
        iterator =
            NewIterator<LimitOffsetIterator>(thd, mem_root, std::move(job.children[0]), param.limit, param.offset,
                                             param.count_all_rows, param.reject_multiple_rows, send_records);
        break;
      }
      case AccessPath::STREAM: {
        const auto &param = path->stream();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, param.join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<StreamingIterator>(thd, mem_root, std::move(job.children[0]), param.temp_table_param,
                                                  param.table, param.provide_rowid, param.join, param.ref_slice);
        break;
      }
      case AccessPath::MATERIALIZE: {
        // The table access path should be a single iterator, not a tree.
        // (ALTERNATIVE counts as a single iterator in this regard.)
        assert(path->materialize().table_path->type == AccessPath::TABLE_SCAN ||
               path->materialize().table_path->type == AccessPath::LIMIT_OFFSET ||
               path->materialize().table_path->type == AccessPath::REF ||
               path->materialize().table_path->type == AccessPath::REF_OR_NULL ||
               path->materialize().table_path->type == AccessPath::EQ_REF ||
               path->materialize().table_path->type == AccessPath::ALTERNATIVE ||
               path->materialize().table_path->type == AccessPath::CONST_TABLE ||
               path->materialize().table_path->type == AccessPath::INDEX_SCAN ||
               path->materialize().table_path->type == AccessPath::INDEX_RANGE_SCAN ||
               path->materialize().table_path->type == AccessPath::DYNAMIC_INDEX_RANGE_SCAN);

        MaterializePathParameters *param = path->materialize().param;
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param->m_operands.size() + 1);
          todo.push_back(job);
          todo.push_back({path->materialize().table_path, join, eligible_for_batch_mode, &job.children[0], {}});
          for (size_t i = 0; i < param->m_operands.size(); ++i) {
            const MaterializePathParameters::Operand &from = param->m_operands[i];
            todo.push_back({from.subquery_path,
                            from.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[i + 1],
                            {}});
          }
          continue;
        }
        unique_ptr_destroy_only<RowIterator> table_iterator = std::move(job.children[0]);
        Mem_root_array<materialize_iterator::Operand> operands(thd->mem_root, param->m_operands.size());
        for (size_t i = 0; i < param->m_operands.size(); ++i) {
          const MaterializePathParameters::Operand &from = param->m_operands[i];
          materialize_iterator::Operand &to = operands[i];
          to.subquery_iterator = std::move(job.children[i + 1]);
          to.select_number = from.select_number;
          to.join = from.join;
          to.disable_deduplication_by_hash_field = from.disable_deduplication_by_hash_field;
          to.copy_items = from.copy_items;
          to.temp_table_param = from.temp_table_param;
          to.is_recursive_reference = from.is_recursive_reference;
          to.m_first_distinct = from.m_first_distinct;
          to.m_total_operands = from.m_total_operands;
          to.m_operand_idx = from.m_operand_idx;
          to.m_estimated_output_rows = from.subquery_path->num_output_rows();

          if (to.is_recursive_reference) {
            // Find the recursive reference to ourselves; there should be
            // exactly one, as per the standard.
            RowIterator *recursive_reader = FindSingleIteratorOfType(from.subquery_path, AccessPath::FOLLOW_TAIL);
            if (recursive_reader == nullptr) {
              // The recursive reference was optimized away, e.g. due to an
              // impossible WHERE condition, so we're not a recursive
              // reference after all.
              to.is_recursive_reference = false;
            } else {
              to.recursive_reader = down_cast<FollowTailIterator *>(recursive_reader);
            }
          }
        }
        JOIN *subjoin = param->ref_slice == -1 ? nullptr : operands[0].join;

        iterator = unique_ptr_destroy_only<RowIterator>(
            materialize_iterator::CreateIterator(thd, std::move(operands), param, std::move(table_iterator), subjoin));

        break;
      }
      case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
        const auto &param = path->materialize_information_schema_table();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.table_path, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<MaterializeInformationSchemaTableIterator>(thd, mem_root, std::move(job.children[0]),
                                                                          param.table_list, param.condition);
        break;
      }
      case AccessPath::APPEND: {
        const auto &param = path->append();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, param.children->size());
          todo.push_back(job);
          for (size_t child_idx = 0; child_idx < param.children->size(); ++child_idx) {
            const AppendPathParameters &child_param = (*param.children)[child_idx];
            todo.push_back({child_param.path,
                            child_param.join,
                            /*eligible_for_batch_mode=*/true,
                            &job.children[child_idx],
                            {}});
          }
          continue;
        }
        // TODO(sgunders): Consider just sending in the array here,
        // changing types in the constructor.
        std::vector<unique_ptr_destroy_only<RowIterator>> children;
        children.reserve(param.children->size());
        for (unique_ptr_destroy_only<RowIterator> &child : job.children) {
          children.push_back(std::move(child));
        }
        iterator = NewIterator<AppendIterator>(thd, mem_root, std::move(children));
        break;
      }
      case AccessPath::WINDOW: {
        const auto &param = path->window();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        if (param.needs_buffering) {
          iterator = NewIterator<BufferingWindowIterator>(thd, mem_root, std::move(job.children[0]),
                                                          param.temp_table_param, join, param.ref_slice);
        } else {
          iterator = NewIterator<WindowIterator>(thd, mem_root, std::move(job.children[0]), param.temp_table_param,
                                                 join, param.ref_slice);
        }
        break;
      }
      case AccessPath::WEEDOUT: {
        const auto &param = path->weedout();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<WeedoutIterator>(thd, mem_root, std::move(job.children[0]), param.weedout_table,
                                                param.tables_to_get_rowid_for);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES: {
        const auto &param = path->remove_duplicates();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesIterator>(thd, mem_root, std::move(job.children[0]), join,
                                                         param.group_items, param.group_items_size);
        break;
      }
      case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
        const auto &param = path->remove_duplicates_on_index();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<RemoveDuplicatesOnIndexIterator>(thd, mem_root, std::move(job.children[0]), param.table,
                                                                param.key, param.loosescan_key_len);
        break;
      }
      case AccessPath::ALTERNATIVE: {
        const auto &param = path->alternative();
        if (job.children.is_null()) {
          job.AllocChildren(mem_root, 2);
          todo.push_back(job);
          todo.push_back({param.child, join, eligible_for_batch_mode, &job.children[0], {}});
          todo.push_back({param.table_scan_path, join, eligible_for_batch_mode, &job.children[1], {}});
          continue;
        }
        iterator =
            NewIterator<AlternativeIterator>(thd, mem_root, param.table_scan_path->table_scan().table,
                                             std::move(job.children[0]), std::move(job.children[1]), param.used_ref);
        break;
      }
      case AccessPath::CACHE_INVALIDATOR: {
        const auto &param = path->cache_invalidator();
        if (job.children.is_null()) {
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<CacheInvalidatorIterator>(thd, mem_root, std::move(job.children[0]), param.name);
        break;
      }
      case AccessPath::DELETE_ROWS: {
        const auto &param = path->delete_rows();
        if (job.children.is_null()) {
          // Setting up tables for delete must be done before the child
          // iterators are created, as some of the child iterators need to see
          // the final read set when they are constructed, so doing it in
          // DeleteRowsIterator's constructor or Init() is too late.
          SetUpTablesForDelete(thd, join);
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = NewIterator<DeleteRowsIterator>(thd, mem_root, std::move(job.children[0]), join,
                                                   param.tables_to_delete_from, param.immediate_tables);
        break;
      }
      case AccessPath::UPDATE_ROWS: {
        const auto &param = path->update_rows();
        if (job.children.is_null()) {
          // Do the final setup for UPDATE before the child iterators are
          // created.
          if (FinalizeOptimizationForUpdate(join)) {
            return nullptr;
          }
          SetupJobsForChildren(mem_root, param.child, join, eligible_for_batch_mode, &job, &todo);
          continue;
        }
        iterator = CreateUpdateRowsIterator(thd, mem_root, join, std::move(job.children[0]));
        break;
      }
      case AccessPath::SAMPLE_SCAN: { /* LCOV_EXCL_LINE */
        // SampleScan can be executed only in the secondary engine.
        assert(false); /* LCOV_EXCL_LINE */
      }
    }

    if (iterator == nullptr) {
      return nullptr;
    }

    path->iterator = iterator.get();
    *job.destination = std::move(iterator);
  }
  return ret;
}

unique_ptr_destroy_only<RowIterator> PathGenerator::CreateIteratorFromAccessPath(THD *thd, OptimizeContext *context,
                                                                                 AccessPath *path, JOIN *join,
                                                                                 bool eligible_for_batch_mode) {
  return PathGenerator::CreateIteratorFromAccessPath(thd, thd->mem_root, context, path, join, eligible_for_batch_mode);
}

}  // namespace Optimizer
}  // namespace ShannonBase