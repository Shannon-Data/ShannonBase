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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs rapid.
*/
#include "storage/rapid_engine/monitor/rapid_monitor.h"

#include <inttypes.h>
#include <atomic>
#include <chrono>
#include <shared_mutex>

#include "storage/rapid_engine/imcs/worker.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/populate/log_populate.h"

namespace ShannonBase {
namespace RapidMonitor {
namespace {
size_t get_populator_worker_thread_count() noexcept { return Populate::get_populator_worker_thread_count(); }
uint64_t get_populator_worker_pending_bytes() noexcept { return Populate::get_populator_worker_pending_bytes(); }
uint64_t get_populator_loop_counter() noexcept { return Populate::get_populator_loop_counter(); }
}  // namespace

void collect_rapid_monitor_metrics(Metrics &metrics) {
  metrics.rapid_pop_thread_running = Populate::shannon_propagation_thread_started.load(std::memory_order_acquire);
  metrics.rapid_pop_loop_counter = get_populator_loop_counter();
  metrics.rapid_pop_data_sz = Populate::shannon_pop_data_sz.load(std::memory_order_acquire);
  metrics.total_buffer_tables = Populate::pop_buff_table_count();
  {
    std::shared_lock<std::shared_mutex> lk(Populate::shannon_pop_table_mutex);
    metrics.tables_in_progress = Populate::shannon_pop_tables.size();
  }
  metrics.total_worker_threads = get_populator_worker_thread_count();
  metrics.worker_pending_bytes = get_populator_worker_pending_bytes();

  if (shannon_loaded_tables) metrics.loaded_tables = shannon_loaded_tables->size();

  auto worker_pool = Imcs::BkgWorkerPool::try_instance();
  if (worker_pool) {
    const auto &pool_metrics = worker_pool->metrics();
    metrics.bg_pool_queue_size = pool_metrics.queue_size.load(std::memory_order_relaxed);
    metrics.bg_active_workers = pool_metrics.active_workers.load(std::memory_order_relaxed);
    metrics.bg_total_workers = pool_metrics.total_workers.load(std::memory_order_relaxed);
    metrics.bg_concurrent_gc = pool_metrics.concurrent_gc.load(std::memory_order_relaxed);
    metrics.bg_concurrent_compact = pool_metrics.concurrent_compact.load(std::memory_order_relaxed);
    metrics.bg_concurrent_stats = pool_metrics.concurrent_stats.load(std::memory_order_relaxed);
  }
}

void print_rapid_monitor_info(FILE *file) {
  Metrics metrics;
  collect_rapid_monitor_metrics(metrics);

  fprintf(file,
          "rapid log pop thread : %s\n"
          "rapid log pop thread loops: %" PRIu64
          "\n"
          "rapid log data remaining size: %" PRIu64
          " KB\n"
          "rapid log data remaining tables: %zu\n"
          "rapid log tables in progress: %zu\n"
          "rapid log worker threads active: %zu\n"
          "rapid log worker pending size: %" PRIu64
          " KB\n"
          "rapid tables loaded: %zu\n"
          "rapid background pool queue size: %zu\n"
          "rapid background active workers: %zu/%zu\n"
          "rapid background gc active: %u\n"
          "rapid background compact active: %u\n"
          "rapid background stats update active: %u\n",
          metrics.rapid_pop_thread_running ? "running" : "stopped", metrics.rapid_pop_loop_counter,
          metrics.rapid_pop_data_sz / 1024, metrics.total_buffer_tables, metrics.tables_in_progress,
          metrics.total_worker_threads, metrics.worker_pending_bytes / 1024, metrics.loaded_tables,
          metrics.bg_pool_queue_size, metrics.bg_active_workers, metrics.bg_total_workers, metrics.bg_concurrent_gc,
          metrics.bg_concurrent_compact, metrics.bg_concurrent_stats);
}
}  // namespace RapidMonitor
}  // namespace ShannonBase
