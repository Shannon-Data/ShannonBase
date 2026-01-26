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

#include "sql/join_optimizer/access_path.h"                         // AccessPath
#include "sql/sql_lex.h"                                            // query_expression
#include "storage/rapid_engine/imcs/table.h"                        // RpdTable
#include "storage/rapid_engine/optimizer/writable_access_path.inc"  //Papid Params

namespace ShannonBase {
namespace Optimizer {
AccessPath *ScanTable::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::TABLE_SCAN;
  path->table_scan().table = this->source_table;
  if (scan_type == ScanType::FULL_TABLE_SCAN) path->vectorized = true;

  path->vectorized = this->vectorized;

  auto rapid_scan_params = new (thd->mem_root) ShannonBase::Optimizer::RapidScanParameters();
  rapid_scan_params->rpd_table = this->rpd_table;
  rapid_scan_params->use_storage_index = this->use_storage_index;
  rapid_scan_params->projected_columns = this->projected_columns;
  rapid_scan_params->limit = this->limit;
  rapid_scan_params->offset = this->offset;
  path->secondary_engine_data = rapid_scan_params;
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

  path->secondary_engine_data = nullptr;
  return path;
}

std::string Filter::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Filter (vectorized)";
}

AccessPath *NestLoopJoin::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::NESTED_LOOP_JOIN;
  path->nested_loop_join().join_type = JoinType::INNER;
  path->nested_loop_join().outer = (!children.empty() && children[0]) ? this->children[0]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().inner = (!children.empty() && children[1]) ? this->children[1]->ToAccessPath(thd) : nullptr;
  path->nested_loop_join().pfs_batch_mode = this->pfs_batch_mode;
  path->nested_loop_join().already_expanded_predicates = this->already_expanded_predicates;
  path->nested_loop_join().join_predicate = this->source_join_predicate;
  path->nested_loop_join().equijoin_predicates = this->equijoin_predicates;

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
  path->hash_join().allow_spill_to_disk = this->original_path->hash_join().allow_spill_to_disk;
  path->hash_join().join_predicate = this->original_path->hash_join().join_predicate;
  path->hash_join().store_rowids = this->original_path->hash_join().store_rowids;
  path->hash_join().rewrite_semi_to_inner = false;
  path->hash_join().tables_to_get_rowid_for = this->original_path->hash_join().tables_to_get_rowid_for;

  path->secondary_engine_data = nullptr;
  return path;
}

std::string HashJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Hash Join (vectorized)";
}

AccessPath *LocalAgg::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::AGGREGATE;

  path->aggregate().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->aggregate().olap = this->olap;

  path->secondary_engine_data = nullptr;
  return path;
}

std::string LocalAgg::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Local Aggregate";
}

AccessPath *GlobalAgg::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::AGGREGATE;
  path->secondary_engine_data = nullptr;
  return path;
}

std::string GlobalAgg::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Global Aggregate";
}

AccessPath *TopN::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::SORT;

  path->sort().child = (!children.empty() && children[0]) ? children[0]->ToAccessPath(thd) : nullptr;
  path->sort().order = this->order;
  path->sort().limit = this->limit;
  path->sort().filesort = this->filesort;
  path->sort().remove_duplicates = this->original_path ? this->original_path->sort().remove_duplicates : false;
  path->sort().unwrap_rollup = this->original_path ? this->original_path->sort().unwrap_rollup : false;
  path->sort().force_sort_rowids = this->original_path ? this->original_path->sort().force_sort_rowids : false;

  path->secondary_engine_data = nullptr;
  return path;
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

  path->secondary_engine_data = nullptr;
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

  path->secondary_engine_data = nullptr;
  return path;
}

std::string Limit::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Limit (limit=" + std::to_string(limit) + ", offset=" + std::to_string(offset) + ")";
}

AccessPath *ZeroRows::ToAccessPath(THD *thd) {
  auto *path = new (thd->mem_root) AccessPath();
  path->type = AccessPath::ZERO_ROWS;
  path->secondary_engine_data = nullptr;
  return path;
}

std::string ZeroRows::ToString(int indent) const { return "→ Zero Rows"; }

AccessPath *MySQLNative::ToAccessPath(THD *thd) { return original_path; }

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