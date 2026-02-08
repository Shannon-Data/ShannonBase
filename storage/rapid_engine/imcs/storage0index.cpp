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

   The fundmental code for imcs.

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/imcs/storage0index.h"

#include <limits.h>

namespace ShannonBase {
namespace Imcs {
void StorageIndex::update(uint32 col_idx, double value) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];
  stats.sum.fetch_add(value, std::memory_order_relaxed);

  double current_min = stats.min_value.load(std::memory_order_relaxed);
  while (value < current_min) {
    if (stats.min_value.compare_exchange_weak(current_min, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }

  double current_max = stats.max_value.load(std::memory_order_relaxed);
  while (value > current_max) {
    if (stats.max_value.compare_exchange_weak(current_max, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }
}

void StorageIndex::update_null(uint32 col_idx) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];
  stats.null_count.fetch_add(1, std::memory_order_relaxed);
  stats.has_null.store(true, std::memory_order_relaxed);
  m_dirty.store(true, std::memory_order_relaxed);
}

void StorageIndex::rebuild(const Imcu *imcu) {}

const StorageIndex::ColumnStats *StorageIndex::get_column_stats_snapshot(uint32 col_idx) const {
  if (col_idx >= m_num_columns) return nullptr;

  std::shared_lock lock(m_mutex);
  return &m_column_stats[col_idx];
}

bool StorageIndex::can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const {
  if (predicates.empty()) return false;  // No predicates, cannot skip

  // Top-level predicates are implicitly ANDed together
  // Can skip if ANY top-level predicate allows skipping
  for (const auto &pred_ptr : predicates) {
    if (!pred_ptr) continue;

    if (can_skip_predicate(pred_ptr.get())) {
      DBUG_PRINT("storage_index", ("IMCU can be skipped due to predicate: %s", pred_ptr->to_string().c_str()));
      return true;  // Found a predicate that guarantees no matches
    }
  }
  return false;  // Cannot definitively skip this IMCU
}

bool StorageIndex::can_skip_predicate(const Predicate *pred) const {
  if (!pred) return false;

  // Dispatch based on predicate type
  if (pred->is_compound()) {
    return can_skip_compound_predicate(static_cast<const Compound_Predicate *>(pred));
  } else {
    return can_skip_simple_predicate(static_cast<const Simple_Predicate *>(pred));
  }
}

bool StorageIndex::can_skip_simple_predicate(const Simple_Predicate *pred) const {
  if (!pred) return false;

  uint32 col_idx = pred->column_id;
  // Validate column index
  if (col_idx >= m_num_columns) return false;
  if (pred->value.type == PredicateValueType::INT64 || pred->value.type == PredicateValueType::DOUBLE) {
    const double min_val = get_min_value(col_idx);
    const double max_val = get_max_value(col_idx);
    const bool has_nulls = get_has_null(col_idx);
    const size_t null_cnt = get_null_count(col_idx);
    const size_t total_rows = m_num_columns;
    // Evaluate based on operator
    switch (pred->op) {
      case PredicateOperator::EQUAL: {
        // pattern: col = value: Can skip if value is outside [min, max] range
        double target = pred->value.as_double();
        if (target < min_val || target > max_val) return true;
      } break;
      case PredicateOperator::NOT_EQUAL: {
        // col != value: Can skip only if ALL values equal the target (min == max == value)
        double target = pred->value.as_double();
        if (are_equal(min_val, max_val) && are_equal(min_val, target)) return true;
      } break;
      case PredicateOperator::GREATER_THAN: {
        // col > value: Can skip if max_val <= value (all values too small)
        double target = pred->value.as_double();
        if (max_val <= target) return true;
      } break;
      case PredicateOperator::GREATER_EQUAL: {
        // col >= value: Can skip if max_val < value
        double target = pred->value.as_double();
        if (max_val < target) return true;
      } break;
      case PredicateOperator::LESS_THAN: {
        // col < value: Can skip if min_val >= value (all values too large)
        double target = pred->value.as_double();
        if (min_val >= target) return true;
      } break;
      case PredicateOperator::LESS_EQUAL: {
        // col <= value: Can skip if min_val > value
        double target = pred->value.as_double();
        if (min_val > target) return true;
      } break;
      case PredicateOperator::BETWEEN: {
        // col BETWEEN min_value AND max_value
        // Can skip if ranges don't overlap
        double lower = pred->value.as_double();
        double upper = pred->value2.as_double();
        if (max_val < lower || min_val > upper) return true;
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // col NOT BETWEEN min_value AND max_value: Can skip if entire IMCU range is within the BETWEEN range
        double lower = pred->value.as_double();
        double upper = pred->value2.as_double();
        if (min_val >= lower && max_val <= upper) return true;
      } break;
      case PredicateOperator::IN: {
        // col IN (val1, val2, val3, ...) : Can skip if all values in the IN list are outside [min, max]
        bool all_outside = true;
        for (const auto &val : pred->value_list) {
          double target = val.as_double();
          if (target >= min_val && target <= max_val) {
            all_outside = false;
            break;
          }
        }
        if (all_outside) return true;
      } break;
      case PredicateOperator::NOT_IN: {
        // col NOT IN (val1, val2, val3, ...)
        // Can skip only if IMCU contains exactly the excluded values
        // This is rare, so we conservatively return false
        // (Too complex to determine precisely with min/max only)
      } break;
      case PredicateOperator::IS_NULL: {
        // col IS NULL: Can skip if no NULL values exist
        if (!has_nulls) return true;
      } break;
      case PredicateOperator::IS_NOT_NULL: {
        // col IS NOT NULL: Can skip if ALL values are NULL
        if (null_cnt >= total_rows && total_rows > 0) return true;
      } break;
      case PredicateOperator::LIKE:
      case PredicateOperator::NOT_LIKE:
      case PredicateOperator::REGEXP:
      case PredicateOperator::NOT_REGEXP: {
        // String pattern matching - cannot reliably skip with min/max
        // Would need more advanced string statistics (prefix trees, etc.)
        DBUG_PRINT("storage_index", ("String pattern predicate, cannot use Storage Index"));
        break;
      }
      default:
        DBUG_PRINT("storage_index", ("Unknown operator %d, cannot use Storage Index", static_cast<int>(pred->op)));
        break;
    }
  } else {
    // TODO:
  }
  return false;  // Cannot skip
}

bool StorageIndex::can_skip_compound_predicate(const Compound_Predicate *pred) const {
  if (!pred || pred->children.empty()) return false;

  switch (pred->op) {
    case PredicateOperator::AND: {
      // Logical AND: (pred1 AND pred2 AND pred3)
      // Can skip if ANY child predicate allows skipping
      // Because if ANY conjunct is false, entire AND is false
      for (const auto &child : pred->children) {
        if (can_skip_predicate(child.get())) {
          DBUG_PRINT("storage_index", ("AND: child predicate allows skip: %s", child->to_string().c_str()));
          return true;  // One false conjunct → entire AND is false
        }
      }
      DBUG_PRINT("storage_index", ("AND: no child predicate allows skip, cannot skip"));
      return false;  // All children might have matches
    } break;
    case PredicateOperator::OR: {
      // Logical OR: (pred1 OR pred2 OR pred3)
      // Can skip only if ALL child predicates allow skipping
      // Because if ANY disjunct is true, entire OR is true
      for (const auto &child : pred->children) {
        if (!can_skip_predicate(child.get())) {
          DBUG_PRINT("storage_index", ("OR: child predicate prevents skip: %s", child->to_string().c_str()));
          return false;  // One child might have matches → cannot skip
        }
      }
      DBUG_PRINT("storage_index", ("OR: all child predicates allow skip, SKIP"));
      return true;  // All disjuncts are false → entire OR is false
    } break;
    case PredicateOperator::NOT: {
      // Logical NOT: NOT(pred)
      // Can skip if child predicate would NOT allow skipping
      // (Inverses the logic)
      if (pred->children.size() != 1) {
        DBUG_PRINT("storage_index", ("NOT: expected 1 child, got %zu, cannot evaluate", pred->children.size()));
        return false;
      }
      bool child_allows_skip = can_skip_predicate(pred->children[0].get());
      // If child says "can skip" (no matches), then NOT(child) has ALL matches
      // If child says "cannot skip" (has matches), then NOT(child) might skip
      // This is complex - be conservative
      DBUG_PRINT("storage_index", ("NOT: child_allows_skip=%d, conservative: cannot skip", child_allows_skip));
      return false;  // Conservative: don't skip on NOT predicates
    } break;
    default:
      DBUG_PRINT("storage_index", ("Unknown compound operator %d", static_cast<int>(pred->op)));
      return false;
  }
}

double StorageIndex::estimate_selectivity(const std::vector<Predicate> &predicates) const {
  if (predicates.empty()) return 1.0;
  double selectivity = 1.0;

  for (const auto &pred : predicates) {
    // Each predicate estimates its own selectivity using this StorageIndex
    double pred_selectivity = pred.estimate_selectivity(this);
    // Combine selectivities (assume independence)
    selectivity *= pred_selectivity;
  }
  // Clamp result to reasonable range
  return std::max(0.0001, std::min(1.0, selectivity));
}

void StorageIndex::update_string_stats(uint32 col_idx, const std::string &value) {
  if (col_idx >= m_num_columns) return;

  auto &stats = m_column_stats[col_idx];
  std::lock_guard lock(stats.m_string_mutex);
  if (stats.min_string.empty() || value < stats.min_string) {
    stats.min_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }
  if (stats.max_string.empty() || value > stats.max_string) {
    stats.max_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }
}

bool StorageIndex::serialize(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  // Write number of columns
  out.write(reinterpret_cast<const char *>(&m_num_columns), sizeof(m_num_columns));

  // Write statistics for each column (load atomic values)
  for (const auto &stats : m_column_stats) {
    double min_val = stats.min_value.load(std::memory_order_acquire);
    double max_val = stats.max_value.load(std::memory_order_acquire);
    double sum_val = stats.sum.load(std::memory_order_acquire);
    double avg_val = stats.avg.load(std::memory_order_acquire);
    size_t null_cnt = stats.null_count.load(std::memory_order_acquire);
    bool has_nulls = stats.has_null.load(std::memory_order_acquire);
    size_t distinct_cnt = stats.distinct_count.load(std::memory_order_acquire);

    out.write(reinterpret_cast<const char *>(&min_val), sizeof(min_val));
    out.write(reinterpret_cast<const char *>(&max_val), sizeof(max_val));
    out.write(reinterpret_cast<const char *>(&sum_val), sizeof(sum_val));
    out.write(reinterpret_cast<const char *>(&avg_val), sizeof(avg_val));
    out.write(reinterpret_cast<const char *>(&null_cnt), sizeof(null_cnt));
    out.write(reinterpret_cast<const char *>(&has_nulls), sizeof(has_nulls));
    out.write(reinterpret_cast<const char *>(&distinct_cnt), sizeof(distinct_cnt));

    // Serialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len = stats.min_string.size();
    size_t max_str_len = stats.max_string.size();
    out.write(reinterpret_cast<const char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      out.write(stats.min_string.c_str(), min_str_len);
    }
    out.write(reinterpret_cast<const char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      out.write(stats.max_string.c_str(), max_str_len);
    }
  }
  return out.good();
}

bool StorageIndex::deserialize(std::istream &in) {
  std::unique_lock lock(m_mutex);

  // Read number of columns
  in.read(reinterpret_cast<char *>(&m_num_columns), sizeof(m_num_columns));
  m_column_stats.resize(m_num_columns);
  // Read statistics for each column (store to atomic values)
  for (auto &stats : m_column_stats) {
    double min_val, max_val, sum_val, avg_val;
    size_t null_cnt, distinct_cnt;
    bool has_nulls;

    in.read(reinterpret_cast<char *>(&min_val), sizeof(min_val));
    in.read(reinterpret_cast<char *>(&max_val), sizeof(max_val));
    in.read(reinterpret_cast<char *>(&sum_val), sizeof(sum_val));
    in.read(reinterpret_cast<char *>(&avg_val), sizeof(avg_val));
    in.read(reinterpret_cast<char *>(&null_cnt), sizeof(null_cnt));
    in.read(reinterpret_cast<char *>(&has_nulls), sizeof(has_nulls));
    in.read(reinterpret_cast<char *>(&distinct_cnt), sizeof(distinct_cnt));

    stats.min_value.store(min_val);
    stats.max_value.store(max_val);
    stats.sum.store(sum_val);
    stats.avg.store(avg_val);
    stats.null_count.store(null_cnt);
    stats.has_null.store(has_nulls);
    stats.distinct_count.store(distinct_cnt);

    // Deserialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len, max_str_len;
    in.read(reinterpret_cast<char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      stats.min_string.resize(min_str_len);
      in.read(&stats.min_string[0], min_str_len);
    }
    in.read(reinterpret_cast<char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      stats.max_string.resize(max_str_len);
      in.read(&stats.max_string[0], max_str_len);
    }
  }
  return in.good();
}
}  // namespace Imcs
}  // namespace ShannonBase