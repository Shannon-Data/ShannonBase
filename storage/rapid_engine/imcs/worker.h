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
#ifndef __SHANNONBASE_RAPID_WORKER_HTREAD_H__
#define __SHANNONBASE_RAPID_WORKER_HTREAD_H__
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "storage/rapid_engine/include/rapid_types.h"
namespace ShannonBase {
namespace Imcs {
/**
 * @file BkgWorkerPool .h
 * @brief Background worker pool for database maintenance tasks
 *
 * Features:
 * - Garbage Collection (GC)
 * - IMCU Compression
 * - Statistics Update
 * - Priority-based task scheduling
 * - Thread-safe task queue
 */
// Forward declarations
class RpdTable;
class Imcu;
/**
 * @brief Background worker pool for handling maintenance tasks asynchronously
 *
 * This pool manages a set of worker threads that process maintenance tasks
 * in the background, including garbage collection, IMCU compression, and
 * statistics updates. Tasks are executed based on priority and scheduling time.
 */
class BkgWorkerPool : public MemoryObject {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  /**
   * @brief Types of maintenance tasks supported by the pool
   */
  enum class TaskType {
    GC,            ///< Garbage Collection - reclaims unused space
    COMPACT,       ///< IMCU Compression - optimizes storage layout
    STATS_UPDATE,  ///< Statistics Update - refreshes optimizer statistics
    CUSTOM = 99
  };

  /**
   * @brief Priority levels for task scheduling
   */
  enum class Priority {
    PRIORITY_LOW = 1,     ///< Low priority tasks (e.g., statistics update)
    PRIORITY_NORMAL = 2,  ///< Normal priority tasks (e.g., compression)
    PRIORITY_HIGH = 3,    ///< High priority tasks (e.g., urgent GC)
    CRITICAL = 4
  };

  enum class TaskResult {
    kSuccess,   // This attempt succeeded; task lifetime ends.
    kRetry,     // This attempt failed transiently; scheduler may re-run.
    kFailed,    // Permanent failure; no more retries.
    kCancelled  // Task was cancelled / pool is shutting down.
  };

  enum class TaskState { kPending, kReady, kRunning, kRetryWaiting, kCompleted, kFailed, kCancelled };

  enum class ShutdownMode {
    kDrain,         // Finish all accepted work (ignore retry/backoff delays).
    kCancelPending  // Cancel queued/scheduled work; wait for running tasks.
  };

  enum class PoolState { kRunning, kDraining, kStopping, kStopped };

  struct RetryPolicy {
    uint32_t max_attempts{3};
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{30000};
    double multiplier{2.0};
  };

  /**
   * @brief Represents a task to be executed by the worker pool.
   *
   * A task's Run() reports the result of a single execution attempt.  Retry
   * scheduling and the task lifecycle are owned by the pool, not by the task.
   */
  struct Task {
    TaskType type{};
    Priority priority{Priority::PRIORITY_LOW};
    std::function<TaskResult()> func;
    std::string task_id;
    // Non-empty only for maintenance work that must have at most one pending/
    // running instance for a given target incarnation.
    std::string dedup_key;
    RetryPolicy retry_policy;

    std::atomic<TaskState> state{TaskState::kPending};
    uint32_t attempt{0};
    uint64_t sequence{0};
    std::chrono::steady_clock::time_point scheduled_time{};
  };
  using TaskPtr = std::shared_ptr<Task>;

  static BkgWorkerPool &instance();

  explicit BkgWorkerPool(size_t num_workers = 4);
  ~BkgWorkerPool() { shutdown(true); }

  static BkgWorkerPool *try_instance();
  static inline bool is_shutdown() { return s_shutdown_called.load(std::memory_order_acquire); }
  static void shutdown_all(bool wait_completion = true);

  // Delete copy and move operations
  BkgWorkerPool(const BkgWorkerPool &) = delete;
  BkgWorkerPool &operator=(const BkgWorkerPool &) = delete;
  BkgWorkerPool(BkgWorkerPool &&) = delete;
  BkgWorkerPool &operator=(BkgWorkerPool &&) = delete;

  /**
   * @brief Submits a task that reports its own result (supports kRetry).
   * @param type Type of task
   * @param func The function to execute (returns TaskResult)
   * @param priority Task priority (default: NORMAL)
   */
  std::string submit(TaskType type, std::function<TaskResult()> func, Priority prio = Priority::PRIORITY_NORMAL,
                     uint32_t max_retries = 5);

