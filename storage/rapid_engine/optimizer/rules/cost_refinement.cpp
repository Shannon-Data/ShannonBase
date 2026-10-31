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
#include "storage/rapid_engine/optimizer/rules/cost_refinement.h"
#include <algorithm>
#include <cmath>
#include "sql/item.h"
#include "sql/opt_trace.h"
#include "sql/sql_class.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/cost/planning_statistics.h"
#include "storage/rapid_engine/optimizer/optimizer.h"

namespace ShannonBase {
namespace Optimizer {
std::string CostRefinement::name() { return "CostRefinement"; }

void CostRefinement::apply(Plan &root) { Refine(current_thd, root.get()); }

bool CostRefinement::Refine(THD *thd, PlanNode *node) {
  if (!node) return false;

  bool child_refined{false};
  for (auto &child : node->children) {
    const bool refined = Refine(thd, child.get());
    if (node->children.size() == 1) child_refined = refined;
  }

  if (node->children.empty() || !node->children[0]) return false;

  if (node->type() != PlanNode::Type::LOCAL_AGGREGATE) {
    if (!child_refined) return false;
    if (node->type() == PlanNode::Type::STREAM) {
      node->estimated_rows = node->children[0]->estimated_rows;
      return true;
    }

    if (node->type() == PlanNode::Type::SORT && !static_cast<Sort *>(node)->remove_duplicates) {
      node->estimated_rows = std::min(node->children[0]->estimated_rows, static_cast<Sort *>(node)->limit);
      return true;
    }
    return false;
  }

  auto *aggregate = static_cast<LocalAgg *>(node);
  const auto *child = aggregate->children[0].get();
  double input_rows = child->estimated_rows;
  if (input_rows == HA_POS_ERROR || input_rows == 0) {
    input_rows = child->original_path ? child->original_path->num_output_rows() : -1;
  }

  if (!std::isfinite(input_rows) || input_rows < 0) return false;

  double groups{0.0};
  const char *source{nullptr};
  if (aggregate->group_by.empty()) {
    groups = 1;
    source = "implicit_global_group";
  } else {
    if (aggregate->olap != olap_type::UNSPECIFIED_OLAP_TYPE ||
        !PlanningStatistics::Groups(thd, aggregate->group_by, input_rows, &groups))
      return false;
    source = aggregate->group_by.size() == 1 ? "imcs_single_column_ndv" : "imcs_ndv_product";
  }

  if (!std::isfinite(groups) || groups >= static_cast<double>(HA_POS_ERROR)) return false;

  const auto previous = aggregate->estimated_rows;
  aggregate->estimated_rows = static_cast<ha_rows>(std::ceil(groups));

  if (thd && thd->opt_trace.is_started()) {
    Opt_trace_object trace(&thd->opt_trace);
    trace.add_alnum("decision", "rapid_cost_refinement");
    trace.add_alnum("source", source)
        .add("previous_groups", previous)
        .add("input_rows", input_rows)
        .add("estimated_groups", groups)
        .add("cost", aggregate->cost);
  }
  return true;
}
}  // namespace Optimizer
}  // namespace ShannonBase
