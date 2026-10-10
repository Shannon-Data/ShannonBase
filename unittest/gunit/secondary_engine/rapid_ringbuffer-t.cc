/* Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

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

/**
 * Unit test for MPMC RingBuffer (log_buffer.h).
 *
 * Verifies that the ring buffer works correctly through multiple
 * wrap-around cycles — specifically testing the fix for the off-by-one
 * in try_pop() that caused permanent deadlock after one full lap.
 */

#include "storage/rapid_engine/populate/log_buffer.h"
#include "storage/rapid_engine/populate/row_image_buffer.h"
#include "storage/rapid_engine/populate/propagation_mode.h"

#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace shannon_ringbuffer_unittest {

TEST(PropagationModeTest, TransactionRequirementsAndReservedBackend) {
  using namespace ShannonBase::Populate;
  EXPECT_TRUE(propagation_query_supported(ChangePropagationMode::CAPTURE, false, true));
  EXPECT_TRUE(propagation_query_supported(ChangePropagationMode::COMMITTED_BINLOG, true, false));
  EXPECT_FALSE(propagation_query_supported(ChangePropagationMode::COMMITTED_BINLOG, false, false));
  EXPECT_FALSE(propagation_query_supported(ChangePropagationMode::COMMITTED_BINLOG, true, true));
  EXPECT_TRUE(propagation_backend_available(ChangePropagationMode::CAPTURE));
  EXPECT_FALSE(propagation_backend_available(ChangePropagationMode::COMMITTED_BINLOG));
}

TEST(RowImageBufferTest, ProducerExitAndConsumerRelease) {
  std::shared_ptr<unsigned char[]> record;
  std::thread producer([&] {
    record = ShannonBase::Populate::RowImageBufferPool::acquire(512);
    record[0] = 17;
    record[511] = 29;
  });
  producer.join();
  auto post = std::shared_ptr<unsigned char[]>(record, record.get() + 256);
  record.reset();
  EXPECT_EQ(post[255], 29);
  std::thread consumer([image = std::move(post)]() mutable {
    EXPECT_EQ(image[255], 29);
    image.reset();
  });
  consumer.join();
}

TEST(RowImageBufferTest, ConcurrentReturnsAndReuse) {
  using ShannonBase::Populate::RowImageBufferPool;
  EXPECT_FALSE(RowImageBufferPool::acquire(0));
  std::vector<std::shared_ptr<unsigned char[]>> images;
  for (size_t i = 0; i < 300; ++i) {
    auto image = RowImageBufferPool::acquire(513);
    image[0] = static_cast<unsigned char>(i);
    image[512] = static_cast<unsigned char>(i + 1);
    images.push_back(std::move(image));
  }
  std::thread consumer([&] {
    for (size_t i = 0; i < images.size(); ++i) {
      EXPECT_EQ(images[i][0], static_cast<unsigned char>(i));
      EXPECT_EQ(images[i][512], static_cast<unsigned char>(i + 1));
      images[i].reset();
    }
  });
  for (size_t i = 0; i < 500; ++i) {
    auto image = RowImageBufferPool::acquire(513);
    image[512] = 71;
    EXPECT_EQ(image[512], 71);
  }
  consumer.join();
  auto large = RowImageBufferPool::acquire(65537);
  large[65536] = 83;
  EXPECT_EQ(large[65536], 83);
}

using ShannonBase::Populate::Ringbuffer;

constexpr size_t kSmallBufferSize = 16;  // Small buffer to force quick wrap

// A small buffer wrapper that overrides BufferSize for testing.
template <typename T, size_t BufSize = kSmallBufferSize>
class TestRingbuffer : public Ringbuffer<T, BufSize> {};

TEST(RingbufferTest, BasicPutPop) {
  Ringbuffer<int> rb;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(rb.try_put(i));
  }
  for (int i = 0; i < 100; ++i) {
    int val = -1;
    ASSERT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, i);
  }
}

