/* Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <gtest/gtest.h>
#include <vector>

#include "storage/rapid_engine/imcs/col0stats.h"

namespace ShannonBase {
namespace Imcs {

TEST(ColumnStatisticsTest, ConstantRangeSelectivity) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  stats.update(42.0);
  stats.finalize();

  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(42.0, 42.0), 1.0);
  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(0.0, 42.0), 1.0);
  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(42.0, 100.0), 1.0);
  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(0.0, 41.9), 0.0);
  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(43.0, 100.0), 0.0);
}

TEST(ColumnStatisticsTest, StringRangeSelectivityFallback) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_VAR_STRING);
  stats.update(std::string("abc"));
  stats.finalize();

  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(0.0, 1.0), 0.5);
  EXPECT_DOUBLE_EQ(stats.estimate_range_selectivity(1.0, 0.0), 0.0);
}

TEST(ColumnStatisticsTest, NullSelectivity) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  stats.update_null();
  stats.finalize();

  EXPECT_DOUBLE_EQ(stats.estimate_null_selectivity(), 1.0);
}

TEST(EquiHeightHistogramTest, BuildAndEstimateSelectivity) {
  ColumnStatistics::EquiHeightHistogram hist(2);
  hist.build({1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0});

  EXPECT_EQ(hist.get_bucket_count(), 2u);
  EXPECT_EQ(hist.get_total_rows(), 8u);
  EXPECT_DOUBLE_EQ(hist.estimate_selectivity(1.0, 2.0), 0.5);
  EXPECT_DOUBLE_EQ(hist.estimate_equality_selectivity(1.0), 0.25);
  // 3.5 is absent from the data but falls inside the [3,4] bucket, and an
  // equi-height histogram cannot tell those apart: it spreads the bucket's
  // rows over its distinct values (4 rows / 2 values / 8 total). Expecting 0
  // here would demand per-value precision the structure does not carry.
  EXPECT_DOUBLE_EQ(hist.estimate_equality_selectivity(3.5), 0.25);
  // Outside every bucket, though, absence *is* provable.
  EXPECT_DOUBLE_EQ(hist.estimate_equality_selectivity(9.0), 0.0);
}

TEST(HyperLogLogTest, AddAndMergeEstimations) {
  ColumnStatistics::HyperLogLog hll1;
  ColumnStatistics::HyperLogLog hll2;

  for (uint64_t i = 1; i <= 50; ++i) {
    hll1.add(i);
    hll2.add(i + 50);
  }

  uint64_t estimate1 = hll1.estimate();
  uint64_t estimate2 = hll2.estimate();
  EXPECT_GT(estimate1, 0u);
  EXPECT_GT(estimate2, 0u);

  hll1.merge(hll2);
  uint64_t merged_estimate = hll1.estimate();
  EXPECT_GE(merged_estimate, estimate1);
  EXPECT_GE(merged_estimate, estimate2);
}

TEST(ColumnStatisticsTest, EqualitySelectivityUsesNDVFallback) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  stats.update(1.0);
  stats.update(2.0);
  stats.finalize();

  EXPECT_DOUBLE_EQ(stats.estimate_equality_selectivity(1.0), 0.5);
}

TEST(ReservoirSamplerTest, SampleRateAndSize) {
  ColumnStatistics::ReservoirSampler sampler(10);
  for (uint64_t i = 0; i < 1000; ++i) {
    sampler.add(static_cast<double>(i));
  }

  EXPECT_EQ(sampler.get_samples().size(), 10u);
  EXPECT_NEAR(sampler.get_sample_rate(), 0.01, 1e-6);
}

/**
 * Below 100 samples finalize() must publish no histogram rather than leave the
 * previous one standing -- estimate_range_selectivity() prefers the histogram
 * over every other estimate.
 *
 * A guard, not a live bug: ReservoirSampler never shrinks, so today a second
 * finalize() cannot see fewer samples. It matters the moment anything resets
 * the sampler or reuses a ColumnStatistics across a reload.
 */
TEST(ColumnStatisticsTest, FinalizePublishesNoHistogramBelowTheSampleFloor) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  for (int i = 0; i < 99; ++i) stats.update(static_cast<double>(i));
  stats.finalize();

  EXPECT_EQ(nullptr, stats.get_histogram()) << "a histogram was built from fewer than 100 samples";

  // Crossing the floor publishes one describing the values fed in.
  for (int i = 99; i < 400; ++i) stats.update(static_cast<double>(i));
  stats.finalize();
  ASSERT_NE(nullptr, stats.get_histogram());
  EXPECT_EQ(400u, stats.get_histogram()->get_total_rows());
}

/** Same reasoning for variance: too few samples must zero it, not leave the
 *  previous value published. */
TEST(ColumnStatisticsTest, FinalizeZeroesVarianceBelowTwoSamples) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  stats.update(10.0);
  stats.finalize();

  EXPECT_DOUBLE_EQ(0.0, stats.get_basic_stats().variance.load());
  EXPECT_DOUBLE_EQ(0.0, stats.get_basic_stats().stddev.load());

  // Two samples give a real variance, so the zero above is the guard firing.
  stats.update(20.0);
  stats.finalize();
  EXPECT_GT(stats.get_basic_stats().variance.load(), 0.0);
  EXPECT_GT(stats.get_basic_stats().stddev.load(), 0.0);
}

/** The published version must advance on every finalize(): a reader seeing the
 *  same version twice keeps its cached estimate. */
TEST(ColumnStatisticsTest, FinalizeAdvancesThePublishedVersion) {
  ColumnStatistics stats(0, "col", MYSQL_TYPE_LONG);
  const uint64_t before = stats.get_basic_stats().version.load();

  stats.update(1.0);
  stats.finalize();
  const uint64_t after_one = stats.get_basic_stats().version.load();
  EXPECT_GT(after_one, before);

  stats.update(2.0);
  stats.finalize();
  EXPECT_GT(stats.get_basic_stats().version.load(), after_one);
}

}  // namespace Imcs
}  // namespace ShannonBase
