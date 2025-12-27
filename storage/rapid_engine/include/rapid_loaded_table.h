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
#include <string>

namespace ShannonBase {
class RapidShare;
namespace Autopilot {
class SelfLoadManager;
}

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
extern Autopilot::SelfLoadManager *self_load_mngr_inst;
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_STATS_LOADED_TABLE_INFO_H__