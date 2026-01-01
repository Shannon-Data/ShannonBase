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

#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
class RapidShare;
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

struct logical_part_loaded_t {
  uint id;
  std::string name;
  uint64_t load_scn;
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

  // The timestamp of the last query that referenced the table.
  std::chrono::system_clock::time_point last_queried;

  // The load start/end timestamp for the table.
  std::chrono::system_clock::time_point load_start_stamp;
  std::chrono::system_clock::time_point load_end_stamp;

  // Indicates the source of the last successful recovery for a table.
  recovery_source_t recovery_source{recovery_source_t::OBJECT_STORAGE};

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
  uint tid;

  std::string schema_name, table_name, secondary_engine;

  uint64_t estimated_size{0};

  bool partitioned{false};

  bool excluded_from_self_load{false};

  table_access_stats_t stats;

  rpd_table_meta_info_t meta_info;

  std::string full_name() const { return schema_name + ":" + table_name; }
};

// Map from (db_name, table_name) to the RapidShare with table state.
class LoadedTables {
  // "db:table" <-> Share.
  std::map<std::string, RapidShare *> m_tables;
  mutable std::mutex m_mutex;

 public:
  LoadedTables() = default;
  virtual ~LoadedTables();

  void add(const std::string &db, const std::string &table, RapidShare *rs);

  RapidShare *get(const std::string &db, const std::string &table);

  void erase(const std::string &db, const std::string &table);

  auto size() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_tables.size();
  }

  void table_infos(uint index, ulonglong &tid, std::string &schema, std::string &table);
};

// all the loaded tables information.
extern LoadedTables *shannon_loaded_tables;

extern Autopilot::SelfLoadManager *shannon_self_load_mgr_inst;

// the max memory size of rpd engine, initialized in xx_rapid.cc
extern uint64 shannon_rpd_mem_sz_max;

extern std::atomic<size_t> shannon_rpd_allocated_mem_size;
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_LOADED_TABLE_INFO_H__