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

#include "my_dbug.h"

// Kernel instrumentation is compiled out of release builds. NDEBUG is what an
// optimized build actually defines here; DBUG_OFF is honoured too in case a
// build sets it. Then the counters' storage and their updates disappear instead
// of only being hidden from SHOW STATUS. The operational surface is
// RAPID_STATUS_VARS; everything below is diagnosis.
#if defined(DBUG_OFF) || defined(NDEBUG)
#define RAPID_INSTRUMENTATION 0
#else
#define RAPID_INSTRUMENTATION 1
#endif

#if RAPID_INSTRUMENTATION
#define RAPID_COUNT(field, value) \
  (::ShannonBase::RapidMonitor::rapid_counters.field.fetch_add((value), std::memory_order_relaxed))
#else
#define RAPID_COUNT(field, value) ((void)(value))
#endif

#if RAPID_INSTRUMENTATION
namespace ShannonBase {
namespace RapidMonitor {
/**
 * Logs what the statement just finished contributed, not the totals: a test asks
 * "did this query reject any probe row" and a running total cannot answer it.
 * `emit` is the DBUG flag, so the baseline keeps moving while the flag is off.
 */
void log_rapid_counters(const char *tag, bool emit);
}  // namespace RapidMonitor
}  // namespace ShannonBase

/**
 * Statement-end hook for the instrumentation counters. Debug builds only, and it
 * writes to the error log rather than exposing anything through SQL: the counters
 * are internal, and a release build has neither them nor this call.
 */
#define RAPID_LOG_STATS(tag)                                       \
  do {                                                             \
    if (DBUG_EVALUATE_IF("rapid_stats", true, false))              \
      ::ShannonBase::RapidMonitor::log_rapid_counters(tag, true);  \
    else                                                           \
      ::ShannonBase::RapidMonitor::log_rapid_counters(tag, false); \
  } while (0)
#else
#define RAPID_LOG_STATS(tag) ((void)0)
#endif

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

  //  Query Execution -- operational subset.
  uint64_t query_rows_read_total{0};
  uint64_t query_offload_total{0};
  uint64_t query_offload_fallback_total{0};

#if RAPID_INSTRUMENTATION
  // Per-operator instrumentation: diagnosis only, absent from release.
  uint64_t query_scans_total{0};
  uint64_t query_index_lookups_total{0};
  uint64_t query_vectorized_window_rows_total{0};
  uint64_t query_vectorized_window_simd_rows_total{0};
  uint64_t query_vectorized_window_scalar_rows_total{0};
  uint64_t query_vectorized_window_spill_rows_total{0};
  uint64_t query_vectorized_window_spill_bytes_total{0};
  uint64_t query_vectorized_hash_join_spill_rows_total{0};
  uint64_t query_hash_join_bloom_builds_total{0};
  uint64_t query_hash_join_bloom_bytes_total{0};
  uint64_t query_hash_join_bloom_checks_total{0};
  uint64_t query_hash_join_bloom_rejects_total{0};
  uint64_t query_hash_join_bloom_scan_rejects_total{0};
  uint64_t query_hash_join_bloom_disabled_total{0};
  uint64_t query_vectorized_aggregate_spill_rows_total{0};
  uint64_t query_vectorized_aggregate_batch_rows_total{0};
  uint64_t query_vectorized_aggregate_scaled_values_total{0};
  uint64_t query_vectorized_aggregate_scalar_decimal_values_total{0};
  uint64_t query_vectorized_sort_batch_rows_total{0};
  uint64_t query_vectorized_ref_batch_rows_total{0};
  uint64_t query_vectorized_expression_rows_total{0};
  uint64_t query_like_mask_rows_total{0};
  uint64_t cu_bitpacked_columns_total{0};
  uint64_t query_bitpacked_predicate_rows_total{0};
  uint64_t query_bitpacked_unpack_rows_total{0};
  uint64_t query_bitpacked_aggregate_rows_total{0};
  uint64_t query_parallel_workers_total{0};
  uint64_t query_parallel_tasks_total{0};
  uint64_t query_parallel_rows_total{0};
  uint64_t query_parallel_spills_total{0};
  uint64_t query_parallel_fallbacks_total{0};
  uint64_t query_parallel_join_build_rows_total{0};
  uint64_t query_parallel_join_build_workers_total{0};
  uint64_t query_parallel_join_finalize_partitions_total{0};
  uint64_t query_parallel_join_finalize_workers_total{0};
  uint64_t query_parallel_join_build_fallbacks_total{0};
  uint64_t query_like_scalar_rows_total{0};
  uint64_t query_dictionary_predicate_rows_total{0};
  uint64_t query_vectorized_like_rows_total{0};
  uint64_t query_vectorized_dictionary_minmax_rows_total{0};
  uint64_t query_late_predicate_rows_total{0};
  uint64_t query_late_projection_cells_total{0};
  uint64_t query_scalar_expression_rows_total{0};
  uint64_t query_vectorized_stream_batch_rows_total{0};
  uint64_t query_vectorized_materialize_batch_rows_total{0};
  uint64_t query_vectorized_aggregate_row_materializations_total{0};
  uint64_t query_vectorized_aggregate_dict_cache_hits_total{0};
  uint64_t query_vectorized_aggregate_dict_cache_misses_total{0};
  // Sum of per-operator arena peaks, useful as a delta around one query.
  uint64_t query_vectorized_aggregate_hash_mem_peak_bytes_sum_total{0};
  // Largest single operator arena peak observed since server start.
  uint64_t query_vectorized_aggregate_hash_memory_peak_bytes_max{0};
  uint64_t query_vectorized_sort_rows_total{0};
  uint64_t query_vectorized_sort_spill_rows_total{0};
