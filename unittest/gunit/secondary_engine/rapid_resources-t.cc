/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
#include "storage/rapid_engine/utils/memory_pool.h"
#include "storage/rapid_engine/utils/utils.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/resource_management/res_mgmt.h"
#include "storage/rapid_engine/resource_management/io_scheduler.h"

namespace ShannonBase::ResMgmt {
TEST(RapidResourcesTest, SystemPoolPolicyUsesActualRootCapacity) {
  constexpr size_t mib = 1024 * 1024;
  const auto limits = system_memory_limits(2048 * mib);
  EXPECT_EQ(256 * mib, limits.pool);
  EXPECT_EQ(192 * mib, limits.query_total);
  EXPECT_EQ(64 * mib, limits.query);
  EXPECT_EQ(16 * mib, limits.operation);
  EXPECT_EQ(0u, system_memory_limits(1).pool);
  EXPECT_LE(system_memory_limits(SIZE_MAX).pool, SIZE_MAX / 8);
}

class RapidSystemPoolTest : public ::testing::Test {
 protected:
  std::shared_ptr<Utils::MemoryPool> root;
  void SetUp() override {
    Utils::MemoryPool::Config config(128 * 1024 * 1024);
    config.small_pool_ratio = 0;
    config.auto_defragmentation = false;
    root = std::make_shared<Utils::MemoryPool>(config);
    ASSERT_TRUE(initialize_system_pool(root));
  }
  void TearDown() override { shutdown_system_pool(); }
};
TEST_F(RapidSystemPoolTest, QueryMemoryBudgetExplicitlyListsSupportedOperators) {
  EXPECT_STREQ("sort", query_memory_operator_name(QueryMemoryOperator::kSort));
  EXPECT_STREQ("hash_aggregate", query_memory_operator_name(QueryMemoryOperator::kHashAggregate));
  EXPECT_STREQ("hash_join", query_memory_operator_name(QueryMemoryOperator::kHashJoin));
  EXPECT_STREQ("window", query_memory_operator_name(QueryMemoryOperator::kWindow));
  EXPECT_STREQ("invalid", query_memory_operator_name(QueryMemoryOperator::kCount));

  int query = 0;
  auto grant = reserve_query_memory(&query, QueryMemoryOperator::kSort,
                                   system_memory_limits().operation / 2);
  EXPECT_TRUE(grant);
  EXPECT_GT(grant.bytes(), 0u);
}

TEST_F(RapidSystemPoolTest, QueryBuffersAllocateFromPoolAndReturnCapacity) {
  const auto root_used = root->stats().used_bytes;
  EXPECT_EQ(system_memory_limits().pool, root_used);
  {
    BoundedMemoryResource resource(4096);
    std::pmr::vector<uint64_t> values(&resource);
    values.resize(128, 47);
    EXPECT_EQ(1024u, system_pool_stats().used);
    EXPECT_EQ(root_used, root->stats().used_bytes);
    void *aligned = resource.allocate(256, 256);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(aligned) % 256);
    resource.deallocate(aligned, 256, 256);
    EXPECT_EQ(47u, values.back());
  }
  EXPECT_EQ(0u, system_pool_stats().used);
  EXPECT_GE(system_pool_stats().peak, 1024u);
}
TEST_F(RapidSystemPoolTest, ExhaustionHasNoHeapFallbackAndFreeAllowsReuse) {
  const size_t capacity = system_pool_stats().capacity;
  auto buffer = allocate_system_buffer(capacity);
  ASSERT_TRUE(buffer);
  EXPECT_FALSE(allocate_system_buffer(1));
  PoolMemoryResource resource;
  EXPECT_THROW(resource.allocate(1), std::bad_alloc);
  EXPECT_EQ(capacity, system_pool_stats().used);
  buffer.reset();
  EXPECT_EQ(0u, system_pool_stats().used);
  EXPECT_TRUE(allocate_system_buffer(capacity));
}
TEST_F(RapidSystemPoolTest, LiveGrantPreventsResizeAndBufferRetainsParent) {
  int query = 0;
  auto grant = reserve_query_memory(&query, QueryMemoryOperator::kSort, system_memory_limits().operation);
  ASSERT_TRUE(grant);
  EXPECT_FALSE(initialize_system_pool(root));
  grant.reset();
  auto buffer = allocate_system_buffer(1024);
  ASSERT_TRUE(buffer);
  EXPECT_FALSE(initialize_system_pool(root));
  shutdown_system_pool();
  root.reset();
  buffer[0] = 73;
  EXPECT_EQ(73, buffer[0]);
  buffer.reset();
}
TEST_F(RapidSystemPoolTest, ColumnMoveAndGrowthReturnPooledStorage) {
  Field_long field(11, true, "value", false);
  {
    BoundedMemoryResource resource(4096);
    Executor::ColumnChunk column(&field, 4, sizeof(int32_t), &resource);
    int32_t value = 73;
    ASSERT_TRUE(column.add(reinterpret_cast<const uchar *>(&value), sizeof(value), false));
    ASSERT_TRUE(column.add(nullptr, 0, true));
    ASSERT_TRUE(column.grow(32));
    Executor::ColumnChunk moved(std::move(column));
    EXPECT_TRUE(moved.nullable_fast(1));
    EXPECT_EQ(73, *reinterpret_cast<const int32_t *>(moved.data_fast(0)));
    EXPECT_EQ(resource.used(), system_pool_stats().used);
  }
  EXPECT_EQ(0u, system_pool_stats().used);
}

