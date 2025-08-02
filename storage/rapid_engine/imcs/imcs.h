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
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

#include "my_inttypes.h"
#include "sql/field.h"
#include "sql/handler.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/concurrent.h"

class ha_innobase;
class ha_innopart;
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

  inline static boost::asio::thread_pool *pool() { return m_imcs_pool.get(); }
  inline static Imcs *instance() {
    std::call_once(one, [&] { m_instance = new Imcs(); });
    return m_instance;
  }

  // get cu at Nth index key
  // Cu *at(std::string_view schema, std::string_view table, size_t indexx);

  /**create all cus needed by source table, and ready to write the data into.*/
  int create_table_memo(const Rapid_load_context *context, const TABLE *source);

  /** load the current table rows data into imcs. the caller's responsible
   for moving to next row */
  int load_table(const Rapid_load_context *context, const TABLE *source);

  /** load the current partition table rows data into imcs. the caller's responsible
   for moving to next row */
  int load_parttable(const Rapid_load_context *context, const TABLE *source);

  // unload the table rows data from imcs.
  int unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                   bool error_if_not_loaded);

  // insert a row into IMCS, where located at 'rowid'.
  int insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf);

  // insert a row into IMCS to specific address from log_parser thread.
  int write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                         std::unordered_map<std::string, mysql_field_t> &fields);

  // delete a row in IMCS by using its rowid.
  int delete_row(const Rapid_load_context *context, row_id_t rowid);

  // delete a row in IMCS by using its rowid. if vector is empty that means delete all rows.
  int delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids);

  // update a cu in IMCS by using its rowid.
  int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key, const uchar *new_field_data,
                 size_t nlen);

  /** row_id[in], which row will be updated.
   *  upd_recs[in], new values of updating row at row_id.
   */
  int update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                          std::unordered_map<std::string, mysql_field_t> &upd_recs);

  int build_indexes_from_keys(const Rapid_load_context *context, std::map<std::string, key_info_t> &keys,
                              row_id_t rowid);

  int build_indexes_from_log(const Rapid_load_context *context, std::map<std::string, mysql_field_t> &field_values,
                             row_id_t rowid);

  int rollback_changes_by_trxid(Transaction::ID trxid);

  void cleanup(std::string &sch_name, std::string &table_name);

  inline row_id_t reserve_row_id(std::string &sch_table) {
    if (m_tables.size() == 0 || m_tables.find(sch_table) == m_tables.end()) return INVALID_ROW_ID;
    return m_tables[sch_table]->reserve_id(nullptr);
  }

  inline RapidTable *get_table(std::string &sch_table) {
    std::shared_lock lk(m_table_mutex);
    if (m_tables.find(sch_table) == m_tables.end())
      return nullptr;
    else
      return m_tables[sch_table].get();
  }

  inline std::unordered_map<std::string, std::unique_ptr<RapidTable>> &get_tables() {
    std::shared_lock lk(m_table_mutex);
    return m_tables;
  }

  inline RapidTable *get_parttable(std::string &sch_table) {
    std::shared_lock lk(m_table_mutex);
    if (m_parttables.find(sch_table) == m_parttables.end())
      return nullptr;
    else
      return m_parttables[sch_table].get();
  }

 private:
  Imcs(Imcs &&) = delete;
  Imcs(Imcs &) = delete;
  Imcs &operator=(const Imcs &) = delete;
  Imcs &operator=(const Imcs &&) = delete;

  int load_innodb(const Rapid_load_context *context, ha_innobase *file);
  int load_innodb_parallel(const Rapid_load_context *context, ha_innobase *file);
  int load_innodbpart(const Rapid_load_context *context, ha_innopart *file);

  int unload_innodb(const Rapid_load_context *context, const char *db_name, const char *table_name,
                    bool error_if_not_loaded);

  int unload_innodbpart(const Rapid_load_context *context, const char *db_name, const char *table_name,
                        bool error_if_not_loaded);

 private:
  typedef struct {
    // if you dont use this, remove the boost_thread and boost_system libs in cmake file.
    // thread id
    std::thread::id tid;

    // this thread work whether done or not.
    std::atomic<bool> scan_done{false};

    // # of rows scan in this thread.
    std::atomic<size_t> n_rows{0};

    //# of column and row len of a row in this thread work.
    std::atomic<ulong> n_cols{0}, row_len{0};
    std::vector<ulong> col_offsets;
    std::vector<ulong> null_byte_offsets;
    std::vector<ulong> null_bitmasks;
  } parall_scan_cookie_t;

  // imcs instance
  static Imcs *m_instance;

  // initialization flag, only once.
  static std::once_flag one;

  // initialization flag.
  std::atomic<uint8> m_inited{0};

  static std::unique_ptr<boost::asio::thread_pool> m_imcs_pool;

  std::shared_mutex m_table_mutex;
  // loaded tables. key format: schema_name + ":" + table_name.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_tables;

  // loaded partitioned tables. key format: schema_name + ":" + table_name.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_parttables;

  // the current version of imcs.
  uint m_version{SHANNON_RPD_VERSION};

  const char *m_magic = "SHANNON_MAGIC_IMCS";
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_H__