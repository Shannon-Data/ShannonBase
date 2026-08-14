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

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include "mysqld_error.h"   // my_error
#include "sql/sql_class.h"  // THD

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/trx/transaction.h"

namespace ShannonBase {
extern ulonglong shannon_rpd_purge_efficiency_threshold;
extern int32 shannon_rpd_gc_interval_time;
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
      m_auto_cv.wait_for(lock, std::chrono::seconds(ShannonBase::shannon_rpd_engine_cfg.gc_interval_seconds),
                         []() { return !m_auto_thread_running.load(std::memory_order_acquire); });
    }
    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    auto pool = BkgWorkerPool::try_instance();
    if (!pool || BkgWorkerPool::is_shutdown()) break;

    auto imcs = ShannonBase::Imcs::Imcs::instance();

    // 1. auto GC —— gc_interval_scn is the minimum interval between two consecutive GC operations, but the real safe
    // SCN is determined by the actual active transactions.
    uint64_t current_scn = TransactionCoordinator::instance().get_current_scn();
    uint64_t last = m_last_gc_scn.load(std::memory_order_acquire);
    if (current_scn > last && current_scn - last >= ShannonBase::shannon_rpd_engine_cfg.gc_interval_scn) {
      uint64_t safe_scn = TransactionCoordinator::instance().get_gc_safe_scn();

      imcs->for_each_table([&](RpdTable *table) {
        if (m_auto_thread_running.load(std::memory_order_acquire)) {
          pool->schedule_gc(table, safe_scn);
        }
      });
      m_last_gc_scn.store(current_scn, std::memory_order_release);
    }

    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    // 2. auto compaction —— to increase ref cnt so that the imcu being scanned by RapidCursor will not be compacted
    // concurrently.
    imcs->for_each_table([&](RpdTable *table) {
      if (!m_auto_thread_running.load(std::memory_order_acquire)) return;
      auto imcus = table->get_imcus();
      for (auto &imcu : imcus) {
        if (!imcu || imcu->has_active_readers() || !imcu->needs_compaction()) continue;
        if (m_auto_thread_running.load(std::memory_order_acquire)) {
          pool->schedule_compact(table, imcu);
        }
      }
    });

    if (!m_auto_thread_running.load(std::memory_order_acquire)) break;

    // 3. stats update
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

static std::string maintenance_key(const char *kind, const void *table, const void *object = nullptr) {
  std::ostringstream ss;
  ss << kind << ':' << table;
  if (object) ss << ':' << object;
  return ss.str();
}

void BkgWorkerPool::worker_loop() {
  my_thread_init();

  for (;;) {
    TaskPtr task;
    {
      std::unique_lock<std::mutex> lk(m_mutex);
      m_worker_cv.wait(lk, [this] { return should_worker_exit_locked() || !m_ready_queue.empty(); });
      if (should_worker_exit_locked()) break;
      if (m_ready_queue.empty()) continue;

      task = std::move(m_ready_queue.front());
      m_ready_queue.pop_front();
      update_queue_size_locked();

      if (task->state.load(std::memory_order_acquire) == TaskState::kCancelled) continue;

      task->state.store(TaskState::kRunning, std::memory_order_release);
      ++m_running_tasks;
      m_metrics.active_workers.fetch_add(1, std::memory_order_relaxed);
      if (task->type == TaskType::GC)
        m_metrics.concurrent_gc.fetch_add(1, std::memory_order_relaxed);
      else if (task->type == TaskType::COMPACT)
        m_metrics.concurrent_compact.fetch_add(1, std::memory_order_relaxed);
      else if (task->type == TaskType::STATS_UPDATE)
        m_metrics.concurrent_stats.fetch_add(1, std::memory_order_relaxed);
    }

    TaskResult result;
    try {
      result = task->func();
    } catch (const std::exception &e) {
      DBUG_PRINT("rapid_worker", ("background task threw: %s", e.what()));
      result = TaskResult::kFailed;
    } catch (...) {
      DBUG_PRINT("rapid_worker", ("background task threw a non-std exception"));
      result = TaskResult::kFailed;
    }

    {
      std::lock_guard<std::mutex> lk(m_mutex);
      --m_running_tasks;
      m_metrics.active_workers.fetch_sub(1, std::memory_order_relaxed);
      if (task->type == TaskType::GC)
        m_metrics.concurrent_gc.fetch_sub(1, std::memory_order_relaxed);
      else if (task->type == TaskType::COMPACT)
        m_metrics.concurrent_compact.fetch_sub(1, std::memory_order_relaxed);
      else if (task->type == TaskType::STATS_UPDATE)
        m_metrics.concurrent_stats.fetch_sub(1, std::memory_order_relaxed);

      handle_task_result_locked(task, result);

      if (m_state == PoolState::kDraining && no_outstanding_work_locked()) {
        m_state = PoolState::kStopping;
        m_worker_cv.notify_all();
      }
      m_scheduler_cv.notify_all();
      m_shutdown_cv.notify_all();
    }
  }

  my_thread_end();
}

