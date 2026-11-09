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
#include <mutex>
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
        // Unknown until a physical compressed-byte measurement is available.
        // Do not report the uncompressed estimate as compressed storage.
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
  metrics.recovery_storage_restores = rapid_counters.recovery_storage_restores.load(std::memory_order_relaxed);
  metrics.recovery_primary_reloads = rapid_counters.recovery_primary_reloads.load(std::memory_order_relaxed);

  metrics.compact_total_runs = rapid_counters.compact_total_runs.load(std::memory_order_relaxed);
  metrics.compact_total_merged_rows = rapid_counters.compact_total_merged_rows.load(std::memory_order_relaxed);
  metrics.compact_last_run_duration_us = rapid_counters.compact_last_run_duration_us.load(std::memory_order_relaxed);

  //  Query Execution
#if RAPID_INSTRUMENTATION
  metrics.query_scans_total = rapid_counters.query_scans_total.load(std::memory_order_relaxed);
  metrics.query_index_lookups_total = rapid_counters.query_index_lookups_total.load(std::memory_order_relaxed);
#endif
  metrics.query_rows_read_total = rapid_counters.query_rows_read_total.load(std::memory_order_relaxed);
  metrics.query_offload_total = rapid_counters.query_offload_total.load(std::memory_order_relaxed);
