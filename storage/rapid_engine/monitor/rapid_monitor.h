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
#ifndef __SHANNONBASE_RAPID_MONITOR_H__
#define __SHANNONBASE_RAPID_MONITOR_H__

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ShannonBase {
namespace Populate {
class PopulatorImpl;
}
namespace RapidMonitor {

/**
 * @struct Metrics
 * @brief Comprehensive runtime metrics for the ShannonBase Rapid Engine.
 *
 * These metrics are designed to be scraped by Prometheus and visualized in
 * Grafana.  Every field is exposed as a SHOW STATUS variable (prefixed with
 * "rapid_") so that standard MySQL exporters (e.g. mysqld_exporter) can
 * collect them without code changes.
 *
 * Categories:
 *  - Memory Pool
 *  - IMCS (In-Memory Column Store)
 *  - Population / Propagation
 *  - Background Worker Pool
 *  - Garbage Collection
 *  - Compaction
 *  - Query Execution
 *  - Transactions
 */
struct Metrics {
  //  Memory Pool
  size_t mempool_capacity_bytes{0};
  size_t mempool_allocated_bytes{0};
  size_t mempool_used_bytes{0};
  size_t mempool_peak_usage_bytes{0};
  double mempool_usage_percentage{0.0};
  size_t mempool_alloc_count{0};
  size_t mempool_dealloc_count{0};
  size_t mempool_failed_allocs{0};
  size_t mempool_expansion_count{0};
  size_t mempool_defrag_count{0};

  //  IMCS
  size_t loaded_tables{0};
  size_t loaded_part_tables{0};
  size_t total_imcus{0};
  size_t total_cus{0};
  uint64_t total_rows{0};
  uint64_t total_physical_rows{0};
  uint64_t estimated_data_size_bytes{0};
  uint64_t estimated_compressed_size_bytes{0};

  //  Population / Propagation
  bool rapid_pop_thread_running{false};
  uint64_t rapid_pop_loop_counter{0};
  uint64_t rapid_pop_data_sz{0};
  size_t total_buffer_tables{0};
  size_t tables_in_progress{0};
  size_t total_worker_threads{0};
  uint64_t worker_pending_bytes{0};

  //  Background Worker Pool
  size_t bg_pool_queue_size{0};
  size_t bg_active_workers{0};
  size_t bg_total_workers{0};
  uint32_t bg_concurrent_gc{0};
  uint32_t bg_concurrent_compact{0};
  uint32_t bg_concurrent_stats{0};
  uint64_t bg_tasks_submitted{0};
  uint64_t bg_tasks_completed{0};
  uint64_t bg_tasks_failed{0};
  uint64_t bg_tasks_cancelled{0};
  uint64_t bg_tasks_retried{0};

  //  Garbage Collection
  uint64_t gc_total_runs{0};
  uint64_t gc_total_purged_rows{0};
  uint64_t gc_total_purged_versions{0};
  uint64_t gc_last_run_scn{0};
  uint64_t gc_last_run_duration_us{0};

  //  Compaction
  uint64_t compact_total_runs{0};
  uint64_t compact_total_merged_rows{0};
  uint64_t compact_last_run_duration_us{0};

  //  Query Execution
  uint64_t query_scans_total{0};
  uint64_t query_index_lookups_total{0};
  uint64_t query_rows_read_total{0};
  uint64_t query_offload_total{0};
  uint64_t query_offload_fallback_total{0};

  //  Transactions
  uint64_t active_transactions{0};
  uint64_t transaction_commits_total{0};
  uint64_t transaction_rollbacks_total{0};
};

/**
 * Global atomic counters for operational metrics that are incremented
 * from hot paths (query execution, GC, compaction, transactions).
 * These are lock-free and can be sampled by collect_rapid_monitor_metrics().
 */
struct RapidCounters {
  // GC
  std::atomic<uint64_t> gc_total_runs{0};
  std::atomic<uint64_t> gc_total_purged_rows{0};
  std::atomic<uint64_t> gc_total_purged_versions{0};
  std::atomic<uint64_t> gc_last_run_scn{0};
  std::atomic<uint64_t> gc_last_run_duration_us{0};

  // Compaction
  std::atomic<uint64_t> compact_total_runs{0};
  std::atomic<uint64_t> compact_total_merged_rows{0};
  std::atomic<uint64_t> compact_last_run_duration_us{0};

  // Query Execution
  std::atomic<uint64_t> query_scans_total{0};
  std::atomic<uint64_t> query_index_lookups_total{0};
  std::atomic<uint64_t> query_rows_read_total{0};
  std::atomic<uint64_t> query_offload_total{0};
  std::atomic<uint64_t> query_offload_fallback_total{0};

  // Transactions
  std::atomic<uint64_t> active_transactions{0};
  std::atomic<uint64_t> transaction_commits_total{0};
  std::atomic<uint64_t> transaction_rollbacks_total{0};
};

/** Global rapid engine counters. */
extern RapidCounters rapid_counters;

void collect_rapid_monitor_metrics(Metrics &metrics);
void print_rapid_monitor_info(FILE *file);

/**
 * Convenience helpers to increment operational counters from anywhere
 * in the rapid engine without pulling in the full monitor header.
 */
inline void rapid_counter_gc_run(uint64_t purged_rows, uint64_t purged_versions, uint64_t scn, uint64_t duration_us) {
  rapid_counters.gc_total_runs.fetch_add(1, std::memory_order_relaxed);
  rapid_counters.gc_total_purged_rows.fetch_add(purged_rows, std::memory_order_relaxed);
  rapid_counters.gc_total_purged_versions.fetch_add(purged_versions, std::memory_order_relaxed);
  rapid_counters.gc_last_run_scn.store(scn, std::memory_order_relaxed);
  rapid_counters.gc_last_run_duration_us.store(duration_us, std::memory_order_relaxed);
}

inline void rapid_counter_compact_run(uint64_t merged_rows, uint64_t duration_us) {
  rapid_counters.compact_total_runs.fetch_add(1, std::memory_order_relaxed);
  rapid_counters.compact_total_merged_rows.fetch_add(merged_rows, std::memory_order_relaxed);
  rapid_counters.compact_last_run_duration_us.store(duration_us, std::memory_order_relaxed);
}

inline void rapid_counter_query_scan() { rapid_counters.query_scans_total.fetch_add(1, std::memory_order_relaxed); }

inline void rapid_counter_query_index_lookup() {
  rapid_counters.query_index_lookups_total.fetch_add(1, std::memory_order_relaxed);
}

inline void rapid_counter_query_rows_read(uint64_t n) {
  rapid_counters.query_rows_read_total.fetch_add(n, std::memory_order_relaxed);
}

inline void rapid_counter_query_offload() {
  rapid_counters.query_offload_total.fetch_add(1, std::memory_order_relaxed);
}

inline void rapid_counter_query_offload_fallback() {
  rapid_counters.query_offload_fallback_total.fetch_add(1, std::memory_order_relaxed);
}

inline void rapid_counter_txn_begin() { rapid_counters.active_transactions.fetch_add(1, std::memory_order_relaxed); }

inline void rapid_counter_txn_commit() {
  rapid_counters.active_transactions.fetch_sub(1, std::memory_order_relaxed);
  rapid_counters.transaction_commits_total.fetch_add(1, std::memory_order_relaxed);
}

inline void rapid_counter_txn_rollback() {
  rapid_counters.active_transactions.fetch_sub(1, std::memory_order_relaxed);
  rapid_counters.transaction_rollbacks_total.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace RapidMonitor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_MONITOR_H__
