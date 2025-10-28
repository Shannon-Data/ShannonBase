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
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {

struct RpdEngineConfig {
  // IMCU Configuration
  size_t rows_per_imcu = SHANNON_ROWS_IN_CHUNK;             // Number of rows per IMCU
  size_t max_imcu_size_mb = SHANNON_MAX_IMCU_MEMRORY_SIZE;  // Maximum IMCU size (MB)

  // Compression Configuration
  Compress::Compression_level compression_level = Compress::MEDIUM;
  bool enable_dictionary_encoding = true;
  size_t dictionary_max_size = 65536;  // Maximum dictionary entries

  // Memory Configuration
  size_t memory_pool_size_mb = SHANNON_DEFAULT_MEMRORY_SIZE;  // Memory pool size (MB)
  size_t max_memory_usage_mb = SHANNON_DEFAULT_MEMRORY_SIZE;  // Maximum memory usage (MB)

  // Concurrency Configuration
  size_t max_concurrent_transactions = 1000;
  size_t background_worker_threads = 4;

  // GC Configuration
  size_t gc_interval_seconds = 60;          // GC interval (seconds)
  double gc_version_ratio_threshold = 2.0;  // Trigger GC when version/row ratio > 2.0
  size_t gc_min_version_count = 10000;      // Minimum version count to trigger GC

  // Compaction Configuration
  double compact_delete_ratio_threshold = 0.3;  // Trigger compaction when delete ratio > 30%
  size_t compact_min_delete_count = 10000;      // Minimum delete count to trigger compaction
  size_t compact_interval_seconds = 300;        // Compaction interval (seconds)

  // Query Configuration
  size_t scan_batch_size = SHANNON_BATCH_NUM;  // Scan batch size
  bool enable_storage_index = true;            // Enable Storage Index
  bool enable_predicate_pushdown = true;       // Enable predicate pushdown

  // Statistics Configuration
  size_t stats_update_interval_seconds = 300;  // Statistics update interval (seconds)
  bool auto_update_stats = true;               // Auto-update statistics

  // Debug Configuration
  bool enable_detailed_logging = false;
  bool enable_performance_counters = true;
};
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_OBJECT_H__