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

#include "storage/rapid_engine/recovery/load_persist.h"

#include <cctype>
#include <unordered_map>

#include "include/my_dbug.h"
#include "include/my_inttypes.h"
#include "include/mysql/components/services/log_builtins.h"  // LogErr
#include "mysql/strings/m_ctype.h"                           // system_charset_info
#include "sql/handler.h"                                     // HA_ERR_*, handler::NONE, store_record()
#include "sql/sql_class.h"                                   // THD
#include "sql/table.h"                                       // TABLE, Field

#include "storage/rapid_engine/utils/utils.h"  // Util::open_table_by_name / close_table
/**
 * implementation: secondary_load flag persistence.
 */
namespace ShannonBase {
namespace Recovery {
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
  auto pos = opts.find(TOKEN);
  if (pos == std::string::npos) return false;
  size_t after = pos + sizeof(TOKEN) - 1 /* '\0' */;
  if (after < opts.size() && std::isdigit(static_cast<unsigned char>(opts[after]))) return false;
  return true;
}

/**
 * @brief Strip the existing secondary_load token and re-add it when needed.
 *
 * The options column holds a space-separated "key=value" string, e.g.:
 *   "secondary_engine=rapid secondary_load=1 partitioned"
 *
 * @param current_opts  Current value of the options field.
 * @param set_loaded    true  → write "secondary_load=1".
 *                      false → remove the token entirely.
 * @return Updated options string.
 */
static std::string rebuild_options(const std::string &current_opts, bool set_loaded) {
  static const std::string TOKEN_PREFIX = "secondary_load=";

  std::string opts = current_opts;

  // Always strip the existing token first.
  auto pos = opts.find(TOKEN_PREFIX);
  if (pos != std::string::npos) {
    auto end_pos = opts.find(' ', pos);
    if (end_pos == std::string::npos) {
      // Token is at the tail; eat a preceding space as well.
      if (pos > 0 && opts[pos - 1] == ' ') --pos;
      opts.erase(pos);
    } else {
      // Token is in the middle; remove it plus the trailing space.
      opts.erase(pos, end_pos - pos + 1 /* include space */);
    }
  }

  // Trim trailing whitespace left by the erase above.
  while (!opts.empty() && opts.back() == ' ') opts.pop_back();

  if (set_loaded) {
    if (!opts.empty()) opts += ' ';
    opts += "secondary_load=1";
  }

  return opts;
}

/**
 * @brief Scan mysql.schemata and build a schema_id → schema_name map.
 *
 * Follows the same pattern as SelfLoadManager::load_mysql_schema_info().
 */
static int build_schema_id_map(THD *thd, std::unordered_map<longlong, std::string> &out) {
  DBUG_TRACE;

  TABLE *schemata = ShannonBase::Utils::Util::open_table_by_name(thd, "mysql", "schemata", TL_READ_WITH_SHARED_LOCKS);
  if (!schemata) {
    // open_table_by_name already emits a warning.
    ShannonBase::Utils::Util::close_table(thd, schemata);
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "rapid_load_persist: cannot open mysql.schemata");
    return HA_ERR_GENERIC;
  }

  if (schemata->file->inited == handler::NONE && schemata->file->ha_rnd_init(true)) {
    ShannonBase::Utils::Util::close_table(thd, schemata);
    return HA_ERR_GENERIC;
  }

  ShannonBase::Utils::ColumnMapGuard guard(schemata, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

  int tmp;
  while ((tmp = schemata->file->ha_rnd_next(schemata->record[0])) != HA_ERR_END_OF_FILE) {
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    longlong id = (*(schemata->field + kSchemataId))->val_int();

    String buf;
    std::string name = (*(schemata->field + kSchemataName))->val_str(&buf)->c_ptr();

    out.emplace(id, std::move(name));
  }

  schemata->file->ha_rnd_end();
  ShannonBase::Utils::Util::close_table(thd, schemata);
  return 0;
}

int LoadFlagManager::set_flag(THD *thd, const std::string &schema_name, const std::string &table_name, bool loaded) {
  DBUG_TRACE;
  DBUG_PRINT("recovery",
             ("LoadFlagManager::set_flag %s.%s loaded=%d", schema_name.c_str(), table_name.c_str(), loaded ? 1 : 0));

  std::unordered_map<longlong, std::string> schema_map;
  if (int r = build_schema_id_map(thd, schema_map); r != 0) return r;

  longlong target_schema_id = -1;
  for (const auto &[id, name] : schema_map) {
    if (name == schema_name) {
      target_schema_id = id;
      break;
    }
  }
  if (target_schema_id < 0) {
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: schema '%s' not found in schemata",
           schema_name.c_str());
    return HA_ERR_GENERIC;
  }