void BkgWorkerPool::scheduler_loop() {
  my_thread_init();

  std::unique_lock<std::mutex> lk(m_mutex);
  while (!should_scheduler_exit_locked()) {
    if (m_scheduled_queue.empty()) {
      m_scheduler_cv.wait(lk, [this] { return should_scheduler_exit_locked() || !m_scheduled_queue.empty(); });
      continue;
    }

    const TimePoint next = m_scheduled_queue.top().scheduled_time;
    m_scheduler_cv.wait_until(lk, next, [this, next] {
      return should_scheduler_exit_locked() || m_scheduled_queue.empty() ||
             m_scheduled_queue.top().scheduled_time < next;
    });

    if (should_scheduler_exit_locked()) break;

    const TimePoint now = Clock::now();
    while (!m_scheduled_queue.empty() && m_scheduled_queue.top().scheduled_time <= now) {
      TaskPtr task = std::move(m_scheduled_queue.top().task);
      m_scheduled_queue.pop();
      update_queue_size_locked();

      if (task->state.load(std::memory_order_acquire) == TaskState::kCancelled) continue;

      task->state.store(TaskState::kReady, std::memory_order_release);
      enqueue_ready_locked(std::move(task));
    }
    m_worker_cv.notify_all();
  }

  my_thread_end();
}

void BkgWorkerPool::schedule_locked(TaskPtr task, TimePoint t) {
  task->scheduled_time = t;
  task->sequence = m_next_sequence++;
  m_scheduled_queue.push(ScheduledTask{t, task->sequence, std::move(task)});
  update_queue_size_locked();
}

void BkgWorkerPool::enqueue_ready_locked(TaskPtr task) {
  auto pos = std::find_if(m_ready_queue.begin(), m_ready_queue.end(), [&](const TaskPtr &queued) {
    if (task->priority != queued->priority)
      return static_cast<int>(task->priority) > static_cast<int>(queued->priority);
    return task->sequence < queued->sequence;
  });
  m_ready_queue.insert(pos, std::move(task));
  update_queue_size_locked();
}

