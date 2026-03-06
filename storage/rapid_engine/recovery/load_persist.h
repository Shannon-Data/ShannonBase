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
#ifndef __SHANNONBASE_RECOVERY_LOAD_PERSIST_H__
#define __SHANNONBASE_RECOVERY_LOAD_PERSIST_H__
/**
 * Persist secondary_load information for ShannonBase restart recovery.
 */
#include <string>
#include <vector>
#include "my_inttypes.h"

class THD;
namespace ShannonBase {
namespace Recovery {
/**
 * @brief Represents a table that has the secondary_load flag set in the DD.
 */
struct SecondaryLoadedTable {
  std::string schema_name;
  std::string table_name;
  bool is_partitioned{false};
};

/**
 * @class LoadFlagManager
 * @brief Manages the `secondary_load` flag in mysql.tables options column.
 *
 * The `secondary_load` option is a boolean flag stored in the `options` column
 * of mysql.tables (the Data Dictionary). It records whether a table was
 * successfully loaded into the ShannonBase IMCS secondary engine. This flag
 * is used during restart recovery to determine which tables need to be
 * reloaded.
 */
class LoadFlagManager {
 public:
  static LoadFlagManager &instance() {
    static LoadFlagManager inst;
    return inst;
  }

  /**
   * @brief Set or clear the secondary_load flag for a given table.
   *
   * This is called after a successful SECONDARY_LOAD or SECONDARY_UNLOAD
   * operation completes. It updates the `options` column in mysql.tables
   * by adding or removing the "secondary_load=1" token.
   *
   * @param thd         MySQL thread descriptor.
   * @param schema_name Database name.
   * @param table_name  Table name.
   * @param loaded      true  → set the flag (table is now loaded).
   *                    false → clear the flag (table is now unloaded).
   * @return 0 on success, non-zero on failure.
   */
  int set_flag(THD *thd, const std::string &schema_name, const std::string &table_name, bool loaded);

  /**
   * @brief Query INFORMATION_SCHEMA.TABLES to find all tables with
   *        secondary_load=1 in their CREATE_OPTIONS.
   *
   * This is called during restart recovery from a background DD Worker thread.
   * The query reads from INFORMATION_SCHEMA rather than the raw DD tables so
   * that it respects the normal access-control path.
   *
   * @param thd   MySQL thread descriptor (must have sufficient privileges).
   * @param out   Output vector filled with SecondaryLoadedTable entries.
   * @return 0 on success, non-zero on failure.
   */
  int query_loaded_tables(THD *thd, std::vector<SecondaryLoadedTable> &out);

  /**
   * @brief Check whether the secondary_load flag is set for a specific table.
   *
   * @param thd         MySQL thread descriptor.
   * @param schema_name Database name.
   * @param table_name  Table name.
   * @param[out] loaded Set to true if the flag is set.
   * @return 0 on success, non-zero on failure.
   */
  int is_table_flagged(THD *thd, const std::string &schema_name, const std::string &table_name, bool &loaded);

 private:
  LoadFlagManager() = default;
  ~LoadFlagManager() = default;
  LoadFlagManager(const LoadFlagManager &) = delete;
  LoadFlagManager &operator=(const LoadFlagManager &) = delete;
};
}  // namespace Recovery
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RECOVERY_LOAD_PERSIST_H__