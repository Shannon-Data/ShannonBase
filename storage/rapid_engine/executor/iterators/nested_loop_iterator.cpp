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

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/executor/iterators/nested_loop_iterator.h"

#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/opt_trace.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/optimizer/utils.h"

namespace ShannonBase {
namespace Executor {
bool VectorizedNestedLoopIterator::CanVectorize(AccessPath *path) {
  bool supported = path && path->type == AccessPath::NESTED_LOOP_JOIN;
  if (!supported) return false;
  const JoinType type = path->nested_loop_join().join_type;
  if (type != JoinType::INNER && type != JoinType::OUTER && type != JoinType::SEMI && type != JoinType::ANTI)
    return false;
  size_t tables = 0;
  WalkTablesUnderAccessPath(
      path,
      [&](TABLE *table) {
        if (!table || !table->s || !table->read_set || ++tables > 64) {
          supported = false;
          return false;
        }
        for (uint i = 0; i < table->s->fields; ++i) {
          if (!bitmap_is_set(table->read_set, i)) continue;
          Field *field = table->field[i];
          if (!field || Utils::IsOffPageField(field) ||
              (Utils::Util::normalized_length(field) != field->pack_length() && !RecordDictionary(field)))
            supported = false;
          // A four-byte CHAR/VARCHAR still holds SQL bytes, not a dictionary ID.
          if (field && Utils::Util::is_string(field->type()) && field->real_type() != MYSQL_TYPE_ENUM &&
              field->real_type() != MYSQL_TYPE_SET && !RecordDictionary(field))
            supported = false;
        }
        return false;
      },
      true);
  return supported;
}

bool SelectVectorizedNestedLoop(THD *thd, AccessPath *path, bool batch_consumer) {
  if (!thd || !VectorizedNestedLoopIterator::CanVectorize(path)) return false;
  bool force = false;
  DBUG_EXECUTE_IF("rapid_disable_vectorized_nlj", { return false; });
  DBUG_EXECUTE_IF("rapid_force_vectorized_nlj", { force = true; });
  if (!batch_consumer && !force) return false;
  double columns = 0, strings = 0, width = 0, record_width = 0;
  WalkTablesUnderAccessPath(
      path,
      [&](TABLE *table) {
        record_width += table->s->null_bytes + 1;
        for (uint i = 0; i < table->s->fields; ++i) {
          if (!bitmap_is_set(table->read_set, i)) continue;
          const Field *field = table->field[i];
          ++columns;
          width += Utils::Util::normalized_length(field);
          record_width += field->pack_length();
          strings += RecordDictionary(field) != nullptr;
        }
        return false;
      },
      true);
  using NestedLoopBatchEstimator = ShannonBase::Optimizer::RpdCostEstimator::NestedLoopBatchEstimator;
  using ShannonBase::Optimizer::RapidCostConstants;
  const double budget = std::min<size_t>(Utils::Util::hash_join_memory_budget(thd), 2 * 1024 * 1024);
  const auto work = NestedLoopBatchEstimator::estimate(
      path->num_output_rows(), columns, strings, width, record_width, budget, RapidCostConstants::kHashBuildPerRow,
      RapidCostConstants::kVectorDecompCostPerCell, RapidCostConstants::kHashLookupPerRow,
      RapidCostConstants::kArtRangeSetupCost, RapidCostConstants::kHashBuildCopyPerByte);
  const bool selected = force || work.choose_batches();
  Opt_trace_object trace(&thd->opt_trace, "rapid_nested_loop_transport");
  trace.add("batch_consumer", batch_consumer)
      .add("estimates_valid", work.valid)
      .add("output_rows", path->num_output_rows())
      .add("projected_columns", columns)
      .add("dictionary_columns", strings)
      .add("retained_bytes", work.retained_bytes)
      .add("budget_bytes", budget)
      .add("scalar_cost", work.scalar)
      .add("batch_cost", work.batches)
      .add("chosen_batches", selected)
      .add("forced_for_test", force);
  return selected;
}

VectorizedNestedLoopIterator::VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                                                           unique_ptr_destroy_only<RowIterator> inner, JoinType type,
                                                           bool pfs_batch_mode,
                                                           const Prealloced_array<TABLE *, 4> &tables, size_t budget,
                                                           double estimate)
    : VectorizedRecordIterator(
          thd,
          NewIterator<NestedLoopIterator>(thd, thd->mem_root, std::move(outer), std::move(inner), type, pfs_batch_mode),
          tables, budget, estimate, "nested_loop_join", ResMgmt::QueryMemoryOperator::kNestedLoop) {}
}  // namespace Executor
}  // namespace ShannonBase
