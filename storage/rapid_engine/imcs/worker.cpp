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

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"

namespace ShannonBase {
namespace Imcs {
// Assuming these classes have the following methods:
// - RpdTable::garbage_collect(uint64_t)
// - Imcu::compact()
// - RpdTable::update_statistics()

BkgWorkerPool ::BkgWorkerPool(size_t num_workers) : m_running(true), m_tasks_completed(0), m_tasks_failed(0) {
  // Create worker threads
  for (size_t i = 0; i < num_workers; ++i) {
    m_workers.emplace_back(&BkgWorkerPool ::worker_thread, this);
  }
}

BkgWorkerPool ::~BkgWorkerPool() {
  // Signal all threads to stop
  m_running.store(false);
  m_cv.notify_all();

  // Wait for all threads to finish
  for (auto &worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void BkgWorkerPool ::submit_task(Task_Type type, std::function<void()> func, Priority priority) {
  Task task;
  task.type = type;
  task.func = std::move(func);
  task.priority = priority;
  task.scheduled_time = std::chrono::system_clock::now();

  {
    std::lock_guard lock(m_queue_mutex);
    m_task_queue.push(std::move(task));
  }

  // Notify one waiting worker
  m_cv.notify_one();
}

void BkgWorkerPool ::schedule_gc(RpdTable *table, uint64_t min_active_scn) {
  submit_task(
      GC, [table, min_active_scn]() { table->garbage_collect(min_active_scn); }, PRIORITY_LOW);
}

void BkgWorkerPool ::schedule_compact(RpdTable *table, Imcu *imcu) {
  submit_task(
      COMPACT, [table, imcu]() { imcu->compact(); }, PRIORITY_NORMAL);
}

void BkgWorkerPool ::schedule_stats_update(RpdTable *table) {
  submit_task(
      STATS_UPDATE, [table]() { table->update_statistics(); }, PRIORITY_LOW);
}

void BkgWorkerPool ::worker_thread() {
  while (m_running.load()) {
    Task task;

    {
      std::unique_lock lock(m_queue_mutex);

      // Wait for tasks or shutdown signal
      m_cv.wait(lock, [this] { return !m_task_queue.empty() || !m_running.load(); });

      // Check if we should exit
      if (!m_running.load() && m_task_queue.empty()) {
        break;
      }

      // Get the highest priority task
      if (!m_task_queue.empty()) {
        task = std::move(const_cast<Task &>(m_task_queue.top()));
        m_task_queue.pop();
      } else {
        continue;
      }
    }

    // Execute the task
    try {
      task.func();
      m_tasks_completed.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception &e) {
      m_tasks_failed.fetch_add(1, std::memory_order_relaxed);
      std::cerr << "Background task failed: " << e.what() << std::endl;
    }
  }
}
}  // namespace Imcs
}  // namespace ShannonBase