TEST(RapidResourcesTest, SpillDevicesHaveIndependentDepthAndMoveOnlyOwnership) {
  IoDepthGovernor governor;
  auto first = governor.acquire(1, 1, std::chrono::milliseconds(0), [] { return false; });
  ASSERT_TRUE(first);
  EXPECT_FALSE(governor.acquire(1, 1, std::chrono::milliseconds(0), [] { return false; }));
  auto other = governor.acquire(2, 1, std::chrono::milliseconds(0), [] { return false; });
  ASSERT_TRUE(other);
  auto moved = std::move(first);
  first.reset();
  EXPECT_EQ(2u, governor.snapshot().active);
  moved.reset();
  other.reset();
  EXPECT_EQ(0u, governor.snapshot().active);
  EXPECT_EQ(0u, governor.snapshot().waiting);
  EXPECT_EQ(1u, governor.snapshot().rejected);
}
TEST(RapidResourcesTest, CancelledSpillWaiterDoesNotLeakAdmission) {
  IoDepthGovernor governor;
  auto held = governor.acquire(1, 1, std::chrono::milliseconds(0), [] { return false; });
  ASSERT_TRUE(held);
  std::atomic<bool> cancel{false};
  auto waiter = std::async(std::launch::async, [&] {
    return governor.acquire(1, 1, std::chrono::seconds(2), [&] { return cancel.load(); });
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!governor.snapshot().waiting && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  EXPECT_EQ(1u, governor.snapshot().waiting);
  cancel.store(true);
  EXPECT_FALSE(waiter.get());
  held.reset();
  EXPECT_EQ(0u, governor.snapshot().active);
  EXPECT_EQ(0u, governor.snapshot().waiting);
  EXPECT_TRUE(governor.acquire(1, 1, std::chrono::milliseconds(0), [] { return false; }));
}

TEST(RapidResourcesTest, ApplyPoolRespectsAdmissionAndMemoryPlanningBounds) {
  constexpr size_t mib = 1024 * 1024;
  EXPECT_EQ(1u, make_apply_pool_size(1, 2048 * mib));
  EXPECT_EQ(2u, make_apply_pool_size(2, 2048 * mib));
  EXPECT_EQ(2u, make_apply_pool_size(16, 16 * mib));
  EXPECT_EQ(32u, make_apply_pool_size(1024, 2048 * mib));
  EXPECT_EQ(1u, make_apply_pool_size(2, mib));
}

TEST(RapidResourcesTest, WorkAdmissionAndMoveReleaseExactlyOnce) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::SOFT, {1, 1, 1}));
  auto first = governor.try_acquire(WorkClass::AP);
  ASSERT_TRUE(first);
  EXPECT_FALSE(governor.try_acquire(WorkClass::AP));
  auto moved = std::move(first);
  first.reset();
  EXPECT_EQ(1u, governor.snapshot(WorkClass::AP).active);
  moved.reset(); moved.reset();
  EXPECT_EQ(0u, governor.snapshot(WorkClass::AP).active);
  EXPECT_EQ(1u, governor.snapshot(WorkClass::AP).rejected);
  EXPECT_TRUE(governor.try_acquire(WorkClass::AP));
}
TEST(RapidResourcesTest, BackgroundClassesCannotBorrowEachOthersQuota) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::SOFT, {1, 1, 1}));
  auto sync = governor.try_acquire(WorkClass::SYNC);
  auto maint = governor.try_acquire(WorkClass::MAINT);
  auto ap = governor.try_acquire(WorkClass::AP);
  EXPECT_TRUE(sync); EXPECT_TRUE(maint); EXPECT_TRUE(ap);
  EXPECT_FALSE(governor.try_acquire(WorkClass::SYNC));
  EXPECT_FALSE(governor.configure(IsolationMode::OFF, {1,1,1}));
}
TEST(RapidResourcesTest, ObserveCountsWithoutRejectingAtLimit) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::OBSERVE, {1, 1, 1}));
  auto first = governor.try_acquire(WorkClass::AP);
  auto second = governor.try_acquire(WorkClass::AP);
  EXPECT_TRUE(first); EXPECT_TRUE(second);
  EXPECT_EQ(2u, governor.snapshot(WorkClass::AP).peak);
}
static bool wait_for_governor_queue(WorkGovernor &governor, size_t count) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (governor.snapshot(WorkClass::SYNC).waiting != count && std::chrono::steady_clock::now() < end)
    std::this_thread::yield();
  return governor.snapshot(WorkClass::SYNC).waiting == count;
}
TEST(RapidResourcesTest, CancelledHeadDoesNotBlockNextWaiter) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::SOFT, {1, 1, 1}));
  auto active = governor.try_acquire(WorkClass::SYNC);
  std::atomic<bool> cancel{false};
  auto first = std::async(std::launch::async, [&] { return governor.acquire(WorkClass::SYNC, [&] {return cancel.load();}); });
  EXPECT_TRUE(wait_for_governor_queue(governor, 1));
  auto second = std::async(std::launch::async, [&] { return governor.acquire(WorkClass::SYNC, [] {return false;}); });
  EXPECT_TRUE(wait_for_governor_queue(governor, 2));
  cancel.store(true);
  const bool cancelled = first.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(cancelled);
  if (!cancelled) governor.stop();
  EXPECT_FALSE(first.get());
  active.reset();
  const bool ready = second.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(ready);
  if (!ready) governor.stop();
  auto lease = second.get();
  EXPECT_TRUE(lease);
  lease.reset();
  EXPECT_EQ(0u, governor.snapshot(WorkClass::SYNC).active);
  EXPECT_EQ(0u, governor.snapshot(WorkClass::SYNC).waiting);
}
TEST(RapidResourcesTest, ShutdownWakesWaitersAndRejectsNewWork) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::SOFT, {1, 1, 1}));
  auto active = governor.try_acquire(WorkClass::SYNC);
  auto waiter = std::async(std::launch::async, [&] {return governor.acquire(WorkClass::SYNC, [] {return false;});});
  EXPECT_TRUE(wait_for_governor_queue(governor, 1));
  governor.stop();
  EXPECT_FALSE(waiter.get());
  EXPECT_FALSE(governor.try_acquire(WorkClass::AP));
  EXPECT_EQ(1u, governor.snapshot(WorkClass::SYNC).active);
  active.reset();
  EXPECT_EQ(0u, governor.snapshot(WorkClass::SYNC).active);
  EXPECT_EQ(0u, governor.snapshot(WorkClass::SYNC).waiting);
}
TEST(RapidResourcesTest, WaitingWorkIsGrantedInFifoOrder) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::SOFT, {1, 1, 1}));
  auto active = governor.try_acquire(WorkClass::SYNC);
  auto first = std::async(std::launch::async, [&] {return governor.acquire(WorkClass::SYNC, [] {return false;});});
  EXPECT_TRUE(wait_for_governor_queue(governor, 1));
  auto second = std::async(std::launch::async, [&] {return governor.acquire(WorkClass::SYNC, [] {return false;});});
  EXPECT_TRUE(wait_for_governor_queue(governor, 2));
  active.reset();
  const bool ready = first.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(ready);
  if (!ready) governor.stop();
  auto lease = first.get();
  EXPECT_TRUE(lease);
  EXPECT_EQ(std::future_status::timeout, second.wait_for(std::chrono::milliseconds(20)));
  lease.reset();
  governor.stop(); // Release/cancel race is legal; either result owns no leaked grant.
  auto next = second.get(); next.reset();
  EXPECT_EQ(0u, governor.snapshot(WorkClass::SYNC).active);
}

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
  BoundedMemoryResource resource(40, std::pmr::new_delete_resource());
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
  BoundedMemoryResource resource(160, std::pmr::new_delete_resource());
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
        BoundedMemoryResource auxiliary(grant.bytes() / 16, std::pmr::new_delete_resource());
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
  BoundedMemoryResource resource(96, std::pmr::new_delete_resource());
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
  BoundedMemoryResource resource(old_bytes + replacement - 1, std::pmr::new_delete_resource());
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
  BoundedMemoryResource resource(96, std::pmr::new_delete_resource());
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
    BoundedMemoryResource resource(lease.bytes(), std::pmr::new_delete_resource());
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

