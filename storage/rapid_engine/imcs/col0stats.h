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

   The fundmental code for imcs. Column Unit.
*/
#ifndef __SHANNONBASE_COL_STATS_H__
#define __SHANNONBASE_COL_STATS_H__

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/memory_pool.h"

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/imcs/varlen0data.h"

class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;

namespace Imcs {
class Dictionary;
class RpdTable;
class Imcu;
class ColumnStatistics {
 public:
  struct BasicStats {
    // Numerical statistics
    double min_value;
    double max_value;
    double sum;
    double avg;
    double variance;  // Variance
    double stddev;    // Standard deviation

    // Count statistics
    uint64_t row_count;       // Total row count
    uint64_t null_count;      // NULL count
    uint64_t distinct_count;  // Unique value count (estimated)

    // Data characteristics
    double null_fraction;  // NULL fraction
    double cardinality;    // Cardinality (distinct_count / row_count)

    BasicStats();
  };

  struct StringStats {
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    double avg_length;    // Average length
    uint64_t max_length;  // Maximum length
    uint64_t min_length;  // Minimum length

    uint64_t empty_count;  // Empty string count

    StringStats();
  };

  // Histogram
  /**
   * Equi-Height Histogram
   * - Each bucket contains the same number of rows
   * - Suitable for uneven data distribution
   */
  class EquiHeightHistogram {
   public:
    struct Bucket {
      double lower_bound;       // Lower bound
      double upper_bound;       // Upper bound
      uint64_t count;           // Row count
      uint64_t distinct_count;  // Distinct value count

      Bucket();
    };

    explicit EquiHeightHistogram(size_t num_buckets = 64);

    /**
     * Build histogram
     */
    void build(const std::vector<double> &values);

    /**
     * Estimate selectivity
     */
    double estimate_selectivity(double lower, double upper) const;

    /**
     * Estimate equality selectivity
     */
    double estimate_equality_selectivity(double value) const;

    /**
     * Get total row count
     */
    uint64_t get_total_rows() const;

    /**
     * Get bucket count
     */
    size_t get_bucket_count() const;

    /**
     * Get buckets
     */
    const std::vector<Bucket> &get_buckets() const;

   private:
    std::vector<Bucket> m_buckets;
    size_t m_bucket_count;
  };

  // Quantiles
  struct Quantiles {
    static constexpr size_t NUM_QUANTILES = 100;  // Percentiles

    double values[NUM_QUANTILES + 1];  // 0%, 1%, 2%, ..., 100%

    Quantiles();

    /**
     * Compute quantiles
     */
    void compute(const std::vector<double> &sorted_values);

    /**
     * Get specified percentile
     */
    double get_percentile(double p) const;

    /**
     * Get median
     */
    double get_median() const;
  };

  // HyperLogLog (Cardinality Estimation)
  /**
   * HyperLogLog algorithm
   * - Used to estimate number of distinct values (NDV)
   * - Space complexity: O(m), where m is number of registers
   * - Error rate: approximately 1.04 / sqrt(m)
   */
  class HyperLogLog {
   public:
    HyperLogLog();

    /**
     * Add value
     */
    void add(uint64_t hash);

    /**
     * Estimate cardinality
     */
    uint64_t estimate() const;

    /**
     * Merge another HyperLogLog
     */
    void merge(const HyperLogLog &other);

   private:
    static constexpr size_t NUM_REGISTERS = 1024;  // 2^10
    static constexpr size_t REGISTER_BITS = 10;

    std::vector<uint8_t> m_registers;

    /**
     * Count leading zeros
     */
    static uint8_t count_leading_zeros(uint64_t x);
  };

  // Sampler
  /**
   * Reservoir Sampling
   * - Used for random sampling of fixed number of samples
   */
  class ReservoirSampler {
   public:
    explicit ReservoirSampler(size_t sample_size = 10000);

    /**
     * Add value
     */
    void add(double value);

    /**
     * Get samples
     */
    const std::vector<double> &get_samples() const;

    /**
     * Get sample rate
     */
    double get_sample_rate() const;

   private:
    std::vector<double> m_samples;
    size_t m_sample_size;
    size_t m_seen_count;
  };

  ColumnStatistics(uint32_t col_id, const std::string &col_name, enum_field_types col_type);

  /**
   * Update statistics (single value)
   */
  void update(double value);

  /**
   * Update statistics (string)
   */
  void update(const std::string &value);

  /**
   * Record NULL value
   */
  void update_null();

  /**
   * Finalize statistics (compute derived values)
   */
  void finalize();

  const BasicStats &get_basic_stats() const;

  const StringStats *get_string_stats() const;

  const EquiHeightHistogram *get_histogram() const;

  const Quantiles *get_quantiles() const;

  /**
   * Estimate selectivity (range query)
   */
  double estimate_range_selectivity(double lower, double upper) const;

  /**
   * Estimate selectivity (equality query)
   */
  double estimate_equality_selectivity(double value) const;

  /**
   * Estimate NULL selectivity
   */
  double estimate_null_selectivity() const;

  bool serialize(std::ostream &out) const;

  bool deserialize(std::istream &in);

  void dump(std::ostream &out) const;

 private:
  // Column metadata
  uint32_t m_column_id;
  std::string m_column_name;
  enum_field_types m_column_type;

  // Basic statistics
  BasicStats m_basic_stats;

  // String statistics (optional)
  std::unique_ptr<StringStats> m_string_stats;

  // Histogram
  std::unique_ptr<EquiHeightHistogram> m_histogram;

  // Quantiles
  std::unique_ptr<Quantiles> m_quantiles;

  // HyperLogLog (cardinality estimation)
  std::unique_ptr<HyperLogLog> m_hll;

  // Sampler
  std::unique_ptr<ReservoirSampler> m_sampler;

  // Update time
  std::chrono::system_clock::time_point m_last_update;

  // Version number (for detecting staleness)
  uint64_t m_version;

  /**
   * Compute variance
   */
  void compute_variance();

  /**
   * Build histogram
   */
  void build_histogram();

  /**
   * Compute quantiles
   */
  void compute_quantiles();

  /**
   * Check if string type
   */
  bool is_string_type() const;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COL_STATS_H__