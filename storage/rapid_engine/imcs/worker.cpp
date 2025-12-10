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

   Copyright (c) 2023, 2024, 2025, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Rapid Table.
*/
#include "storage/rapid_engine/imcs/worker.h"

#include <exception>
#include <iostream>
#include <random>
#include "mysqld_error.h"   // my_error
#include "sql/sql_class.h"  // THD

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"

#include "storage/rapid_engine/trx/transaction.h"
namespace ShannonBase {
extern ulonglong rpd_gc_interval_scn;
extern int32 rpd_gc_interval_time;
namespace Imcs {
std::mutex BkgWorkerPool::m_auto_cv_mutex;
std::condition_variable BkgWorkerPool::m_auto_cv;

std::atomic<uint64_t> BkgWorkerPool::m_last_gc_scn{0};
std::thread BkgWorkerPool::m_auto_thread;
std::atomic<bool> BkgWorkerPool::m_auto_thread_running{false};

std::unique_ptr<BkgWorkerPool> BkgWorkerPool::m_instance;
std::once_flag BkgWorkerPool::m_once;
std::atomic<bool> BkgWorkerPool::s_shutdown_called{false};

void BkgWorkerPool::auto_maintenance_thread() {
  my_thread_init();

  while (m_auto_thread_running.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lock(m_auto_cv_mutex);
      m_auto_cv.wait_for(lock, std::chrono::seconds(ShannonBase::rpd_gc_interval_time),
                         []() { return !m_auto_thread_running.load(std::memory_order_acquire); });
    }

    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    auto pool = BkgWorkerPool::try_instance();
    if (!pool || BkgWorkerPool::is_shutdown()) break;

    auto imcs = ShannonBase::Imcs::Imcs::instance();
    // 1. auto GC
    uint64_t current_scn = TransactionCoordinator::instance().get_current_scn();
    uint64_t last = m_last_gc_scn.load(std::memory_order_acquire);
    if (current_scn > last && current_scn - last >= ShannonBase::rpd_gc_interval_scn) {
      imcs->for_each_table([&](RpdTable *table) {
        if (m_auto_thread_running.load(std::memory_order_acquire)) {
          uint64_t min_active_scn = current_scn - ShannonBase::rpd_gc_interval_scn;
          pool->schedule_gc(table, min_active_scn);
        }
      });
      m_last_gc_scn.store(current_scn, std::memory_order_release);
    }

    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    // 2. auto compaction
    imcs->for_each_table([&](RpdTable *table) {
      if (!m_auto_thread_running.load(std::memory_order_acquire)) return;
      table->foreach_imcu([&](Imcu *imcu) {
        if (!imcu || !imcu->needs_compaction()) return;
        if (m_auto_thread_running.load(std::memory_order_acquire)) {
          pool->schedule_compact(table, imcu);
        }
      });
    });

    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    // 3. stats update - 10 mins interval
    static auto last_stats_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::minutes>(now - last_stats_time).count() >= 10) {
      imcs->for_each_table([&](RpdTable *table) {
        if (m_auto_thread_running.load(std::memory_order_acquire)) {
          pool->schedule_stats_update(table);
        }
      });
      last_stats_time = now;
    }
  }

  my_thread_end();
}

static std::string gen_task_id() {
  static std::atomic<uint64_t> seq{0};
  thread_local std::mt19937_64 rng{std::random_device{}()};
  std::stringstream ss;
  ss << "rapid_task_" << std::hex << rng() << "_" << seq++;
  return ss.str();
}

