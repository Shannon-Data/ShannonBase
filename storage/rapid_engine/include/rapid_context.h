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

#include "storage/rapid_engine/include/rapid_const.h"

class trx_t;
class ReadView;

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
  //current transaction.
  trx_t* m_trx;
  //current active read view
  ReadView* m_readview; //???
  //the current db and table name.
  std::string m_current_db, m_current_table;
  //the primary key of this table.
  struct {
   //primary key of this innodb rows.
   uint64 m_pk;
   //trxid of this innodb rows.
   uint64 m_trxid;
  } m_extra_info;
  TABLE* m_table;
};


} //ns:shannonbase
#endif