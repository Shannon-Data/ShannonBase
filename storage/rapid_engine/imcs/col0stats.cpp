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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/col0stats.h"

#include <limits.h>
#include <iostream>
#include <random>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type

#include "storage/innobase/include/ut0dbg.h"

namespace ShannonBase {
namespace Imcs {
void ColumnStatistics::EquiHeightHistogram::build(const std::vector<double> &values) {
  if (values.empty()) return;

  // 1. Sort
  std::vector<double> sorted_values = values;
  std::sort(sorted_values.begin(), sorted_values.end());

  // 2. Calculate bucket size
  size_t total_rows = sorted_values.size();
  size_t rows_per_bucket = std::max(size_t(1), total_rows / m_bucket_count);

  // 3. Allocate buckets
  m_buckets.clear();

  for (size_t i = 0; i < total_rows; i += rows_per_bucket) {
    Bucket bucket;

    size_t end = std::min(i + rows_per_bucket, total_rows);

    bucket.lower_bound = sorted_values[i];
    bucket.upper_bound = sorted_values[end - 1];
    bucket.count = end - i;

    // Calculate distinct value count
    std::unordered_set<double> distinct;
    for (size_t j = i; j < end; j++) {
      distinct.insert(sorted_values[j]);
    }
    bucket.distinct_count = distinct.size();

    m_buckets.push_back(bucket);
  }
}

double ColumnStatistics::EquiHeightHistogram::estimate_selectivity(double lower, double upper) const {
  if (m_buckets.empty()) return 0.5;

  uint64 estimated_rows = 0;
  uint64 total_rows = 0;

  for (const auto &bucket : m_buckets) {
    total_rows += bucket.count;

    // Calculate overlap between bucket and query range
    if (bucket.upper_bound < lower || bucket.lower_bound > upper) {
      // No overlap
      continue;
    }

    if (bucket.lower_bound >= lower && bucket.upper_bound <= upper) {
      // Fully contained
      estimated_rows += bucket.count;
    } else {
      // Partial overlap
      double bucket_range = bucket.upper_bound - bucket.lower_bound;
      double overlap_range = std::min(bucket.upper_bound, upper) - std::max(bucket.lower_bound, lower);

      if (bucket_range > 0) {
        double ratio = overlap_range / bucket_range;
        estimated_rows += static_cast<uint64>(bucket.count * ratio);
      }
    }
  }

  if (total_rows == 0) return 0.0;
  return static_cast<double>(estimated_rows) / total_rows;
}

double ColumnStatistics::EquiHeightHistogram::estimate_equality_selectivity(double value) const {
  for (const auto &bucket : m_buckets) {
    if (value >= bucket.lower_bound && value <= bucket.upper_bound) {
      if (bucket.distinct_count > 0) {
        return 1.0 / bucket.distinct_count * (static_cast<double>(bucket.count) / get_total_rows());
      }
    }
  }
  return 0.0;
}

uint64 ColumnStatistics::EquiHeightHistogram::get_total_rows() const {
  uint64 total = 0;
  for (const auto &bucket : m_buckets) {
    total += bucket.count;
  }
  return total;
}

void ColumnStatistics::Quantiles::compute(const std::vector<double> &sorted_values) {
  if (sorted_values.empty()) return;

  size_t n = sorted_values.size();

  for (size_t i = 0; i <= NUM_QUANTILES; i++) {
    double percentile = static_cast<double>(i) / NUM_QUANTILES;
    size_t idx = static_cast<size_t>(percentile * (n - 1));
    values[i] = sorted_values[idx];
  }
}

double ColumnStatistics::Quantiles::get_percentile(double p) const {
  if (p < 0.0) p = 0.0;
  if (p > 1.0) p = 1.0;

  size_t idx = static_cast<size_t>(p * NUM_QUANTILES);
  return values[idx];
}

void ColumnStatistics::HyperLogLog::add(uint64 hash) {
  // 1. Extract register index (low REGISTER_BITS bits)
  size_t idx = hash & ((1 << REGISTER_BITS) - 1);

  // 2. Calculate leading zero count
  uint64 remaining = hash >> REGISTER_BITS;
  uint8_t leading_zeros = count_leading_zeros(remaining) + 1;

  // 3. Update register (take maximum)
  if (leading_zeros > m_registers[idx]) {
    m_registers[idx] = leading_zeros;
  }
}

uint64 ColumnStatistics::HyperLogLog::estimate() const {
  double alpha = 0.7213 / (1.0 + 1.079 / NUM_REGISTERS);

  double sum = 0.0;
  int zero_count = 0;

  for (size_t i = 0; i < NUM_REGISTERS; i++) {
    sum += 1.0 / (1ULL << m_registers[i]);
    if (m_registers[i] == 0) {
      zero_count++;
    }
  }

  double estimate = alpha * NUM_REGISTERS * NUM_REGISTERS / sum;

  // Small range correction
  if (estimate <= 2.5 * NUM_REGISTERS && zero_count > 0) {
    estimate = NUM_REGISTERS * std::log(static_cast<double>(NUM_REGISTERS) / zero_count);
  }

  return static_cast<uint64>(estimate);
}

void ColumnStatistics::HyperLogLog::merge(const HyperLogLog &other) {
  for (size_t i = 0; i < NUM_REGISTERS; i++) {
    m_registers[i] = std::max(m_registers[i], other.m_registers[i]);
  }
}

void ColumnStatistics::ReservoirSampler::add(double value) {
  m_seen_count++;

  if (m_samples.size() < m_sample_size) {
    m_samples.push_back(value);
  } else {
    // Random replacement
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, m_seen_count - 1);
    size_t idx = dist(rng);
    if (idx < m_sample_size) {
      m_samples[idx] = value;
    }
  }
}

ColumnStatistics::ColumnStatistics(uint32_t col_id, const std::string &col_name, enum_field_types col_type)
    : m_column_id(col_id), m_column_name(col_name), m_column_type(col_type), m_version(0) {
  m_last_update = std::chrono::system_clock::now();

  // Initialize HyperLogLog
  m_hll = std::make_unique<HyperLogLog>();

  // Initialize sampler
  m_sampler = std::make_unique<ReservoirSampler>();

  // String type needs string statistics
  if (is_string_type()) {
    m_string_stats = std::make_unique<StringStats>();
  }
}

void ColumnStatistics::update(double value) {
  // Update basic statistics
  m_basic_stats.row_count.fetch_add(1, std::memory_order_relaxed);
  m_basic_stats.sum.fetch_add(value, std::memory_order_relaxed);

  double old_min = m_basic_stats.min_value.load(std::memory_order_relaxed);
  while (value < old_min) {
    if (m_basic_stats.min_value.compare_exchange_weak(old_min, value, std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
      break;
    }
  }

  double old_max = m_basic_stats.max_value.load(std::memory_order_relaxed);
  while (value > old_max) {
    if (m_basic_stats.max_value.compare_exchange_weak(old_max, value, std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
      break;
    }
  }

  m_sampler->add(value);
  uint64 hash = std::hash<double>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update(const std::string &value) {
  const uint64 new_count = m_basic_stats.row_count.fetch_add(1, std::memory_order_relaxed) + 1;

  if (m_string_stats) {
    std::lock_guard<std::mutex> lk(m_string_mutex);
    if (value.empty()) {
      m_string_stats->empty_count++;
    }

    if (m_string_stats->min_string.empty() || value < m_string_stats->min_string) {
      m_string_stats->min_string = value;
    }
    if (value > m_string_stats->max_string) {
      m_string_stats->max_string = value;
    }

    const size_t len = value.length();
    // Incremental mean: avg_n = avg_{n-1} + (x - avg_{n-1}) / n
    m_string_stats->avg_length += (static_cast<double>(len) - m_string_stats->avg_length) / new_count;
    m_string_stats->max_length = std::max(m_string_stats->max_length, len);
    m_string_stats->min_length = std::min(m_string_stats->min_length, len);
  }

  // Update HyperLogLog
  const uint64 hash = std::hash<std::string>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update_null() {
  m_basic_stats.row_count.fetch_add(1, std::memory_order_relaxed);
  m_basic_stats.null_count.fetch_add(1, std::memory_order_relaxed);
}

void ColumnStatistics::finalize() {
  // Snapshot all atomic fields once; finalize() is called from a single
  // maintenance thread after data collection is complete, so a relaxed load
  // of each value and a subsequent seq_cst fence is sufficient.
  const uint64 row_count = m_basic_stats.row_count.load(std::memory_order_acquire);
  const uint64 null_count = m_basic_stats.null_count.load(std::memory_order_acquire);
  const double sum = m_basic_stats.sum.load(std::memory_order_acquire);

  if (row_count > 0) {
    m_basic_stats.avg = sum / static_cast<double>(row_count);
    m_basic_stats.null_fraction = static_cast<double>(null_count) / static_cast<double>(row_count);
  }

  // Estimate unique value count from HyperLogLog
  const uint64 ndv = m_hll->estimate();
  m_basic_stats.distinct_count = ndv;

  if (row_count > 0) {
    m_basic_stats.cardinality = static_cast<double>(ndv) / static_cast<double>(row_count);
  }

  // Calculate variance and standard deviation (uses m_basic_stats.avg just set above)
  compute_variance();

  // Build histogram from reservoir samples
  build_histogram();

  // Calculate quantiles from reservoir samples
  compute_quantiles();

  m_last_update = std::chrono::system_clock::now();
  m_version++;
}

double ColumnStatistics::estimate_range_selectivity(double lower, double upper) const {
  if (m_histogram) return m_histogram->estimate_selectivity(lower, upper);

  if (lower > upper) return 0.0;

  // No numeric histogram is available for string columns. Return a default
  // unknown selectivity instead of using invalid numeric min/max values.
  if (is_string_type()) return 0.5;

  // Fallback: uniform distribution assumption
  const double min_v = m_basic_stats.min_value.load(std::memory_order_relaxed);
  const double max_v = m_basic_stats.max_value.load(std::memory_order_relaxed);
  if (are_equal(min_v, max_v)) {
    // Constant column: if the query range contains the single value, selectivity is 1.0.
    return (lower <= min_v && upper >= max_v) ? 1.0 : 0.0;
  }

  const double range = max_v - min_v;
  const double query_range = upper - lower;
  return std::min(1.0, std::max(0.0, query_range / range));
}

double ColumnStatistics::estimate_equality_selectivity(double value) const {
  if (m_histogram) {
    return m_histogram->estimate_equality_selectivity(value);
  }

  // Fallback: assume uniform distribution
  if (m_basic_stats.distinct_count > 0) {
    return 1.0 / m_basic_stats.distinct_count;
  }

  return 0.1;  // Default 10%
}

double ColumnStatistics::estimate_null_selectivity() const {
  // null_fraction is a plain double set in finalize(); no atomic needed.
  return m_basic_stats.null_fraction;
}

// ---------------------------------------------------------------------------
// Binary layout (all multi-byte values are native-endian):
//
//  [MAGIC: uint32]              0xC015'7A71
//  [FORMAT_VERSION: uint16]     1
//  [col_id: uint32]
//  [name_len: size_t][name: char × name_len]
//  [col_type: enum_field_types]
//  --- BasicStats ---
//  [row_count: uint64]
//  [null_count: uint64]
//  [sum: double]
//  [min_value: double]
//  [max_value: double]
//  [avg: double]
//  [null_fraction: double]
//  [variance: double]
//  [stddev: double]
//  [distinct_count: uint64]
//  [cardinality: double]
//  --- StringStats (has_string_stats: bool) ---
//  [min_len:size_t][min_str: char × min_len]
//  [max_len:size_t][max_str: char × max_len]
//  [avg_length: double]
//  [min_length: size_t]
//  [max_length: size_t]
//  [empty_count: uint64]
//  --- HyperLogLog (has_hll: bool) ---
//  [registers: uint8_t × NUM_REGISTERS]
//  --- EquiHeightHistogram (has_histogram: bool) ---
//  [bucket_count: size_t]
//  for each bucket:
//    [lower_bound: double][upper_bound: double]
//    [count: uint64][distinct_count: uint64]
//  --- Quantiles (has_quantiles: bool) ---
//  [values: double × (NUM_QUANTILES + 1)]
// ---------------------------------------------------------------------------

static constexpr uint32_t kColStatsMagic = 0xC0157A71u;
static constexpr uint16_t kColStatsVersion = 1u;

static bool write_string(std::ostream &out, const std::string &s) {
  const size_t len = s.size();
  out.write(reinterpret_cast<const char *>(&len), sizeof(len));
  if (len > 0) out.write(s.data(), static_cast<std::streamsize>(len));
  return out.good();
}

static bool read_string(std::istream &in, std::string &s) {
  size_t len = 0;
  in.read(reinterpret_cast<char *>(&len), sizeof(len));
  if (!in.good()) return false;
  if (len > 0) {
    s.resize(len);
    in.read(&s[0], static_cast<std::streamsize>(len));
  } else {
    s.clear();
  }
  return in.good();
}

template <typename T>
static bool write_pod(std::ostream &out, const T &v) {
  out.write(reinterpret_cast<const char *>(&v), sizeof(v));
  return out.good();
}

template <typename T>
static bool read_pod(std::istream &in, T &v) {
  in.read(reinterpret_cast<char *>(&v), sizeof(v));
  return in.good();
}

bool ColumnStatistics::serialize(std::ostream &out) const {
  // Header
  if (!write_pod(out, kColStatsMagic)) return false;
  if (!write_pod(out, kColStatsVersion)) return false;

  // Column identity
  if (!write_pod(out, m_column_id)) return false;
  if (!write_string(out, m_column_name)) return false;
  if (!write_pod(out, m_column_type)) return false;

  // BasicStats
  const uint64 row_count = m_basic_stats.row_count.load(std::memory_order_acquire);
  const uint64 null_count = m_basic_stats.null_count.load(std::memory_order_acquire);
  const double sum = m_basic_stats.sum.load(std::memory_order_acquire);
  const double min_v = m_basic_stats.min_value.load(std::memory_order_acquire);
  const double max_v = m_basic_stats.max_value.load(std::memory_order_acquire);

  if (!write_pod(out, row_count)) return false;
  if (!write_pod(out, null_count)) return false;
  if (!write_pod(out, sum)) return false;
  if (!write_pod(out, min_v)) return false;
  if (!write_pod(out, max_v)) return false;

  const double avg = m_basic_stats.avg.load(std::memory_order_acquire);
  const double null_fraction = m_basic_stats.null_fraction.load(std::memory_order_acquire);
  const double variance = m_basic_stats.variance.load(std::memory_order_acquire);
  const double stddev = m_basic_stats.stddev.load(std::memory_order_acquire);
  const uint64 distinct_cnt = m_basic_stats.distinct_count.load(std::memory_order_acquire);
  const double cardinality = m_basic_stats.cardinality.load(std::memory_order_acquire);

  if (!write_pod(out, avg)) return false;
  if (!write_pod(out, null_fraction)) return false;
  if (!write_pod(out, variance)) return false;
  if (!write_pod(out, stddev)) return false;
  if (!write_pod(out, distinct_cnt)) return false;
  if (!write_pod(out, cardinality)) return false;

  // StringStats
  const bool has_str = (m_string_stats != nullptr);
  if (!write_pod(out, has_str)) return false;
  if (has_str) {
    std::lock_guard<std::mutex> lk(m_string_mutex);
    if (!write_string(out, m_string_stats->min_string)) return false;
    if (!write_string(out, m_string_stats->max_string)) return false;
    if (!write_pod(out, m_string_stats->avg_length)) return false;
    if (!write_pod(out, m_string_stats->min_length)) return false;
    if (!write_pod(out, m_string_stats->max_length)) return false;
    if (!write_pod(out, m_string_stats->empty_count)) return false;
  }

  // HyperLogLog – always present after construction
  const bool has_hll = (m_hll != nullptr);
  if (!write_pod(out, has_hll)) return false;
  if (has_hll) {
    // m_registers is uint8_t[NUM_REGISTERS]; write the raw array.
    out.write(reinterpret_cast<const char *>(m_hll->m_registers.data()),
              static_cast<std::streamsize>(m_hll->m_registers.size()));
    if (!out.good()) return false;
  }

  // EquiHeightHistogram
  const bool has_hist = (m_histogram != nullptr);
  if (!write_pod(out, has_hist)) return false;
  if (has_hist) {
    const auto &buckets = m_histogram->m_buckets;
    const size_t nbuckets = buckets.size();
    if (!write_pod(out, nbuckets)) return false;
    for (const auto &b : buckets) {
      if (!write_pod(out, b.lower_bound)) return false;
      if (!write_pod(out, b.upper_bound)) return false;
      if (!write_pod(out, b.count)) return false;
      if (!write_pod(out, b.distinct_count)) return false;
    }
  }

  // Quantiles
  const bool has_quant = (m_quantiles != nullptr);
  if (!write_pod(out, has_quant)) return false;
  if (has_quant) {
    // values[] is double[NUM_QUANTILES + 1]
    out.write(reinterpret_cast<const char *>(m_quantiles->values),
              static_cast<std::streamsize>(sizeof(m_quantiles->values)));
    if (!out.good()) return false;
  }

  return out.good();
}

bool ColumnStatistics::deserialize(std::istream &in) {
  // Verify magic and format version before touching any state.
  uint32_t magic = 0;
  uint16_t version = 0;
  if (!read_pod(in, magic) || magic != kColStatsMagic) return false;
  if (!read_pod(in, version) || version != kColStatsVersion) return false;

  // Column identity
  if (!read_pod(in, m_column_id)) return false;
  if (!read_string(in, m_column_name)) return false;
  if (!read_pod(in, m_column_type)) return false;

  // BasicStats
  uint64 row_count = 0, null_count = 0, distinct_count = 0;
  double sum = 0, min_v = 0, max_v = 0;
  double avg = 0, null_fraction = 0, variance = 0, stddev = 0, cardinality = 0;

  if (!read_pod(in, row_count)) return false;
  if (!read_pod(in, null_count)) return false;
  if (!read_pod(in, sum)) return false;
  if (!read_pod(in, min_v)) return false;
  if (!read_pod(in, max_v)) return false;
  if (!read_pod(in, avg)) return false;
  if (!read_pod(in, null_fraction)) return false;
  if (!read_pod(in, variance)) return false;
  if (!read_pod(in, stddev)) return false;
  if (!read_pod(in, distinct_count)) return false;
  if (!read_pod(in, cardinality)) return false;

  // Store atomics with release so subsequent acquire loads see the values.
  m_basic_stats.row_count.store(row_count, std::memory_order_release);
  m_basic_stats.null_count.store(null_count, std::memory_order_release);
  m_basic_stats.sum.store(sum, std::memory_order_release);
  m_basic_stats.min_value.store(min_v, std::memory_order_release);
  m_basic_stats.max_value.store(max_v, std::memory_order_release);
  // Plain fields
  m_basic_stats.avg = avg;
  m_basic_stats.null_fraction = null_fraction;
  m_basic_stats.variance = variance;
  m_basic_stats.stddev = stddev;
  m_basic_stats.distinct_count = distinct_count;
  m_basic_stats.cardinality = cardinality;

  // StringStats
  bool has_str = false;
  if (!read_pod(in, has_str)) return false;
  if (has_str) {
    m_string_stats = std::make_unique<StringStats>();
    std::lock_guard<std::mutex> lk(m_string_mutex);
    if (!read_string(in, m_string_stats->min_string)) return false;
    if (!read_string(in, m_string_stats->max_string)) return false;
    if (!read_pod(in, m_string_stats->avg_length)) return false;
    if (!read_pod(in, m_string_stats->min_length)) return false;
    if (!read_pod(in, m_string_stats->max_length)) return false;
    if (!read_pod(in, m_string_stats->empty_count)) return false;
  } else {
    m_string_stats.reset();
  }

  // HyperLogLog
  bool has_hll = false;
  if (!read_pod(in, has_hll)) return false;
  if (has_hll) {
    m_hll = std::make_unique<HyperLogLog>();
    in.read(reinterpret_cast<char *>(m_hll->m_registers.data()),
            static_cast<std::streamsize>(m_hll->m_registers.size()));
    if (!in.good()) return false;
  } else {
    m_hll.reset();
  }

  // EquiHeightHistogram
  bool has_hist = false;
  if (!read_pod(in, has_hist)) return false;
  if (has_hist) {
    size_t nbuckets = 0;
    if (!read_pod(in, nbuckets)) return false;
    m_histogram = std::make_unique<EquiHeightHistogram>(nbuckets);
    m_histogram->m_buckets.resize(nbuckets);
    for (auto &b : m_histogram->m_buckets) {
      if (!read_pod(in, b.lower_bound)) return false;
      if (!read_pod(in, b.upper_bound)) return false;
      if (!read_pod(in, b.count)) return false;
      if (!read_pod(in, b.distinct_count)) return false;
    }
  } else {
    m_histogram.reset();
  }

  // Quantiles
  bool has_quant = false;
  if (!read_pod(in, has_quant)) return false;
  if (has_quant) {
    m_quantiles = std::make_unique<Quantiles>();
    in.read(reinterpret_cast<char *>(m_quantiles->values), static_cast<std::streamsize>(sizeof(m_quantiles->values)));
    if (!in.good()) return false;
  } else {
    m_quantiles.reset();
  }

  m_last_update = std::chrono::system_clock::now();
  return in.good();
}

void ColumnStatistics::dump(std::ostream &out) const {
  const uint64 row_count = m_basic_stats.row_count.load(std::memory_order_acquire);
  const uint64 null_count = m_basic_stats.null_count.load(std::memory_order_acquire);
  const double min_v = m_basic_stats.min_value.load(std::memory_order_acquire);
  const double max_v = m_basic_stats.max_value.load(std::memory_order_acquire);

  out << "Column Statistics for '" << m_column_name << "' (ID: " << m_column_id << "):\n";
  out << "  Type: " << static_cast<int>(m_column_type) << "\n";
  out << "  Row Count: " << row_count << "\n";
  out << "  NULL Count: " << null_count << " (" << (m_basic_stats.null_fraction * 100.0) << "%)\n";
  out << "  Distinct Count: " << m_basic_stats.distinct_count << " (Cardinality: " << m_basic_stats.cardinality
      << ")\n";
  out << "  Min: " << min_v << "\n";
  out << "  Max: " << max_v << "\n";
  out << "  Avg: " << m_basic_stats.avg << "\n";
  out << "  StdDev: " << m_basic_stats.stddev << "\n";

  if (m_string_stats) {
    std::lock_guard<std::mutex> lk(m_string_mutex);
    out << "  String Stats:\n";
    out << "    Min String: " << m_string_stats->min_string << "\n";
    out << "    Max String: " << m_string_stats->max_string << "\n";
    out << "    Avg Length: " << m_string_stats->avg_length << "\n";
    out << "    Empty Count: " << m_string_stats->empty_count << "\n";
  }

  if (m_histogram) {
    out << "  Histogram: " << m_histogram->get_bucket_count() << " buckets\n";
  }
}

void ColumnStatistics::compute_variance() {
  const auto &samples = m_sampler->get_samples();
  if (samples.size() < 2) return;

  double mean = m_basic_stats.avg;
  double sum_sq_diff = 0.0;

  for (double value : samples) {
    double diff = value - mean;
    sum_sq_diff += diff * diff;
  }

  m_basic_stats.variance = sum_sq_diff / (samples.size() - 1);
  m_basic_stats.stddev = std::sqrt(m_basic_stats.variance);
}

void ColumnStatistics::build_histogram() {
  const auto &samples = m_sampler->get_samples();
  if (samples.size() < 100) return;  // Too few samples

  m_histogram = std::make_unique<EquiHeightHistogram>(64);
  m_histogram->build(samples);
}

void ColumnStatistics::compute_quantiles() {
  auto samples = m_sampler->get_samples();
  if (samples.size() < 100) return;

  std::sort(samples.begin(), samples.end());

  m_quantiles = std::make_unique<Quantiles>();
  m_quantiles->compute(samples);
}

bool ColumnStatistics::is_string_type() const {
  return m_column_type == MYSQL_TYPE_VARCHAR || m_column_type == MYSQL_TYPE_STRING ||
         m_column_type == MYSQL_TYPE_VAR_STRING || m_column_type == MYSQL_TYPE_BLOB;
}
}  // namespace Imcs
}  // namespace ShannonBase