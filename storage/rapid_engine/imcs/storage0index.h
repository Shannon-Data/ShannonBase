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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_STORAGE0INDEX_H__
#define __SHANNONBASE_STORAGE0INDEX_H__

#include <atomic>  //std::atomic<T>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "storage/rapid_engine/imcs/predicate.h"
class Field;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Imcu;
/**
 * Storage Index, Every IMCU header automatically creates and manages In-Memory Storage Indexes (IM storage indexes) for
 * its CUs. An IM storage index stores the minimum and maximum for all columns within the IMCU.
 * - Column statistics at IMCU level
 * - Used for query optimization: skip irrelevant IMCUs
 * - Similar to Oracle's Storage Index
 */
class StorageIndex {
 public:
  struct SHANNON_ALIGNAS ColumnStats {
    // Atomic basic statistics
    std::atomic<double> min_value;
    std::atomic<double> max_value;
    std::atomic<double> sum;
    std::atomic<double> avg;

    // Atomic NULL Statistics
    std::atomic<size_t> null_count;
    std::atomic<bool> has_null;

    // Atomic Cardinality Statistics
    std::atomic<size_t> distinct_count;  // Estimated value (HyperLogLog)

    // String Statistics - non-atomic, protected by mutex
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    // Data Distribution - non-atomic, protected by mutex
    std::vector<double> histogram;  // Histogram

    // Mutex for string and vector operations
    mutable std::mutex m_string_mutex;

    ColumnStats()
        : min_value(DBL_MAX),
          max_value(DBL_MIN),
          sum(0.0),
          avg(0.0),
          null_count(0),
          has_null(false),
          distinct_count(0) {}
    ColumnStats(const ColumnStats &other)
        : min_value(other.min_value.load(std::memory_order_acquire)),
          max_value(other.max_value.load(std::memory_order_acquire)),
          sum(other.sum.load(std::memory_order_acquire)),
          avg(other.avg.load(std::memory_order_acquire)),
          null_count(other.null_count.load(std::memory_order_acquire)),
          has_null(other.has_null.load(std::memory_order_acquire)),
          distinct_count(other.distinct_count.load(std::memory_order_acquire)) {
      std::lock_guard lock(other.m_string_mutex);
      min_string = other.min_string;
      max_string = other.max_string;
      histogram = other.histogram;
    }

    ColumnStats &operator=(const ColumnStats &other) {
      if (this != &other) {
        min_value.store(other.min_value.load(std::memory_order_acquire));
        max_value.store(other.max_value.load(std::memory_order_acquire));
        sum.store(other.sum.load(std::memory_order_acquire));
        avg.store(other.avg.load(std::memory_order_acquire));
        null_count.store(other.null_count.load(std::memory_order_acquire));
        has_null.store(other.has_null.load(std::memory_order_acquire));
        distinct_count.store(other.distinct_count.load(std::memory_order_acquire));

        std::lock_guard lock1(m_string_mutex, std::adopt_lock);
        std::lock_guard lock2(other.m_string_mutex, std::adopt_lock);
        std::lock(m_string_mutex, other.m_string_mutex);

        min_string = other.min_string;
        max_string = other.max_string;
        histogram = other.histogram;
      }
      return *this;
    }

    ColumnStats(ColumnStats &&other) noexcept
        : min_value(other.min_value.load(std::memory_order_acquire)),
          max_value(other.max_value.load(std::memory_order_acquire)),
          sum(other.sum.load(std::memory_order_acquire)),
          avg(other.avg.load(std::memory_order_acquire)),
          null_count(other.null_count.load(std::memory_order_acquire)),
          has_null(other.has_null.load(std::memory_order_acquire)),
          distinct_count(other.distinct_count.load(std::memory_order_acquire)),
          min_string(std::move(other.min_string)),
          max_string(std::move(other.max_string)),
          histogram(std::move(other.histogram)) {}

    ColumnStats &operator=(ColumnStats &&other) noexcept {
      if (this != &other) {
        min_value.store(other.min_value.load(std::memory_order_acquire));
        max_value.store(other.max_value.load(std::memory_order_acquire));
        sum.store(other.sum.load(std::memory_order_acquire));
        avg.store(other.avg.load(std::memory_order_acquire));
        null_count.store(other.null_count.load(std::memory_order_acquire));
        has_null.store(other.has_null.load(std::memory_order_acquire));
        distinct_count.store(other.distinct_count.load(std::memory_order_acquire));

        std::lock_guard lock(m_string_mutex);
        min_string = std::move(other.min_string);
        max_string = std::move(other.max_string);
        histogram = std::move(other.histogram);
      }
      return *this;
    }
  };