#if RAPID_INSTRUMENTATION
  metrics.query_vectorized_window_rows_total =
      rapid_counters.query_vectorized_window_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_window_simd_rows_total =
      rapid_counters.query_vectorized_window_simd_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_window_scalar_rows_total =
      rapid_counters.query_vectorized_window_scalar_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_window_spill_rows_total =
      rapid_counters.query_vectorized_window_spill_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_window_spill_bytes_total =
      rapid_counters.query_vectorized_window_spill_bytes_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_builds_total =
      rapid_counters.query_hash_join_bloom_builds_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_bytes_total =
      rapid_counters.query_hash_join_bloom_bytes_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_checks_total =
      rapid_counters.query_hash_join_bloom_checks_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_rejects_total =
      rapid_counters.query_hash_join_bloom_rejects_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_scan_rejects_total =
      rapid_counters.query_hash_join_bloom_scan_rejects_total.load(std::memory_order_relaxed);
  metrics.query_hash_join_bloom_disabled_total =
      rapid_counters.query_hash_join_bloom_disabled_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_hash_join_spill_rows_total =
      rapid_counters.query_vectorized_hash_join_spill_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_spill_rows_total =
      rapid_counters.query_vectorized_aggregate_spill_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_scaled_values_total =
      rapid_counters.query_vectorized_aggregate_scaled_values_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_scalar_decimal_values_total =
      rapid_counters.query_vectorized_aggregate_scalar_decimal_values_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_batch_rows_total =
      rapid_counters.query_vectorized_aggregate_batch_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_sort_batch_rows_total =
      rapid_counters.query_vectorized_sort_batch_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_ref_batch_rows_total =
      rapid_counters.query_vectorized_ref_batch_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_expression_rows_total =
      rapid_counters.query_vectorized_expression_rows_total.load(std::memory_order_relaxed);
  metrics.query_like_mask_rows_total = rapid_counters.query_like_mask_rows_total.load(std::memory_order_relaxed);
  metrics.cu_bitpacked_columns_total = rapid_counters.cu_bitpacked_columns_total.load(std::memory_order_relaxed);
  metrics.query_bitpacked_predicate_rows_total =
      rapid_counters.query_bitpacked_predicate_rows_total.load(std::memory_order_relaxed);
  metrics.query_bitpacked_unpack_rows_total =
      rapid_counters.query_bitpacked_unpack_rows_total.load(std::memory_order_relaxed);
  metrics.query_bitpacked_aggregate_rows_total =
      rapid_counters.query_bitpacked_aggregate_rows_total.load(std::memory_order_relaxed);
  metrics.query_parallel_workers_total = rapid_counters.query_parallel_workers_total.load(std::memory_order_relaxed);
  metrics.query_parallel_tasks_total = rapid_counters.query_parallel_tasks_total.load(std::memory_order_relaxed);
  metrics.query_parallel_rows_total = rapid_counters.query_parallel_rows_total.load(std::memory_order_relaxed);
  metrics.query_parallel_spills_total = rapid_counters.query_parallel_spills_total.load(std::memory_order_relaxed);
  metrics.query_parallel_fallbacks_total =
      rapid_counters.query_parallel_fallbacks_total.load(std::memory_order_relaxed);
  metrics.query_parallel_join_build_rows_total =
      rapid_counters.query_parallel_join_build_rows_total.load(std::memory_order_relaxed);
  metrics.query_parallel_join_build_workers_total =
      rapid_counters.query_parallel_join_build_workers_total.load(std::memory_order_relaxed);
  metrics.query_parallel_join_finalize_partitions_total =
      rapid_counters.query_parallel_join_finalize_partitions_total.load(std::memory_order_relaxed);
  metrics.query_parallel_join_finalize_workers_total =
      rapid_counters.query_parallel_join_finalize_workers_total.load(std::memory_order_relaxed);
  metrics.query_parallel_join_build_fallbacks_total =
      rapid_counters.query_parallel_join_build_fallbacks_total.load(std::memory_order_relaxed);
  metrics.query_like_scalar_rows_total = rapid_counters.query_like_scalar_rows_total.load(std::memory_order_relaxed);
  metrics.query_dictionary_predicate_rows_total =
      rapid_counters.query_dictionary_predicate_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_like_rows_total =
      rapid_counters.query_vectorized_like_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_dictionary_minmax_rows_total =
      rapid_counters.query_vectorized_dictionary_minmax_rows_total.load(std::memory_order_relaxed);
  metrics.query_late_predicate_rows_total =
      rapid_counters.query_late_predicate_rows_total.load(std::memory_order_relaxed);
  metrics.query_late_projection_cells_total =
      rapid_counters.query_late_projection_cells_total.load(std::memory_order_relaxed);
  metrics.query_storage_index_skipped_imcus_total =
      rapid_counters.query_storage_index_skipped_imcus_total.load(std::memory_order_relaxed);
  metrics.query_scalar_expression_rows_total =
      rapid_counters.query_scalar_expression_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_stream_batch_rows_total =
      rapid_counters.query_vectorized_stream_batch_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_materialize_batch_rows_total =
      rapid_counters.query_vectorized_materialize_batch_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_row_materializations_total =
      rapid_counters.query_vectorized_aggregate_row_materializations_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_dict_cache_hits_total =
      rapid_counters.query_vectorized_aggregate_dict_cache_hits_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_dict_cache_misses_total =
      rapid_counters.query_vectorized_aggregate_dict_cache_misses_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_hash_mem_peak_bytes_sum_total =
      rapid_counters.query_vectorized_aggregate_hash_mem_peak_bytes_sum_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_aggregate_hash_memory_peak_bytes_max =
      rapid_counters.query_vectorized_aggregate_hash_memory_peak_bytes_max.load(std::memory_order_relaxed);
  metrics.query_vectorized_sort_rows_total =
      rapid_counters.query_vectorized_sort_rows_total.load(std::memory_order_relaxed);
  metrics.query_vectorized_sort_spill_rows_total =
      rapid_counters.query_vectorized_sort_spill_rows_total.load(std::memory_order_relaxed);
#endif
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
          "rapid_query_rows_read_total           : %" PRIu64
          "\n"
          "rapid_query_offload_total             : %" PRIu64
          "\n"
          "rapid_query_offload_fallback_total    : %" PRIu64 "\n",
          metrics.query_rows_read_total, metrics.query_offload_total, metrics.query_offload_fallback_total);