#endif

  //  Transactions
  uint64_t active_transactions{0};
  uint64_t transaction_commits_total{0};
  uint64_t transaction_rollbacks_total{0};

  //  Recovery
  // Checkpoints whose snapshot became durable but whose WAL could not then be
  // truncated. The checkpoint itself succeeded, so this is not an error the
  // caller can act on -- but it is the one failure that lets the WAL grow
  // without bound, and a log line alone is not something an operator can
  // alert on.
  uint64_t recovery_storage_restores{0};
  uint64_t recovery_primary_reloads{0};
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

  // Query Execution -- operational subset.
  std::atomic<uint64_t> query_rows_read_total{0};
  std::atomic<uint64_t> query_offload_total{0};
  std::atomic<uint64_t> query_offload_fallback_total{0};

#if RAPID_INSTRUMENTATION
  std::atomic<uint64_t> query_scans_total{0};
  std::atomic<uint64_t> query_index_lookups_total{0};
  std::atomic<uint64_t> query_vectorized_window_rows_total{0};
  std::atomic<uint64_t> query_vectorized_window_simd_rows_total{0};
  std::atomic<uint64_t> query_vectorized_window_scalar_rows_total{0};
  std::atomic<uint64_t> query_vectorized_window_spill_rows_total{0};
  std::atomic<uint64_t> query_vectorized_window_spill_bytes_total{0};
  std::atomic<uint64_t> query_vectorized_hash_join_spill_rows_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_builds_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_bytes_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_checks_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_rejects_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_scan_rejects_total{0};
  std::atomic<uint64_t> query_hash_join_bloom_disabled_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_spill_rows_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_batch_rows_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_scaled_values_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_scalar_decimal_values_total{0};
  std::atomic<uint64_t> query_vectorized_sort_batch_rows_total{0};
  std::atomic<uint64_t> query_vectorized_ref_batch_rows_total{0};
  std::atomic<uint64_t> query_vectorized_expression_rows_total{0};
  std::atomic<uint64_t> query_like_mask_rows_total{0};
  std::atomic<uint64_t> cu_bitpacked_columns_total{0};
  std::atomic<uint64_t> query_bitpacked_predicate_rows_total{0};
  std::atomic<uint64_t> query_bitpacked_unpack_rows_total{0};
  std::atomic<uint64_t> query_bitpacked_aggregate_rows_total{0};
  std::atomic<uint64_t> query_parallel_workers_total{0};
  std::atomic<uint64_t> query_parallel_tasks_total{0};
  std::atomic<uint64_t> query_parallel_rows_total{0};
  std::atomic<uint64_t> query_parallel_spills_total{0};
  std::atomic<uint64_t> query_parallel_fallbacks_total{0};
  std::atomic<uint64_t> query_parallel_join_build_rows_total{0};
  std::atomic<uint64_t> query_parallel_join_build_workers_total{0};
  std::atomic<uint64_t> query_parallel_join_finalize_partitions_total{0};
  std::atomic<uint64_t> query_parallel_join_finalize_workers_total{0};
  std::atomic<uint64_t> query_parallel_join_build_fallbacks_total{0};
  std::atomic<uint64_t> query_like_scalar_rows_total{0};
  std::atomic<uint64_t> query_dictionary_predicate_rows_total{0};
  std::atomic<uint64_t> query_vectorized_like_rows_total{0};
  std::atomic<uint64_t> query_vectorized_dictionary_minmax_rows_total{0};
  std::atomic<uint64_t> query_late_predicate_rows_total{0};
  std::atomic<uint64_t> query_late_projection_cells_total{0};
  std::atomic<uint64_t> query_scalar_expression_rows_total{0};
  std::atomic<uint64_t> query_vectorized_stream_batch_rows_total{0};
  std::atomic<uint64_t> query_vectorized_materialize_batch_rows_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_row_materializations_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_dict_cache_hits_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_dict_cache_misses_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_hash_mem_peak_bytes_sum_total{0};
  std::atomic<uint64_t> query_vectorized_aggregate_hash_memory_peak_bytes_max{0};
  std::atomic<uint64_t> query_vectorized_sort_rows_total{0};
  std::atomic<uint64_t> query_vectorized_sort_spill_rows_total{0};
