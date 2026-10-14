/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023 - 2026, Shannon Data AI and/or its affiliates.
*/
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include "storage/rapid_engine/recovery/parallel_load.h"

using namespace ShannonBase::Recovery;
using namespace std::chrono_literals;

TEST(NativeLoadPolicy, SmallAndSingleTasksStayInline) {
  EXPECT_FALSE(NativeLoadPolicy::parallel(1024, 100, 8));
  EXPECT_FALSE(NativeLoadPolicy::parallel(UINT64_MAX, 1, 8));
  EXPECT_FALSE(NativeLoadPolicy::parallel(UINT64_MAX, 100, 1));
  EXPECT_TRUE(NativeLoadPolicy::parallel(NativeLoadPolicy::parallel_threshold, 2, 2));
  EXPECT_EQ(NativeLoadPolicy::worker_limit(0), 1u);
  EXPECT_EQ(NativeLoadPolicy::worker_limit(1), 1u);
  EXPECT_EQ(NativeLoadPolicy::worker_limit(4), 2u);
  EXPECT_EQ(NativeLoadPolicy::worker_limit(128), 8u);
  EXPECT_EQ(saturated_add(UINT64_MAX, 1), UINT64_MAX);
  EXPECT_EQ(saturated_multiply(UINT64_MAX, 2), UINT64_MAX);
}

TEST(NativeLoadExecutor, SmallBatchUsesCallerWithoutStartingWorkers) {
  std::atomic<unsigned> initialized{0};
  NativeLoadExecutor executor(4, 100, [&] {
    ++initialized;
    return true;
  });
  const auto caller = std::this_thread::get_id();
  unsigned calls = 0;
  std::vector<NativeLoadExecutor::Task> tasks(5, {10, [&] {
                                                    EXPECT_EQ(std::this_thread::get_id(), caller);
                                                    ++calls;
                                                    return true;
                                                  }});
  EXPECT_TRUE(executor.run(tasks, false));
  EXPECT_EQ(calls, 5u);
  EXPECT_EQ(initialized.load(), 0u);
}

TEST(NativeLoadExecutor, ParallelCallbacksOverlapAndAreDrained) {
  NativeLoadExecutor executor(2, 100);
  std::mutex mutex;
  std::condition_variable cv;
  unsigned entered = 0;
  std::atomic<unsigned> finished{0};
  auto callback = [&] {
    std::unique_lock lock(mutex);
    ++entered;
    cv.notify_all();
    bool overlap = cv.wait_for(lock, 5s, [&] { return entered == 2; });
    ++finished;
    return overlap;
  };
  EXPECT_TRUE(executor.run({{40, callback}, {40, callback}}, true));
  EXPECT_EQ(finished.load(), 2u);
}

TEST(NativeLoadExecutor, ConcurrentBatchesAndInlineLoadsShareMemoryAdmission) {
  NativeLoadExecutor executor(4, 100);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<unsigned> other_calls{0};
  auto first = std::async(std::launch::async, [&] {
    return executor.run({{100,
                          [&] {
                            entered.set_value();
                            released.wait();
                            return true;
                          }}},
                        true);
  });
  const auto ready = entered.get_future().wait_for(5s);
  EXPECT_EQ(ready, std::future_status::ready);
  auto second = std::async(std::launch::async, [&] {
    return executor.run({{1,
                          [&] {
                            ++other_calls;
                            return true;
                          }}},
                        true);
  });
  auto inline_load = std::async(std::launch::async, [&] {
    return executor.run({{1,
                          [&] {
                            ++other_calls;
                            return true;
                          }}},
                        false);
  });
  EXPECT_EQ(second.wait_for(50ms), std::future_status::timeout);
  EXPECT_EQ(inline_load.wait_for(50ms), std::future_status::timeout);
  EXPECT_EQ(other_calls.load(), 0u);
  release.set_value();
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_TRUE(inline_load.get());
  EXPECT_EQ(other_calls.load(), 2u);
}

TEST(NativeLoadExecutor, OversizedTaskRunsExclusively) {
  NativeLoadExecutor executor(4, 100);
  std::atomic<unsigned> active{0}, calls{0};
  std::vector<NativeLoadExecutor::Task> tasks(8, {UINT64_MAX, [&] {
                                                    EXPECT_EQ(active.fetch_add(1), 0u);
                                                    std::this_thread::sleep_for(1ms);
                                                    EXPECT_EQ(active.fetch_sub(1), 1u);
                                                    ++calls;
                                                    return true;
                                                  }});
  EXPECT_TRUE(executor.run(tasks, true));
  EXPECT_EQ(calls.load(), 8u);
}

