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

   The fundmental code for imcs. IMCS - in memory column store.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_IMCS_H__
#define __SHANNONBASE_IMCS_H__

#include <atomic>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "my_inttypes.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

namespace ShannonBase {
class Rapid_load_context;
namespace Imcs {
class DataTable;
/** An IMCS is consist of CUs. Some chunks are made of a CU. Header and body is
 * two parts of a CU. Header has meta information about this CU, and the body
 * has the read data. All chunks stored consecutively in a CU.  key format of a
 * CU is listed as following: `db_name_str` + ":" + `table_name_str` + ":" +
 * `field_index`. */
class Imcs : public MemoryObject {
 public:
  // make ctor and dctor private.
  Imcs() {}
  virtual ~Imcs() {}

  int initialize();
  int deinitialize();
  // gets initialized flag.
  inline bool initialized() { return m_inited; }

  inline static Imcs *instance() {
    std::call_once(one, [&] { m_instance = new Imcs(); });
    return m_instance;
  }

  inline std::unordered_map<std::string, std::unique_ptr<Cu>> &get_cus() { return m_cus; }
  // get cu pointer by its key.
  Cu *get_cu(std::string_view key);

  // get cu at Nth index key
  Cu *at(size_t indexx);

  /**create all cus needed by source table, and ready to write the data into.*/
  int create_table_memo(const Rapid_load_context *context, const TABLE *source);

  /** load the current table rows data into imcs. the caller's responsible
   for moving to next row */
  int load_table(const Rapid_load_context *context, const TABLE *source);

  // unload the table rows data from imcs.
  int unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                   bool error_if_not_loaded);

  // insert a row into IMCS, where located at 'rowid'.
  int insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf);

  // delete a row in IMCS by using its rowid.
  int delete_row(const Rapid_load_context *context, row_id_t rowid);

  // delete a row in IMCS by using its rowid. if vector is empty that means delete all rows.
  int delete_rows(const Rapid_load_context *context, std::vector<row_id_t> &rowids);

  // update a cu in IMCS by using its rowid.
  int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key, const uchar *new_field_data,
                 size_t nlen);

  /** row_id[in], which row will be updated.
   *  upd_recs[in], new values of updating row at row_id.
   */
  int update_row(const Rapid_load_context *context, row_id_t row_id,
                 std::map<std::string, std::unique_ptr<uchar[]>> &upd_recs);

  // get the source table by key string.
  inline std::vector<std::string> source_key(std::string &sch_tb_name_key) {
    if (m_source_keys.find(sch_tb_name_key) == m_source_keys.end()) return std::vector<std::string>();
    return m_source_keys[sch_tb_name_key];
  }

  // get the key length by key string. key string is db_name + ":" + table_name.
  inline size_t key_length(std::string &key) {
    auto it =
        std::find_if(m_cus.begin(), m_cus.end(), [&key](const auto &pair) { return pair.first.rfind(key, 0) == 0; });
    if (it != m_cus.end()) {
      return it->second.get()->header()->m_key_len;
    } else {
      return 0u;
    }
    return 0u;
  }

 private:
  Imcs(Imcs &&) = delete;
  Imcs(Imcs &) = delete;
  Imcs &operator=(const Imcs &) = delete;
  Imcs &operator=(const Imcs &&) = delete;

  // imcs instance
  static Imcs *m_instance;

  // initialization flag, only once.
  static std::once_flag one;

  // initialization flag.
  std::atomic<uint8> m_inited{0};

  // the loaded cus. key format: db + ':' + table_name + ':'
  // + field_name + ":".
  std::unordered_map<std::string, std::unique_ptr<Cu>> m_cus;

  // the loaded cus. key format: db + ':' + table_name.
  // value format: park part1 name , key part2 name.
  std::unordered_map<std::string, std::vector<std::string>> m_source_keys;

  // the current version of imcs.
  uint m_version{SHANNON_RPD_VERSION};

  const char *m_magic = "SHANNON_MAGIC_IMCS";
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_H__