/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include "storage/rapid_engine/utils/utils.h"
#include <thread>
#include <vector>

#include "storage/rapid_engine/resource_management/res_mgmt.h"

namespace ShannonBase::ResMgmt {
TEST(RapidResourcesTest, PlanSharingLeavesRoomForParentOperators) {
  int query;
  MemoryBudget budget(256, 64);
  std::vector<MemoryBudget::Lease> leases;
  {
    PlanningMemoryScope scope(&query, 64, 8);
    for (int i = 0; i < 8; ++i) {
      leases.push_back(budget.Reserve(&query, PlanningMemoryScope::LimitRequest(&query, 16)));
      EXPECT_EQ(8u, leases.back().bytes());
    }
    EXPECT_EQ(64u, budget.reserved());
    {
      PlanningMemoryScope nested(&query, 64, 1);
      EXPECT_EQ(8u, PlanningMemoryScope::LimitRequest(&query, 16));
    }
  }
  EXPECT_EQ(16u, PlanningMemoryScope::LimitRequest(&query, 16));
  leases.clear();
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, QueryAndGlobalReservationsShareOneLimit) {
  MemoryBudget budget(100, 60);
  int a, b;
  auto first = budget.Reserve(&a, 40);
  auto second = budget.Reserve(&a, 40);
  EXPECT_EQ(40u, first.bytes());
  EXPECT_EQ(20u, second.bytes());
  auto other = budget.Reserve(&b, 60);
  EXPECT_EQ(40u, other.bytes());
  EXPECT_FALSE(budget.Reserve(&b, 1));
  EXPECT_EQ(100u, budget.reserved());
  second.Reset();
  EXPECT_EQ(80u, budget.reserved());
  EXPECT_EQ(20u, budget.Reserve(&b, 30).bytes());
}

TEST(RapidResourcesTest, RejectedMinimumDoesNotConsumeCapacity) {
  MemoryBudget budget(100, 60);
  int query;
  EXPECT_FALSE(budget.Reserve(&query, 80, 64));
  EXPECT_EQ(0u, budget.reserved());
  EXPECT_EQ(60u, budget.Reserve(&query, 60, 60).bytes());
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, LeaseMoveTransfersOwnershipExactlyOnce) {
  MemoryBudget budget(100, 100);
  int query;
  auto first = budget.Reserve(&query, 60);
  auto second = budget.Reserve(&query, 40);
  second = std::move(first);
  EXPECT_EQ(60u, budget.reserved());
  EXPECT_EQ(0u, first.bytes());
  second.Reset();
  EXPECT_EQ(0u, budget.reserved());
  EXPECT_EQ(100u, budget.Reserve(&query, 100).bytes());
}

TEST(RapidResourcesTest, ConcurrentOperatorsCannotDuplicateHeadroom) {
  MemoryBudget budget(4096, 1024);
  std::atomic<bool> violated{false};
  std::vector<std::thread> workers;
  int queries[16];
  for (auto &query : queries) {
    workers.emplace_back([&budget, &violated, key = &query] {
      for (int iteration = 0; iteration < 1000; ++iteration) {
        auto lease = budget.Reserve(key, 1024, 1024);
        if (budget.reserved() > 4096) violated.store(true);
        std::this_thread::yield();
      }
    });
  }
  for (auto &worker : workers) worker.join();
  EXPECT_FALSE(violated.load());
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, ReallocationChargesBothBuffersAndPreservesOldValue) {
  BoundedMemoryResource resource(40);
  {
    std::pmr::vector<unsigned char> bytes(&resource);
    bytes.resize(16, 7);
    EXPECT_THROW(bytes.reserve(32), std::bad_alloc);
    EXPECT_EQ(16u, resource.used());
    ASSERT_EQ(16u, bytes.size());
    for (auto value : bytes) EXPECT_EQ(7, value);
    EXPECT_LE(resource.peak(), 40u);
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, FailedAllocationUnwindsReservation) {
  MemoryBudget budget(64, 64);
  int query;
  EXPECT_THROW({
    auto lease = budget.Reserve(&query, 64);
    BoundedMemoryResource resource(lease.bytes());
    std::pmr::vector<unsigned char> bytes(&resource);
    bytes.resize(65);
  }, std::bad_alloc);
  EXPECT_EQ(0u, budget.reserved());
}
}  // namespace ShannonBase::ResMgmt

namespace ShannonBase::Utils {
TEST(MaintenanceWaitTest, ShorteningRestartsDeadline) {
  using namespace std::chrono_literals;
  MaintenanceWait timer(10s);
  std::atomic<bool> expired{false};
  std::promise<void> waiting;
  std::thread waiter([&] { expired = timer.wait([&] { waiting.set_value(); }); });
  waiting.get_future().wait();
  timer.set_interval(10ms);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!expired && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  EXPECT_TRUE(expired);
  timer.stop();
  waiter.join();
}

TEST(MaintenanceWaitTest, LengtheningDoesNotRunAtOldDeadline) {
  using namespace std::chrono_literals;
  MaintenanceWait timer(50ms);
  std::atomic<bool> expired{false};
  std::promise<void> waiting;
  std::thread waiter([&] { expired = timer.wait([&] { waiting.set_value(); }); });
  waiting.get_future().wait();
  timer.set_interval(10s);
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(expired);
  timer.stop();
  waiter.join();
  EXPECT_FALSE(expired);
}

TEST(MaintenanceWaitTest, ConfiguredStartupAndRestartUseCurrentInterval) {
  using namespace std::chrono_literals;
  MaintenanceWait timer(10s);
  timer.set_interval(1ms);
  timer.start();
  EXPECT_EQ(1ms, timer.interval());
  EXPECT_TRUE(timer.wait());
  timer.stop();
  EXPECT_FALSE(timer.wait());
  timer.set_interval(2ms);
  timer.start();
  EXPECT_TRUE(timer.wait());
  timer.stop();
}
}  // namespace ShannonBase::Utils