TEST(NativeLoadExecutor, ExceptionCancelsPendingWorkAndDrainsRunningCallbacks) {
  NativeLoadExecutor executor(2, 100);
  std::promise<void> entered, release;
  auto entered_future = entered.get_future().share();
  auto released = release.get_future().share();
  std::atomic<unsigned> pending_calls{0}, finished{0};
  std::vector<NativeLoadExecutor::Task> tasks{{10,
                                               [&]() -> bool {
                                                 entered_future.wait();
                                                 throw std::bad_alloc();
                                               }},
                                              {10,
                                               [&] {
                                                 entered.set_value();
                                                 released.wait();
                                                 ++finished;
                                                 return true;
                                               }},
                                              {10, [&] {
                                                 ++pending_calls;
                                                 return true;
                                               }}};
  auto batch = std::async(std::launch::async, [&] { return executor.run(tasks, true); });
  EXPECT_EQ(entered_future.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(batch.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  EXPECT_FALSE(batch.get());
  EXPECT_EQ(finished.load(), 1u);
  EXPECT_EQ(pending_calls.load(), 0u);
  EXPECT_TRUE(executor.run({{1, [] { return true; }}}, true));
}

TEST(NativeLoadExecutor, InitializationFailureDoesNotInvokeCallbacks) {
  NativeLoadExecutor executor(2, 100, [] { return false; });
  std::atomic<unsigned> calls{0};
  EXPECT_FALSE(executor.run({{1,
                              [&] {
                                ++calls;
                                return true;
                              }}},
                            true));
  EXPECT_EQ(calls.load(), 0u);
}

TEST(NativeLoadExecutor, StopRejectsFurtherBatchesAndEndsWorkerContexts) {
  std::atomic<unsigned> started{0}, ended{0};
  NativeLoadExecutor executor(
      2, 100,
      [&] {
        ++started;
        return true;
      },
      [&] { ++ended; });
  EXPECT_TRUE(executor.run({{1, [] { return true; }}}, true));
  executor.stop();
  EXPECT_EQ(started.load(), 2u);
  EXPECT_EQ(ended.load(), 2u);
  EXPECT_FALSE(executor.run({{1, [] { return true; }}}, true));
  EXPECT_FALSE(executor.run({{1, [] { return true; }}}, false));
}

TEST(NativeLoadExecutor, IndependentBoundedReadersPreserveColumnRanges) {
  NativeLoadExecutor executor(2, 100);
  const std::string file = "prefixAAAABBBBBsuffix";
  auto read = [&](size_t offset, size_t length, std::string expected) {
    std::istringstream input(file);
    input.seekg(offset);
    NativeColumnStream slice(input.rdbuf(), length);
    std::istream bounded(&slice);
    std::string output(length, '\0');
    bounded.read(output.data(), length);
    return output == expected && slice.remaining() == 0 && bounded.peek() == std::char_traits<char>::eof();
  };
  EXPECT_TRUE(
      executor.run({{10, [&] { return read(6, 4, "AAAA"); }}, {10, [&] { return read(10, 5, "BBBBB"); }}}, true));
}

TEST(NativeLoadExecutor, StopWaitsForInlineCallbacksAndCancelsTheRemainder) {
  NativeLoadExecutor executor(2, 100);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<unsigned> calls{0};
  auto batch = std::async(std::launch::async, [&] {
    return executor.run({{1,
                          [&] {
                            entered.set_value();
                            released.wait();
                            return true;
                          }},
                         {1,
                          [&] {
                            ++calls;
                            return true;
                          }}},
                        false);
  });
  EXPECT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  auto stopped = std::async(std::launch::async, [&] { executor.stop(); });
  EXPECT_EQ(stopped.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  stopped.get();
  EXPECT_FALSE(batch.get());
  EXPECT_EQ(calls.load(), 0u);
}

TEST(NativeLoadExecutor, ConcurrentTablesShareTheWorkerLimit) {
  NativeLoadExecutor executor(2, 100000);
  std::atomic<unsigned> active{0}, finished{0};
  auto load = [&] {
    EXPECT_LE(active.fetch_add(1), 1u);
    std::this_thread::sleep_for(1ms);
    active.fetch_sub(1);
    ++finished;
    return true;
  };
  std::vector<NativeLoadExecutor::Task> tasks(12, {1, load});
  std::vector<std::future<bool>> batches;
  for (unsigned i = 0; i < 4; ++i)
    batches.push_back(std::async(std::launch::async, [&, i] { return executor.run(tasks, i != 0); }));
  for (auto &batch : batches) EXPECT_TRUE(batch.get());
  EXPECT_EQ(finished.load(), 48u);
}

TEST(NativeLoadExecutor, FailureSkipsUndispatchedTasks) {
  NativeLoadExecutor executor(1, 100);
  unsigned calls = 0;
  EXPECT_FALSE(executor.run({{1, [] { return false; }},
                             {1,
                              [&] {
                                ++calls;
                                return true;
                              }}},
                            true));
  EXPECT_EQ(calls, 0u);
}
