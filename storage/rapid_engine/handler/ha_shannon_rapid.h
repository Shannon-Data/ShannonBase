/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef __PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H__
#define __PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H__
#include <mutex>

#include "my_base.h"
#include "sql/handler.h"
#include "thr_lock.h"

class THD;
struct TABLE;
struct TABLE_SHARE;

namespace dd {
class Table;
}

namespace ShannonBase {
class Transaction;
namespace Imcs {
class DataTable;
}

struct RapidShare {
  THR_LOCK lock;
  RapidShare() { thr_lock_init(&lock); }
  ~RapidShare() { thr_lock_delete(&lock); }

  // Not copyable. The THR_LOCK object must stay where it is in memory
  // after it has been initialized.
  RapidShare(const TABLE &table) {
    thr_lock_init(&lock);
    m_source_table = &table;
  }
  RapidShare &operator=(const RapidShare &) = delete;

  // source table.
  ulonglong m_tableid{0};
  const char *m_db_name{nullptr}, *m_table_name{nullptr};
  handler *file{nullptr};
  const TABLE *m_source_table;
};

/**
 * The SB storage engine is used for testing MySQL server functionality
 * related to secondary storage engines.
 *
 * There are currently no secondary storage engines mature enough to be merged
 * into mysql-trunk. Therefore, this bare-minimum storage engine, with no
 * actual functionality and implementing only the absolutely necessary handler
 * interfaces to allow setting it as a secondary engine of a table, was created
 * to facilitate pushing MySQL server code changes to mysql-trunk with test
 * coverage without depending on ongoing work of other storage engines.
 *
 * @note This shannon storage engine does not support being set as a primary
 * storage engine.
 */
class ha_rapid : public handler {
 public:
  ha_rapid(handlerton *hton, TABLE_SHARE *table_share);

 private:
  int create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) override { return HA_ERR_WRONG_COMMAND; }

  int open(const char *name, int mode, unsigned int test_if_locked, const dd::Table *table_def) override;

  int close() override;

  int rnd_init(bool scan) override;

  int rnd_end() override;

  int rnd_next(uchar *buf) override;

  int rnd_pos(unsigned char *, unsigned char *) override { return HA_ERR_WRONG_COMMAND; }

  int info(unsigned int) override;

  int index_init(uint keynr, bool sorted) override;

  int index_end() override;

  int index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) override;

  int index_read_last(uchar *buf, const uchar *key, uint key_len) override;

  int index_next(uchar *buf) override;

  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;

  int index_prev(uchar *buf) override;

  int index_first(uchar *buf) override;

  int index_last(uchar *buf) override;

  int read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range_arg, bool sorted) override;

  int read_range_next() override;

  ha_rows records_in_range(unsigned int index, key_range *min_key, key_range *max_key) override;

  void position(const unsigned char *) override {}

  int records(ha_rows *num_rows) override;

  double scan_time() override;

  unsigned long index_flags(unsigned int, unsigned int, bool) const override;

  auto is_push_down() { return (pushed_idx_cond) ? true : false; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, thr_lock_type lock_type) override;

  Table_flags table_flags() const override;

  const char *table_type() const override;

  /**
   * Load table into the secondary engine.
   *
   * @param[in] table - table to be loaded
   * @param[out] skip_metadata_update - should the DD metadata be updated for
   * the load of this table
   * @return 0 if success
   */
  int load_table(const TABLE &table, bool *skip_metadata_update) override;
  /**
   * Unload the table from secondary engine
   *
   * @param[in] db_name
   * @param[in] table_name
   * @param[in] error_if_not_loaded - whether to report an error if the table is
   * already not present in the secondary engine.
   * @return 0 if success
   */
  int unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) override;

  int start_stmt(THD *const thd, thr_lock_type lock_type) override MY_ATTRIBUTE((__warn_unused_result__));

  THR_LOCK_DATA m_lock;

  RapidShare *m_share{nullptr};

  THD *m_thd{nullptr};

  /** this is set to 1 when we are starting a table scan but have
  not yet fetched any row, else false */
  bool m_start_of_scan{false};

  std::unique_ptr<ShannonBase::Imcs::DataTable> m_data_table;
  std::string m_failed_reason;
};

}  // namespace ShannonBase

#endif  // __PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H__