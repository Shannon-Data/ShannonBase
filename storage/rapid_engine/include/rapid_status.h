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
class handlerton;
namespace ShannonBase {
class LoadedTables;
class RapidShare;
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

// Map from (db_name, table_name) to the RapidShare with table state.
class LoadedTables {
  std::map<std::string, RapidShare *> m_tables;
  std::mutex m_mutex;

 public:
  void add(const std::string &db, const std::string &table, RapidShare *rs);

  RapidShare *get(const std::string &db, const std::string &table);

  void erase(const std::string &db, const std::string &table);

  auto size() const { return m_tables.size(); }

  void table_infos(uint index, ulonglong &tid, std::string &schema, std::string &table);
};

// all the loaded tables information.
extern LoadedTables *shannon_loaded_tables;

// all column infos of all loaded tables, which's used for
// performance_schema.rpd_column_xxx.
extern rpd_columns_container rpd_columns_info;

// the max memory size of rpd engine, initialized in xx_rapid.cc
extern uint64 rpd_mem_sz_max;

extern std::atomic<size_t> rapid_allocated_mem_size;
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_H__