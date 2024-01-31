/* Copyright (c) 2018, 2023, Oracle and/or its affiliates.

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
*/

#ifndef PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H_
#define PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H_

#include "my_base.h"
#include "sql/handler.h"
#include "thr_lock.h"
#include "include/field_types.h"  //field types.

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/reader/imcs_reader.h"
/* clang-format off */
class THD;
struct TABLE;
struct TABLE_SHARE;
class dict_index_t;

namespace dd {
class Table;
}

namespace ShannonBase {
struct RapidShare {
  RapidShare() { thr_lock_init(&m_lock);}
  RapidShare(const char* db_name,
             const char* table_name) : m_db_name(db_name), m_table_name(table_name)
            { thr_lock_init(&m_lock); }
  ~RapidShare() { thr_lock_delete(&m_lock); }

  // Not copyable. The THR_LOCK object must stay where it is in memory
  // after it has been initialized.
  RapidShare(const RapidShare &) = delete;
  THR_LOCK m_lock;
  RapidShare &operator=(const RapidShare &) = delete;
  const char* m_db_name {nullptr};
  const char* m_table_name {nullptr};
  handler* file {nullptr};
};

/**
 * The shannon rapid storage engine is used for testing MySQL server functionality
 * related to secondary storage engines.
 *
 * There are currently no secondary storage engines mature enough to be merged
 * into mysql-trunk. Therefore, this bare-minimum storage engine, with no
 * actual functionality and implementing only the absolutely necessary handler
 * interfaces to allow setting it as a secondary engine of a table, was created
 * to facilitate pushing MySQL server code changes to mysql-trunk with test
 * coverage without depending on ongoing work of other storage engines.
 *
 * @note This storage engine does not support being set as a primary
 * storage engine.
 */
class ha_rapid : public handler {
 public:
  ha_rapid(handlerton *hton, TABLE_SHARE *table_share);

  const char *table_type() const override;
  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_BTREE;
  }
  /** Check if SE supports specific key algorithm. */
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    /* This method is never used for FULLTEXT or SPATIAL keys.
    We rely on handler::ha_table_flags() to check if such keys
    are supported. */
    assert(key_alg != HA_KEY_ALG_FULLTEXT && key_alg != HA_KEY_ALG_RTREE);
    return (key_alg == HA_KEY_ALG_BTREE || key_alg == HA_KEY_ALG_HASH);
  }
  uint max_supported_keys() const override;

  uint max_supported_key_length() const override;

 private:
  int create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) override;

  int open(const char *name, int mode, unsigned int test_if_locked,
           const dd::Table *table_def) override;

  int close() override;

  int rnd_init(bool) override;

  int rnd_next(unsigned char *) override;

  int rnd_end() override;

  int rnd_pos(unsigned char *, unsigned char *) override {
    return HA_ERR_WRONG_COMMAND;
  }
  
  int read_range_first(const key_range *start_key, const key_range *end_key,
                       bool eq_range_arg, bool sorted) override;

  int read_range_next() override;

  int info(unsigned int) override;

  int index_init(uint keynr, bool sorted) override;

  int index_end() override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;

  int index_read_last(uchar *buf, const uchar *key, uint key_len) override;

  int index_next(uchar *buf) override;

  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;

  int index_prev(uchar *buf) override;

  int index_first(uchar *buf) override;

  int index_last(uchar *buf) override;

#if 0
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;

  int multi_range_read_next(char **range_info) override;

  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param, uint n_ranges,
                                      uint *bufsz, uint *flags,
                                      Cost_estimate *cost) override;

  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;
#endif
  ha_rows records_in_range(unsigned int index, key_range *min_key,
                           key_range *max_key) override;

  void position(const unsigned char *) override {}

  unsigned long index_flags(unsigned int, unsigned int, bool) const override;

  Item *idx_cond_push(uint keyno, Item *idx_cond) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  Table_flags table_flags() const override;

  int load_table(const TABLE &table) override;

  int unload_table(const char *db_name, const char *table_name,
                   bool error_if_not_loaded) override;

  THD* m_rpd_thd{nullptr};
  THR_LOCK_DATA m_lock;
  /** this is set to 1 when we are starting a table scan but have
      not yet fetched any row, else false */
  bool m_start_of_scan {false};
  /** information for MySQL table locking */
  RapidShare *m_share;
  std::unique_ptr<ShannonBase::RapidContext> m_rpd_context;
  //imscs reader;
  std::unique_ptr<ImcsReader> m_imcs_reader;

  dict_index_t* m_primary_key;
  enum_field_types m_key_type;
};

}  // namespace ShannonBase

#endif  // PLUGIN_SECONDARY_ENGINE_SHANNON_HA_RAPID_H_
