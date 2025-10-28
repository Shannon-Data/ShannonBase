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
#ifndef __SHANNONBASE_CONTEXT_H__
#define __SHANNONBASE_CONTEXT_H__

#include "include/trx0types.h"  //trx_id_t
#include "sql/sql_class.h"
#include "sql/sql_lex.h"  //Secondary_engine_execution_context
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/trx/transaction.h"

class trx_t;
class ReadView;
class TABLE;
class JOIN;
class THD;

namespace ShannonBase {
class Transaction;
namespace Compress {
class Dictionary;
}
namespace Imcs {
class Imcs;
class Cu;
extern SHANNON_THREAD_LOCAL ShannonBase::Imcs::Imcs *current_imcs_instance;
}  // namespace Imcs

extern std::unordered_map<std::string, SYS_FIELD_TYPE_ID> current_sys_field_map;
/**
  Statement context class for the Shannon Rapid engine.
*/
class Rapid_statement_context : public Secondary_engine_statement_context {
 public:
  Rapid_statement_context() = default;
  virtual ~Rapid_statement_context() {}
};

/**
  Execution context class for the RAPID engine. It allocates some data
  on the heap when it is constructed, and frees it when it is
  destructed, so that LeakSanitizer and Valgrind can detect if the
  server doesn't destroy the object when the query execution has
  completed.
*/
class Rapid_execution_context : public Secondary_engine_execution_context {
 public:
  Rapid_execution_context() : m_data(std::make_unique<char[]>(10)) {}
  /**
    Checks if the specified cost is the lowest cost seen so far for executing
    the given JOIN.
  */
  bool BestPlanSoFar(const JOIN &join, double cost);

 private:
  std::unique_ptr<char[]> m_data;
  /// The JOIN currently being optimized.
  const JOIN *m_current_join{nullptr};
  /// The cost of the best plan seen so far for the current JOIN.
  double m_best_cost;
};

class Rapid_pop_context : public Secondary_engine_execution_context {
 public:
  uint64_t m_start_lsn;
  // current schema name and table name.
  std::string m_schema_name, m_table_name;
  // trx id.
  Transaction::ID m_trxid{0};

  // key length, DATA_ROW_ID_LEN OR KEY LEN;
  uint8 m_key_len{0};

  // key info. which is rowid or primary key/unique key.
  std::unique_ptr<uchar[]> m_key_buff{nullptr};
};

class ha_rapid;
// used in imcs.
class Rapid_ha_data {
 public:
  Rapid_ha_data() : m_trx(nullptr) {}

  ~Rapid_ha_data() {}

  ShannonBase::Transaction *get_trx() const { return m_trx; }

  void set_trx(ShannonBase::Transaction *t) { m_trx = t; }

 private:
  ShannonBase::Transaction *m_trx;
};

class Rapid_context : public Secondary_engine_execution_context {
 public:
  class extra_info_t {
   public:
    extra_info_t() {}
    ~extra_info_t() {}
    // trxid of this innodb rows.
    Transaction::ID m_trxid{0};

    Transaction::ID m_scn{0};

    Compress::Encoding_type m_algo{Compress::Encoding_type::SORTED};

    // index scan info.
    // the active key no.
    ushort m_keynr{MAX_FIELDS};

    // the active index name.
    std::string m_key_name;

    ha_rkey_function m_find_flag{HA_READ_INVALID};

    // the active key length.
    uint m_key_len{0};

    // the active key memo.
    std::shared_ptr<uchar[]> m_key_buff{nullptr};

    // partition info.
    std::unordered_map<std::string, uint> m_partition_infos;

    // active partitio info.
    static SHANNON_THREAD_LOCAL std::string m_active_part_key;
  };

  // current openning schema name and table name.
  std::string m_schema_name, m_table_name, m_sch_tb_name;

  // current openning table extra information.
  extra_info_t m_extra_info;
};

class Rapid_load_context : public Rapid_context {
 public:
  Rapid_load_context() : m_trx(nullptr), m_table(nullptr), m_local_dict(nullptr), m_thd{nullptr} {}
  virtual ~Rapid_load_context() = default;

  // current transaction.
  Transaction *m_trx{nullptr};

  // the primary key of this table.
  TABLE *m_table{nullptr};

  // the dictionary for this Cu.
  Compress::Dictionary *m_local_dict{nullptr};

  Populate::change_record_buff_t::off_page_data_t *m_offpage_data0{nullptr};
  Populate::change_record_buff_t::off_page_data_t *m_offpage_data1{nullptr};
  // current thd here.
  THD *m_thd{nullptr};
};

class Rapid_scan_context : public Rapid_context {
 public:
  size_t limit{SIZE_MAX}, rows_returned;

  // current transaction.
  Transaction *m_trx{nullptr};

  // the primary key of this table.
  TABLE *m_table{nullptr};

  // current thd here.
  THD *m_thd{nullptr};
};

}  // namespace ShannonBase
#endif