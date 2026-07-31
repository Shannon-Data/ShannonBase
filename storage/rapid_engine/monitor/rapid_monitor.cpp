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

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/worker.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/utils/memory_pool.h"

namespace ShannonBase {
extern std::shared_ptr<Utils::MemoryPool> shannon_rpd_memory_pool;

namespace RapidMonitor {

RapidCounters rapid_counters;

namespace {
size_t get_populator_worker_thread_count() noexcept { return Populate::get_populator_worker_thread_count(); }
uint64_t get_populator_worker_pending_bytes() noexcept { return Populate::get_populator_worker_pending_bytes(); }
uint64_t get_populator_loop_counter() noexcept { return Populate::get_populator_loop_counter(); }
}  // namespace

void collect_rapid_monitor_metrics(Metrics &metrics) {
  // Memory Pool
  if (shannon_rpd_memory_pool) {
    auto stats = shannon_rpd_memory_pool->stats();
    metrics.mempool_capacity_bytes = stats.total_capacity;
    metrics.mempool_allocated_bytes = stats.allocated_bytes;
    metrics.mempool_used_bytes = stats.used_bytes;
    metrics.mempool_peak_usage_bytes = stats.peak_usage;
    metrics.mempool_usage_percentage = stats.usage_percentage;
    metrics.mempool_alloc_count = stats.allocation_count;
    metrics.mempool_dealloc_count = stats.deallocation_count;
    metrics.mempool_failed_allocs = stats.failed_allocations;
    metrics.mempool_expansion_count = stats.expansion_count;
    metrics.mempool_defrag_count = stats.defragmentation_count;
  }

  //  IMCS
  auto imcs = Imcs::Imcs::instance();
  if (imcs && imcs->initialized()) {
    metrics.loaded_tables = 0;
    metrics.loaded_part_tables = 0;
    metrics.total_imcus = 0;
    metrics.total_cus = 0;
    metrics.total_rows = 0;
    metrics.total_physical_rows = 0;
    metrics.estimated_data_size_bytes = 0;
    metrics.estimated_compressed_size_bytes = 0;

    imcs->for_each_table([&](Imcs::RpdTable *t) {
      if (!t) return;
      // Distinguish normal vs partition tables via the virtual type().
      if (t->type() == Imcs::RpdTable::TYPE::NORAMAL)
        metrics.loaded_tables++;
      else if (t->type() == Imcs::RpdTable::TYPE::PARTTABLE)
        metrics.loaded_part_tables++;

      metrics.total_rows += t->count_total_rows();

      auto imcus = t->get_imcus();
      metrics.total_imcus += imcus.size();
      for (const auto &imcu : imcus) {
        if (!imcu) continue;
        metrics.total_physical_rows += imcu->get_row_count();
        metrics.total_cus += imcu->get_column_count();
        auto sz = imcu->estimate_size();
        metrics.estimated_data_size_bytes += sz;
        // compressed_size not directly exposed; approximate via estimate_size.
        metrics.estimated_compressed_size_bytes += sz;
      }
    });
  }

  //  Population / Propagation
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

  //  Background Worker Pool
  auto worker_pool = Imcs::BkgWorkerPool::try_instance();
  if (worker_pool) {
    const auto &pool_metrics = worker_pool->metrics();
    metrics.bg_pool_queue_size = pool_metrics.queue_size.load(std::memory_order_relaxed);
    metrics.bg_active_workers = pool_metrics.active_workers.load(std::memory_order_relaxed);
    metrics.bg_total_workers = pool_metrics.total_workers.load(std::memory_order_relaxed);
    metrics.bg_concurrent_gc = pool_metrics.concurrent_gc.load(std::memory_order_relaxed);
    metrics.bg_concurrent_compact = pool_metrics.concurrent_compact.load(std::memory_order_relaxed);
    metrics.bg_concurrent_stats = pool_metrics.concurrent_stats.load(std::memory_order_relaxed);
    metrics.bg_tasks_submitted = pool_metrics.submitted.load(std::memory_order_relaxed);
    metrics.bg_tasks_completed = pool_metrics.completed.load(std::memory_order_relaxed);
    metrics.bg_tasks_failed = pool_metrics.failed.load(std::memory_order_relaxed);
    metrics.bg_tasks_cancelled = pool_metrics.cancelled.load(std::memory_order_relaxed);
    metrics.bg_tasks_retried = pool_metrics.retried.load(std::memory_order_relaxed);
  }

  //  Garbage Collection
  metrics.gc_total_runs = rapid_counters.gc_total_runs.load(std::memory_order_relaxed);
  metrics.gc_total_purged_rows = rapid_counters.gc_total_purged_rows.load(std::memory_order_relaxed);
  metrics.gc_total_purged_versions = rapid_counters.gc_total_purged_versions.load(std::memory_order_relaxed);
  metrics.gc_last_run_scn = rapid_counters.gc_last_run_scn.load(std::memory_order_relaxed);
  metrics.gc_last_run_duration_us = rapid_counters.gc_last_run_duration_us.load(std::memory_order_relaxed);

  //  Compaction
  metrics.compact_total_runs = rapid_counters.compact_total_runs.load(std::memory_order_relaxed);
  metrics.compact_total_merged_rows = rapid_counters.compact_total_merged_rows.load(std::memory_order_relaxed);
  metrics.compact_last_run_duration_us = rapid_counters.compact_last_run_duration_us.load(std::memory_order_relaxed);

  //  Query Execution
  metrics.query_scans_total = rapid_counters.query_scans_total.load(std::memory_order_relaxed);
  metrics.query_index_lookups_total = rapid_counters.query_index_lookups_total.load(std::memory_order_relaxed);
  metrics.query_rows_read_total = rapid_counters.query_rows_read_total.load(std::memory_order_relaxed);
  metrics.query_offload_total = rapid_counters.query_offload_total.load(std::memory_order_relaxed);
  metrics.query_offload_fallback_total = rapid_counters.query_offload_fallback_total.load(std::memory_order_relaxed);

  //  Transactions
  metrics.active_transactions = rapid_counters.active_transactions.load(std::memory_order_relaxed);
  metrics.transaction_commits_total = rapid_counters.transaction_commits_total.load(std::memory_order_relaxed);
  metrics.transaction_rollbacks_total = rapid_counters.transaction_rollbacks_total.load(std::memory_order_relaxed);
}

void print_rapid_monitor_info(FILE *file) {
  Metrics metrics;
  collect_rapid_monitor_metrics(metrics);

  fprintf(file,
          "==\n"
          "  ShannonBase Rapid Engine Monitor\n"
          "==\n");

  /*  Memory Pool  */
  fprintf(file,
          "\n-- Memory Pool --\n"
          "rapid_mempool_capacity_bytes          : %zu\n"
          "rapid_mempool_allocated_bytes         : %zu\n"
          "rapid_mempool_used_bytes              : %zu\n"
          "rapid_mempool_peak_usage_bytes        : %zu\n"
          "rapid_mempool_usage_percentage        : %.2f %%\n"
          "rapid_mempool_alloc_count             : %zu\n"
          "rapid_mempool_dealloc_count           : %zu\n"
          "rapid_mempool_failed_allocs           : %zu\n"
          "rapid_mempool_expansion_count         : %zu\n"
          "rapid_mempool_defrag_count            : %zu\n",
          metrics.mempool_capacity_bytes, metrics.mempool_allocated_bytes, metrics.mempool_used_bytes,
          metrics.mempool_peak_usage_bytes, metrics.mempool_usage_percentage, metrics.mempool_alloc_count,
          metrics.mempool_dealloc_count, metrics.mempool_failed_allocs, metrics.mempool_expansion_count,
          metrics.mempool_defrag_count);

  /*  IMCS  */
  fprintf(file,
          "\n-- IMCS (In-Memory Column Store) --\n"
          "rapid_loaded_tables                   : %zu\n"
          "rapid_loaded_part_tables              : %zu\n"
          "rapid_total_imcus                     : %zu\n"
          "rapid_total_cus                       : %zu\n"
          "rapid_total_rows                      : %" PRIu64
          "\n"
          "rapid_total_physical_rows             : %" PRIu64
          "\n"
          "rapid_estimated_data_size_bytes       : %" PRIu64
          "\n"
          "rapid_estimated_compressed_size_bytes : %" PRIu64 "\n",
          metrics.loaded_tables, metrics.loaded_part_tables, metrics.total_imcus, metrics.total_cus, metrics.total_rows,
          metrics.total_physical_rows, metrics.estimated_data_size_bytes, metrics.estimated_compressed_size_bytes);

  /*  Population  */
  fprintf(file,
          "\n-- Population / Propagation --\n"
          "rapid_pop_thread_running              : %s\n"
          "rapid_pop_loop_counter                : %" PRIu64
          "\n"
          "rapid_pop_data_remaining_kb           : %" PRIu64
          "\n"
          "rapid_pop_buffer_tables               : %zu\n"
          "rapid_pop_tables_in_progress          : %zu\n"
          "rapid_pop_worker_threads              : %zu\n"
          "rapid_pop_worker_pending_kb           : %" PRIu64 "\n",
          metrics.rapid_pop_thread_running ? "running" : "stopped", metrics.rapid_pop_loop_counter,
          metrics.rapid_pop_data_sz / 1024, metrics.total_buffer_tables, metrics.tables_in_progress,
          metrics.total_worker_threads, metrics.worker_pending_bytes / 1024);

  /*  Background Worker Pool  */
  fprintf(file,
          "\n-- Background Worker Pool --\n"
          "rapid_bg_pool_queue_size              : %zu\n"
          "rapid_bg_active_workers               : %zu / %zu\n"
          "rapid_bg_concurrent_gc                : %u\n"
          "rapid_bg_concurrent_compact           : %u\n"
          "rapid_bg_concurrent_stats             : %u\n"
          "rapid_bg_tasks_submitted              : %" PRIu64
          "\n"
          "rapid_bg_tasks_completed              : %" PRIu64
          "\n"
          "rapid_bg_tasks_failed                 : %" PRIu64
          "\n"
          "rapid_bg_tasks_cancelled              : %" PRIu64
          "\n"
          "rapid_bg_tasks_retried                : %" PRIu64 "\n",
          metrics.bg_pool_queue_size, metrics.bg_active_workers, metrics.bg_total_workers, metrics.bg_concurrent_gc,
          metrics.bg_concurrent_compact, metrics.bg_concurrent_stats, metrics.bg_tasks_submitted,
          metrics.bg_tasks_completed, metrics.bg_tasks_failed, metrics.bg_tasks_cancelled, metrics.bg_tasks_retried);

  /*  GC  */
  fprintf(file,
          "\n-- Garbage Collection --\n"
          "rapid_gc_total_runs                   : %" PRIu64
          "\n"
          "rapid_gc_total_purged_rows            : %" PRIu64
          "\n"
          "rapid_gc_total_purged_versions        : %" PRIu64
          "\n"
          "rapid_gc_last_run_scn                 : %" PRIu64
          "\n"
          "rapid_gc_last_run_duration_us         : %" PRIu64 "\n",
          metrics.gc_total_runs, metrics.gc_total_purged_rows, metrics.gc_total_purged_versions,
          metrics.gc_last_run_scn, metrics.gc_last_run_duration_us);

  /*  Compaction  */
  fprintf(file,
          "\n-- Compaction --\n"
          "rapid_compact_total_runs              : %" PRIu64
          "\n"
          "rapid_compact_total_merged_rows       : %" PRIu64
          "\n"
          "rapid_compact_last_run_duration_us    : %" PRIu64 "\n",
          metrics.compact_total_runs, metrics.compact_total_merged_rows, metrics.compact_last_run_duration_us);

  /*  Query Execution  */
  fprintf(file,
          "\n-- Query Execution --\n"
          "rapid_query_scans_total               : %" PRIu64
          "\n"
          "rapid_query_index_lookups_total       : %" PRIu64
          "\n"
          "rapid_query_rows_read_total           : %" PRIu64
          "\n"
          "rapid_query_offload_total             : %" PRIu64
          "\n"
          "rapid_query_offload_fallback_total    : %" PRIu64 "\n",
          metrics.query_scans_total, metrics.query_index_lookups_total, metrics.query_rows_read_total,
          metrics.query_offload_total, metrics.query_offload_fallback_total);

  /*  Transactions  */
  fprintf(file,
          "\n-- Transactions --\n"
          "rapid_active_transactions             : %" PRIu64
          "\n"
          "rapid_transaction_commits_total       : %" PRIu64
          "\n"
          "rapid_transaction_rollbacks_total     : %" PRIu64 "\n",
          metrics.active_transactions, metrics.transaction_commits_total, metrics.transaction_rollbacks_total);

  fprintf(file, "\n==\n");
}
}  // namespace RapidMonitor
}  // namespace ShannonBase