bool BkgWorkerPool::remove_queued_task_locked(const TaskPtr &task) {
  bool removed = false;

  for (auto it = m_ready_queue.begin(); it != m_ready_queue.end();) {
    if (*it == task) {
      it = m_ready_queue.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }

  decltype(m_scheduled_queue) kept;
  while (!m_scheduled_queue.empty()) {
    ScheduledTask item = m_scheduled_queue.top();
    m_scheduled_queue.pop();
    if (item.task == task) {
      removed = true;
      continue;
    }
    kept.push(std::move(item));
  }
  m_scheduled_queue.swap(kept);
  update_queue_size_locked();
  return removed;
}

void BkgWorkerPool::release_dedup_locked(const TaskPtr &task) {
  if (task && !task->dedup_key.empty()) m_dedup_keys.erase(task->dedup_key);
}

void BkgWorkerPool::update_queue_size_locked() {
  m_metrics.queue_size.store(m_ready_queue.size() + m_scheduled_queue.size(), std::memory_order_relaxed);
}

void BkgWorkerPool::handle_task_result_locked(TaskPtr task, TaskResult result) {
  switch (result) {
    case TaskResult::kSuccess:
      complete_task_locked(task);
      break;
    case TaskResult::kRetry:
      schedule_retry_locked(task);
      break;
    case TaskResult::kFailed:
      fail_task_locked(task);
      break;
    case TaskResult::kCancelled:
      cancel_task_locked(task);
      break;
  }
}

void BkgWorkerPool::complete_task_locked(TaskPtr task) {
  task->state.store(TaskState::kCompleted, std::memory_order_release);
  m_metrics.completed.fetch_add(1, std::memory_order_relaxed);
  m_tasks_by_id.erase(task->task_id);
  release_dedup_locked(task);
}

void BkgWorkerPool::fail_task_locked(TaskPtr task) {
  task->state.store(TaskState::kFailed, std::memory_order_release);
  m_metrics.failed.fetch_add(1, std::memory_order_relaxed);
  m_tasks_by_id.erase(task->task_id);
  release_dedup_locked(task);
}

void BkgWorkerPool::cancel_task_locked(TaskPtr task) {
  if (!task) return;
  const TaskState state = task->state.load(std::memory_order_acquire);
  if (state == TaskState::kCompleted || state == TaskState::kFailed || state == TaskState::kCancelled) {
    m_tasks_by_id.erase(task->task_id);
    release_dedup_locked(task);
    return;
  }

  task->state.store(TaskState::kCancelled, std::memory_order_release);
  m_metrics.cancelled.fetch_add(1, std::memory_order_relaxed);
  m_tasks_by_id.erase(task->task_id);
  release_dedup_locked(task);
}

void BkgWorkerPool::schedule_retry_locked(TaskPtr task) {
  // During cancel-pending shutdown, a running task that reports kRetry must
  // not be re-scheduled; its remaining retries are cancelled.
  if (m_state == PoolState::kStopping || m_state == PoolState::kStopped) {
    cancel_task_locked(task);
    return;
  }

  ++task->attempt;
  if (task->attempt >= task->retry_policy.max_attempts) {
    fail_task_locked(task);
    return;
  }

  task->state.store(TaskState::kRetryWaiting, std::memory_order_release);
  m_metrics.retried.fetch_add(1, std::memory_order_relaxed);

  const TimePoint t = (m_state == PoolState::kDraining)
                          ? Clock::now()
                          : Clock::now() + calculate_backoff(task->retry_policy, task->attempt - 1);
  schedule_locked(task, t);
}

std::chrono::milliseconds BkgWorkerPool::calculate_backoff(const RetryPolicy &policy, uint32_t retry_index) {
  const double multiplier = std::pow(policy.multiplier, static_cast<double>(retry_index));
  int64_t delay = static_cast<int64_t>(static_cast<double>(policy.initial_delay.count()) * multiplier);
  if (delay < 0 || delay > policy.max_delay.count()) delay = policy.max_delay.count();
  return std::chrono::milliseconds(delay);
}

bool BkgWorkerPool::no_outstanding_work_locked() const {
  return m_ready_queue.empty() && m_scheduled_queue.empty() && m_running_tasks == 0;
}

bool BkgWorkerPool::should_worker_exit_locked() const {
  if (m_state == PoolState::kStopped) return true;
  if (m_state == PoolState::kStopping) return m_ready_queue.empty();
  return false;
}

bool BkgWorkerPool::should_scheduler_exit_locked() const {
  return m_state == PoolState::kStopped || m_state == PoolState::kStopping;
}

void BkgWorkerPool::begin_drain_locked() {
  m_state = PoolState::kDraining;
  while (!m_scheduled_queue.empty()) {
    TaskPtr task = std::move(m_scheduled_queue.top().task);
    m_scheduled_queue.pop();
    if (task->state.load(std::memory_order_acquire) == TaskState::kCancelled) continue;
    task->state.store(TaskState::kReady, std::memory_order_release);
    enqueue_ready_locked(std::move(task));
  }
  update_queue_size_locked();
}

void BkgWorkerPool::begin_cancel_pending_locked() {
  m_state = PoolState::kStopping;
  while (!m_ready_queue.empty()) {
    TaskPtr task = std::move(m_ready_queue.front());
    m_ready_queue.pop_front();
    cancel_task_locked(task);
  }
  while (!m_scheduled_queue.empty()) {
    TaskPtr task = std::move(m_scheduled_queue.top().task);
    m_scheduled_queue.pop();
    cancel_task_locked(task);
  }
  update_queue_size_locked();
}

BkgWorkerPool &BkgWorkerPool::instance() {
  std::call_once(m_once, []() { m_instance.reset(new BkgWorkerPool(4)); });

  if (!m_instance) {
    static std::unique_ptr<BkgWorkerPool> s_dummy;
    static std::once_flag s_dummy_once;
    std::call_once(s_dummy_once, []() {
      s_dummy.reset(new BkgWorkerPool(0));
      std::lock_guard<std::mutex> lk(s_dummy->m_mutex);
      s_dummy->m_state = PoolState::kStopped;
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

BkgWorkerPool::BkgWorkerPool(size_t num_workers) {
  m_metrics.total_workers.store(num_workers, std::memory_order_relaxed);

  for (size_t i = 0; i < num_workers; ++i) m_workers.emplace_back(&BkgWorkerPool::worker_loop, this);

  if (num_workers > 0) {
    m_scheduler_thread = std::thread(&BkgWorkerPool::scheduler_loop, this);

    bool expected = false;
    if (m_auto_thread_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      m_auto_thread = std::thread(&BkgWorkerPool::auto_maintenance_thread);
    }
  }
}

void BkgWorkerPool ::schedule_gc(RpdTable *table, uint64_t min_active_scn) {
  if (!table) return;
  const uint64_t table_id = table->meta().table_id;
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  auto exact = imcs->get_rpd_table_shared(table_id);
  if (!exact || exact.get() != table) return;

  const std::string dedup_key = maintenance_key("gc", table);
  submit_unique(
      TaskType::GC,
      [table_id, table = std::move(exact), min_active_scn]() -> TaskResult {
        auto current = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
        if (!current || current.get() != table.get()) return TaskResult::kCancelled;
        table->garbage_collect(min_active_scn);
        return TaskResult::kSuccess;
      },
      Priority::PRIORITY_LOW, 0, dedup_key);
}

void BkgWorkerPool ::schedule_compact(RpdTable *table, std::shared_ptr<Imcu> imcu) {
  if (!table || !imcu) return;
  const uint64_t table_id = table->meta().table_id;
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  auto exact = imcs->get_rpd_table_shared(table_id);
  if (!exact || exact.get() != table) return;

  const std::string dedup_key = maintenance_key("compact", table, imcu.get());
  submit_unique(
      TaskType::COMPACT,
      [table_id, table = std::move(exact), imcu = std::move(imcu)]() -> TaskResult {
        auto current = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
        if (!current || current.get() != table.get()) return TaskResult::kCancelled;
        table->compact_imcu(imcu);
        return TaskResult::kSuccess;
      },
      Priority::PRIORITY_NORMAL, 0, dedup_key);
}

void BkgWorkerPool ::schedule_stats_update(RpdTable *table) {
  if (!table) return;
  const uint64_t table_id = table->meta().table_id;
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  auto exact = imcs->get_rpd_table_shared(table_id);
  if (!exact || exact.get() != table) return;

  const std::string dedup_key = maintenance_key("stats", table);
  submit_unique(
      TaskType::STATS_UPDATE,
      [table_id, table = std::move(exact)]() -> TaskResult {
        auto current = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
        if (!current || current.get() != table.get()) return TaskResult::kCancelled;
        table->update_statistics();
        return TaskResult::kSuccess;
      },
      Priority::PRIORITY_LOW, 0, dedup_key);
}

std::string BkgWorkerPool::submit_unique(TaskType type, std::function<TaskResult()> func, Priority prio,
                                         uint32_t max_retries, std::string dedup_key) {
  auto task = std::make_shared<Task>();
  task->task_id = gen_task_id();
  task->type = type;
  task->priority = prio;
  task->dedup_key = std::move(dedup_key);
  task->retry_policy.max_attempts = max_retries + 1;
  task->func = std::move(func);

  const std::string task_id = task->task_id;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_state != PoolState::kRunning) return {};
    if (!task->dedup_key.empty() && !m_dedup_keys.insert(task->dedup_key).second) return {};
    m_tasks_by_id[task_id] = task;
    schedule_locked(std::move(task), Clock::now());
    m_metrics.submitted.fetch_add(1, std::memory_order_relaxed);
  }
  m_scheduler_cv.notify_one();
  return task_id;
}

std::string BkgWorkerPool::submit(TaskType type, std::function<TaskResult()> func, Priority prio,
                                  uint32_t max_retries) {
  return submit_unique(type, std::move(func), prio, max_retries, {});
}

std::string BkgWorkerPool::submit(TaskType type, std::function<int()> func, Priority prio, uint32_t max_retries) {
  return submit(
      type,
      [original = std::move(func)]() -> TaskResult {
        const int rc = original();
        return rc == 0 ? TaskResult::kSuccess : TaskResult::kFailed;
      },
      prio, max_retries);
}

bool BkgWorkerPool::cancel(const std::string &task_id) {
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_tasks_by_id.find(task_id);
  if (it == m_tasks_by_id.end()) return false;

  TaskPtr task = it->second;
  const TaskState s = task->state.load(std::memory_order_acquire);
  if (s == TaskState::kRunning || s == TaskState::kCompleted || s == TaskState::kFailed || s == TaskState::kCancelled) {
    return false;
  }

  remove_queued_task_locked(task);
  cancel_task_locked(task);
  m_worker_cv.notify_all();
  m_scheduler_cv.notify_all();
  return true;
}

void BkgWorkerPool::shutdown_all(bool wait_completion) {
  if (s_shutdown_called.exchange(true, std::memory_order_acq_rel)) return;

  m_auto_thread_running.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(m_auto_cv_mutex);
    m_auto_cv.notify_all();
  }

  if (m_auto_thread.joinable()) {
    (m_auto_thread.get_id() != std::this_thread::get_id()) ? m_auto_thread.join() : m_auto_thread.detach();
  }

  if (m_instance) {
    m_instance->shutdown(wait_completion ? ShutdownMode::kDrain : ShutdownMode::kCancelPending);
    m_instance.reset();
  }
}

void BkgWorkerPool::shutdown(ShutdownMode mode) {
  bool expected = false;
  if (!m_shutdown_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

  m_auto_thread_running.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_auto_cv_mutex);
    m_auto_cv.notify_all();
  }

  {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_state == PoolState::kStopped) return;

    if (mode == ShutdownMode::kDrain) {
      if (m_state == PoolState::kRunning) begin_drain_locked();
      // If there is no accepted work at all, drain is already complete.
      if (m_state == PoolState::kDraining && no_outstanding_work_locked()) m_state = PoolState::kStopping;
    } else {
      if (m_state != PoolState::kStopping) begin_cancel_pending_locked();
    }
  }
  m_worker_cv.notify_all();
  m_scheduler_cv.notify_all();

  if (m_scheduler_thread.joinable()) {
    if (m_scheduler_thread.get_id() != std::this_thread::get_id())
      m_scheduler_thread.join();
    else
      m_scheduler_thread.detach();
  }

  for (auto &worker : m_workers) {
    if (!worker.joinable()) continue;
    (worker.get_id() != std::this_thread::get_id()) ? worker.join() : worker.detach();
  }
  m_workers.clear();

  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_state = PoolState::kStopped;
  }
}
}  // namespace Imcs
}  // namespace ShannonBase