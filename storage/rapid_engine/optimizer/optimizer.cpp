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

   Copyright (c) 2023 - 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/hash_join_iterator.h"  //HashJoinIterator
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"                      //Query_expression
#include "sql/sql_optimizer.h"                //JOIN
#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/optimizer/rules/const_fold_rule.h"
#include "storage/rapid_engine/utils/utils.h"

#include "storage/rapid_engine/optimizer/path/access_path.h"
#include "storage/rapid_engine/populate/log_commons.h"

namespace ShannonBase {
namespace Optimizer {
Timer::Timer() { m_begin = std::chrono::steady_clock::now(); }
std::chrono::nanoseconds Timer::lap() {
  const auto now = std::chrono::steady_clock::now();
  const auto lap_duration = std::chrono::nanoseconds{now - m_begin};
  m_begin = now;
  return lap_duration;
}
std::string Timer::lap_formatted() {
  auto stream = std::stringstream{};
  return stream.str();
}

// ctor
Optimizer::Optimizer(std::shared_ptr<Query_expression> &expr, const std::shared_ptr<CostEstimator> &cost_estimator) {}

bool Optimizer::RapidEstimateJoinCostHGO(THD *thd, const JOIN &join, double *secondary_engine_cost) {
  *secondary_engine_cost = join.best_read * 0.8;
  // using Rapid Engine cost estimatino algorithm.
  return false;
}

bool Optimizer::CanPathBeVectorized(AccessPath *path) {
  if (path == nullptr) return true;

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN: {
      TABLE *table{nullptr};
      if (path->type == AccessPath::TABLE_SCAN)
        table = path->table_scan().table;
      else if (path->type == AccessPath::INDEX_SCAN)
        table = path->index_scan().table;
      bool is_secondary_engine = table->s->is_secondary_engine();
      bool is_loaded = shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
      bool has_sufficient_data = (table->file->stats.records >= SHANNON_VECTOR_WIDTH);
      return is_secondary_engine && is_loaded && has_sufficient_data;
    }
    case AccessPath::INDEX_RANGE_SCAN:  // INDEX_RANGE_SCAN: Can be vectorized with sufficient data
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return true;
    case AccessPath::FILTER:  // FILTER: Vectorizable if the child can be vectorized
      return true;
    case AccessPath::SORT:  // SORT: Can be vectorized for in-memory sorts
      return true;
    case AccessPath::AGGREGATE: {  // AGGREGATE: Vectorizable with sufficient output rows and child support
      return true;
    }
    case AccessPath::HASH_JOIN: {  // HASH_JOIN: Vectorizable under specific conditions
      auto &hash_join = path->hash_join();
      // Cannot vectorize if storing rowids or allowing disk spilling
      if (hash_join.store_rowids) return false;
      if (hash_join.allow_spill_to_disk) return false;
      return hash_join.join_predicate != nullptr;
    }
    case AccessPath::NESTED_LOOP_JOIN:  // NESTED_LOOP_JOIN: Generally not suitable for vectorization
      return false;
    case AccessPath::FOLLOW_TAIL:  // These access methods are inherently non-vectorizable
    case AccessPath::MRR:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::INDEX_MERGE:
      return false;
    default:  // Default conservative approach: assume vectorizable
      return true;
  }
}

bool Optimizer::CheckChildVectorization(AccessPath *child_path) {
  if (child_path == nullptr) return true;
  // Check current path.
  if (!CanPathBeVectorized(child_path)) return false;

  // recursive all children path.
  switch (child_path->type) {
    case AccessPath::HASH_JOIN:
      return CheckChildVectorization(child_path->hash_join().outer) &&
             CheckChildVectorization(child_path->hash_join().inner);
    case AccessPath::NESTED_LOOP_JOIN:
      return CheckChildVectorization(child_path->nested_loop_join().outer) &&
             CheckChildVectorization(child_path->nested_loop_join().inner);
    case AccessPath::FILTER:
      return CheckChildVectorization(child_path->filter().child);
    case AccessPath::SORT:
      return CheckChildVectorization(child_path->sort().child);
    case AccessPath::AGGREGATE:
      return CheckChildVectorization(child_path->aggregate().child);
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
      return true;
    default:
      return true;
  }
}

/**
 * @brief Builds customized access paths for secondary engine optimization
 *
 * Creates specialized AccessPath types (Vectorized Table Scan, GPU Join, etc.)
 * for secondary engine execution. The function examines each AccessPath type
 * and creates optimized versions when possible.
 *
 * Why not use original AccessPath directly?
 * - Future extensions may require custom AccessPath types (e.g., RapidAccessPath)
 * - Original AccessPath tree will be freed and replaced with rapid_path
 * - Enables specialized iterators in PathGenerator::CreateIteratorFromAccessPath
 *
 * @param context Optimization context with vectorization capabilities
 * @param path Input AccessPath to optimize
 * @param join JOIN structure for query context
 * @return AccessPath* Optimized AccessPath, or nullptr if no optimization applied
 */
AccessPath *Optimizer::OptimizeAndRewriteAccessPath(OptimizeContext *context, AccessPath *path, const JOIN *join) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN: {
      // create vectorized table scan if it can.
      context->can_vectorized = Optimizer::CanPathBeVectorized(path);
      if (path->vectorized == context->can_vectorized) return nullptr;  // has vectorized, not need the new AP.

      // create vectorized table scan if it can.
      auto rapid_path = new (current_thd->mem_root) AccessPath();
      rapid_path->vectorized = context->can_vectorized;
      rapid_path->type = AccessPath::TABLE_SCAN;
      rapid_path->count_examined_rows = true;
      rapid_path->table_scan().table = path->table_scan().table;
      rapid_path->iterator = nullptr;
      rapid_path->secondary_engine_data = path->secondary_engine_data;

      return rapid_path;
    } break;
    case AccessPath::INDEX_SCAN: {
    } break;
    case AccessPath::SAMPLE_SCAN: {
    } break;
    case AccessPath::REF: {
    } break;
    case AccessPath::REF_OR_NULL: {
    } break;
    case AccessPath::EQ_REF: {
    } break;
    case AccessPath::PUSHED_JOIN_REF: {
    } break;
    case AccessPath::FULL_TEXT_SEARCH: {
    } break;
    case AccessPath::CONST_TABLE: {
    } break;
    case AccessPath::MRR: {
    } break;
    case AccessPath::FOLLOW_TAIL: {
    } break;
    case AccessPath::INDEX_RANGE_SCAN: {
    } break;
    case AccessPath::INDEX_MERGE: {
    } break;
    case AccessPath::ROWID_INTERSECTION: {
    } break;
    case AccessPath::ROWID_UNION: {
    } break;
    case AccessPath::INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::GROUP_INDEX_SKIP_SCAN: {
    } break;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN: {
    } break;

