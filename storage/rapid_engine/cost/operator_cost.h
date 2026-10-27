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

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_OPERATOR_COST_H__
#define __SHANNONBASE_OPERATOR_COST_H__

#include <algorithm>
#include <cmath>
#include <optional>

namespace ShannonBase {
namespace Optimizer {
// All fields use the same cost unit. Startup is paid before the first row;
// reusable is the subset of startup that survives a rescan. Cardinalities and
// physical ordering are deliberately outside this accounting contract.
struct OperatorCost {
  double startup{0};
  double streaming{0};
  double reusable{0};

  double total() const { return startup + streaming; }
  double rescan() const { return total() - reusable; }
  bool valid() const {
    return std::isfinite(total()) && std::isfinite(reusable) && startup >= 0 && streaming >= 0 && reusable >= 0 &&
           reusable <= startup;
  }
  static std::optional<OperatorCost> from(double total, double startup, double reusable) {
    OperatorCost cost{startup, total - startup, reusable};
    return cost.valid() ? std::optional<OperatorCost>(cost) : std::nullopt;
  }
};

inline std::optional<OperatorCost> LimitCost(const OperatorCost &child, double rows, double end, double offset,
                                             bool count_all_rows) {
  if (!child.valid() || !std::isfinite(rows) || rows < 0 || !std::isfinite(end) || end < 0 || !std::isfinite(offset) ||
      offset < 0)
    return std::nullopt;
  // end already includes OFFSET. Never add the two unsigned row limits.
  const double start_fraction = rows > 0 ? std::min(rows, offset) / rows : 0;
  const double end_fraction = rows > 0 ? std::min(rows, end) / rows : 0;
  const double startup = child.startup + start_fraction * child.streaming;
  const double total = count_all_rows ? child.total() : child.startup + end_fraction * child.streaming;
  return OperatorCost::from(total, std::min(startup, total), child.reusable);
}

inline std::optional<OperatorCost> MaterializeCost(double input_cost, double write_cost, double scan_cost,
                                                   bool rematerialize) {
  const double build = input_cost + write_cost;
  OperatorCost result{build, scan_cost, rematerialize ? 0 : build};
  return result.valid() && input_cost >= 0 && write_cost >= 0 ? std::optional<OperatorCost>(result) : std::nullopt;
}

inline std::optional<OperatorCost> NestedLoopCost(const OperatorCost &outer, const OperatorCost &inner,
                                                  double outer_rows) {
  if (!outer.valid() || !inner.valid() || !std::isfinite(outer_rows) || outer_rows < 0) return std::nullopt;
  if (outer_rows == 0) return outer;
  const double total = outer.total() + inner.total() + std::max(0.0, outer_rows - 1) * inner.rescan();
  const double startup = outer.startup + inner.startup;
  return OperatorCost::from(total, startup, outer.reusable + inner.reusable);
}

// Ingest all N rows even for K=1; K>N cannot cost more than a full sort.
inline double SortCpuWork(double rows, double retained_rows, double per_row_cost) {
  if (!std::isfinite(rows) || !std::isfinite(retained_rows) || rows < 0 || retained_rows < 0 ||
      !std::isfinite(per_row_cost) || per_row_cost < 0)
    return -1;

  if (rows == 0 || retained_rows == 0) return 0;
  return rows * (1 + std::log2(std::max(1.0, std::min(rows, retained_rows)))) * per_row_cost;
}
}  // namespace Optimizer
}  // namespace ShannonBase
#endif
