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

uint8_t ColumnStatistics::HyperLogLog::count_leading_zeros(uint64 x) {
  if (x == 0) return 64;

  uint8_t count = 0;
  while ((x & (1ULL << 63)) == 0) {
    count++;
    x <<= 1;
  }
  return count;
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
  m_basic_stats.row_count++;
  m_basic_stats.sum += value;
  m_basic_stats.min_value = std::min(m_basic_stats.min_value, value);
  m_basic_stats.max_value = std::max(m_basic_stats.max_value, value);

  // Add to sampler
  m_sampler->add(value);

  // Update HyperLogLog
  uint64 hash = std::hash<double>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update(const std::string &value) {
  m_basic_stats.row_count++;

  if (value.empty()) {
    if (m_string_stats) {
      m_string_stats->empty_count++;
    }
  }

  if (m_string_stats) {
    if (value < m_string_stats->min_string || m_string_stats->min_string.empty()) {
      m_string_stats->min_string = value;
    }
    if (value > m_string_stats->max_string) {
      m_string_stats->max_string = value;
    }

    size_t len = value.length();
    m_string_stats->avg_length =
        (m_string_stats->avg_length * (m_basic_stats.row_count - 1) + len) / m_basic_stats.row_count;
    m_string_stats->max_length = std::max(m_string_stats->max_length, len);
    m_string_stats->min_length = std::min(m_string_stats->min_length, len);
  }

  // Update HyperLogLog
  uint64 hash = std::hash<std::string>{}(value);
  m_hll->add(hash);
}

void ColumnStatistics::update_null() {
  m_basic_stats.row_count++;
  m_basic_stats.null_count++;
}

void ColumnStatistics::finalize() {
  // Calculate average
  if (m_basic_stats.row_count > 0) {
    m_basic_stats.avg = m_basic_stats.sum / m_basic_stats.row_count;
    m_basic_stats.null_fraction = static_cast<double>(m_basic_stats.null_count) / m_basic_stats.row_count;
  }

  // Estimate unique value count
  m_basic_stats.distinct_count = m_hll->estimate();

  if (m_basic_stats.row_count > 0) {
    m_basic_stats.cardinality = static_cast<double>(m_basic_stats.distinct_count) / m_basic_stats.row_count;
  }

  // Calculate variance and standard deviation
  compute_variance();

  // Build histogram
  build_histogram();

  // Calculate quantiles
  compute_quantiles();

  // Update timestamp and version
  m_last_update = std::chrono::system_clock::now();
  m_version++;
}

double ColumnStatistics::estimate_range_selectivity(double lower, double upper) const {
  if (m_histogram) {
    return m_histogram->estimate_selectivity(lower, upper);
  }

  // Fallback: assume uniform distribution
  if (m_basic_stats.max_value == m_basic_stats.min_value) {
    return 0.0;
  }

  double range = m_basic_stats.max_value - m_basic_stats.min_value;
  double query_range = upper - lower;

  return std::min(1.0, query_range / range);
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

double ColumnStatistics::estimate_null_selectivity() const { return m_basic_stats.null_fraction; }

bool ColumnStatistics::serialize(std::ostream &out) const {
  // Serialize column metadata
  out.write(reinterpret_cast<const char *>(&m_column_id), sizeof(m_column_id));

  size_t name_len = m_column_name.length();
  out.write(reinterpret_cast<const char *>(&name_len), sizeof(name_len));
  out.write(m_column_name.c_str(), name_len);

  out.write(reinterpret_cast<const char *>(&m_column_type), sizeof(m_column_type));

  // Serialize basic statistics
  out.write(reinterpret_cast<const char *>(&m_basic_stats), sizeof(m_basic_stats));

  // Serialize string statistics
  bool has_string_stats = (m_string_stats != nullptr);
  out.write(reinterpret_cast<const char *>(&has_string_stats), sizeof(has_string_stats));

  if (has_string_stats) {
    // TODO: Serialize string statistics
  }

  // TODO: Serialize histogram, quantiles, etc.

  return out.good();
}

bool ColumnStatistics::deserialize(std::istream &in) {
  // TODO: Implement deserialization
  return in.good();
}

void ColumnStatistics::dump(std::ostream &out) const {
  out << "Column Statistics for '" << m_column_name << "' (ID: " << m_column_id << "):\n";
  out << "  Type: " << static_cast<int>(m_column_type) << "\n";
  out << "  Row Count: " << m_basic_stats.row_count << "\n";
  out << "  NULL Count: " << m_basic_stats.null_count << " (" << (m_basic_stats.null_fraction * 100) << "%)\n";
  out << "  Distinct Count: " << m_basic_stats.distinct_count << " (Cardinality: " << m_basic_stats.cardinality
      << ")\n";
  out << "  Min: " << m_basic_stats.min_value << "\n";
  out << "  Max: " << m_basic_stats.max_value << "\n";
  out << "  Avg: " << m_basic_stats.avg << "\n";
  out << "  StdDev: " << m_basic_stats.stddev << "\n";

  if (m_string_stats) {
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