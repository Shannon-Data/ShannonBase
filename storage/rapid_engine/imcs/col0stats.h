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

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "my_inttypes.h"  //uintxxx

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
  struct SHANNON_ALIGNAS BasicStats {
    // Numerical statistics
    double min_value{DBL_MAX};
    double max_value{DBL_MIN};
    double sum{0.0};
    double avg{0.0};
    double variance{0.0};  // Variance
    double stddev{0.0};    // Standard deviation

    // Count statistics
    uint64 row_count{0};       // Total row count
    uint64 null_count{0};      // NULL count
    uint64 distinct_count{0};  // Unique value count (estimated)

    // Data characteristics
    double null_fraction{0.0};  // NULL fraction
    double cardinality{0.0};    // Cardinality (distinct_count / row_count)
  };

  struct SHANNON_ALIGNAS StringStats {
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    double avg_length{0.0};         // Average length
    uint64 max_length{0};           // Maximum length
    uint64 min_length{UINT64_MAX};  // Minimum length
    uint64 empty_count{0};          // Empty string count
  };

  // Histogram
  /**
   * Equi-Height Histogram
   * - Each bucket contains the same number of rows
   * - Suitable for uneven data distribution
   */
  class EquiHeightHistogram {
   public:
    struct SHANNON_ALIGNAS Bucket {
      double lower_bound{DBL_MAX};  // Lower bound
      double upper_bound{DBL_MIN};  // Upper bound
      uint64 count{0};              // Row count
      uint64 distinct_count{0};     // Distinct value count
    };

    explicit EquiHeightHistogram(size_t num_buckets = 64) : m_bucket_count(num_buckets) {
      m_buckets.reserve(num_buckets);
    }

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
    uint64 get_total_rows() const;

    /**
     * Get bucket count
     */
    size_t get_bucket_count() const { return m_buckets.size(); }

    /**
     * Get buckets
     */
    const std::vector<Bucket> &get_buckets() const { return m_buckets; }

   private:
    std::vector<Bucket> m_buckets;
    size_t m_bucket_count;
  };

  // Quantiles
  struct SHANNON_ALIGNAS Quantiles {
    static constexpr size_t NUM_QUANTILES = 100;  // Percentiles

    double values[NUM_QUANTILES + 1];  // 0%, 1%, 2%, ..., 100%

    Quantiles() { std::fill(std::begin(values), std::end(values), 0.0); }

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
    double get_median() const { return values[50]; }
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
    HyperLogLog() : m_registers(NUM_REGISTERS, 0) {}

    /**
     * Add value
     */
    void add(uint64 hash);

    /**
     * Estimate cardinality
     */
    uint64 estimate() const;

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
    static uint8_t count_leading_zeros(uint64 x);
  };

  // Sampler
  /**
   * Reservoir Sampling
   * - Used for random sampling of fixed number of samples
   */
  class ReservoirSampler {
   public:
    explicit ReservoirSampler(size_t sample_size = 10000) : m_sample_size(sample_size), m_seen_count(0) {
      m_samples.reserve(sample_size);
    }

    /**
     * Add value
     */
    void add(double value);

    /**
     * Get samples
     */
    const std::vector<double> &get_samples() const { return m_samples; }

    /**
     * Get sample rate
     */
    double get_sample_rate() const {
      if (m_seen_count == 0) return 0.0;
      return static_cast<double>(m_samples.size()) / m_seen_count;
    }

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

  const BasicStats &get_basic_stats() const { return m_basic_stats; }

  const StringStats *get_string_stats() const { return m_string_stats.get(); }

  const EquiHeightHistogram *get_histogram() const { return m_histogram.get(); }

  const Quantiles *get_quantiles() const { return m_quantiles.get(); }

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
  uint64 m_version;

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