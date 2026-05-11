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
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "my_inttypes.h"  //uintxxx

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/imcs/varlen0data.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_types.h"

class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;

namespace Imcs {
class Dictionary;
class RpdTable;
class Imcu;
class ColumnStatistics : public MemoryObject {
 public:
  struct SHANNON_ALIGNAS BasicStats {
    std::atomic<double> min_value{std::numeric_limits<double>::max()};
    std::atomic<double> max_value{std::numeric_limits<double>::lowest()};
    std::atomic<double> sum{0.0};
    std::atomic<double> avg{0.0};
    std::atomic<double> variance{0.0};
    std::atomic<double> stddev{0.0};

    // Count statistics
    std::atomic<uint64> row_count{0};
    std::atomic<uint64> null_count{0};
    std::atomic<uint64> distinct_count{0};

    // Data characteristics
    std::atomic<double> null_fraction{0.0};
    std::atomic<double> cardinality{0.0};

    BasicStats() = default;
    BasicStats(const BasicStats &other)
        : min_value(other.min_value.load()),
          max_value(other.max_value.load()),
          sum(other.sum.load()),
          avg(other.avg.load()),
          variance(other.variance.load()),
          stddev(other.stddev.load()),
          row_count(other.row_count.load()),
          null_count(other.null_count.load()),
          distinct_count(other.distinct_count.load()),
          null_fraction(other.null_fraction.load()),
          cardinality(other.cardinality.load()) {}
  };

  struct SHANNON_ALIGNAS StringStats {
    std::string min_string;  // Lexicographical minimum
    std::string max_string;  // Lexicographical maximum

    double avg_length{0.0};         // Average length
    uint64 max_length{0};           // Maximum length
    uint64 min_length{UINT64_MAX};  // Minimum length
    uint64 empty_count{0};          // Empty string count
  };

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

    void build(const std::vector<double> &values);

    double estimate_selectivity(double lower, double upper) const;
    double estimate_equality_selectivity(double value) const;

    uint64 get_total_rows() const;

    size_t get_bucket_count() const { return m_buckets.size(); }
    const std::vector<Bucket> &get_buckets() const { return m_buckets; }

   private:
    friend class ColumnStatistics;

    std::vector<Bucket> m_buckets;
    size_t m_bucket_count;
  };

  // Quantiles
  struct SHANNON_ALIGNAS Quantiles {
    static constexpr size_t NUM_QUANTILES = 100;  // Percentiles

    double values[NUM_QUANTILES + 1];  // 0%, 1%, 2%, ..., 100%

    Quantiles() { std::fill(std::begin(values), std::end(values), 0.0); }

    void compute(const std::vector<double> &sorted_values);

    double get_percentile(double p) const;
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
    void add(uint64 hash);
    uint64 estimate() const;
    void merge(const HyperLogLog &other);

   private:
    friend class ColumnStatistics;

    static constexpr size_t NUM_REGISTERS = 1024;  // 2^10
    static constexpr size_t REGISTER_BITS = 10;

    std::vector<uint8_t> m_registers;

    static inline uint8_t count_leading_zeros(uint64 x) {
      return static_cast<uint8_t>(x == 0 ? 64 : __builtin_clzll(x));
    }
  };

  /**
   * Reservoir Sampling
   * - Used for random sampling of fixed number of samples
   */
  class ReservoirSampler {
   public:
    explicit ReservoirSampler(size_t sample_size = 10000) : m_sample_size(sample_size), m_seen_count(0) {
      m_samples.reserve(sample_size);
    }

    void add(double value);
    const std::vector<double> &get_samples() const { return m_samples; }
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

  void update(double value);
  void update(const std::string &value);
  void update_null();

  void finalize();

  const BasicStats &get_basic_stats() const { return m_basic_stats; }
  const StringStats *get_string_stats() const { return m_string_stats.get(); }

  const EquiHeightHistogram *get_histogram() const { return m_histogram.get(); }

  const Quantiles *get_quantiles() const { return m_quantiles.get(); }

  double estimate_range_selectivity(double lower, double upper) const;
  double estimate_equality_selectivity(double value) const;
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

  // Protects m_string_stats members (std::string min/max/lengths are not
  // atomic; concurrent update(string) calls would data-race without a lock).
  mutable std::mutex m_string_mutex;

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

  void compute_variance();
  void build_histogram();
  void compute_quantiles();
  bool is_string_type() const;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COL_STATS_H__