#if RAPID_INSTRUMENTATION
  /*  Query Execution -- diagnosis only, compiled out of release builds.  */
  fprintf(file,
          "rapid_query_scans_total               : %" PRIu64
          "\n"
          "rapid_query_index_lookups_total       : %" PRIu64
          "\n"
          "rapid_query_vectorized_window_rows_total: %" PRIu64
          "\n"
          "rapid_query_vectorized_window_simd_rows_total: %" PRIu64
          "\n"
          "rapid_query_vectorized_window_scalar_rows_total: %" PRIu64
          "\n"
          "rapid_query_vectorized_window_spill_rows_total: %" PRIu64
          "\n"
          "rapid_query_vectorized_window_spill_bytes_total: %" PRIu64 "\n",
          metrics.query_scans_total, metrics.query_index_lookups_total, metrics.query_vectorized_window_rows_total,
          metrics.query_vectorized_window_simd_rows_total, metrics.query_vectorized_window_scalar_rows_total,
          metrics.query_vectorized_window_spill_rows_total, metrics.query_vectorized_window_spill_bytes_total);
#endif

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

#if RAPID_INSTRUMENTATION
namespace {
/** One counter the per-statement line reports. */
struct CounterRef {
  const char *name;
  const std::atomic<uint64_t> *counter;
};

// Every counter the engine keeps, so a test can match any of them by name
// without this table having to grow. The name is what the line prints: the
// curated short names first (bloom_checks, sort_rows, ...) because tests read
// them, then the counter's own name for everything else.
// clang-format off
constexpr CounterRef kStatCounters[] = {
#define RAPID_STAT(name, field) {name, &rapid_counters.field},
    RAPID_STAT("scans", query_scans_total) RAPID_STAT("bloom_checks", query_hash_join_bloom_checks_total) RAPID_STAT(
        "bloom_rejects",
        query_hash_join_bloom_rejects_total) RAPID_STAT("bloom_scan_rejects",
                                                        query_hash_join_bloom_scan_rejects_total) RAPID_STAT("bloom_"
                                                                                                             "disabled",
                                                                                                             query_hash_join_bloom_disabled_total)
        RAPID_STAT("parallel_rows", query_parallel_rows_total) RAPID_STAT("parallel_fallbacks", query_parallel_fallbacks_total) RAPID_STAT(
            "sort_rows", query_vectorized_sort_rows_total) RAPID_STAT("sort_spill",
                                                                      query_vectorized_sort_spill_rows_total) RAPID_STAT("window_rows",
                                                                                                                         query_vectorized_window_rows_total)
            RAPID_STAT("window_spill", query_vectorized_window_spill_rows_total) RAPID_STAT("bitpack_unpack", query_bitpacked_unpack_rows_total) RAPID_STAT(
                "bitpack_predicate", query_bitpacked_predicate_rows_total) RAPID_STAT("bitpack_aggregate",
                                                                                      query_bitpacked_aggregate_rows_total)
                RAPID_STAT("late_rows", query_late_predicate_rows_total)
                    RAPID_STAT("si_skipped_imcus", query_storage_index_skipped_imcus_total)
                        RAPID_STAT("gc_total_runs", gc_total_runs) RAPID_STAT(
                    "gc_total_purged_rows",
                    gc_total_purged_rows) RAPID_STAT("gc_total_purged_versions",
                                                     gc_total_purged_versions) RAPID_STAT("gc_last_run_scn",
                                                                                          gc_last_run_scn)
                    RAPID_STAT("gc_last_run_duration_us", gc_last_run_duration_us) RAPID_STAT("compact_total_runs", compact_total_runs) RAPID_STAT(
                        "compact_total_merged_rows",
                        compact_total_merged_rows) RAPID_STAT("compact_last_run_duration_us",
                                                              compact_last_run_duration_us)
                        RAPID_STAT("query_rows_read", query_rows_read_total) RAPID_STAT("query_offload", query_offload_total) RAPID_STAT(
                            "query_offload_fallback",
                            query_offload_fallback_total) RAPID_STAT("query_index_lookups", query_index_lookups_total)
                            RAPID_STAT("window_simd_rows", query_vectorized_window_simd_rows_total) RAPID_STAT(
                                "window_scalar_rows",
                                query_vectorized_window_scalar_rows_total) RAPID_STAT("window_spill_bytes", query_vectorized_window_spill_bytes_total)
                                RAPID_STAT("hash_join_spill_rows", query_vectorized_hash_join_spill_rows_total) RAPID_STAT(
                                    "bloom_builds",
                                    query_hash_join_bloom_builds_total) RAPID_STAT("bloom_bytes", query_hash_join_bloom_bytes_total)
                                    RAPID_STAT("aggregate_spill_rows", query_vectorized_aggregate_spill_rows_total) RAPID_STAT(
                                        "aggregate_batch_rows",
                                        query_vectorized_aggregate_batch_rows_total)
                                        RAPID_STAT("aggregate_scaled_values", query_vectorized_aggregate_scaled_values_total) RAPID_STAT(
                                            "aggregate_scalar_decimal_values",
                                            query_vectorized_aggregate_scalar_decimal_values_total)
                                            RAPID_STAT("sort_batch_rows", query_vectorized_sort_batch_rows_total) RAPID_STAT(
                                                "ref_batch_rows",
                                                query_vectorized_ref_batch_rows_total) RAPID_STAT("expression_rows",
                                                                                                  query_vectorized_expression_rows_total)
                                                RAPID_STAT("like_mask_rows", query_like_mask_rows_total) RAPID_STAT(
                                                    "bitpacked_columns",
                                                    cu_bitpacked_columns_total) RAPID_STAT("parallel_workers", query_parallel_workers_total)
                                                    RAPID_STAT("parallel_tasks", query_parallel_tasks_total) RAPID_STAT(
                                                        "parallel_spills",
                                                        query_parallel_spills_total)
                                                        RAPID_STAT("join_build_rows", query_parallel_join_build_rows_total) RAPID_STAT(
                                                            "join_build_workers", query_parallel_join_build_workers_total) RAPID_STAT("join_finalize_partitions",
                                                                                                                                      query_parallel_join_finalize_partitions_total)
                                                            RAPID_STAT("join_finalize_workers", query_parallel_join_finalize_workers_total) RAPID_STAT(
                                                                "join_build_fallbacks",
                                                                query_parallel_join_build_fallbacks_total) RAPID_STAT("like_scalar_rows",
                                                                                                                      query_like_scalar_rows_total)
                                                                RAPID_STAT("dictionary_predicate_rows", query_dictionary_predicate_rows_total) RAPID_STAT(
                                                                    "like_rows",
                                                                    query_vectorized_like_rows_total) RAPID_STAT("dicti"
                                                                                                                 "onary"
                                                                                                                 "_minm"
                                                                                                                 "ax_"
                                                                                                                 "rows",
                                                                                                                 query_vectorized_dictionary_minmax_rows_total)
                                                                    RAPID_STAT("late_projection_cells", query_late_projection_cells_total) RAPID_STAT(
                                                                        "scalar_expression_rows",
                                                                        query_scalar_expression_rows_total)
                                                                        RAPID_STAT("stream_batch_rows", query_vectorized_stream_batch_rows_total) RAPID_STAT(
                                                                            "materialize_batch_rows",
                                                                            query_vectorized_materialize_batch_rows_total)
                                                                            RAPID_STAT(
                                                                                "aggregate_row_"
                                                                                "materializations",
                                                                                query_vectorized_aggregate_row_materializations_total)
                                                                                RAPID_STAT(
                                                                                    "aggregate_dict_cache_hits",
                                                                                    query_vectorized_aggregate_dict_cache_hits_total)
                                                                                    RAPID_STAT(
                                                                                        "aggregate_dict_cache_"
                                                                                        "misses",
                                                                                        query_vectorized_aggregate_dict_cache_misses_total)
                                                                                        RAPID_STAT(
                                                                                            "aggregate_hash_mem_"
                                                                                            "peak_bytes_sum",
                                                                                            query_vectorized_aggregate_hash_mem_peak_bytes_sum_total)
                                                                                            RAPID_STAT(
                                                                                                "aggregate_hash_"
                                                                                                "memory_peak_bytes_"
                                                                                                "max",
                                                                                                query_vectorized_aggregate_hash_memory_peak_bytes_max)
                                                                                                RAPID_STAT(
                                                                                                    "active_"
                                                                                                    "transactions",
                                                                                                    active_transactions)
                                                                                                    RAPID_STAT(
                                                                                                        "tra"
                                                                                                        "nsa"
                                                                                                        "cti"
                                                                                                        "on_"
                                                                                                        "com"
                                                                                                        "mit"
                                                                                                        "s",
                                                                                                        transaction_commits_total)
                                                                                                        RAPID_STAT(
                                                                                                            "transa"
                                                                                                            "ction_"
                                                                                                            "rollba"
                                                                                                            "cks",
                                                                                                            transaction_rollbacks_total)
                                                                                                            RAPID_STAT(
                                                                                                                "re"
                                                                                                                "co"
                                                                                                                "ve"
                                                                                                                "ry"
                                                                                                                "_s"
                                                                                                                "to"
                                                                                                                "ra"
                                                                                                                "ge"
                                                                                                                "_r"
                                                                                                                "es"
                                                                                                                "to"
                                                                                                                "re"
                                                                                                                "s",
                                                                                                                recovery_storage_restores)
                                                                                                                RAPID_STAT(
                                                                                                                    "re"
                                                                                                                    "co"
                                                                                                                    "ve"
                                                                                                                    "ry"
                                                                                                                    "_p"
                                                                                                                    "ri"
                                                                                                                    "ma"
                                                                                                                    "ry"
                                                                                                                    "_r"
                                                                                                                    "el"
                                                                                                                    "oa"
                                                                                                                    "d"
                                                                                                                    "s",
                                                                                                                    recovery_primary_reloads)
#undef RAPID_STAT
};
// clang-format on

constexpr size_t kStatCounterCount = sizeof(kStatCounters) / sizeof(kStatCounters[0]);
}  // namespace

void log_rapid_counters(const char *tag, bool emit) {
  // The baseline moves on every statement, so turning the flag on mid-session
  // reports that statement rather than everything since the server started.
  static std::mutex lock;
  static uint64_t previous[kStatCounterCount] = {};

  std::lock_guard<std::mutex> guard(lock);

  uint64_t current[kStatCounterCount];
  for (size_t i = 0; i < kStatCounterCount; ++i) current[i] = kStatCounters[i].counter->load(std::memory_order_relaxed);

  if (emit) {
    // One line per statement: a test that wants "did this query reject a probe
    // row" cannot get an answer out of a running total.
    char line[8192];
    size_t used = static_cast<size_t>(snprintf(line, sizeof line, "RAPID_STATS [%s]", tag));
    if (used >= sizeof line) used = sizeof line - 1;
    for (size_t i = 0; i < kStatCounterCount && used + 64 < sizeof line; ++i) {
      used += static_cast<size_t>(
          snprintf(line + used, sizeof line - used, " %s=%" PRIu64, kStatCounters[i].name, current[i] - previous[i]));
    }
    if (used >= sizeof line) used = sizeof line - 1;
    line[used++] = '\n';
    line[used] = '\0';
    fputs(line, stderr);
  }

  for (size_t i = 0; i < kStatCounterCount; ++i) previous[i] = current[i];
}
#endif

}  // namespace RapidMonitor
}  // namespace ShannonBase
