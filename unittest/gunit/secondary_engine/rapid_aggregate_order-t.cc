// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <limits>

#include "storage/rapid_engine/cost/cost.h"
using AggregateOrderEstimator = ShannonBase::Optimizer::RpdCostEstimator::AggregateOrderEstimator;

TEST(RapidAggregateOrder, ScaleAndBudgetChangeTheChoice) {
  // Identical shape, projection and cost rates. Scaling the data crosses the
  // memory boundary without an SF-specific row-count cutoff.
  const auto small = AggregateOrderEstimator::estimate(4000, 1000, 41, 320, 1024 * 1024);
  const auto large = AggregateOrderEstimator::estimate(40000, 10000, 41, 320, 1024 * 1024);
  const auto granted = AggregateOrderEstimator::estimate(40000, 10000, 41, 320, 4 * 1024 * 1024);
  EXPECT_FALSE(small.prefer_ordered(.003, .003, 2.0 / 16384));
  EXPECT_TRUE(large.prefer_ordered(.003, .003, 2.0 / 16384));
  EXPECT_FALSE(granted.prefer_ordered(.003, .003, 2.0 / 16384));
}

TEST(RapidAggregateOrder, FewGroupsStayInMemoryAtLargeInputCardinality) {
  const auto work = AggregateOrderEstimator::estimate(12000000, 4, 41, 320, 1024 * 1024);
  EXPECT_EQ(0, work.spill_bytes);
  EXPECT_FALSE(work.prefer_ordered(.003, .003, 2.0 / 16384));
}

TEST(RapidAggregateOrder, InvalidStatisticsCannotSelectStreaming) {
  for (double rows : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(AggregateOrderEstimator::estimate(rows, 1, 41, 320, 1024).prefer_ordered(.003, .003, 1));
  EXPECT_FALSE(AggregateOrderEstimator::estimate(10, 11, 41, 320, 1024).valid);
}