namespace ShannonBase::ResMgmt {
TEST(RapidAutoResourcesTest, InsufficientPhysicalCoresUseSoftwareAdmission) {
  std::vector<CpuTopologyEntry> topology;
  for (unsigned cpu = 0; cpu < 8; ++cpu) topology.push_back({cpu, 0, int(cpu / 2)});
  EXPECT_FALSE(make_auto_cpu_plan(topology, {4, 2, 1}).available);
}
TEST(RapidAutoResourcesTest, SmtSiblingsStayTogetherAndTpGetsPriority) {
  std::vector<CpuTopologyEntry> topology;
  for (unsigned cpu = 0; cpu < 24; ++cpu) topology.push_back({cpu, 0, int(cpu / 2)});
  auto plan = make_auto_cpu_plan(topology, {4, 2, 1});
  ASSERT_TRUE(plan.available);
  EXPECT_EQ(16u, plan.cpus[0].size());
  EXPECT_EQ(4u, plan.cpus[1].size());
  EXPECT_EQ(2u, plan.cpus[2].size());
  EXPECT_EQ(2u, plan.cpus[3].size());
  for (const auto &cpus : plan.cpus) {
    for (auto cpu : cpus) EXPECT_NE(cpus.end(), std::find(cpus.begin(), cpus.end(), cpu ^ 1));
  }
  EXPECT_EQ((std::array<size_t, 3>{2, 2, 1}), plan.limits);
}
TEST(RapidAutoResourcesTest, SparseCpuIdsAndPackagesRemainDistinct) {
  const std::vector<CpuTopologyEntry> topology{{3, 0, 0}, {9, 0, 1}, {21, 1, 0}, {32, 1, 1}, {47, 2, 0}};
  auto plan = make_auto_cpu_plan(topology, {4, 2, 1});
  ASSERT_TRUE(plan.available);
  EXPECT_EQ((std::vector<unsigned>{3, 9}), plan.cpus[0]);
  EXPECT_EQ((std::vector<unsigned>{21}), plan.cpus[1]);
  EXPECT_EQ((std::vector<unsigned>{32}), plan.cpus[2]);
  EXPECT_EQ((std::vector<unsigned>{47}), plan.cpus[3]);
  EXPECT_EQ((std::array<size_t, 3>{1, 2, 1}), plan.limits);
}
TEST(RapidAutoResourcesTest, ExplicitBudgetsAreUpperBounds) {
  std::vector<CpuTopologyEntry> topology;
  for (unsigned cpu = 0; cpu < 48; ++cpu) topology.push_back({cpu, 0, int(cpu)});
  EXPECT_EQ((std::array<size_t, 3>{1, 1, 1}), make_auto_cpu_plan(topology, {1, 1, 1}).limits);
}
TEST(RapidAutoResourcesTest, AutoAdmissionRejectsAndReleasesExcessQueries) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::AUTO, {1, 1, 1}));
  auto active = governor.try_acquire(WorkClass::AP);
  ASSERT_TRUE(active);
  EXPECT_FALSE(governor.idle());
  EXPECT_FALSE(governor.try_acquire(WorkClass::AP));
  active.reset();
  EXPECT_TRUE(governor.idle());
  EXPECT_TRUE(governor.try_acquire(WorkClass::AP));
}
}  // namespace ShannonBase::ResMgmt

namespace ShannonBase::ResMgmt {
TEST(RapidAutoResourcesTest, ReconfigurationCanPauseAdmissionUntilSetupFinishes) {
  WorkGovernor governor;
  ASSERT_TRUE(governor.configure(IsolationMode::AUTO, {1, 1, 1}, false));
  EXPECT_TRUE(governor.stopping());
  EXPECT_FALSE(governor.try_acquire(WorkClass::AP));
  ASSERT_TRUE(governor.configure(IsolationMode::AUTO, {1, 1, 1}));
  EXPECT_FALSE(governor.stopping());
  EXPECT_TRUE(governor.try_acquire(WorkClass::AP));
}
}  // namespace ShannonBase::ResMgmt