  TABLE *cat = ShannonBase::Utils::Util::open_table_by_name(thd, "mysql", "tables", TL_WRITE);
  if (!cat) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: cannot open mysql.tables");
    return HA_ERR_GENERIC;
  }

  if (cat->file->inited == handler::NONE && cat->file->ha_rnd_init(true)) {
    ShannonBase::Utils::Util::close_table(thd, cat);
    return HA_ERR_GENERIC;
  }

  // TYPE::ALL populates both read_set and write_set – required for ha_update_row.
  ShannonBase::Utils::ColumnMapGuard guard(cat, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);

  int tmp;
  int ret = HA_ERR_GENERIC;  // updated to 0 on success

  while ((tmp = cat->file->ha_rnd_next(cat->record[0])) != HA_ERR_END_OF_FILE) {
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    // Filter by schema_id
    longlong row_sch_id = (*(cat->field + kTablesSchemaId))->val_int();
    if (row_sch_id != target_schema_id) continue;

    // Filter by table name
    String name_buf;
    std::string row_name = (*(cat->field + kTablesName))->val_str(&name_buf)->c_ptr();
    if (row_name != table_name) continue;

    String opt_buf;
    std::string current_opts = (*(cat->field + kTablesOptions))->val_str(&opt_buf)->c_ptr();
    std::string new_opts = rebuild_options(current_opts, loaded);

    // store_record copies record[0] → record[1] (old image).
    store_record(cat, record[1]);

    Field *opt_fld = *(cat->field + kTablesOptions);
    opt_fld->store(new_opts.c_str(), new_opts.length(), system_charset_info);

    // record[1] = old, record[0] = new.
    int err = cat->file->ha_update_row(cat->record[1], cat->record[0]);
    if (err == 0 || err == HA_ERR_RECORD_IS_THE_SAME) {
      ret = 0;
      DBUG_PRINT("recovery", ("set_flag: options updated '%s' → '%s' for %s.%s", current_opts.c_str(), new_opts.c_str(),
                              schema_name.c_str(), table_name.c_str()));
    } else {
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: ha_update_row error %d for %s.%s", err,
             schema_name.c_str(), table_name.c_str());
    }
    break;  // (schema_id, table_name) is unique in mysql.tables
  }

  cat->file->ha_rnd_end();
  ShannonBase::Utils::Util::close_table(thd, cat);

  if (ret != 0) {
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: row not found for %s.%s", schema_name.c_str(),
           table_name.c_str());
  }
  return ret;
}

int LoadFlagManager::query_loaded_tables(THD *thd, std::vector<SecondaryLoadedTable> &out) {
  DBUG_TRACE;
  out.clear();

  std::unordered_map<longlong, std::string> schema_map;
  if (int r = build_schema_id_map(thd, schema_map); r != 0) return r;

  TABLE *cat = ShannonBase::Utils::Util::open_table_by_name(thd, "mysql", "tables", TL_READ_WITH_SHARED_LOCKS);
  if (!cat) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::query_loaded_tables: cannot open mysql.tables");
    return HA_ERR_GENERIC;
  }

  if (cat->file->inited == handler::NONE && cat->file->ha_rnd_init(true)) {
    ShannonBase::Utils::Util::close_table(thd, cat);
    return HA_ERR_GENERIC;
  }

  ShannonBase::Utils::ColumnMapGuard guard(cat, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

  int tmp;
  while ((tmp = cat->file->ha_rnd_next(cat->record[0])) != HA_ERR_END_OF_FILE) {
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    longlong sch_id = (*(cat->field + kTablesSchemaId))->val_int();
    auto it = schema_map.find(sch_id);
    if (it == schema_map.end()) continue;
    if (is_system_schema(it->second)) continue;

    String name_buf;
    std::string table_name = (*(cat->field + kTablesName))->val_str(&name_buf)->c_ptr();

    String opt_buf;
    std::string opts = (*(cat->field + kTablesOptions))->val_str(&opt_buf)->c_ptr();

    if (!has_secondary_load_flag(opts)) continue;

    SecondaryLoadedTable entry;
    entry.schema_name = it->second;
    entry.table_name = table_name;
    // Detect partitioned tables: check the options string for "partitioned"
    // keyword, exactly as load_mysql_tables_info does.
    entry.is_partitioned =
        (opts.find("partitioned") != std::string::npos || opts.find("PARTITIONED") != std::string::npos);

    DBUG_PRINT("recovery", ("query_loaded_tables: found %s.%s (partitioned=%d)", entry.schema_name.c_str(),
                            entry.table_name.c_str(), entry.is_partitioned ? 1 : 0));

    out.push_back(std::move(entry));
  }

  cat->file->ha_rnd_end();
  ShannonBase::Utils::Util::close_table(thd, cat);

  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
         "LoadFlagManager::query_loaded_tables: found %zu table(s) with "
         "secondary_load=1",
         out.size());
  return 0;
}

int LoadFlagManager::is_table_flagged(THD *thd, const std::string &schema_name, const std::string &table_name,
                                      bool &loaded) {
  DBUG_TRACE;
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
