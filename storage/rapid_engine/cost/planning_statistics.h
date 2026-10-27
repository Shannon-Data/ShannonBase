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
#ifndef __SHANNON_PLANNING_STATISTICS_H__
#define __SHANNON_PLANNING_STATISTICS_H__
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "storage/rapid_engine/imcs/col0stats.h"

class THD;
class Field;
class Item;
namespace ShannonBase {
namespace Optimizer {
// Owned by the query context, shared by all aliases and physical alternatives.
// Never consults live resource-pool occupancy.
struct ColumnEstimate {
  double row_count{0}, non_null_distinct_values{0}, null_fraction{0};
  uint64_t version{0};
};

class PlanningStatistics {
 public:
  explicit PlanningStatistics(uint64_t query_id) : m_query_id(query_id) {}
  static bool Column(THD *thd, const Field *field, ColumnEstimate *result);
  static bool Predicate(THD *thd, Item *condition, double *selectivity);
  static bool Groups(THD *thd, const std::vector<Item *> &keys, double input_rows, double *groups);

 private:
  static PlanningStatistics *For(THD *thd);
  const Imcs::ColumnStatistics::PlanningSnapshot *Find(const Field *field);
  uint64_t m_query_id;
  std::unordered_map<const Imcs::ColumnStatistics *, Imcs::ColumnStatistics::PlanningSnapshot> m_columns;
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif
