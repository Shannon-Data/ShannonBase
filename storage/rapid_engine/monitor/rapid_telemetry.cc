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
#include "storage/rapid_engine/monitor/rapid_telemetry.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>

#include "my_dbug.h"
#include "mysql/components/my_service.h"
#include "mysql/components/services/bits/psi_metric_bits.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/psi_metric_service.h"
#include "mysql/service_plugin_registry.h"
#include "mysqld_error.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"

namespace ShannonBase {
namespace RapidMonitor {

namespace {

/**
 * Reads one counter on demand.
 *
 * The measured value is not pushed: performance_schema calls this when an
 * exporter scrapes, and `measurement_context` is the address of the atomic the
 * metric describes (see the table below). That keeps the hot paths free of any
 * telemetry bookkeeping beyond the atomic add they already do, and it means a
 * metric costs nothing while nobody is scraping.
 *
 * OTEL integers are int64, the counters are uint64, so a saturated counter has
 * to be clamped rather than wrapped into a negative reading.
 */
void read_counter(void *measurement_context, measurement_delivery_callback_t delivery, void *delivery_context) {
  assert(measurement_context != nullptr);
  assert(delivery != nullptr);

  const auto *counter = static_cast<const std::atomic<uint64_t> *>(measurement_context);
  const uint64_t observed = counter->load(std::memory_order_relaxed);
  const int64_t value = observed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                            ? std::numeric_limits<int64_t>::max()
                            : static_cast<int64_t>(observed);

  delivery->value_int64(delivery_context, value);
}

/**
 * One row per exported metric.
 *
 * `m_measurement_context` deliberately points straight at the field in
 * `rapid_counters`, so adding a metric is adding a row here and nothing else.
 * These are ASYNC_COUNTER rather than observable gauges: every one of them only
 * ever grows, and the exporter is what turns the difference between two scrapes
 * into a rate.
 */
#define RAPID_METRIC(name, description, field)                                                                \
  {                                                                                                           \
    name, "1", description, MetricOTELType::ASYNC_COUNTER, MetricNumType::METRIC_INTEGER, 0, 0, read_counter, \
        static_cast<void *>(&::ShannonBase::RapidMonitor::rapid_counters.field)                               \
  }

// clang-format off
PSI_metric_info_v1 rapid_metrics[] = {
    RAPID_METRIC("rapid.query.rows.read",
                 "Rows returned by Rapid operators to their parent operator.",
                 query_rows_read_total),
    RAPID_METRIC("rapid.query.offload",
                 "Queries planned onto the Rapid engine.", query_offload_total),
    RAPID_METRIC("rapid.query.offload.fallbacks",
                 "Queries planned onto Rapid that had to fall back to MySQL.",
                 query_offload_fallback_total),
    RAPID_METRIC("rapid.query.scans", "Rapid table scans executed.",
                 query_scans_total),
    RAPID_METRIC("rapid.query.index_lookups", "Rapid index lookups executed.",
                 query_index_lookups_total),

    RAPID_METRIC("rapid.aggregate.batch_rows",
                 "Rows consumed by the vectorized hash aggregate.",
                 query_vectorized_aggregate_batch_rows_total),
    RAPID_METRIC("rapid.aggregate.row_materializations",
                 "Aggregate groups materialized into a row per group.",
                 query_vectorized_aggregate_row_materializations_total),
    RAPID_METRIC("rapid.aggregate.dict_cache.hits",
                 "Dictionary cache hits while materializing aggregate groups.",
                 query_vectorized_aggregate_dict_cache_hits_total),
    RAPID_METRIC("rapid.aggregate.dict_cache.misses",
                 "Dictionary cache misses while materializing aggregate groups.",
                 query_vectorized_aggregate_dict_cache_misses_total),
    RAPID_METRIC("rapid.aggregate.spill_rows",
                 "Rows the vectorized hash aggregate wrote to spill files.",
                 query_vectorized_aggregate_spill_rows_total),
    RAPID_METRIC("rapid.aggregate.hash_peak_bytes.sum",
                 "Sum of per-operator peaks of the aggregate hash table.",
                 query_vectorized_aggregate_hash_mem_peak_bytes_sum_total),
    RAPID_METRIC("rapid.aggregate.hash_peak_bytes.max",
                 "Largest peak of the aggregate hash table seen so far.",
                 query_vectorized_aggregate_hash_memory_peak_bytes_max),

    RAPID_METRIC("rapid.window.simd_rows",
                 "Window rows processed by SIMD kernels.",
                 query_vectorized_window_simd_rows_total),
    RAPID_METRIC("rapid.window.scalar_rows",
                 "Window rows processed by the scalar fallback kernels.",
                 query_vectorized_window_scalar_rows_total),
    RAPID_METRIC("rapid.window.spill_rows",
                 "Window rows spilled to disk.", query_vectorized_window_spill_rows_total),
    RAPID_METRIC("rapid.window.spill_bytes",
                 "Bytes written by window operators that spilled.",
                 query_vectorized_window_spill_bytes_total),

    RAPID_METRIC("rapid.sort.rows", "Rows sorted by the vectorized sort.",
                 query_vectorized_sort_rows_total),
    RAPID_METRIC("rapid.sort.spill_rows",
                 "Rows the vectorized sort wrote to sorted runs.",
                 query_vectorized_sort_spill_rows_total),

    RAPID_METRIC("rapid.joins.bloom.builds",
                 "Bloom filters built by the vectorized hash join.",
                 query_hash_join_bloom_builds_total),
    RAPID_METRIC("rapid.joins.bloom.bytes",
                 "Bytes allocated by hash join bloom filters.",
                 query_hash_join_bloom_bytes_total),
    RAPID_METRIC("rapid.joins.bloom.checks",
                 "Probe rows checked against a join bloom filter.",
                 query_hash_join_bloom_checks_total),
    RAPID_METRIC("rapid.joins.bloom.rejects",
                 "Probe rows rejected by a join bloom filter.",
                 query_hash_join_bloom_rejects_total),
    RAPID_METRIC("rapid.joins.bloom.scan_rejects",
                 "Probe rows rejected while scanning a join bloom filter.",
                 query_hash_join_bloom_scan_rejects_total),
    RAPID_METRIC("rapid.joins.bloom.disabled",
                 "Hash joins that ran without a bloom filter.",
                 query_hash_join_bloom_disabled_total),
    RAPID_METRIC("rapid.joins.spill_rows",
                 "Rows the vectorized hash join wrote to spill files.",
                 query_vectorized_hash_join_spill_rows_total),

    RAPID_METRIC("rapid.bitpack.columns",
                 "Columns loaded in bit-packed encoding.",
                 cu_bitpacked_columns_total),
    RAPID_METRIC("rapid.bitpack.unpack_rows",
                 "Rows decoded from bit-packed encoding.",
                 query_bitpacked_unpack_rows_total),
    RAPID_METRIC("rapid.bitpack.predicate_rows",
                 "Rows evaluated directly on bit-packed encoding.",
                 query_bitpacked_predicate_rows_total),
    RAPID_METRIC("rapid.bitpack.aggregate_rows",
                 "Rows aggregated directly on bit-packed encoding.",
                 query_bitpacked_aggregate_rows_total),

    RAPID_METRIC("rapid.parallel.workers",
                 "Worker threads started by the parallel executor.",
                 query_parallel_workers_total),
    RAPID_METRIC("rapid.parallel.tasks", "Parallel tasks executed.",
                 query_parallel_tasks_total),
    RAPID_METRIC("rapid.parallel.rows", "Rows produced by parallel tasks.",
                 query_parallel_rows_total),
    RAPID_METRIC("rapid.parallel.spills", "Parallel task spills.",
                 query_parallel_spills_total),
    RAPID_METRIC("rapid.parallel.fallbacks",
                 "Parallel tasks that fell back to serial execution.",
                 query_parallel_fallbacks_total),

    RAPID_METRIC("rapid.gc.runs", "Garbage collection passes executed.",
                 gc_total_runs),
    RAPID_METRIC("rapid.gc.purged_versions",
                 "Versions reclaimed by garbage collection.",
                 gc_total_purged_versions),
};
// clang-format on

#undef RAPID_METRIC

PSI_meter_info_v1 rapid_meter = {"rapid",
                                 "ShannonBase Rapid engine kernel metrics. Per-operator counters that used to "
                                 "be reachable only from a debug build; exposed through the telemetry "
                                 "metrics service instead of SHOW GLOBAL STATUS.",
                                 0, /* m_frequency, the metrics are polled by the exporter */
                                 0, /* m_flags */
                                 0, /* m_key, assigned by performance_schema on registration */
                                 rapid_metrics,
                                 static_cast<unsigned int>(sizeof(rapid_metrics) / sizeof(rapid_metrics[0]))};

/** True from a successful registration until the matching unregistration. */
std::atomic<bool> rapid_metrics_registered{false};

/**
 * Set by the first attempt made after the server is up. performance_schema
 * cannot be enabled at runtime, so a failure here is final and there is no
 * point trying again on every statement.
 */
std::atomic<bool> rapid_registration_settled{false};

bool do_register_rapid_metrics() {
  SERVICE_TYPE(registry) *plugin_registry = mysql_plugin_registry_acquire();
  if (plugin_registry == nullptr) return false;

  my_service<SERVICE_TYPE(psi_metric_v1)> metric_service("psi_metric_v1", plugin_registry);
  if (!metric_service.is_valid()) {
    mysql_plugin_registry_release(plugin_registry);
    return false;
  }

  metric_service->register_meters(&rapid_meter, 1);
  mysql_plugin_registry_release(plugin_registry);
  rapid_metrics_registered = true;
  return true;
}

}  // namespace

void ensure_rapid_metrics() {
  // The common case once the meter is up: one plain bool read per statement.
  if (rapid_metrics_registered) return;
  if (rapid_registration_settled.exchange(true)) return;

  if (!do_register_rapid_metrics())
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
           "rapid: psi_metric_v1 unavailable (is performance_schema disabled?), "
           "kernel metrics are not exported.");
}

void unregister_rapid_metrics() {
  if (!rapid_metrics_registered) return;
  rapid_metrics_registered = false;

  SERVICE_TYPE(registry) *plugin_registry = mysql_plugin_registry_acquire();
  if (plugin_registry == nullptr) return;

  my_service<SERVICE_TYPE(psi_metric_v1)> metric_service("psi_metric_v1", plugin_registry);
  if (metric_service.is_valid()) metric_service->unregister_meters(&rapid_meter, 1);

  mysql_plugin_registry_release(plugin_registry);
}

}  // namespace RapidMonitor
}  // namespace ShannonBase
