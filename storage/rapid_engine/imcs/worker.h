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
#include <functional>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "storage/rapid_engine/include/rapid_object.h"
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
    TASK_OK = 0,
    TASK_CANCELLED = 1,
    TASK_TIMEOUT = 2,
    TASK_FAILED_PERMANENT = 3,
    TASK_RETRY_LATER = 4
  };

  /**
   * @brief Represents a task to be executed by the worker pool
   */
  struct Task {
    TaskType type{};
    std::function<int()> func;
    Priority priority{Priority::PRIORITY_LOW};
    std::chrono::system_clock::time_point enqueue_time;
    std::chrono::system_clock::time_point scheduled_time;
    uint32_t retry_count{0};
    uint32_t max_retries{5};
    std::chrono::seconds timeout{300};
    std::string task_id;

    bool operator<(const Task &rhs) const {
      if (priority != rhs.priority) return priority < rhs.priority;
      return scheduled_time > rhs.scheduled_time;
    }
  };

  static BkgWorkerPool &instance();

  explicit BkgWorkerPool(size_t num_workers = 4);
  ~BkgWorkerPool() = default;

  static BkgWorkerPool *try_instance();
  static inline bool is_shutdown() { return s_shutdown_called.load(std::memory_order_acquire); }
  static void shutdown_all(bool wait_completion = true);

  // Delete copy and move operations
  BkgWorkerPool(const BkgWorkerPool &) = delete;
  BkgWorkerPool &operator=(const BkgWorkerPool &) = delete;
  BkgWorkerPool(BkgWorkerPool &&) = delete;
  BkgWorkerPool &operator=(BkgWorkerPool &&) = delete;

  /**
   * @brief Submits a generic task to the worker pool
   * @param type Type of task
   * @param func The function to execute
   * @param priority Task priority (default: NORMAL)
   */
  std::string submit(TaskType type, std::function<int()> func, Priority prio = Priority::PRIORITY_NORMAL,
                     std::chrono::seconds timeout = std::chrono::seconds(300), uint32_t max_retries = 5);

  /**
   * @brief Schedules a garbage collection task
   * @param table The table to perform GC on
   * @param min_active_scn Minimum active system change number
   */
  void schedule_gc(RpdTable *table, uint64_t min_active_scn);

  /**
   * @brief Schedules an IMCU compression task
   * @param table The table containing the IMCU
   * @param imcu The IMCU to compress
   */
  void schedule_compact(RpdTable *table, Imcu *imcu);

  /**
   * @brief Schedules a statistics update task
   * @param table The table to update statistics for
   */
  void schedule_stats_update(RpdTable *table);

  bool cancel(const std::string &task_id);

  void shutdown(bool wait = true);

  struct Metrics {
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> cancelled{0};
    std::atomic<uint64_t> retried{0};
    std::atomic<size_t> queue_size{0};
    std::atomic<size_t> active_workers{0};
    std::atomic<size_t> total_workers{0};
  };
  const Metrics &metrics() const { return m_metrics; }

 private:
  static std::atomic<uint64_t> m_last_gc_scn;
  static std::thread m_auto_thread;
  static std::atomic<bool> m_auto_thread_running;
  static std::once_flag m_once;
  static std::atomic<bool> s_shutdown_called;

  static void auto_maintenance_thread();
  /**
   * @brief Worker thread function that processes tasks from the queue
   */
  void worker_thread();
  TaskResult execute_with_policy(const Task &task);

  static std::unique_ptr<BkgWorkerPool> m_instance;
  static std::mutex m_auto_cv_mutex;
  static std::condition_variable m_auto_cv;

  std::unordered_map<std::string, std::atomic<bool>> m_cancelled_tasks;
  std::shared_mutex m_cancelled_tasks_mutex;

  std::priority_queue<Task> m_queue;
  mutable std::mutex m_queue_mutex;
  std::condition_variable m_cv;

  std::vector<std::thread> m_workers;
  std::atomic<bool> m_shutdown{false};
  Metrics m_metrics;

  std::atomic<uint32_t> m_concurrent_gc{0};
  std::atomic<uint32_t> m_concurrent_compact{0};
  std::atomic<uint32_t> m_concurrent_stats{0};
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_WORKER_HTREAD_H__