/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */

#include "storage/rapid_engine/executor/iterators/sort_iterator.h"

#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {
void CheckSort(const std::vector<unsigned char> &keys, size_t width) {
  const size_t rows = keys.size() / width;
  std::vector<uint32_t> order(rows), reference(rows);
  std::iota(order.begin(), order.end(), 0);
  reference = order;
  const auto less = [&](uint32_t a, uint32_t b) {
    return memcmp(keys.data() + size_t{a} * width, keys.data() + size_t{b} * width, width) < 0;
  };
  std::sort(reference.begin(), reference.end(), less);
  std::vector<size_t> scratch(ShannonBase::Executor::FixedKeySortScratchSize(width));
  ShannonBase::Executor::SortFixedDistinctKeys(order.data(), rows, keys.data(), width, scratch.data());
  // Check every output key, and separately verify the index permutation: equal
  // keys may pick different representatives, but no row may be lost or copied.
  for (size_t i = 0; i < rows; ++i) {
    ASSERT_LT(order[i], rows);
    ASSERT_EQ(0, memcmp(keys.data() + size_t{order[i]} * width,
                        keys.data() + size_t{reference[i]} * width, width));
  }
  std::sort(order.begin(), order.end());
  for (size_t i = 0; i < rows; ++i) ASSERT_EQ(i, order[i]);
}

TEST(RapidFixedKeySort, ArbitraryEncodedKeysAndPartitionBoundaries) {
  std::mt19937_64 random(1026);
  for (size_t width = 1; width <= ShannonBase::Executor::kRadixMaxKeyBytes; ++width) {
    for (size_t rows : {0, 1, 2, 63, 64, 65, 511, 512, 513, 8193}) {
      SCOPED_TRACE(::testing::Message() << "width=" << width << " rows=" << rows);
      std::vector<unsigned char> keys(rows * width);
      for (auto &byte : keys) byte = random();
      CheckSort(keys, width);
    }
  }
}

TEST(RapidFixedKeySort, DuplicateSkewAndLongCommonPrefixes) {
  std::mt19937_64 random(22);
  for (size_t width = 1; width <= ShannonBase::Executor::kRadixMaxKeyBytes; ++width) {
    std::vector<unsigned char> keys(10003 * width, 0xa5);
    for (size_t i = 0; i < 10003; ++i) keys[i * width + width - 1] = (i % 10) ? 0 : random() % 37;
    CheckSort(keys, width);
  }
}

TEST(RapidFixedKeySort, OrderedReverseOrderedAndConstant) {
  for (size_t width = 1; width <= ShannonBase::Executor::kRadixMaxKeyBytes; ++width) {
    for (int shape = 0; shape < 3; ++shape) {
      std::vector<unsigned char> keys(10003 * width, 0);
      for (size_t i = 0; i < 10003; ++i)
        keys[i * width + width - 1] = shape == 0 ? i / 40 : shape == 1 ? (10002 - i) / 40 : 1;
      CheckSort(keys, width);
    }
  }
}

TEST(RapidFixedKeySort, WideKeysRequireLargerRuns) {
  using ShannonBase::Executor::UseRadixForDistinct;
  EXPECT_FALSE(UseRadixForDistinct(511, 9));
  EXPECT_TRUE(UseRadixForDistinct(512, 9));
  EXPECT_FALSE(UseRadixForDistinct(4095, 10));
  EXPECT_TRUE(UseRadixForDistinct(4096, 10));
  EXPECT_FALSE(UseRadixForDistinct(1024, 16));
  EXPECT_TRUE(UseRadixForDistinct(4096, 16));
  EXPECT_FALSE(UseRadixForDistinct(1000000, 17));
  EXPECT_FALSE(UseRadixForDistinct(1000000, 0));
}
}  // namespace
