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
  EXPECT_DOUBLE_EQ(hist.estimate_equality_selectivity(3.5), 0.0);
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

}  // namespace Imcs
}  // namespace ShannonBase
