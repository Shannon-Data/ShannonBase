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

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
class trx_t;
class ReadView;
class TABLE;

namespace ShannonBase {
class ShannonBaseContext {
public:
  ShannonBaseContext() {}
  virtual ~ShannonBaseContext() {}
  ShannonBaseContext(ShannonBaseContext&) = delete;
  ShannonBaseContext& operator=(const ShannonBaseContext&) = delete;
};

class RapidContext : public ShannonBaseContext {
public:
  class extra_info_t{
    public:
      extra_info_t(): m_pk(0), m_trxid(0) {}
      ~extra_info_t() {}
      //primary key of this innodb rows.
      uint64 m_pk {0};
      //trxid of this innodb rows.
      uint64 m_trxid {0};
      Compress::Dictionary::Dictionary_algo_t m_algo {Compress::Dictionary::Dictionary_algo_t::SORTED};
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
};

} //ns:shannonbase
#endif