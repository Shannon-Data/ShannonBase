/*
   Copyright (c) 2014, 2023, Oracle and/or its affiliates.

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

   Shannon Data AI.
*/
#ifndef __SHANNONBASE_RPD_CONFIG_H__
#define __SHANNONBASE_RPD_CONFIG_H__
#include <cstring>
#include "storage/rapid_engine/autopilot/loader.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_table_info.h"
#include "storage/rapid_engine/populate/log_commons.h"

namespace ShannonBase {
struct SHANNON_ALIGNAS RpdEngineConfig {
  // IMCU Configuration
  size_t rows_per_imcu{SHANNON_ROWS_IN_CHUNK};  // Number of rows per IMCU

  // Compression Configuration
  Compress::COMPRESS_LEVEL compression_level{Compress::COMPRESS_LEVEL::DEFAULT};
  bool enable_dictionary_encoding{true};
  size_t dictionary_max_size{65536};  // Maximum dictionary entries

  // Memory Configuration
  uint64 memory_pool_size_mb{SHANNON_DEFAULT_MEMRORY_SIZE};  // Memory pool size (MB)
  uint64 max_memory_usage_mb{SHANNON_DEFAULT_MEMRORY_SIZE};  // Maximum memory usage (MB)

  // Propagation Configuration
  ulonglong pop_buff_sz_max{ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE};

  // Rapid change propagation mode, including DIRECT_NOTIFICATION, REDO_LOG_PARSE, HYBRID.
  long unsigned int propagate_mode{static_cast<int>(ShannonBase::Populate::PropagateMode::DIRECT_NOTIFICATION)};

  // Parallel loading threshold Configuration
  ulonglong para_load_threshold{ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD};
  ulonglong para_parttb_load_threshold{ShannonBase::SHANNON_PARALLEL_PARTTB_THRESHOLD};

  // rapid async parsing column number threshold, to accelerate large column number table scan.
  int async_column_threshold{ShannonBase::DEFAULT_N_FIELD_PARALLEL};

  // self-load related parameters.
  bool self_load_enabled{false};
  ulonglong self_load_interval_sec{SHANNON_DEFAULT_SELF_LOAD_INTERVAL};  // 24hurs
  bool self_load_skip_quiet_check{false};
  int self_load_base_relation_fill_percentage{SHANNON_DEFAULT_SELF_LOAD_FILL_PERCENTAGE};  // percentage.

  // Concurrency Configuration
  size_t max_concurrent_transactions{1000};
  size_t background_worker_threads{3};

  // GC Configuration
  ulonglong gc_interval_seconds{ShannonBase::SHANNON_DEFAULT_GC_INTERVAL_TIME};  // GC interval (seconds)
  ulonglong gc_batch_size{SHANNON_DEFAULT_MAX_PURGER_TIMEOUT};
  ulonglong gc_interval_scn{ShannonBase::SHANNON_DEFAULT_GC_INTERVAL_SCN};
  double gc_version_ratio_threshold{
      SHANNON_DEFAULT_PURGE_EFFICIENCY_THRESHOLD};               // Trigger GC when version/row ratio >
                                                                 // SHANNON_DEFAULT_PURGE_EFFICIENCY_THRESHOLD
  ulonglong gc_min_version{SHANNON_DEFAULT_MAX_PURGER_TIMEOUT};  // Minimum version to trigger GC

  // Compaction Configuration
  double compact_delete_ratio_threshold{0.3};  // Trigger compaction when delete ratio > 30%
  size_t compact_min_delete_count{10000};      // Minimum delete count to trigger compaction
  size_t compact_interval_seconds{300};        // Compaction interval (seconds)

  // Query Configuration
  size_t scan_batch_size{SHANNON_BATCH_NUM};  // Scan batch size
  bool enable_storage_index{true};            // Enable Storage Index
  bool enable_predicate_pushdown{true};       // Enable predicate pushdown

  // Statistics Configuration
  size_t stats_update_interval_seconds{300};  // Statistics update interval (seconds)
  bool auto_update_stats{true};               // Auto-update statistics

  bool enable_vectorized{true};  // enable vecotrized feature.

  // Debug Configuration
  bool enable_detailed_logging{false};
  bool enable_performance_counters{true};

  inline static RpdEngineConfig &Configuration() {
    static std::unique_ptr<RpdEngineConfig> instance = std::make_unique<RpdEngineConfig>();
    return *instance;
  }
};

// shannonbase engine configuration.
extern RpdEngineConfig shannon_rpd_engine_cfg;

// all the loaded tables information.
extern LoadedTables *shannon_loaded_tables;

extern Autopilot::SelfLoadManager *shannon_self_load_mgr_inst;
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_OBJECT_H__