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
#ifndef __SHANNONBASE_RPD_STATS_LOADED_TABLE_INFO_H__
#define __SHANNONBASE_RPD_STATS_LOADED_TABLE_INFO_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#include "storage/rapid_engine/include/rapid_arch_inf.h"

namespace ShannonBase {
struct RapidShare;
namespace Autopilot {
class SelfLoadManager;
}

enum class pool_type_t { SNAPSHOT, TRANSACTIONAL, VOLATILE };

enum class load_type_t { USER, SELF };

enum class load_status_t {
  NOLOAD_RPDGSTABSTATE,
  LOADING_RPDGSTABSTATE,
  AVAIL_RPDGSTABSTATE,
  UNLOADING_RPDGSTABSTATE,
  INRECOVERY_RPDGSTABSTATE,
  STALE_RPDGSTABSTATE,
  UNAVAIL_RPDGSTABSTATE,
  RECOVERYFAILED_RPDGSTABSTATE
};

enum class recovery_source_t { MYSQL_INNODB, OBJECT_STORAGE };

// Reason a table became stale, mirroring HeatWave's rpd_tables.STALE_REASON.
// OK is the steady state; the remaining values are kept in the documented
// order so the SQL ENUM ordinal matches the C++ enumerator.
enum class stale_reason_t {
  OK,
  ERROR_CLUSTER_OOM,
  ERROR_PKEY_NOT_FOUND,
  ERROR_UU_OVERLAP,
  ERROR_INVALID_CP_PACKET,
  ERROR_UU_TOO_LARGE,
  DECOMPRESSION_TIMEOUT,
  ERROR_CLUSTER_OTHER,
  TABLE_TYPE_MISMATCH,
  RPD_CP_RESET,
  DROP_PARTITION_FAILED,
  ADD_PARTITION_FAILED,
  ALTER_PARTITION_FAILED,
  PARTITION_UNLOAD_FAILED,
  PARTITION_LOAD_FAILED,
  RELOAD_REQUIRED,
  RPD_PARSING_OOM,
  RPD_PARSER_ERROR,
  UNIDENTIFIED_ERROR
};

struct logical_part_loaded_t {
  uint id{0};
  std::string name;
  uint64_t load_scn{0};
  load_type_t load_type{load_type_t::USER};
};

struct rpd_table_meta_info_t {
  // The system change number (SCN) of the table snapshot. and The SCN up to which changes are persisted.
  uint64_t snapshot_scn{0}, persisted_scn{0};

  // The load pool type of the table.
  pool_type_t pool_type{pool_type_t::SNAPSHOT};

  // The data placement type.
  int data_placement_type{0};

  // The number of rows that are loaded for the table. The value is set initially when the table is loaded, and updated
  // as changes are propagated.
  uint64 nrows{0};

  // The load status of the table.
  load_status_t load_status{load_status_t::NOLOAD_RPDGSTABSTATE};

  // The load progress of the table expressed as a percentage value.
  // 10%: the initialization phase is complete.
  // 10-70%: the transformation to native IMCS format is in progress.
  // 70% - 80%: the transformation to native IMCS format is complete and the aggregation phase is in progress.
  // 80-99%: the recovery phase is in progress.
  // 100%: data load is complete.
  double loading_progress{0.0};

  // The amount of data loaded for the table, in bytes. and The total size of raw Lakehouse data transformed, in bytes.
  uint64 size_bytes{0}, transformation_bytes{0};

  // The number of queries that referenced the table.
  int query_cnt{0};

  // Value of the innodb_parallel_read_threads session variable in effect
  // when the table was (last) loaded, mirroring HeatWave's rpd_mirror
  // RECOMMENDED_READ_THREADS column: the thread count a reload should reuse.
  int recommended_read_threads{0};

  // The timestamp of the last query that referenced the table.
  std::chrono::system_clock::time_point last_queried;

  // The load start/end timestamp for the table.
  std::chrono::system_clock::time_point load_start_stamp;
  std::chrono::system_clock::time_point load_end_stamp;

  // Indicates the source of the last successful recovery for a table.
  recovery_source_t recovery_source{recovery_source_t::MYSQL_INNODB};

  // The timestamp when the latest successful recovery started/ended.
  std::chrono::system_clock::time_point recovery_start_stamp;
  std::chrono::system_clock::time_point recovery_end_stamp;

  // Specifies whether the table is automatically loaded.
  load_type_t load_type{load_type_t::USER};

  // For partitioned tables, contains an array of objects
  std::vector<logical_part_loaded_t> logical_part_loaded_at_scn;

