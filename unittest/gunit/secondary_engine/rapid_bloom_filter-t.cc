/* Copyright (c) 2019, 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#include <gtest/gtest.h>
#include <memory_resource>
#include <random>
#include "storage/rapid_engine/optimizer/runtime_bloom_filter.h"

using ShannonBase::Utils::RuntimeBloomFilter;

TEST(RapidBloomFilter, DisabledAndBudgetFailureNeverReject) {
  RuntimeBloomFilter filter(std::pmr::new_delete_resource());
  EXPECT_TRUE(filter.may_contain(42));
  EXPECT_FALSE(filter.prepare(1000, 1));
  EXPECT_FALSE(filter.prepare(0, 1000));
  EXPECT_FALSE(filter.prepare(SIZE_MAX, SIZE_MAX));
  EXPECT_EQ(0U, filter.bytes());
  EXPECT_TRUE(filter.may_contain(42));
}

TEST(RapidBloomFilter, NoFalseNegativesAndBoundedFalsePositives) {
  for (size_t n : {1U, 16U, 17U, 128U, 10000U}) {
    RuntimeBloomFilter filter(std::pmr::new_delete_resource());
    ASSERT_TRUE(filter.prepare(n, 1024 * 1024));
    std::mt19937_64 keys(123);
    for (size_t i = 0; i < n; ++i) filter.insert(keys());
    keys.seed(123);
    for (size_t i = 0; i < n; ++i) ASSERT_TRUE(filter.may_contain(keys()));
    size_t positives = 0;
    for (size_t i = 0; i < 100000; ++i) positives += filter.may_contain(keys());
    EXPECT_LT(positives, 5000U);  // conservative 5% bound, deterministic seed
    filter.reset();
    EXPECT_EQ(0U, filter.bytes());
    EXPECT_TRUE(filter.may_contain(0));
  }
}

TEST(RapidBloomFilter, RebuildAndDuplicates) {
  RuntimeBloomFilter filter(std::pmr::new_delete_resource());
  ASSERT_TRUE(filter.prepare(128, 4096));
  for (int i = 0; i < 10000; ++i) filter.insert(UINT64_MAX);
  EXPECT_TRUE(filter.may_contain(UINT64_MAX));
  EXPECT_FALSE(filter.may_contain(0));
  ASSERT_TRUE(filter.prepare(128, 4096));
  EXPECT_FALSE(filter.may_contain(UINT64_MAX));
  filter.insert(0);
  EXPECT_TRUE(filter.may_contain(0));
}

TEST(RapidBloomFilter, AllocationFailureIsOptional) {
  RuntimeBloomFilter filter(std::pmr::null_memory_resource());
  EXPECT_FALSE(filter.prepare(128, 4096));
  EXPECT_EQ(0U, filter.bytes());
  EXPECT_TRUE(filter.may_contain(42));
}

TEST(RapidBloomPolicy, EffectiveResourcesAndQueuedWorkReduceAllowance) {
  using ShannonBase::Utils::RuntimeBloomPolicy;
  constexpr size_t mib = 1024 * 1024;
  const auto idle = RuntimeBloomPolicy::choose(64*mib, 16*mib, 512*mib, 32*mib, 64*mib, 1, false);
  const auto busy = RuntimeBloomPolicy::choose(64*mib, 16*mib, 512*mib, 32*mib, 64*mib, 8, true);
  EXPECT_GT(idle.max_bytes, busy.max_bytes);
  EXPECT_LT(idle.min_reject_percent, busy.min_reject_percent);
  const auto full = RuntimeBloomPolicy::choose(64*mib, 16*mib, 512*mib, 512*mib, 64*mib, 1, false);
  EXPECT_EQ(0U, full.max_bytes);
  const auto reserved = RuntimeBloomPolicy::choose(64*mib, 16*mib, 512*mib, 32*mib, 512*mib, 1, false);
  EXPECT_EQ(0U, reserved.max_bytes);
  const auto exhausted = RuntimeBloomPolicy::choose(64*mib, 64*mib, 512*mib, 32*mib, 64*mib, 1, false);
  EXPECT_EQ(0U, exhausted.max_bytes);
  const auto unknown = RuntimeBloomPolicy::choose(64*mib, 16*mib, 0, 0, 0, 1, false);
  EXPECT_EQ(0U, unknown.max_bytes);
}

TEST(RapidBloomPolicy, ScanPlacementRecoversWithHysteresis) {
  using ShannonBase::Utils::RuntimeBloomPolicy;
  const auto choose = RuntimeBloomPolicy::scan_pushdown_after_sample;
  EXPECT_TRUE(choose(true, 0, 0));
  EXPECT_FALSE(choose(false, 0, 0));
  EXPECT_FALSE(choose(true, 4096, 2047));
  EXPECT_TRUE(choose(true, 4096, 2048));
  EXPECT_FALSE(choose(false, 4096, 3071));
  EXPECT_TRUE(choose(false, 4096, 3072));

  bool active = true;
  active = choose(active, 4096, 819);   // Initial 20% rejection: local only.
  EXPECT_FALSE(active);
  active = choose(active, 4096, 2457);  // 60% must not oscillate placement.
  EXPECT_FALSE(active);
  active = choose(active, 4096, 3276);  // Later 80% rejection: scan again.
  EXPECT_TRUE(active);
  active = choose(active, 4096, 2457);
  EXPECT_TRUE(active);
  active = choose(active, 4096, 819);
  EXPECT_FALSE(active);
  EXPECT_TRUE(choose(false, SIZE_MAX, SIZE_MAX));
}