  StorageIndex(size_t num_columns) : m_num_columns(num_columns) {
    std::unique_lock lock(m_mutex);
    m_column_stats.resize(num_columns);
  }

  StorageIndex(const StorageIndex &other)
      : m_dirty(other.m_dirty.load(std::memory_order_acquire)), m_num_columns(other.m_num_columns) {
    std::shared_lock lock(other.m_mutex);
    m_column_stats = other.m_column_stats;
  }

  StorageIndex &operator=(const StorageIndex &other) {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::shared_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_column_stats = other.m_column_stats;
      m_num_columns = other.m_num_columns;
      m_dirty.store(other.m_dirty.load(std::memory_order_acquire));
    }
    return *this;
  }

  StorageIndex(StorageIndex &&other) noexcept
      : m_dirty(other.m_dirty.load(std::memory_order_acquire)), m_num_columns(other.m_num_columns) {
    std::unique_lock lock1(m_mutex, std::defer_lock);
    std::unique_lock lock2(other.m_mutex, std::defer_lock);
    std::lock(lock1, lock2);

    m_column_stats = std::move(other.m_column_stats);
    other.m_num_columns = 0;
  }

  StorageIndex &operator=(StorageIndex &&other) noexcept {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::unique_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_column_stats = std::move(other.m_column_stats);
      m_num_columns = other.m_num_columns;
      m_dirty.store(other.m_dirty.load(std::memory_order_acquire));
      other.m_num_columns = 0;
    }
    return *this;
  }

  /**
   * Update single column statistics (called during INSERT/UPDATE)
   * @param col_idx: Column index
   * @param value: Numeric value (converted)
   */
  void update(uint32 col_idx, double value);

  /**
   * Update NULL statistics
   */
  void update_null(uint32 col_idx);

  /**
   * Batch rebuild statistics (scan all data)
   * @param imcu: Owner IMCU
   */
  void rebuild(const Imcu *imcu);

  /**
   * Get column statistics - returns a snapshot
   */
  const ColumnStats *get_column_stats_snapshot(uint32 col_idx) const;

  /**
   * Get individual atomic values (for read-only access)
   */
  inline double get_min_value(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return DBL_MAX;
    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].min_value.load(std::memory_order_acquire);
  }

  inline double get_max_value(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return DBL_MIN;
    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].max_value.load(std::memory_order_acquire);
  }

  inline size_t get_null_count(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return 0;
    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].null_count.load(std::memory_order_acquire);
  }

  inline bool get_has_null(uint32 col_idx) const {
    if (col_idx >= m_num_columns) return false;
    std::shared_lock lock(m_mutex);
    return m_column_stats[col_idx].has_null.load(std::memory_order_acquire);
  }

  /**
   * Check if IMCU can be skipped (based on predicates)
   * @param predicates: List of predicates
   * @return: Returns true if can be skipped
   */
  bool can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const;

  /**
   * Estimate selectivity
   * @param predicates: List of predicates
   * @return: Selectivity [0.0, 1.0]
   */
  double estimate_selectivity(const std::vector<Predicate> &predicates) const;

  /**
   * Update string statistics (requires mutex protection)
   */
  void update_string_stats(uint32 col_idx, const std::string &value);

  inline void mark_dirty() { m_dirty.store(true, std::memory_order_relaxed); }

  inline bool is_dirty() const { return m_dirty.load(std::memory_order_acquire); }

  inline void clear_dirty() { m_dirty.store(false, std::memory_order_relaxed); }

  bool serialize(std::ostream &out) const;

  bool deserialize(std::istream &in);

 private:
  // Statistics for each column
  std::vector<ColumnStats> m_column_stats;
  // Dirty flag
  std::atomic<bool> m_dirty{false};
  // Concurrency control
  mutable std::shared_mutex m_mutex;
  // Number of columns
  size_t m_num_columns;

 private:
  /**
   * @brief Recursively evaluate a single predicate against Storage Index
   * @param pred Predicate (Simple or Compound)
   * @return true if predicate guarantees no rows match (can skip)
   */
  bool can_skip_predicate(const Predicate *pred) const;

  /**
   * @brief Evaluate a simple predicate against Storage Index
   * @param pred Simple predicate
   * @return true if simple predicate guarantees no rows match
   */
  bool can_skip_simple_predicate(const Simple_Predicate *pred) const;

  /**
   * @brief Evaluate a compound predicate against Storage Index
   * @param pred Compound predicate (AND/OR/NOT)
   * @return true if compound predicate guarantees no rows match
   */
  bool can_skip_compound_predicate(const Compound_Predicate *pred) const;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_STORAGE0INDEX_H__