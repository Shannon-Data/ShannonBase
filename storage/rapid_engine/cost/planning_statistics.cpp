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
#include "storage/rapid_engine/cost/planning_statistics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "sql/item_cmpfunc.h"
#include "sql/sql_class.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/optimizer/utils.h"

namespace ShannonBase {
namespace Optimizer {
PlanningStatistics *PlanningStatistics::For(THD *thd) {
  if (!thd || !thd->lex) return nullptr;

  auto *context = static_cast<Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());
  if (!context) return nullptr;

  if (!context->m_planning_statistics ||
      context->m_planning_statistics->m_query_id != static_cast<uint64_t>(thd->query_id)) {
    try {
      context->m_planning_statistics = std::make_shared<PlanningStatistics>(thd->query_id);
    } catch (const std::bad_alloc &) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return nullptr;
    }
  }
  return context->m_planning_statistics.get();
}

const Imcs::ColumnStatistics::PlanningSnapshot *PlanningStatistics::Find(const Field *field) {
  if (!field || !field->table) return nullptr;

  auto *table = Utils::rpd_lookup_func()(field->table);
  if (!table) return nullptr;

  const auto *stats = table->get_column_stats(field->field_index());
  if (!stats) return nullptr;

  auto found = m_columns.find(stats);
  if (found == m_columns.end()) {
    try {
      found = m_columns.emplace(stats, stats->snapshot_for_costing()).first;
    } catch (const std::bad_alloc &) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return nullptr;
    }
  }
  return &found->second;
}

bool PlanningStatistics::Column(THD *thd, const Field *field, ColumnEstimate *result) {
  if (!result) return false;

  auto *planning = For(thd);
  const auto *stats = planning ? planning->Find(field) : nullptr;
  if (!stats || stats->counters.row_count == 0) return false;

  const auto non_null = stats->counters.row_count - stats->counters.null_count;
  // NDV zero on a populated non-null column means statistics are unfinished.
  if (non_null && stats->ndv == 0) return false;
  *result = {static_cast<double>(stats->counters.row_count), static_cast<double>(stats->ndv),
             static_cast<double>(stats->counters.null_count) / stats->counters.row_count, stats->version};
  return true;
}

bool PlanningStatistics::Groups(THD *thd, const std::vector<Item *> &keys, double input_rows, double *groups) {
  if (!groups || !std::isfinite(input_rows) || input_rows < 0 || keys.empty()) return false;

  double estimate{1};
  for (size_t i = 0; i < keys.size(); ++i) {
    Item *item = keys[i] ? keys[i]->real_item() : nullptr;
    if (!item || item->type() != Item::FIELD_ITEM) return false;
    const Field *field = static_cast<Item_field *>(item)->field;
    bool duplicate{false};
    for (size_t j = 0; j < i; ++j) {
      Item *previous = keys[j]->real_item();
      duplicate |= previous->type() == Item::FIELD_ITEM && static_cast<Item_field *>(previous)->field == field;
    }
    if (duplicate) continue;

    ColumnEstimate column;
    if (!Column(thd, field, &column)) return false;
    // Product is an independence estimate, capped by input cardinality. Do
    // not add a sqrt(input) floor that invents groups absent from the keys.
    const bool has_null = column.null_fraction > 0 || field->table->is_nullable();
    const double ndv = column.non_null_distinct_values + (has_null ? 1.0 : 0.0);
    estimate = std::min(input_rows, estimate * ndv);
  }
  *groups = estimate;
  return true;
}

bool PlanningStatistics::Predicate(THD *thd, Item *condition, double *selectivity) {
  // Local field/constant comparisons only. Join equalities retain the SQL
  // optimizer's common-NDV and multiple-equality consistency rules.
  if (!condition || !selectivity || condition->type() != Item::FUNC_ITEM) return false;

  auto *function = static_cast<Item_func *>(condition);
  if (function->argument_count() != 2) return false;

  auto comparison = function->functype();
  switch (comparison) {
    case Item_func::EQ_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::GE_FUNC:
      break;
    default:
      return false;
  }

  Item *left = function->arguments()[0]->real_item();
  Item *right = function->arguments()[1]->real_item();
  bool reversed{false};
  if (left->const_item() && right->type() == Item::FIELD_ITEM) {
    std::swap(left, right);
    reversed = true;
  }
  if (left->type() != Item::FIELD_ITEM || !right->const_item() || right->is_null()) return false;

  const Field *field = static_cast<Item_field *>(left)->field;
  ColumnEstimate column;
  if (!Column(thd, field, &column)) return false;

  if (reversed) {
    switch (comparison) {
      case Item_func::LT_FUNC:
        comparison = Item_func::GT_FUNC;
        break;
      case Item_func::LE_FUNC:
        comparison = Item_func::GE_FUNC;
        break;
      case Item_func::GT_FUNC:
        comparison = Item_func::LT_FUNC;
        break;
      case Item_func::GE_FUNC:
        comparison = Item_func::LE_FUNC;
        break;
      default:
        break;
    }
  }
  const double non_null = 1.0 - column.null_fraction;
  const auto type = right->result_type();
  const bool numeric = (type == INT_RESULT || type == REAL_RESULT || type == DECIMAL_RESULT) &&
                       (field->result_type() == INT_RESULT || field->result_type() == REAL_RESULT ||
                        field->result_type() == DECIMAL_RESULT) &&
                       !is_temporal_type(field->type());
  if (!numeric) {
    // String equality may use NDV, but ranges require collation-aware stats.
    if (comparison != Item_func::EQ_FUNC || field->result_type() != STRING_RESULT || type != STRING_RESULT ||
        is_temporal_type(field->type()) || column.non_null_distinct_values == 0)
      return false;

    *selectivity = non_null / column.non_null_distinct_values;
    return true;
  }

  const double value = right->val_real();
  if (right->null_value || !std::isfinite(value) || thd->is_error()) return false;

  auto *planning = For(thd);
  const auto *stats = planning ? planning->Find(field) : nullptr;
  if (!stats) return false;

  double estimate{0.0};
  if (comparison == Item_func::EQ_FUNC) {
    if (column.non_null_distinct_values == 0)
      estimate = 0;
    else if (stats->histogram)
      estimate = non_null * stats->histogram->estimate_equality_selectivity(value);
    else
      estimate = non_null / column.non_null_distinct_values;
  } else {
    double lower = stats->minimum, upper = stats->maximum;
    switch (comparison) {
      case Item_func::LT_FUNC:
        upper = std::nextafter(value, -std::numeric_limits<double>::infinity());
        break;
      case Item_func::LE_FUNC:
        upper = value;
        break;
      case Item_func::GT_FUNC:
        lower = std::nextafter(value, std::numeric_limits<double>::infinity());
        break;
      case Item_func::GE_FUNC:
        lower = value;
        break;
      default:
        return false;
    }
    lower = std::max(lower, stats->minimum);
    upper = std::min(upper, stats->maximum);
    if (lower > upper)
      estimate = 0;
    else if (stats->histogram)
      estimate = non_null * stats->histogram->estimate_selectivity(lower, upper);
    else if (stats->maximum == stats->minimum)
      estimate = non_null;
    else
      estimate = non_null * (upper - lower) / (stats->maximum - stats->minimum);
  }
  if (!std::isfinite(estimate)) return false;

  *selectivity = std::clamp(estimate, 0.0, 1.0);
  return true;
}
}  // namespace Optimizer
}  // namespace ShannonBase