#endif

  // Transactions
  std::atomic<uint64_t> active_transactions{0};
  std::atomic<uint64_t> transaction_commits_total{0};
  std::atomic<uint64_t> transaction_rollbacks_total{0};

  // Recovery
  std::atomic<uint64_t> recovery_storage_restores{0};
  std::atomic<uint64_t> recovery_primary_reloads{0};
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

inline void rapid_counter_query_scan() { RAPID_COUNT(query_scans_total, 1); }

inline void rapid_counter_query_index_lookup() { RAPID_COUNT(query_index_lookups_total, 1); }

inline void rapid_counter_query_rows_read(uint64_t n) {
  rapid_counters.query_rows_read_total.fetch_add(n, std::memory_order_relaxed);
}

inline void rapid_counter_query_offload() {
  rapid_counters.query_offload_total.fetch_add(1, std::memory_order_relaxed);
}

/** Rows emitted by the Rapid vectorized window operator (see
 * VectorizedWindowIterator). Zero means every window fell back to MySQL's
 * frame-buffer iterators. */
/**
 * Publish one vectorized window operator's kernel and spill totals.
 *
 * Called once per operator rather than per segment, so the accounting costs one
 * atomic add each per execution. simd_rows and scalar_rows are what tells you
 * whether the columnar kernels are actually being reached: a workload whose
 * scalar share is high is either NULL-heavy or hitting a type the SIMD kernels
 * do not cover, and that is the signal any kernel-dispatch tuning needs.
 */
inline void rapid_counter_vectorized_window_kernel(uint64_t simd_rows, uint64_t scalar_rows, uint64_t spill_rows,
                                                   uint64_t spill_bytes) {
  if (simd_rows != 0) RAPID_COUNT(query_vectorized_window_simd_rows_total, simd_rows);
  if (scalar_rows != 0) RAPID_COUNT(query_vectorized_window_scalar_rows_total, scalar_rows);
  if (spill_rows != 0) RAPID_COUNT(query_vectorized_window_spill_rows_total, spill_rows);
  if (spill_bytes != 0) RAPID_COUNT(query_vectorized_window_spill_bytes_total, spill_bytes);
}

// Rows the vectorized hash join / hash aggregate wrote to a spill file.
inline void rapid_counter_vectorized_hash_join_spill_row() {
  RAPID_COUNT(query_vectorized_hash_join_spill_rows_total, 1);
}
inline void rapid_counter_vectorized_aggregate_spill_row() {
  RAPID_COUNT(query_vectorized_aggregate_spill_rows_total, 1);
}
inline void rapid_counter_vectorized_aggregate_hash_stats(uint64_t batch_rows, uint64_t row_materializations,
                                                          uint64_t cache_hits, uint64_t cache_misses,
                                                          uint64_t peak_bytes) {
  RAPID_COUNT(query_vectorized_aggregate_batch_rows_total, batch_rows);
  RAPID_COUNT(query_vectorized_aggregate_row_materializations_total, row_materializations);
  RAPID_COUNT(query_vectorized_aggregate_dict_cache_hits_total, cache_hits);
  RAPID_COUNT(query_vectorized_aggregate_dict_cache_misses_total, cache_misses);
  RAPID_COUNT(query_vectorized_aggregate_hash_mem_peak_bytes_sum_total, peak_bytes);
#if RAPID_INSTRUMENTATION
  auto &peak_max = rapid_counters.query_vectorized_aggregate_hash_memory_peak_bytes_max;
  uint64_t observed = peak_max.load(std::memory_order_relaxed);
  while (observed < peak_bytes && !peak_max.compare_exchange_weak(observed, peak_bytes, std::memory_order_relaxed)) {
  }
#endif
}

// Rows sorted by the vectorized sort, and rows it wrote to sorted runs on disk.
inline void rapid_counter_vectorized_sort_rows(uint64_t n) { RAPID_COUNT(query_vectorized_sort_rows_total, n); }
inline void rapid_counter_vectorized_sort_spill_rows(uint64_t n) {
  RAPID_COUNT(query_vectorized_sort_spill_rows_total, n);
}

inline void rapid_counter_vectorized_window_rows(uint64_t n) { RAPID_COUNT(query_vectorized_window_rows_total, n); }

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
