/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
#include "storage/rapid_engine/utils/utils.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/resource_management/res_mgmt.h"

namespace ShannonBase::ResMgmt {
TEST(RapidResourcesTest, PlanSharingLeavesRoomForParentOperators) {
  int query = 0;
  MemoryBudget budget(256, 64);
  std::vector<MemoryBudget::Reservation> leases;
  {
    PlanningMemoryScope scope(&query, 64, 8);
    for (int i = 0; i < 8; ++i) {
      leases.push_back(budget.reserve(&query, PlanningMemoryScope::limit_request(&query, 16)));
      EXPECT_EQ(8u, leases.back().bytes());
    }
    EXPECT_EQ(64u, budget.reserved());
    {
      PlanningMemoryScope nested(&query, 64, 1);
      EXPECT_EQ(8u, PlanningMemoryScope::limit_request(&query, 16));
    }
  }
  EXPECT_EQ(16u, PlanningMemoryScope::limit_request(&query, 16));
  leases.clear();
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, QueryAndGlobalReservationsShareOneLimit) {
  MemoryBudget budget(100, 60);
  int a = 0, b = 0;
  auto first = budget.reserve(&a, 40);
  auto second = budget.reserve(&a, 40);
  EXPECT_EQ(40u, first.bytes());
  EXPECT_EQ(20u, second.bytes());
  auto other = budget.reserve(&b, 60);
  EXPECT_EQ(40u, other.bytes());
  EXPECT_FALSE(budget.reserve(&b, 1));
  EXPECT_EQ(100u, budget.reserved());
  second.reset();
  EXPECT_EQ(80u, budget.reserved());
  EXPECT_EQ(20u, budget.reserve(&b, 30).bytes());
}

TEST(RapidResourcesTest, RejectedMinimumDoesNotConsumeCapacity) {
  MemoryBudget budget(100, 60);
  int query = 0;
  EXPECT_FALSE(budget.reserve(&query, 80, 64));
  EXPECT_EQ(0u, budget.reserved());
  EXPECT_EQ(60u, budget.reserve(&query, 60, 60).bytes());
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, LeaseMoveTransfersOwnershipExactlyOnce) {
  MemoryBudget budget(100, 100);
  int query = 0;
  auto first = budget.reserve(&query, 60);
  auto second = budget.reserve(&query, 40);
  second = std::move(first);
  EXPECT_EQ(60u, budget.reserved());
  EXPECT_EQ(0u, first.bytes());
  second.reset();
  EXPECT_EQ(0u, budget.reserved());
  EXPECT_EQ(100u, budget.reserve(&query, 100).bytes());
}

TEST(RapidResourcesTest, ConcurrentOperatorsCannotDuplicateHeadroom) {
  MemoryBudget budget(4096, 1024);
  std::atomic<bool> violated{false};
  std::vector<std::thread> workers;
  int queries[16];
  for (auto &query : queries) {
    workers.emplace_back([&budget, &violated, key = &query] {
      for (int iteration = 0; iteration < 1000; ++iteration) {
        auto lease = budget.reserve(key, 1024, 1024);
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

TEST(RapidResourcesTest, AuxiliaryContainersShareCapacityAndGrowthPeak) {
  BoundedMemoryResource resource(160);
  {
    std::pmr::vector<uint64_t> ordinals(&resource);
    std::pmr::vector<uint8_t> flags(&resource);
    ordinals.resize(8, 17);  // 64 bytes
    flags.resize(32, 1);
    // Retained replacement would fit, but the simultaneous old and new
    // allocations would require 192 bytes.
    EXPECT_THROW(ordinals.reserve(12), std::bad_alloc);
    EXPECT_EQ(96u, resource.used());
    EXPECT_EQ(8u, ordinals.size());
    for (auto ordinal : ordinals) EXPECT_EQ(17u, ordinal);
    // Returning a sibling's capacity makes the same growth legal.
    decltype(flags)(&resource).swap(flags);
    ordinals.reserve(12);
    EXPECT_EQ(96u, resource.used());
    EXPECT_EQ(160u, resource.peak());
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, ConcurrentAuxiliaryRefusalReleasesOperatorGrants) {
  MemoryBudget budget(4096, 1024);
  std::atomic<bool> violated{false};
  int queries[8];
  std::vector<std::thread> workers;
  for (auto &query : queries) {
    workers.emplace_back([&budget, &violated, key = &query] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        auto grant = budget.reserve(key, 1024, 1024);
        if (!grant) continue;
        BoundedMemoryResource auxiliary(grant.bytes() / 16);
        {
          std::pmr::vector<uint64_t> ordinals(&auxiliary);
          ordinals.resize(4, 23);
          try {
            ordinals.reserve(8);
            violated.store(true);
          } catch (const std::bad_alloc &) {
            if (auxiliary.used() != 32 || ordinals.size() != 4 || ordinals[0] != 23) violated.store(true);
          }
        }
        if (auxiliary.used() != 0 || auxiliary.peak() > grant.bytes() / 16 || budget.reserved() > 4096)
          violated.store(true);
      }
    });
  }
  for (auto &worker : workers) worker.join();
  EXPECT_FALSE(violated.load());
  EXPECT_EQ(0u, budget.reserved());
}

TEST(RapidResourcesTest, ExternalChargesCompeteWithAllocatorBuffers) {
  BoundedMemoryResource resource(96);
  {
    BoundedMemoryResource::Reservation external(&resource, 64);
    std::pmr::vector<unsigned char> bytes(&resource);
    bytes.resize(16, 9);
    EXPECT_THROW(bytes.reserve(32), std::bad_alloc);
    EXPECT_EQ(80u, resource.used());
    BoundedMemoryResource::Reservation moved(std::move(external));
    EXPECT_EQ(80u, resource.used());
    moved.reset();
    bytes.reserve(32);
    EXPECT_EQ(32u, resource.used());
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, ColumnGrowthRefusalPreservesRowsAndMovesCharge) {
  Field_long field(11, true, "value", false);
  const size_t old_bytes = 16 + 1 + sizeof(bit_array_t);
  const size_t replacement = 32 + 1 + sizeof(bit_array_t);
  BoundedMemoryResource resource(old_bytes + replacement - 1);
  {
    Executor::ColumnChunk column(&field, 4, sizeof(int32_t), &resource);
    int32_t value = 73;
    ASSERT_TRUE(column.add(reinterpret_cast<const uchar *>(&value), sizeof(value), false));
    ASSERT_TRUE(column.add(nullptr, 0, true));
    EXPECT_THROW(column.grow(8), std::bad_alloc);
    EXPECT_EQ(old_bytes, resource.used());
    EXPECT_EQ(4u, column.capacity());
    EXPECT_EQ(2u, column.size());
    int32_t restored;
    std::memcpy(&restored, column.data_fast(0), sizeof(restored));
    EXPECT_EQ(73, restored);
    EXPECT_TRUE(column.nullable_fast(1));
    Executor::ColumnChunk moved(std::move(column));
    EXPECT_EQ(old_bytes, resource.used());
    EXPECT_EQ(2u, moved.size());
    {
      Executor::ColumnChunk copy(moved);
      EXPECT_EQ(2 * old_bytes, resource.used());
      EXPECT_TRUE(copy.nullable_fast(1));
    }
    EXPECT_EQ(old_bytes, resource.used());
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, PackedRowGrowthRefusalKeepsChargedBuffer) {
  BoundedMemoryResource resource(96);
  {
    std::pmr::vector<char> storage(&resource);
    String row;
    Executor::PrepareBudgetedString(&row, &storage, 32);
    ASSERT_EQ(48u, resource.used());
    ASSERT_FALSE(row.append("row", 3));
    EXPECT_THROW(Executor::PrepareBudgetedString(&row, &storage, 64), std::bad_alloc);
    EXPECT_EQ(48u, resource.used());
    EXPECT_EQ(3u, row.length());
    EXPECT_EQ(0, std::memcmp(row.ptr(), "row", 3));
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, UpstreamFailureRollsBackChargeAndKeepsLiveVector) {
  class FailingResource final : public std::pmr::memory_resource {
   public:
    bool fail{false};

   private:
    void *do_allocate(size_t bytes, size_t alignment) override {
      if (fail) throw std::bad_alloc();
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
      std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
  } upstream;
  BoundedMemoryResource resource(128, &upstream);
  {
    std::pmr::vector<uint64_t> rows(&resource);
    rows.resize(4, 41);
    upstream.fail = true;
    EXPECT_THROW(rows.reserve(8), std::bad_alloc);
    EXPECT_EQ(32u, resource.used());
    EXPECT_EQ(4u, rows.size());
    EXPECT_EQ(41u, rows[0]);
    upstream.fail = false;
    rows.reserve(8);
    EXPECT_EQ(64u, resource.used());
  }
  EXPECT_EQ(0u, resource.used());
}

TEST(RapidResourcesTest, FailedAllocationUnwindsReservation) {
  MemoryBudget budget(64, 64);
  int query = 0;
  EXPECT_THROW({
    auto lease = budget.reserve(&query, 64);
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
