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

#include <algorithm>
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
#include "sql/log.h"             // sql_print_error
#include "sql/sql_class.h"       // THD

#include "storage/rapid_engine/utils/utils.h"  // Util::open_table_by_name / close_table

namespace ShannonBase {
namespace Recovery {
/** Return true for schemas that should never have their flag touched. */
static bool is_system_schema(const std::string &name) {
  return (name == "mysql" || name == "information_schema" || name == "performance_schema" || name == "sys");
}

/**
 * @brief Check whether opts contains exactly "secondary_load=1".
 *
 * The DD stores options as a ';'-separated key=value string. A plain find()
 * for "secondary_load=1" also matches "secondary_load=10" and
 * "foo_secondary_load=1"; require the key to start at the beginning of the
 * string or after a separator, and the value to end at a separator or at the
 * end of the string.
 */
static bool has_secondary_load_flag(const std::string &opts) {
  static const std::string TOKEN = "secondary_load=1";
  for (size_t pos = opts.find(TOKEN); pos != std::string::npos; pos = opts.find(TOKEN, pos + 1)) {
    const size_t end = pos + TOKEN.size();
    const bool starts_key = (pos == 0 || opts[pos - 1] == ';');
    if (starts_key && (end == opts.size() || opts[end] == ';')) return true;
  }
  return false;
}

int LoadFlagManager::query_loaded_tables(THD *thd, std::vector<SecondaryLoadedTable> &out, bool *incomplete) {
  out.clear();
  if (incomplete) *incomplete = false;

  dd::cache::Dictionary_client *client = thd->dd_client();
  if (!client) return HA_ERR_GENERIC;
  dd::cache::Dictionary_client::Auto_releaser releaser(client);

  std::vector<dd::String_type> schema_names;
  if (client->fetch_global_component_names<dd::Schema>(&schema_names)) {
    sql_print_error("LoadFlagManager: cannot list the schemas of the data dictionary; no table can be recovered");
    return HA_ERR_GENERIC;
  }

  for (const auto &schema_name_raw : schema_names) {
    std::string schema_name(schema_name_raw.c_str());
    if (is_system_schema(schema_name)) continue;

    // Release the DD objects fetched for this schema before moving to the next
    // one; with a single releaser for the whole walk every dd::Table of every
    // schema stays pinned in the DD cache until the function returns.
    dd::cache::Dictionary_client::Auto_releaser schema_releaser(client);

    // Each of the three skips below silently drops every loaded table in that
    // schema: they never come back, and the caller cannot tell that apart from
    // a schema that simply had none. Recovery still continues -- one bad schema
    // must not strand the rest -- but the omission is named.
    dd::Schema_MDL_locker mdl_locker(thd);
    if (mdl_locker.ensure_locked(schema_name.c_str())) {
      sql_print_error("LoadFlagManager: cannot lock schema '%s'; its loaded tables will not be recovered",
                      schema_name.c_str());
      if (incomplete) *incomplete = true;
      continue;
    }
    const dd::Schema *schema_ptr = nullptr;
    if (client->acquire(schema_name.c_str(), &schema_ptr) || !schema_ptr) {
      sql_print_error("LoadFlagManager: cannot acquire schema '%s'; its loaded tables will not be recovered",
                      schema_name.c_str());
      if (incomplete) *incomplete = true;
      continue;
    }

    std::vector<const dd::Table *> tables;
    if (client->fetch_schema_components<dd::Table>(schema_ptr, &tables)) {
      sql_print_error("LoadFlagManager: cannot list tables of schema '%s'; its loaded tables will not be recovered",
                      schema_name.c_str());
      if (incomplete) *incomplete = true;
      continue;
    }

    for (const dd::Table *table_ptr : tables) {
      if (!table_ptr) continue;

      std::string opts(table_ptr->options().raw_string().c_str(), table_ptr->options().raw_string().length());
      std::transform(opts.begin(), opts.end(), opts.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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