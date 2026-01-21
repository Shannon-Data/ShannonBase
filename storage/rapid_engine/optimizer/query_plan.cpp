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

#include "sql/sql_lex.h"                      //query_expression
#include "storage/rapid_engine/imcs/table.h"  // RpdTable

namespace ShannonBase {
namespace Optimizer {
std::string ScanTable::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Vectorized Scan " + (rpd_table ? rpd_table->meta().table_name : "UNKNOWN") +
         (use_storage_index ? " [Pruned]" : "");
}

std::string Filter::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Filter (vectorized)";
}

std::string NestLoopJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Nested Loop Join (vectorized)";
}

std::string HashJoin::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Hash Join (vectorized)";
}

std::string LocalAgg::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Local Aggregate";
}

std::string GlobalAgg::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Global Aggregate";
}

std::string TopN::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Top-N (limit=" + std::to_string(limit) + ")";
}

std::string Limit::ToString(int indent) const {
  std::string pad(indent, ' ');
  return pad + "→ Limit (limit=" + std::to_string(limit) + ", offset=" + std::to_string(offset) + ")";
}

std::string ZeroRows::ToString(int indent) const { return "→ Zero Rows"; }

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> cb) {
  if (!node) return;
  cb(node);
  for (auto &child : node->children) {
    WalkPlan(child.get(), cb);
  }
}
}  // namespace Optimizer
}  // namespace ShannonBase