void BkgWorkerPool::worker_thread() {
  my_thread_init();
  while (!m_shutdown.load(std::memory_order_acquire)) {
    Task task;
    bool got_task = false;
    {
      std::unique_lock<std::mutex> lk(m_queue_mutex);
      m_cv.wait(lk, [this] { return !m_queue.empty() || m_shutdown.load(std::memory_order_acquire); });

      if (m_shutdown.load(std::memory_order_acquire)) break;
      if (m_queue.empty()) continue;

      task = std::move(const_cast<Task &>(m_queue.top()));
      m_queue.pop();
      m_metrics.queue_size.store(m_queue.size(), std::memory_order_relaxed);
      got_task = true;
    }

    if (!got_task) continue;

    std::atomic<uint32_t> *counter = nullptr;
    if (task.type == TaskType::GC)
      counter = &m_concurrent_gc;
    else if (task.type == TaskType::COMPACT)
      counter = &m_concurrent_compact;
    else if (task.type == TaskType::STATS_UPDATE)
      counter = &m_concurrent_stats;

    if (counter && counter->fetch_add(1, std::memory_order_relaxed) >= 4) {
      counter->fetch_sub(1, std::memory_order_relaxed);

      if (m_shutdown.load(std::memory_order_acquire)) {
        m_metrics.cancelled.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        m_queue.push(std::move(task));
        m_metrics.queue_size.store(m_queue.size(), std::memory_order_relaxed);
      }
      m_cv.notify_one();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (m_shutdown.load(std::memory_order_acquire)) {
      if (counter) counter->fetch_sub(1, std::memory_order_relaxed);
      m_metrics.cancelled.fetch_add(1, std::memory_order_relaxed);
      break;
    }

    m_metrics.active_workers.fetch_add(1, std::memory_order_relaxed);
    task.func();
    m_metrics.active_workers.fetch_sub(1, std::memory_order_relaxed);

    if (counter) counter->fetch_sub(1, std::memory_order_relaxed);
  }

  my_thread_end();
}

BkgWorkerPool &BkgWorkerPool::instance() {
  std::call_once(m_once, []() { m_instance.reset(new BkgWorkerPool(4)); });

  if (!m_instance) {
    static std::unique_ptr<BkgWorkerPool> s_dummy;
    static std::once_flag s_dummy_once;
    std::call_once(s_dummy_once, []() {
      s_dummy.reset(new BkgWorkerPool(0));
      s_dummy->m_shutdown.store(true, std::memory_order_release);
    });
    return *s_dummy;
  }

  return *m_instance;
}

BkgWorkerPool *BkgWorkerPool::try_instance() {
  if (s_shutdown_called.load(std::memory_order_acquire)) return nullptr;

  std::call_once(m_once, []() { m_instance.reset(new BkgWorkerPool(4)); });
  return m_instance.get();
}

BkgWorkerPool ::BkgWorkerPool(size_t num_workers) {
  m_metrics.total_workers.store(num_workers, std::memory_order_relaxed);
  for (size_t i = 0; i < num_workers; ++i) m_workers.emplace_back(&BkgWorkerPool::worker_thread, this);

  if (num_workers > 0) {
    bool expected = false;
    if (m_auto_thread_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      m_auto_thread = std::thread(&BkgWorkerPool::auto_maintenance_thread);
  }
}

void BkgWorkerPool ::schedule_gc(RpdTable *table, uint64_t min_active_scn) {
  submit(
      TaskType::GC,
      [table, min_active_scn]() -> int {
        table->garbage_collect(min_active_scn);
        return 0;
      },
      Priority::PRIORITY_LOW);
}

void BkgWorkerPool ::schedule_compact(RpdTable *table, Imcu *imcu) {
  submit(
      TaskType::COMPACT,
      [table, imcu]() -> int {
        imcu->compact();
        return 0;
      },
      Priority::PRIORITY_NORMAL);
}

void BkgWorkerPool ::schedule_stats_update(RpdTable *table) {
  submit(
      TaskType::STATS_UPDATE,
      [table]() -> int {
        table->update_statistics();
        return 0;
      },
      Priority::PRIORITY_LOW);
}

BkgWorkerPool::TaskResult BkgWorkerPool::execute_with_policy(const Task &task) {
  std::atomic<bool> cancel_flag{false};
  {
    std::lock_guard<std::shared_mutex> lk(m_cancelled_tasks_mutex);
    m_cancelled_tasks[task.task_id].store(false);
  }

  const auto deadline = std::chrono::system_clock::now() + task.timeout;

  for (uint32_t attempt = 0; attempt <= task.max_retries; ++attempt) {
    // 1. check cancel flag.
    if (cancel_flag.load(std::memory_order_relaxed)) {
      m_metrics.cancelled++;
      return TaskResult::TASK_CANCELLED;
    }

    // 2. check tiimeout.
    if (std::chrono::system_clock::now() > deadline) {
      my_error(ER_SECONDARY_ENGINE, MYF(0), task.task_id.c_str());
      m_metrics.failed++;
      return TaskResult::TASK_TIMEOUT;
    }

    // 3. exec user tasks.
    int rc = task.func();  // 0=OK
    if (rc == 0) {
      m_metrics.completed++;
      return TaskResult::TASK_OK;
    }

    // 4. dealing with failure.
    m_metrics.retried++;
    if (attempt == task.max_retries) {
      my_error(ER_SECONDARY_ENGINE, MYF(0), task.task_id.c_str(), rc);
      m_metrics.failed++;
      return TaskResult::TASK_FAILED_PERMANENT;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100ULL << attempt));
  }

  return TaskResult::TASK_FAILED_PERMANENT;
}

std::string BkgWorkerPool::submit(TaskType type, std::function<int()> func, Priority prio, std::chrono::seconds timeout,
                                  uint32_t max_retries) {
  if (m_shutdown.load(std::memory_order_acquire) || BkgWorkerPool::is_shutdown()) {
    return {};
  }

  Task task;
  task.task_id = gen_task_id();
  task.type = type;
  task.priority = prio;
  task.timeout = timeout;
  task.max_retries = max_retries;
  task.enqueue_time = task.scheduled_time = std::chrono::system_clock::now();

  {
    std::lock_guard<std::shared_mutex> lk(m_cancelled_tasks_mutex);
    m_cancelled_tasks[task.task_id].store(false);
  }

  task.func = [this, task_id = task.task_id, original = std::move(func), deadline = task.scheduled_time + timeout,
               max_retries]() -> int {
    for (uint32_t attempt = 0; attempt <= max_retries; ++attempt) {
      bool is_cancelled = false;
      {
        std::shared_lock<std::shared_mutex> lk(this->m_cancelled_tasks_mutex);
        auto it = this->m_cancelled_tasks.find(task_id);
        is_cancelled = (it == this->m_cancelled_tasks.end()) || it->second.load(std::memory_order_acquire);
      }

      if (is_cancelled || this->m_shutdown.load(std::memory_order_acquire)) {
        this->m_metrics.cancelled.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::shared_mutex> lk(this->m_cancelled_tasks_mutex);
        this->m_cancelled_tasks.erase(task_id);
        return 1;
      }

      if (std::chrono::system_clock::now() > deadline) {
        my_error(ER_SECONDARY_ENGINE, MYF(0), task_id.c_str());
        this->m_metrics.failed.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::shared_mutex> lk(this->m_cancelled_tasks_mutex);
        this->m_cancelled_tasks.erase(task_id);
        return 1;
      }

      int rc = original();
      if (rc == 0) {
        this->m_metrics.completed.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::shared_mutex> lk(this->m_cancelled_tasks_mutex);
        this->m_cancelled_tasks.erase(task_id);
        return 0;
      }

      this->m_metrics.retried.fetch_add(1, std::memory_order_relaxed);
      if (attempt == max_retries) {
        my_error(ER_SECONDARY_ENGINE, MYF(0), task_id.c_str(), rc);
        this->m_metrics.failed.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::shared_mutex> lk(this->m_cancelled_tasks_mutex);
        this->m_cancelled_tasks.erase(task_id);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100ULL << attempt));
      }
    }

    return 1;
  };

  {
    std::lock_guard<std::mutex> lk(m_queue_mutex);
    m_queue.push(std::move(task));
    m_metrics.queue_size.store(m_queue.size(), std::memory_order_relaxed);
    m_metrics.submitted.fetch_add(1, std::memory_order_relaxed);
  }
  m_cv.notify_one();

  return task.task_id;
}

