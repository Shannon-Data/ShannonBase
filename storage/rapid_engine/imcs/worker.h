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
  enum Task_Type {
    GC,           ///< Garbage Collection - reclaims unused space
    COMPACT,      ///< IMCU Compression - optimizes storage layout
    STATS_UPDATE  ///< Statistics Update - refreshes optimizer statistics
  };

  /**
   * @brief Priority levels for task scheduling
   */
  enum Priority {
    PRIORITY_LOW = 1,     ///< Low priority tasks (e.g., statistics update)
    PRIORITY_NORMAL = 5,  ///< Normal priority tasks (e.g., compression)
    PRIORITY_HIGH = 10    ///< High priority tasks (e.g., urgent GC)
  };

  /**
   * @brief Represents a task to be executed by the worker pool
   */
  struct Task {
    Task_Type type;                                        ///< Type of task
    std::function<void()> func;                            ///< The actual task function
    Priority priority;                                     ///< Task priority
    std::chrono::system_clock::time_point scheduled_time;  ///< When task was scheduled

    /**
     * @brief Comparison operator for priority queue (higher priority first)
     */
    bool operator<(const Task &other) const {
      if (priority != other.priority) {
        return priority < other.priority;  // Higher priority comes first
      }
      return scheduled_time > other.scheduled_time;  // Earlier tasks first
    }
  };

  /**
   * @brief Constructs a background worker pool
   * @param num_workers Number of worker threads to create (default: 4)
   */
  explicit BkgWorkerPool(size_t num_workers = 4);

  /**
   * @brief Destructor - stops all worker threads
   */
  ~BkgWorkerPool();

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
  void submit_task(Task_Type type, std::function<void()> func, Priority priority = PRIORITY_NORMAL);

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

  /**
   * @brief Gets the number of completed tasks
   * @return Count of successfully completed tasks
   */
  size_t get_tasks_completed() const { return m_tasks_completed.load(); }

  /**
   * @brief Gets the number of failed tasks
   * @return Count of tasks that failed with exceptions
   */
  size_t get_tasks_failed() const { return m_tasks_failed.load(); }

  /**
   * @brief Gets the number of pending tasks
   * @return Current queue size (approximate)
   */
  size_t get_pending_tasks() const {
    std::lock_guard lock(m_queue_mutex);
    return m_task_queue.size();
  }

 private:
  /**
   * @brief Worker thread function that processes tasks from the queue
   */
  void worker_thread();

 private:
  std::vector<std::thread> m_workers;      ///< Worker threads
  std::priority_queue<Task> m_task_queue;  ///< Priority-based task queue
  mutable std::mutex m_queue_mutex;        ///< Protects task queue access
  std::condition_variable m_cv;            ///< Coordinates worker threads
  std::atomic<bool> m_running;             ///< Pool running state
  std::atomic<size_t> m_tasks_completed;   ///< Completed tasks counter
  std::atomic<size_t> m_tasks_failed;      ///< Failed tasks counter
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_WORKER_HTREAD_H__