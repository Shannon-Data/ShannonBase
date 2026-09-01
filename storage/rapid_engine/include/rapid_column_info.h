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
#include <cstdint>
#include <vector>

#include "include/mysql_com.h"  // NAME_LEN

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

using rpd_column_info_t = shannon_rpd_column_info_t;
using rpd_columns_container = std::vector<rpd_column_info_t>;

// all column infos of all loaded tables, which's used for
// performance_schema.rpd_column_xxx.
extern rpd_columns_container shannon_rpd_columns_info;
extern std::mutex shannon_rpd_columns_mutex;

// Consistent copy of shannon_rpd_columns_info taken under
// shannon_rpd_columns_mutex.  performance_schema readers must use this: the
// vector is mutated by concurrent SECONDARY_LOAD / SECONDARY_UNLOAD, so
// indexing it directly across a scan can outlive a reallocation.
rpd_columns_container rpd_columns_snapshot();

// Locked element count, for the perfschema share's static row-count estimate.
size_t rpd_columns_count();

// Per-column statistics that only exist once data is in the IMCS, so they
// cannot be captured at load time the way schema/table/column names are.
struct rpd_column_live_stats_t {
  uint64_t ndv{0};
  uint64_t dict_size_bytes{0};
  uint64_t avg_byte_width{0};
};

// Fills @p out with the live statistics for (table_id, column_id).  Returns
// false when the table is not currently resident in the IMCS, leaving @p out
// untouched.  Computing dict_size_bytes walks every IMCU of the table, so
// callers that do not need it (rpd_preload_stats) pass want_dict_size=false to
// keep a scan from going quadratic in columns x IMCUs.
bool rapid_column_live_stats(uint table_id, uint column_id, rpd_column_live_stats_t *out, bool want_dict_size = true);
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_H__