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

#include "sql/sql_lex.h" //Secondary_engine_execution_context
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/include/rapid_const.h"

class trx_t;
class ReadView;
class TABLE;
class JOIN;
class THD;

namespace ShannonBase {
/**
  Execution context class for the Rapid engine. It allocates some data
  on the heap when it is constructed, and frees it when it is
  destructed, so that LeakSanitizer and Valgrind can detect if the
  server doesn't destroy the object when the query execution has
  completed.
*/
class Shannon_execution_context : public Secondary_engine_execution_context {
 public:
  Shannon_execution_context() ;
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

class ShannonBaseContext {
public:
  ShannonBaseContext() {}
  virtual ~ShannonBaseContext() {}
  ShannonBaseContext(ShannonBaseContext&) = delete;
  ShannonBaseContext& operator=(const ShannonBaseContext&) = delete;
};

//used in imcs.
class ha_rapid;
class RapidContext : public ShannonBaseContext {
public:
  class extra_info_t{
    public:
      extra_info_t() {}
      ~extra_info_t() {}
      //trxid of this innodb rows.
      uint64 m_trxid {0};
      Compress::Encoding_type m_algo {Compress::Encoding_type::SORTED};

      //index scan info
      uint m_keynr{0};
      double m_key_val {SHANNON_LOWEST_DOUBLE};
      enum_field_types m_key_type;
      ha_rkey_function m_find_flag {HA_READ_INVALID};
  };
  RapidContext(): m_trx(nullptr), m_table(nullptr), m_local_dict(nullptr) {}
  virtual ~RapidContext() = default;
  //current transaction.
  trx_t* m_trx {nullptr};
  //the current db and table name.
  std::string m_current_db, m_current_table;
  //the primary key of this table.
  TABLE* m_table {nullptr};
  //the dictionary for this DB.
  Compress::Dictionary* m_local_dict {nullptr};
  extra_info_t m_extra_info;
  const ha_rapid* m_handler;
};
//used in optimization phase.
class OptimizeContext : public ShannonBaseContext {
public:
  OptimizeContext() = default;
  virtual ~ OptimizeContext() = default;
  THD* m_thd;
};

} //ns:shannonbase
#endif