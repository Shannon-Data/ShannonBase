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
#ifndef __SHANNONBASE_RPD_STATS_H__
#define __SHANNONBASE_RPD_STATS_H__

#include <chrono>
#include <vector>

#include "include/mysql_com.h"  // NAME_LEN

class handlerton;
namespace ShannonBase {
class LoadedTables;
// All the stats of loaded table of rapid.
struct shannon_rpd_column_info_t {
  shannon_rpd_column_info_t() {
    table_id = 0;
    column_id = 0;
    ndv = 0;
    data_placement_index = 0;
    data_dict_bytes = 0;
    avg_byte_width_inc_null = 0;
  }

  /**schema name*/
  char schema_name[NAME_LEN] = {0};
  /**table id of loaded table*/
  uint table_id{0};
  /**table_name loaded into rapid*/
  char table_name[NAME_LEN] = {0};
  /**cloumn name with charset info*/
  char column_name[NAME_LEN] = {0};
  /**columun id of loaded table*/
  uint column_id{0};
  /**The number of distinct values in the column.*/
  longlong ndv{0};
  /**The type of encoding used.*/
  char encoding[NAME_LEN] = {0};
  /**data placement index*/
  uint data_placement_index{0};
  /**The dictionary size per column, in bytes.*/
  longlong data_dict_bytes{0};
  /**avg width byte.*/
  uint32 avg_byte_width_inc_null{0};
};

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
  uint tid;

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

using rpd_column_info_t = shannon_rpd_column_info_t;
using rpd_columns_container = std::vector<rpd_column_info_t>;

// all column infos of all loaded tables, which's used for
// performance_schema.rpd_column_xxx.
extern rpd_columns_container rpd_columns_info;

// the max memory size of rpd engine, initialized in xx_rapid.cc
extern uint64 rpd_mem_sz_max;

extern std::atomic<size_t> rapid_allocated_mem_size;

extern std::map<int, rpd_table_meta_info_t> shannon_loading_tables_meta;
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_H__