    // Basic access paths that don't correspond to a specific table.
    case AccessPath::TABLE_VALUE_CONSTRUCTOR: {
    } break;
    case AccessPath::FAKE_SINGLE_ROW: {
    } break;
    case AccessPath::ZERO_ROWS: {
    } break;
    case AccessPath::ZERO_ROWS_AGGREGATED: {
    } break;
    case AccessPath::MATERIALIZED_TABLE_FUNCTION: {
    } break;
    case AccessPath::UNQUALIFIED_COUNT: {
    } break;

    // Joins.
    case AccessPath::NESTED_LOOP_JOIN: {
    } break;
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL: {
    } break;
    case AccessPath::BKA_JOIN: {
    } break;
    case AccessPath::HASH_JOIN: {
      auto hash_join = path->hash_join();
      if (hash_join.join_predicate != nullptr &&
          (hash_join.rewrite_semi_to_inner || hash_join.allow_spill_to_disk == false)) {
        context->can_vectorized = true;
      }
      if (hash_join.allow_spill_to_disk || hash_join.store_rowids) context->can_vectorized = false;
      if (path->vectorized == context->can_vectorized) return nullptr;

      auto rapid_path = new (current_thd->mem_root) AccessPath();
      rapid_path->vectorized = context->can_vectorized;
      rapid_path->type = AccessPath::HASH_JOIN;
      rapid_path->hash_join().outer = hash_join.outer;
      rapid_path->hash_join().inner = hash_join.inner;
      rapid_path->iterator = nullptr;

      return rapid_path;
    } break;

