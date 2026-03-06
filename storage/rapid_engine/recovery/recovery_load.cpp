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
#include "sql/dd/dd_kill_immunizer.h"                        // dd::DD_kill_immunizer
#include "sql/dd/impl/utils.h"
#include "sql/handler.h"  // HA_ERR_*, handler::NONE, store_record()
#include "sql/sql_base.h"
#include "sql/sql_class.h"  // THD
#include "sql/table.h"      // TABLE, Field

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
class Binlog_guard {
 public:
  explicit Binlog_guard(THD *thd) : m_thd(thd) {
    if (m_thd) {
      m_saved_options = m_thd->variables.option_bits;
      m_thd->variables.option_bits &= ~OPTION_BIN_LOG;
    }
  }

  ~Binlog_guard() {
    if (m_thd) m_thd->variables.option_bits = m_saved_options;
  }

  Binlog_guard(const Binlog_guard &) = delete;
  Binlog_guard &operator=(const Binlog_guard &) = delete;

 private:
  THD *m_thd{nullptr};
  ulonglong m_saved_options{0};
};
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
 * @param loaded    true  → write "secondary_load=1".
 *                      false → write "secondary_load=0"
 * @return Updated options string.
 */
static std::string rebuild_options(const std::string &current_opts, bool loaded) {
  static const std::string TARGET_KEY = "secondary_load";
  static const char DELIMITER = ';';

  if (current_opts.empty()) return TARGET_KEY + "=" + (loaded ? "1" : "0") + DELIMITER;

  std::string opts = current_opts;
  bool ends_with_delim = (opts.back() == DELIMITER);
  if (!ends_with_delim) {
    opts += DELIMITER;
  }

  std::stringstream ss(opts);
  std::string token;
  std::string result;
  bool found = false;
  const std::string target_prefix = TARGET_KEY + "=";

  while (std::getline(ss, token, DELIMITER)) {
    if (token.empty()) continue;

    if (token.find(target_prefix) == 0) {
      found = true;
      result += target_prefix + (loaded ? "1" : "0") + DELIMITER;
    } else {
      result += token + DELIMITER;
    }
  }

  if (!found) result += target_prefix + (loaded ? "1" : "0") + DELIMITER;

  if (!ends_with_delim && !result.empty()) result.pop_back();
  return result;
}

/**
 * @brief Scan mysql.schemata and build a schema_id → schema_name map.
 *
 * Follows the same pattern as SelfLoadManager::load_mysql_schema_info().
 */
static int build_schema_id_map(THD *thd, std::unordered_map<longlong, std::string> &out) {
  TABLE *schemata = ShannonBase::Utils::Util::open_table_by_name(thd, "mysql", "schemata", TL_READ_WITH_SHARED_LOCKS);
  if (!schemata) {
    // open_table_by_name already emits a warning; do NOT call close_table(null).
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "rapid_load_persist: cannot open mysql.schemata");
    return HA_ERR_GENERIC;
  }

  if (schemata->file->inited == handler::NONE && schemata->file->ha_rnd_init(true)) {
    ShannonBase::Utils::Util::close_table(thd, schemata);
    close_thread_tables(thd);
    return HA_ERR_GENERIC;
  }

  // Scope the guard so its destructor runs BEFORE ha_rnd_end / close_table.
  // Without this scope, the guard's dtor would fire at function-exit, after
  // the table has already been freed, causing the SIGSEGV in close_thread_tables.
  {
    ShannonBase::Utils::ColumnMapGuard guard(schemata, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

    int tmp;
    while ((tmp = schemata->file->ha_rnd_next(schemata->record[0])) != HA_ERR_END_OF_FILE) {
      if (tmp == HA_ERR_KEY_NOT_FOUND) break;

      longlong id = (*(schemata->field + kSchemataId))->val_int();

      String buf;
      std::string name = (*(schemata->field + kSchemataName))->val_str(&buf)->c_ptr();

      out.emplace(id, std::move(name));
    }
  }  // ~ColumnMapGuard: bitmaps restored here, while TABLE is still alive

  schemata->file->ha_rnd_end();
  ShannonBase::Utils::Util::close_table(thd, schemata);
  return 0;
}

