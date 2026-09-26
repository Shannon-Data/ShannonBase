/**
   Copyright (c) 2023 - 2026, Shannon Data AI and/or its affiliates.

   PublishFrontier: the contiguous-publication rule that lets several writers
   apply rows to one IMCU concurrently while readers still see only complete
   rows. These tests drive the rule directly -- no IMCU, no storage -- so the
   concurrency property can be asserted on its own.

   A frontier that is merely monotonic is not enough: if it jumps over a slot
   whose cells are still unwritten, a scan reads uninitialized memory. Every test
   below is about that hole, not about the counter reaching its maximum.
*/
#include "storage/rapid_engine/imcs/imcu.h"  //PublishFrontier

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace shannon_rapid_publish_frontier_unittest {

using ShannonBase::Imcs::PublishFrontier;

TEST(PublishFrontierTest, CompletingInOrderPublishesEveryRow) {
  PublishFrontier frontier(8);
  EXPECT_EQ(0u, frontier.published());

  for (size_t slot = 0; slot < 8; ++slot) {
    frontier.complete(slot);
    EXPECT_EQ(slot + 1, frontier.published()) << "an in-order complete publishes through that row";
  }
}

TEST(PublishFrontierTest, TheFrontierStopsAtAHole) {
  PublishFrontier frontier(8);

  frontier.complete(2);
  EXPECT_EQ(0u, frontier.published()) << "row 0 is missing, so nothing is visible";

  frontier.complete(3);
  EXPECT_EQ(0u, frontier.published()) << "filling the far side of a hole changes nothing";

  frontier.complete(0);
  EXPECT_EQ(1u, frontier.published()) << "row 1 is still missing";

  frontier.complete(1);
  EXPECT_EQ(4u, frontier.published()) << "the hole is filled: 0..3 become visible together";
}

TEST(PublishFrontierTest, IsIdempotentAndIgnoresOutOfRangeSlots) {
  PublishFrontier frontier(4);
  frontier.complete(0);
  frontier.complete(0);
  EXPECT_EQ(1u, frontier.published());

  frontier.complete(99);
  EXPECT_EQ(1u, frontier.published()) << "a slot past the capacity must not move the frontier";
}

TEST(PublishFrontierTest, PublishAllAndResetAreForRestores) {
  PublishFrontier frontier(6);
  frontier.publish_all(6);
  EXPECT_EQ(6u, frontier.published());
  frontier.publish_all(99);
  EXPECT_EQ(6u, frontier.published()) << "a restore cannot publish past the capacity";

  frontier.reset();
  EXPECT_EQ(0u, frontier.published());
}

// The cold-start shape: a default-constructed IMCU has one of these, and it must
// publish nothing rather than pretend every slot is readable.
TEST(PublishFrontierTest, APlaceholderFrontierPublishesNothing) {
  PublishFrontier frontier;
  EXPECT_EQ(0u, frontier.published());
  EXPECT_EQ(0u, frontier.capacity());

  frontier.complete(0);
  frontier.publish_all(8);
  EXPECT_EQ(0u, frontier.published()) << "no slots, so nothing may become visible";
}

TEST(PublishFrontierTest, ConcurrentCompletionsNeverPublishAHole) {
  constexpr size_t kRows = 2048;
  constexpr int kWriters = 4;
  PublishFrontier frontier(kRows);

  // Set before complete(), so anything the frontier calls published has been
  // applied by the writer that owns it.
  std::vector<std::atomic<bool>> applied(kRows);
  for (auto &flag : applied) flag.store(false, std::memory_order_relaxed);

  std::atomic<bool> writers_done{false};
  std::atomic<size_t> holes_read{0};

  std::thread reader([&]() {
    while (!writers_done.load(std::memory_order_acquire)) {
      const size_t visible = frontier.published();
      for (size_t slot = 0; slot < visible; ++slot) {
        if (!applied[slot].load(std::memory_order_acquire)) {
          holes_read.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      std::this_thread::yield();
    }
  });

  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      std::vector<size_t> slots;
      for (size_t slot = static_cast<size_t>(w); slot < kRows; slot += kWriters) slots.push_back(slot);
      // Descending, so each writer's first completions are as far from the
      // frontier as they can be and the run is filled in from the back.
      std::reverse(slots.begin(), slots.end());
      for (size_t slot : slots) {
        applied[slot].store(true, std::memory_order_release);
        frontier.complete(slot);
      }
    });
  }

  for (auto &writer : writers) writer.join();
  writers_done.store(true, std::memory_order_release);
  reader.join();

  EXPECT_EQ(0u, holes_read.load()) << "a reader saw a row published before its writer applied it";
  EXPECT_EQ(kRows, frontier.published()) << "once every row is applied the whole range is visible";
}

TEST(PublishFrontierTest, EveryRowIsPublishedEvenWhenCompletionsInterleave) {
  constexpr size_t kRows = 512;
  constexpr int kWriters = 8;
  PublishFrontier frontier(kRows);

  std::atomic<size_t> completed{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      for (size_t round = 0; round < kRows / kWriters; ++round) {
        // A different phase per writer per round: the frontier cannot advance on
        // completion order alone.
        const size_t slot = (static_cast<size_t>(round) * kWriters) + static_cast<size_t>(w);
        frontier.complete(slot);
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &writer : writers) writer.join();

  EXPECT_EQ(kRows, completed.load());
  EXPECT_EQ(kRows, frontier.published()) << "no row may be left behind the frontier";
}

}  // namespace shannon_rapid_publish_frontier_unittest