  /**
   * @brief Legacy adapter: int-returning callback (0 = success, non-zero = failure).
   */
  std::string submit(TaskType type, std::function<int()> func, Priority prio = Priority::PRIORITY_NORMAL,
                     uint32_t max_retries = 5);

  /**
   * @brief Schedules a garbage collection task
   * @param table The table to perform GC on
   * @param min_active_scn Minimum active system change number
   */
  void schedule_gc(RpdTable *table, uint64_t min_active_scn);

  /**
   * @brief Schedules an IMCU compression task
   * @param table The table containing the IMCU
   * @param imcu Shared pointer to the IMCU to compress (keeps IMCU alive
   *   even if Table::compact() replaces m_imcus before the task executes).
   */
  void schedule_compact(RpdTable *table, std::shared_ptr<Imcu> imcu);

  /**
   * @brief Schedules a statistics update task
   * @param table The table to update statistics for
   */
  void schedule_stats_update(RpdTable *table);

  bool cancel(const std::string &task_id);

  void shutdown(bool wait_completion) {
    shutdown(wait_completion ? ShutdownMode::kDrain : ShutdownMode::kCancelPending);
  }
  void shutdown(ShutdownMode mode);

  struct Metrics {
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> cancelled{0};
    std::atomic<uint64_t> retried{0};
    std::atomic<size_t> queue_size{0};
    std::atomic<size_t> active_workers{0};
    std::atomic<size_t> total_workers{0};
    std::atomic<uint32_t> concurrent_gc{0};
    std::atomic<uint32_t> concurrent_compact{0};
    std::atomic<uint32_t> concurrent_stats{0};
  };
  const Metrics &metrics() const { return m_metrics; }

 private:
  static std::atomic<uint64_t> m_last_gc_scn;
  static std::thread m_auto_thread;
  static std::atomic<bool> m_auto_thread_running;
  static std::once_flag m_once;
  static std::atomic<bool> s_shutdown_called;

  static void auto_maintenance_thread();

  void worker_loop();
  void scheduler_loop();

  struct ScheduledTask {
    std::chrono::steady_clock::time_point scheduled_time;
    uint64_t sequence;
    TaskPtr task;
  };
  struct ScheduledTaskCompare {
    bool operator()(const ScheduledTask &a, const ScheduledTask &b) const {
      if (a.scheduled_time != b.scheduled_time) return a.scheduled_time > b.scheduled_time;
      return a.sequence > b.sequence;
    }
  };

  // Submit with an optional maintenance de-duplication key.
  std::string submit_unique(TaskType type, std::function<TaskResult()> func, Priority prio, uint32_t max_retries,
                            std::string dedup_key);

  // All helpers below require m_mutex to be held.
  void schedule_locked(TaskPtr task, std::chrono::steady_clock::time_point t);
  void enqueue_ready_locked(TaskPtr task);
  bool remove_queued_task_locked(const TaskPtr &task);
  void release_dedup_locked(const TaskPtr &task);
  void handle_task_result_locked(TaskPtr task, TaskResult result);
  void complete_task_locked(TaskPtr task);
  void fail_task_locked(TaskPtr task);
  void cancel_task_locked(TaskPtr task);
  void schedule_retry_locked(TaskPtr task);
  void update_queue_size_locked();
  bool no_outstanding_work_locked() const;
  bool should_worker_exit_locked() const;
  bool should_scheduler_exit_locked() const;
  void begin_drain_locked();
  void begin_cancel_pending_locked();
  static std::chrono::milliseconds calculate_backoff(const RetryPolicy &policy, uint32_t retry_index);

  static std::unique_ptr<BkgWorkerPool> m_instance;
  static std::mutex m_auto_cv_mutex;
  static std::condition_variable m_auto_cv;

  mutable std::mutex m_mutex;
  std::condition_variable m_worker_cv;
  std::condition_variable m_scheduler_cv;
  std::condition_variable m_shutdown_cv;

  std::deque<TaskPtr> m_ready_queue;
  std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, ScheduledTaskCompare> m_scheduled_queue;
  std::unordered_map<std::string, TaskPtr> m_tasks_by_id;
  std::unordered_set<std::string> m_dedup_keys;

  std::vector<std::thread> m_workers;
  std::thread m_scheduler_thread;

  // guarded by m_mutex
  PoolState m_state{PoolState::kRunning};
  size_t m_running_tasks{0};
  uint64_t m_next_sequence{0};
  std::atomic<bool> m_shutdown_started{false};

  Metrics m_metrics;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_WORKER_HTREAD_H__