int LoadFlagManager::set_flag(THD *thd, const std::string &schema_name, const std::string &table_name, bool loaded) {
  DBUG_PRINT("recovery",
             ("LoadFlagManager::set_flag %s.%s loaded=%d", schema_name.c_str(), table_name.c_str(), loaded ? 1 : 0));
  Open_tables_backup ot_backup;
  thd->reset_n_backup_open_tables_state(&ot_backup, Open_tables_state::SYSTEM_TABLES);

  int ret = HA_ERR_GENERIC;  // updated to 0 on success

  // Lambda ensures we always restore the backup, even on early return.
  auto do_work = [&]() -> int {
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
      close_thread_tables(thd);
      return HA_ERR_GENERIC;
    }

    // TYPE::ALL populates both read_set and write_set – required for ha_update_row.
    // Scope the guard so its destructor runs BEFORE ha_rnd_end / close_table.
    int inner_ret = HA_ERR_GENERIC;
    {
      ShannonBase::Utils::ColumnMapGuard guard(cat, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
      int tmp;
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
        if (current_opts == new_opts) {
          inner_ret = 0;  // nothing to update, but not an error
          DBUG_PRINT("recovery",
                     ("set_flag: options already up-to-date for %s.%s", schema_name.c_str(), table_name.c_str()));
          break;
        }
        // store_record copies record[0] → record[1] (old image).
        store_record(cat, record[1]);

        Field *opt_fld = *(cat->field + kTablesOptions);
        opt_fld->store(new_opts.c_str(), new_opts.length(), system_charset_info);

        // record[1] = old, record[0] = new.
        int err{0};
        {
          Binlog_guard no_binlog(thd);
          err = cat->file->ha_update_row(cat->record[1], cat->record[0]);
        }
        if (err == 0 || err == HA_ERR_RECORD_IS_THE_SAME) {
          inner_ret = 0;
          DBUG_PRINT("recovery", ("set_flag: options updated '%s' → '%s' for %s.%s", current_opts.c_str(),
                                  new_opts.c_str(), schema_name.c_str(), table_name.c_str()));
        } else {
          LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: ha_update_row error %d for %s.%s", err,
                 schema_name.c_str(), table_name.c_str());
        }
        break;  // (schema_id, table_name) is unique in mysql.tables
      }
    }  // ~ColumnMapGuard: bitmaps restored here, while TABLE is still alive

    cat->file->ha_rnd_end();
    ShannonBase::Utils::Util::close_table(thd, cat);

    // Close all system tables opened in this backup context.
    // Safe: thd->open_tables here contains ONLY the DD tables we opened
    // above; the user's tables (e.g. t1) are in ot_backup and will be
    // restored below.
    close_thread_tables(thd);

    if (inner_ret != 0) {
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::set_flag: row not found for %s.%s",
             schema_name.c_str(), table_name.c_str());
    }
    return inner_ret;
  };

  ret = do_work();

  // Always restore the user THD's original open_tables state.
  thd->restore_backup_open_tables_state(&ot_backup);

  return ret;
}

int LoadFlagManager::query_loaded_tables(THD *thd, std::vector<SecondaryLoadedTable> &out) {
  out.clear();

  Open_tables_backup ot_backup;
  thd->reset_n_backup_open_tables_state(&ot_backup, Open_tables_state::SYSTEM_TABLES);

  int ret = 0;

  auto do_work = [&]() -> int {
    std::unordered_map<longlong, std::string> schema_map;
    if (int r = build_schema_id_map(thd, schema_map); r != 0) return r;

    TABLE *cat = ShannonBase::Utils::Util::open_table_by_name(thd, "mysql", "tables", TL_READ_WITH_SHARED_LOCKS);
    if (!cat) {
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "LoadFlagManager::query_loaded_tables: cannot open mysql.tables");
      return HA_ERR_GENERIC;
    }

    if (cat->file->inited == handler::NONE && cat->file->ha_rnd_init(true)) {
      ShannonBase::Utils::Util::close_table(thd, cat);
      close_thread_tables(thd);
      return HA_ERR_GENERIC;
    }

    // Scope the guard so its destructor runs BEFORE ha_rnd_end / close_table.
    {
      ShannonBase::Utils::ColumnMapGuard guard(cat, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

      int tmp;
      while ((tmp = cat->file->ha_rnd_next(cat->record[0])) != HA_ERR_END_OF_FILE) {
        if (tmp == HA_ERR_KEY_NOT_FOUND) break;

        longlong sch_id = (*(cat->field + kTablesSchemaId))->val_int();
        auto it = schema_map.find(sch_id);
        if (it == schema_map.end() || is_system_schema(it->second)) continue;

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
    }  // ~ColumnMapGuard: bitmaps restored here, while TABLE is still alive

    cat->file->ha_rnd_end();
    ShannonBase::Utils::Util::close_table(thd, cat);

    // Close all system tables opened in this backup context.
    close_thread_tables(thd);

    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
           "LoadFlagManager::query_loaded_tables: found %zu table(s) with "
           "secondary_load=1",
           out.size());
    return 0;
  };

  ret = do_work();

  // Always restore the caller THD's original open_tables state.
  thd->restore_backup_open_tables_state(&ot_backup);

  return ret;
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