  // Contains a list of IDs that correspond to the columns on which zone maps are automatically built.
  std::vector<int> auto_zmp_columns;

  // Advanced Cardinality Estimation (ACE) statistics model is currently associated with the given table
  bool ace_model{false};

  // Why the table went STALE_RPDGSTABSTATE.  Meaningful only while
  // load_status is STALE_RPDGSTABSTATE; OK otherwise.
  stale_reason_t stale_reason{stale_reason_t::OK};
};

struct SHANNON_ALIGNAS table_access_stats_t {
  std::atomic<uint64_t> mysql_access_count{0};
  std::atomic<uint64_t> heatwave_access_count{0};

  std::atomic<double> importance{1.0};

  std::chrono::system_clock::time_point last_queried_time;
  std::chrono::system_clock::time_point last_queried_time_in_rpd;

  enum State { NOT_LOADED = 0, LOADED, INSUFFICIENT_MEMORY } state{NOT_LOADED};

  std::shared_mutex stats_mutex;
  table_access_stats_t()
      : last_queried_time(std::chrono::system_clock::now()),
        last_queried_time_in_rpd(std::chrono::system_clock::now()) {}
};

struct SHANNON_ALIGNAS TableInfo {
  uint tid{0};

  std::string schema_name, table_name, secondary_engine;

  uint64_t estimated_size{0};

  bool partitioned{false};

  bool excluded_from_self_load{false};

  table_access_stats_t stats;

  rpd_table_meta_info_t meta_info;

  // Names of InnoDB partitions that queries have actually touched (after
  // partition pruning), mirroring HeatWave's rpd_mirror QUERIED_PARTITIONS
  // column. Guarded by stats.stats_mutex. Empty for non-partitioned tables.
  std::set<std::string> queried_partitions;

  std::string full_name() const { return schema_name + "." + table_name; }
};

// Immutable value copy of one RPD Mirror entry.  performance_schema readers
// (rpd_tables, rpd_mirror) iterate a vector of these instead of borrowing
// TableInfo* out of the registry: the registry can concurrently unload a
// table and free the TableInfo while a scan is still in flight.
struct TableInfoSnapshot {
  uint tid{0};
  std::string schema_name, table_name;

  // Copied out of table_access_stats_t (whose atomics/mutex make it non-copyable).
  uint64_t mysql_access_count{0};
  uint64_t heatwave_access_count{0};
  double importance{1.0};
  std::chrono::system_clock::time_point last_queried_time;
  std::chrono::system_clock::time_point last_queried_time_in_rpd;
  table_access_stats_t::State state{table_access_stats_t::NOT_LOADED};

  std::set<std::string> queried_partitions;
  rpd_table_meta_info_t meta_info;
};

// Structured key identifying a loaded table. Avoids stringifying "db.table"
// and then re-parsing the components back out of a delimiter-joined string.
struct TableKey {
  std::string schema;
  std::string table;

  bool operator==(const TableKey &rhs) const { return schema == rhs.schema && table == rhs.table; }
  bool operator<(const TableKey &rhs) const { return std::tie(schema, table) < std::tie(rhs.schema, rhs.table); }
};

// Immutable metadata snapshot of a single loaded table, used by monitors and
// SHOW STATUS. Callers consume this outside the registry lock.
struct LoadedTableInfo {
  ulonglong table_id{0};
  std::string schema;
  std::string table;
};

// Registry of tables loaded into the Rapid engine.
//
// Ownership: the registry owns each RapidShare through std::shared_ptr. A
// caller that obtains a SharePtr from get() keeps the share alive even if
// another thread concurrently erases it from the registry. The shared lock
// only serializes map access; it does not need to protect object lifetime.
class LoadedTables {
 public:
  using SharePtr = std::shared_ptr<RapidShare>;

  LoadedTables() = default;
  LoadedTables(const LoadedTables &) = delete;
  LoadedTables &operator=(const LoadedTables &) = delete;

  // Inserts or replaces the entry for (db, table). Ownership is shared with
  // the registry; the previous entry (if any) is released.
  void add(std::string db, std::string table, SharePtr share);

  // Returns the share for (db, table), or nullptr if it is not loaded.
  [[nodiscard]] SharePtr get(const std::string &db, const std::string &table) const;

  void erase(const std::string &db, const std::string &table);

  [[nodiscard]] size_t size() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_tables.size();
  }

  // Returns a consistent copy of all loaded-table metadata.
  [[nodiscard]] std::vector<LoadedTableInfo> snapshot() const;

 private:
  std::map<TableKey, SharePtr> m_tables;
  mutable std::shared_mutex m_mutex;
};
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_LOADED_TABLE_INFO_H__