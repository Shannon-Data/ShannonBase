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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/

#include "storage/rapid_engine/recovery/recovery_load.h"

#include <cctype>
#include <unordered_map>

#include "include/my_dbug.h"
#include "include/my_inttypes.h"
#include "include/mysql/components/services/log_builtins.h"  // LogErr
#include "mysql/strings/m_ctype.h"                           // system_charset_info
#include "sql/dd/cache/dictionary_client.h"                  // dd::cache::Dictionary_client
#include "sql/dd/dd_kill_immunizer.h"                        // dd::DD_kill_immunizer
#include "sql/dd/dd_schema.h"                                // dd::Schema
#include "sql/dd/impl/utils.h"
#include "sql/dd/properties.h"   // dd::Properties
#include "sql/dd/string_type.h"  // dd::String_type
#include "sql/dd/types/table.h"  // dd::Table
#include "sql/handler.h"         // HA_ERR_*, handler::NONE, store_record()
#include "sql/sql_class.h"       // THD

#include "storage/rapid_engine/utils/utils.h"  // Util::open_table_by_name / close_table

namespace ShannonBase {
namespace Recovery {
// RAII guard: temporarily disable binlog for system table updates
// When updating mysql.tables (a system/metadata table), we must disable binlog
// recording because:
//   1. metadata changes should not be replicated (they are schema-dependent)
//   2. calling ha_update_row on system tables from certain contexts (e.g.,
//      DROP DATABASE) can trigger binlog assert failures if binlog state is
//      inconsistent.
//
// This guard saves THD::variables.option_bits, clears OPTION_BIN_LOG, and
// restores the original value on destruction.
static constexpr uint kTablesSchemaId = 1;  // schema_id (FK → mysql.schemata.id)
static constexpr uint kTablesName = 2;      // name      (table name)
static constexpr uint kTablesOptions = 10;  // options   (key=value string)

// mysql.schemata
static constexpr uint kSchemataId = 0;    // id
static constexpr uint kSchemataName = 2;  // name
/** Return true for schemas that should never have their flag touched. */
static bool is_system_schema(const std::string &name) {
  return (name == "mysql" || name == "information_schema" || name == "performance_schema" || name == "sys");
}

/**
 * @brief Check whether opts contains exactly "secondary_load=1".
 *
 * Guards against false positives such as "secondary_load=10".
 */
static bool has_secondary_load_flag(const std::string &opts) {
  static const char TOKEN[] = "secondary_load=1";
  return opts.find(TOKEN) != std::string::npos;
}

int LoadFlagManager::query_loaded_tables(THD *thd, std::vector<SecondaryLoadedTable> &out) {
  out.clear();

  dd::cache::Dictionary_client *client = thd->dd_client();
  if (!client) return HA_ERR_GENERIC;
  dd::cache::Dictionary_client::Auto_releaser releaser(client);

  std::vector<dd::String_type> schema_names;
  if (client->fetch_global_component_names<dd::Schema>(&schema_names)) {
    return HA_ERR_GENERIC;
  }

  for (const auto &schema_name_raw : schema_names) {
    std::string schema_name(schema_name_raw.c_str());
    if (is_system_schema(schema_name)) continue;

    dd::Schema_MDL_locker mdl_locker(thd);
    if (mdl_locker.ensure_locked(schema_name.c_str())) continue;
    const dd::Schema *schema_ptr = nullptr;
    if (client->acquire(schema_name.c_str(), &schema_ptr) || !schema_ptr) continue;

    std::vector<const dd::Table *> tables;
    if (client->fetch_schema_components<dd::Table>(schema_ptr, &tables)) continue;

    for (const dd::Table *table_ptr : tables) {
      if (!table_ptr) continue;

      std::string opts(table_ptr->options().raw_string().c_str(), table_ptr->options().raw_string().length());
      std::transform(opts.begin(), opts.end(), opts.begin(), ::tolower);
      if (has_secondary_load_flag(opts)) {
        SecondaryLoadedTable entry;
        entry.schema_name = schema_name;
        entry.table_name = table_ptr->name().c_str();
        entry.is_partitioned = (table_ptr->partition_type() != dd::Table::PT_NONE);

        DBUG_PRINT("recovery", ("Found table: %s.%s, partitioned: %d", entry.schema_name.c_str(),
                                entry.table_name.c_str(), entry.is_partitioned));
        out.push_back(std::move(entry));
      }
    }
  }

  DBUG_PRINT("recovery", ("LoadFlagManager::query_loaded_tables: found %zu tables", out.size()));
  return 0;
}

int LoadFlagManager::is_table_flagged(THD *thd, const std::string &schema_name, const std::string &table_name,
                                      bool &loaded) {
  loaded = false;

  std::vector<SecondaryLoadedTable> tables;
  if (int r = query_loaded_tables(thd, tables); r != 0) return r;

  for (const auto &t : tables) {
    if (t.schema_name == schema_name && t.table_name == table_name) {
      loaded = true;
      break;
    }
  }
  return 0;
}
}  // namespace Recovery
}  // namespace ShannonBase