    // Composite access paths.
    case AccessPath::FILTER: {
    } break;
    case AccessPath::SORT: {
    } break;
    case AccessPath::AGGREGATE:
    case AccessPath::TEMPTABLE_AGGREGATE: {
      // due to OPTION_NO_CONST_TABLES is set, then in first round optimization, it don't do aggregation optimzation.
      // therefore, all sub queries offload to secondary engine, then do optimization in secondary engine.
      aggregate_evaluated outcome;
      if (join->tables_list && join->implicit_grouping &&
          optimize_aggregated_query(join->thd, join->query_block, *join->fields, join->where_cond, &outcome)) {
        DBUG_PRINT("error", ("Error from optimize_aggregated_query"));
        return nullptr;
      }
      if (outcome == AGGR_DELAYED) {
        return NewUnqualifiedCountAccessPath(join->thd);
      }

      // Check both data sufficiency AND child path vectorization support
      if (path->type == AccessPath::AGGREGATE && path->num_output_rows() == kUnknownRowCount) {
        EstimateAggregateCost(join->thd, path, join->query_block);
      }
      auto n_records = path->num_output_rows_before_filter;
      bool data_sufficient = ((size_t)n_records >= SHANNON_VECTOR_WIDTH);

      bool child_support =
          CheckChildVectorization((path->type == AccessPath::AGGREGATE) ? path->aggregate().child : nullptr);
      context->can_vectorized = data_sufficient && child_support;
      if (path->vectorized == context->can_vectorized) return nullptr;

      auto rapid_path = new (current_thd->mem_root) AccessPath();
      if (path->type == AccessPath::AGGREGATE) {
        rapid_path->vectorized = context->can_vectorized;
        rapid_path->type = AccessPath::AGGREGATE;
        rapid_path->aggregate().child = path->aggregate().child;
        rapid_path->aggregate().olap = path->aggregate().olap;
        rapid_path->has_group_skip_scan = path->has_group_skip_scan;
        rapid_path->set_num_output_rows(path->num_output_rows());
        rapid_path->iterator = path->iterator;
      } else if (path->type == AccessPath::TEMPTABLE_AGGREGATE) {
        rapid_path->vectorized = context->can_vectorized;
        rapid_path->type = AccessPath::TEMPTABLE_AGGREGATE;
        rapid_path->temptable_aggregate().subquery_path = path->temptable_aggregate().subquery_path;
        rapid_path->temptable_aggregate().temp_table_param = path->temptable_aggregate().temp_table_param;
        rapid_path->temptable_aggregate().table = path->temptable_aggregate().table;
        rapid_path->temptable_aggregate().table_path = path->temptable_aggregate().table_path;
        rapid_path->temptable_aggregate().ref_slice = path->temptable_aggregate().ref_slice;
      } else
        assert(false);
      return rapid_path;
    } break;
    case AccessPath::LIMIT_OFFSET: {
    } break;
    case AccessPath::STREAM: {
    } break;
    case AccessPath::MATERIALIZE: {
    } break;
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE: {
    } break;
    case AccessPath::APPEND: {
    } break;
    case AccessPath::WINDOW: {
    } break;
    case AccessPath::WEEDOUT: {
    } break;
    case AccessPath::REMOVE_DUPLICATES: {
    } break;
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX: {
    } break;
    case AccessPath::ALTERNATIVE: {
    } break;
    case AccessPath::CACHE_INVALIDATOR: {
    } break;

    // Access paths that modify tables.
    case AccessPath::DELETE_ROWS: {
    } break;
    case AccessPath::UPDATE_ROWS: {
    } break;
    default:
      break;
  }

  return nullptr;
}

}  // namespace Optimizer
}  // namespace ShannonBase