void BkgWorkerPool::shutdown_all(bool wait_completion) {
  if (s_shutdown_called.exchange(true, std::memory_order_acq_rel)) return;

  m_auto_thread_running.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(m_auto_cv_mutex);
    m_auto_cv.notify_all();
  }

  if (m_auto_thread.joinable()) {
    if (m_auto_thread.get_id() != std::this_thread::get_id()) {
      m_auto_thread.join();
    } else {
      m_auto_thread.detach();
    }
  }

  if (m_instance) {
    m_instance->shutdown(wait_completion);
    m_instance.reset();
  }
}

bool BkgWorkerPool::cancel(const std::string &task_id) {
  std::lock_guard<std::shared_mutex> lk(m_cancelled_tasks_mutex);
  auto it = m_cancelled_tasks.find(task_id);
  if (it != m_cancelled_tasks.end()) {
    it->second.store(true, std::memory_order_release);
    m_metrics.cancelled++;
    return true;
  }
  return false;
}

void BkgWorkerPool::shutdown(bool wait_completion) {
  m_shutdown.store(true, std::memory_order_release);
  m_auto_thread_running.store(false, std::memory_order_release);

  {
    std::lock_guard<std::shared_mutex> write_lock(m_cancelled_tasks_mutex);
    for (auto &[task_id, flag] : m_cancelled_tasks) flag.store(true, std::memory_order_release);
  }

  {
    std::lock_guard<std::mutex> lk(m_queue_mutex);
    std::priority_queue<Task> empty_queue;
    std::swap(m_queue, empty_queue);
    m_metrics.queue_size.store(0, std::memory_order_relaxed);
  }

  m_cv.notify_all();

  if (wait_completion) {
    for (auto &worker : m_workers) {
      if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    }
  } else {
    for (auto &worker : m_workers) {
      if (worker.joinable()) worker.detach();
    }
  }

  m_workers.clear();

  {
    std::lock_guard<std::shared_mutex> write_lock(m_cancelled_tasks_mutex);
    m_cancelled_tasks.clear();
  }
}
}  // namespace Imcs
}  // namespace ShannonBase