TEST(RingbufferTest, WrapAroundSingleProducerSingleConsumer) {
  // Use a buffer size of 16 to force multiple wrap-arounds quickly.
  Ringbuffer<int, 16> rb;

  // Number of full laps + 1 to guarantee wrap-around.
  constexpr size_t kLaps = 5;
  constexpr size_t kTotal = 16 * kLaps + 7;  // 87 entries

  // Producer thread
  std::thread producer([&]() {
    for (size_t i = 0; i < kTotal; ++i) {
      while (!rb.try_put(static_cast<int>(i))) {
        std::this_thread::yield();
      }
    }
  });

  // Consumer thread (single consumer as required by try_pop contract)
  std::thread consumer([&]() {
    for (size_t i = 0; i < kTotal; ++i) {
      int val = -1;
      while (!rb.try_pop(val)) {
        std::this_thread::yield();
      }
      EXPECT_EQ(val, static_cast<int>(i));
    }
  });

  producer.join();
  consumer.join();

  EXPECT_TRUE(rb.empty());
}

TEST(RingbufferTest, WrapAroundMultiProducerSingleConsumer) {
  Ringbuffer<int, 16> rb;

  constexpr size_t kNumProducers = 4;
  constexpr size_t kItemsPerProducer = 50;  // > 16 to force wrap
  constexpr size_t kTotal = kNumProducers * kItemsPerProducer;

  std::atomic<size_t> produced{0};
  std::atomic<size_t> consumed{0};
  std::atomic<bool> done{false};

  // We use per-producer counter to avoid head-of-line blocking issues
  std::vector<std::thread> producers;
  for (size_t p = 0; p < kNumProducers; ++p) {
    producers.emplace_back([&, p]() {
      for (size_t i = 0; i < kItemsPerProducer; ++i) {
        int val = static_cast<int>(p * 10000 + i);
        while (!rb.try_put(val)) {
          std::this_thread::yield();
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
      // Last producer signals done
      static std::atomic<size_t> done_count{0};
      if (done_count.fetch_add(1) + 1 == kNumProducers) {
        done.store(true);
      }
    });
  }

  // Single consumer
  std::thread consumer([&]() {
    while (!done.load() || !rb.empty()) {
      int val = -1;
      if (rb.try_pop(val)) {
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (auto &t : producers) t.join();
  consumer.join();

  EXPECT_EQ(produced.load(), kTotal);
  EXPECT_EQ(consumed.load(), kTotal);
  EXPECT_TRUE(rb.empty());
}

TEST(RingbufferTest, FullBufferDoesNotLoseData) {
  // Fill to capacity, drain, refill — repeat to force wrap.
  Ringbuffer<int, 64> rb;

  for (int lap = 0; lap < 5; ++lap) {
    // Fill
    for (int i = 0; i < 64; ++i) {
      ASSERT_TRUE(rb.try_put(lap * 1000 + i));
    }
    EXPECT_TRUE(rb.full());

    // Drain
    for (int i = 0; i < 64; ++i) {
      int val = -1;
      ASSERT_TRUE(rb.try_pop(val));
      EXPECT_EQ(val, lap * 1000 + i);
    }
    EXPECT_TRUE(rb.empty());
  }
}

TEST(RingbufferTest, ConsumeMethodMatchesTryPop) {
  // Verify that consume() and try_pop() produce consistent seq values
  // (this would have caught the off-by-one in try_pop).
  Ringbuffer<int, 32> rb;

  // Put 32 items
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(rb.try_put(i));
  }

  // Pop 16 via try_pop
  for (int i = 0; i < 16; ++i) {
    int val = -1;
    ASSERT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, i);
  }

  // Pop remaining 16 via consume
  rb.consume(16);

  EXPECT_TRUE(rb.empty());

  // Refill — this would deadlock with the old off-by-one bug
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(rb.try_put(i + 100));
  }
  EXPECT_TRUE(rb.full());
}

TEST(RingbufferTest, ClearAndReuse) {
  Ringbuffer<int, 64> rb;

  // Fill, clear, refill — must not hang
  for (int i = 0; i < 64; ++i) {
    ASSERT_TRUE(rb.try_put(i));
  }
  rb.clear();
  EXPECT_TRUE(rb.empty());

  for (int i = 0; i < 64; ++i) {
    ASSERT_TRUE(rb.try_put(i + 100));
  }
  for (int i = 0; i < 64; ++i) {
    int val = -1;
    ASSERT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, i + 100);
  }
}

}  // namespace shannon_ringbuffer_unittest
