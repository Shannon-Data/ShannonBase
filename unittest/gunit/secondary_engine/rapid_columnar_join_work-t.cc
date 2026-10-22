// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <limits>
#include "storage/rapid_engine/cost/cost.h"

using ShannonBase::Optimizer::ColumnarJoinCostEstimator;

TEST(RapidColumnarJoinWork, WiderBuildCanSpillDespiteFewerRows) {
  const auto narrow = ColumnarJoinCostEstimator::estimate(1000, 900, 8, 9);
  const auto wide = ColumnarJoinCostEstimator::estimate(900, 1000, 2048, 9);
  EXPECT_LT(narrow.retained_bytes, 1024 * 1024);
  EXPECT_GT(wide.retained_bytes, 1024 * 1024);
  EXPECT_LT(narrow.build_copy_bytes, wide.build_copy_bytes);
  EXPECT_EQ(narrow.key_bytes, wide.key_bytes);
}

TEST(RapidColumnarJoinWork, UsesFilteredRowsAndProjectedWidth) {
  const auto full = ColumnarJoinCostEstimator::estimate(10000, 20000, 1024, 9);
  const auto filtered = ColumnarJoinCostEstimator::estimate(100, 20000, 1024, 9);
  const auto projected = ColumnarJoinCostEstimator::estimate(10000, 20000, 8, 9);
  EXPECT_DOUBLE_EQ(full.build_copy_bytes / 100, filtered.build_copy_bytes);
  EXPECT_DOUBLE_EQ(full.retained_bytes / 100, filtered.retained_bytes);
  EXPECT_LT(projected.retained_bytes, full.retained_bytes);
}

TEST(RapidColumnarJoinWork, CompositeKeysChargeBothInputsAndRetainedArena) {
  const auto single = ColumnarJoinCostEstimator::estimate(128, 2048, 8, 9);
  const auto composite = ColumnarJoinCostEstimator::estimate(128, 2048, 8, 27);
  EXPECT_DOUBLE_EQ(single.key_bytes * 3, composite.key_bytes);
  EXPECT_DOUBLE_EQ(single.retained_bytes + 128 * 18, composite.retained_bytes);
  EXPECT_EQ(single.build_copy_bytes, composite.build_copy_bytes);
}

TEST(RapidColumnarJoinWork, EmptyAndUnknownEstimates) {
  const auto empty = ColumnarJoinCostEstimator::estimate(0, 2048, 8, 9);
  EXPECT_EQ(0, empty.retained_bytes);
  EXPECT_EQ(0, empty.build_copy_bytes);
  for (double unknown : {-1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_EQ(0, ColumnarJoinCostEstimator::estimate(unknown, 128, 8, 9).retained_bytes);
    EXPECT_EQ(0, ColumnarJoinCostEstimator::estimate(128, unknown, 8, 9).key_bytes);
  }
}
