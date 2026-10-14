/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundmental code for recovery.

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_RECOVERY_PARALLEL_LOAD_H__
#define __SHANNONBASE_RECOVERY_PARALLEL_LOAD_H__
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <streambuf>
#include <thread>
#include <vector>

namespace ShannonBase::Recovery {
/** Read one length-delimited native column directly from the source stream.
 * A malformed column cannot consume the next column's header or payload.
 * No whole-column staging buffer is allocated here; the CU owns decoding.
 */
class NativeColumnStream final : public std::streambuf {
 public:
  NativeColumnStream(std::streambuf *source, uint64_t length) : m_source(source), m_remaining(length) {}
  uint64_t remaining() const { return m_remaining; }

 protected:
  std::streamsize xsgetn(char *destination, std::streamsize count) override {
    if (count <= 0 || !m_source) return 0;
    const auto bounded = static_cast<std::streamsize>(std::min<uint64_t>(m_remaining, count));
    const auto read = m_source->sgetn(destination, bounded);
    m_remaining -= static_cast<uint64_t>(read);
    return read;
  }
  int_type underflow() override { return m_remaining && m_source ? m_source->sgetc() : traits_type::eof(); }
  int_type uflow() override {
    const auto value = underflow();
    if (!traits_type::eq_int_type(value, traits_type::eof())) {
      m_source->sbumpc();
      --m_remaining;
    }
    return value;
  }

 private:
  std::streambuf *m_source;
  uint64_t m_remaining;
};

inline uint64_t saturated_add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }
inline uint64_t saturated_multiply(uint64_t a, uint64_t b) { return b && a > UINT64_MAX / b ? UINT64_MAX : a * b; }

struct NativeLoadPolicy {
  static constexpr uint64_t parallel_threshold = 16ULL * 1024 * 1024;
  static constexpr uint64_t column_batch_bytes = 4ULL * 1024 * 1024;
  static size_t worker_limit(unsigned cpus = std::thread::hardware_concurrency()) {
    return std::max<size_t>(1, std::min<size_t>(8, cpus / 2));
  }
  static bool parallel(uint64_t bytes, size_t tasks, size_t workers) {
    return bytes >= parallel_threshold && tasks > 1 && workers > 1;
  }
};

/** Shared admission for both inline and pooled loads. Task callbacks must not
 * recursively submit work. A failed batch stops dispatching and drains before
 * returning, so callbacks may borrow the caller's private recovery image.
 * Memory is an estimate of transient decoding, not a bound on resident IMCS.
 * A task exceeding the budget runs exclusively to preserve the serial path.
 */
class NativeLoadExecutor {
 public:
  struct Task {
    uint64_t memory;
    std::function<bool()> load;
  };
  NativeLoadExecutor(size_t workers, uint64_t memory, std::function<bool()> init = {}, std::function<void()> end = {})
      : m_limit(std::max<size_t>(1, workers)),
        m_budget(std::max<uint64_t>(1, memory)),
        m_init(std::move(init)),
        m_end(std::move(end)) {}
  ~NativeLoadExecutor() { stop(); }
  NativeLoadExecutor(const NativeLoadExecutor &) = delete;
  NativeLoadExecutor &operator=(const NativeLoadExecutor &) = delete;
  size_t workers() const { return m_limit; }

  bool run(const std::vector<Task> &tasks, bool parallel) {
    if (!parallel) {
      for (const auto &task : tasks) {
        const auto charge = std::min(task.memory, m_budget);
        {
          std::unique_lock lock(m_mutex);
          m_cv.wait(lock, [&] { return m_stopping || fits(charge); });
          if (m_stopping) return false;
          reserve(charge);
        }
        const bool ok = invoke(task);
        {
          std::lock_guard lock(m_mutex);
          release(charge);
        }
        m_cv.notify_all();
        if (!ok) return false;
      }
      return true;
    }
    Batch batch{tasks};
    std::unique_lock lock(m_mutex);
    if (m_stopping) return false;
    // No batch is published until all allocation/thread creation succeeds.
    // Already-created workers remain idle if creation throws.
    try {
      m_threads.reserve(m_limit);
      while (m_threads.size() < m_limit) m_threads.emplace_back([this] { worker(); });
      m_batches.push_back(&batch);
    } catch (...) {
      return false;
    }
    m_cv.notify_all();
    m_cv.wait(lock, [&] { return batch.next == tasks.size() && batch.running == 0; });
    m_batches.erase(std::find(m_batches.begin(), m_batches.end(), &batch));
    return !batch.failed;
  }

  void stop() {
    {
      std::lock_guard lock(m_mutex);
      m_stopping = true;
      for (auto *batch : m_batches) {
        batch->failed = true;
        batch->next = batch->tasks.size();
      }
    }
    m_cv.notify_all();
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [&] { return m_active == 0; });
    }
    for (auto &thread : m_threads)
      if (thread.joinable()) thread.join();
    m_threads.clear();
  }

 private:
  struct Batch {
    const std::vector<Task> &tasks;
    size_t next{0}, running{0};
    bool failed{false};
  };
  static bool invoke(const Task &task) noexcept {
    try {
      return task.load();
    } catch (...) {
      return false;
    }
  }
  bool fits(uint64_t charge) const { return m_active < m_limit && charge <= m_budget - m_used; }
  void reserve(uint64_t charge) {
    ++m_active;
    m_used += charge;
  }
  void release(uint64_t charge) {
    --m_active;
    m_used -= charge;
  }
  Batch *ready() {
    for (auto *batch : m_batches)
      if (batch->next < batch->tasks.size() && fits(std::min(batch->tasks[batch->next].memory, m_budget))) return batch;
    return nullptr;
  }
  void worker() noexcept {
    bool initialized = false;
    try {
      initialized = !m_init || m_init();
    } catch (...) {
    }
    for (;;) {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [&] { return m_stopping || ready(); });
      if (m_stopping) break;
      auto *batch = ready();
      const auto &task = batch->tasks[batch->next++];
      const auto charge = std::min(task.memory, m_budget);
      ++batch->running;
      reserve(charge);
      lock.unlock();
      const bool ok = initialized && invoke(task);
      lock.lock();
      release(charge);
      --batch->running;
      if (!ok) {
        batch->failed = true;
        batch->next = batch->tasks.size();
      }
      lock.unlock();
      m_cv.notify_all();
    }
    if (initialized && m_end) {
      try {
        m_end();
      } catch (...) {
      }
    }
  }
  const size_t m_limit;
  const uint64_t m_budget;
  std::function<bool()> m_init;
  std::function<void()> m_end;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<std::thread> m_threads;
  std::vector<Batch *> m_batches;
  size_t m_active{0};
  uint64_t m_used{0};
  bool m_stopping{false};
};
}  // namespace ShannonBase::Recovery
#endif
