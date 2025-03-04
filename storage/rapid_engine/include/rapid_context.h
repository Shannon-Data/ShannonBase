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
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/trx/transaction.h"

class trx_t;
class ReadView;
class TABLE;
class JOIN;
class THD;

namespace ShannonBase {
class Transaction;
namespace Imcs {
class Imcs;
class Cu;
extern thread_local ShannonBase::Imcs::Imcs *current_imcs_instance;
}  // namespace Imcs

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

class ha_rapid;
// used in imcs.
class Rapid_load_context : public Secondary_engine_execution_context {
 public:
  class extra_info_t {
   public:
    extra_info_t() {}
    ~extra_info_t() {}
    // trxid of this innodb rows.
    Transaction::ID m_trxid{0};
    Compress::Encoding_type m_algo{Compress::Encoding_type::SORTED};

    // index scan info
    ushort m_keynr{MAX_FIELDS};
    ha_rkey_function m_find_flag{HA_READ_INVALID};
    uint8 m_key_len{0};
    std::unique_ptr<uchar[]> m_key_buff{nullptr};
  };
  Rapid_load_context()
      : m_schema_name(nullptr),
        m_table_name(nullptr),
        m_trx(nullptr),
        m_table(nullptr),
        m_local_dict(nullptr),
        m_thd{nullptr} {}
  virtual ~Rapid_load_context() = default;

  // current schema name and table name.
  char *m_schema_name{nullptr}, *m_table_name{nullptr};

  // current transaction.
  Transaction *m_trx{nullptr};

  // the primary key of this table.
  TABLE *m_table{nullptr};

  // the dictionary for this Cu.
  Compress::Dictionary *m_local_dict{nullptr};

  // current thd here.
  THD *m_thd{nullptr};

  extra_info_t m_extra_info;
};

class Rapid_pop_context : public Secondary_engine_execution_context {
 public:
  uint64_t m_start_lsn;
  // current schema name and table name.
  char *m_schema_name{nullptr}, *m_table_name{nullptr};
  // trx id.
  Transaction::ID m_trxid{0};

  uint8 m_key_len{0};
  std::unique_ptr<uchar[]> m_key_buff{nullptr};
};

class Rapid_ha_data {
 public:
  Rapid_ha_data() : m_trx(nullptr) {}

  ~Rapid_ha_data() {}

  ShannonBase::Transaction *get_trx() const { return m_trx; }

  void set_trx(ShannonBase::Transaction *t) { m_trx = t; }

 private:
  ShannonBase::Transaction *m_trx;
};

}  // namespace ShannonBase
#endif