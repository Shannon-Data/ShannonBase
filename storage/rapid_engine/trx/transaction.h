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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. for transaction.
*/
#ifndef __SHANNONBASE_TRANSACTION_H__
#define __SHANNONBASE_TRANSACTION_H__

#include "sql/current_thd.h"
#include "storage/rapid_engine/include/rapid_object.h"

class THD;
class trx_t;
class ReadView;

namespace ShannonBase {

/**This class is used for an interface of real implementation of trans.
Here, is used as an interface of innobase transaction. In future we can
use any transaction impl to replace innobase's trx used here.
*/

class Transaction : public MemoryObject {
 public:
  // here, we use innodb's trx_id_t as ours. the defined in innodb is: typedef ib_id_t trx_id_t;
  using ID = uint64_t;
  // same order with trx_t::isolation_level_t::
  enum class ISOLATION_LEVEL : uint8 { READ_UNCOMMITTED, READ_COMMITTED, READ_REPEATABLE, SERIALIZABLE };
  enum class STATUS : uint8 { NOT_START, ACTIVE, PREPARED, COMMITTED_IN_MEMORY };

  // gets the existed trx or create a new one if not existed.
  static Transaction *get_or_create_trx(THD *);

  // gets the trx from thd.
  static Transaction *get_trx_from_thd(THD *const thd);

  static ShannonBase::Transaction::ISOLATION_LEVEL get_rpd_isolation_level(THD *thd);

  static void free_trx_from_thd(THD *const thd);

  void set_trx_on_thd(THD *const thd);

  void reset_trx_on_thd(THD *const thd);

  virtual void set_isolation_level(ISOLATION_LEVEL level) { m_iso_level = level; }

  virtual ISOLATION_LEVEL isolation_level() const { return m_iso_level; }

  virtual int begin(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);

  virtual int commit();

  virtual int rollback();

  virtual int begin_stmt(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);

  virtual int rollback_stmt();

  virtual int rollback_to_savepoint(void *const savepoint);

  virtual void set_read_only(bool read_only);

  virtual ReadView *acquire_snapshot();

  virtual int release_snapshot();

  virtual bool has_snapshot() const;

  virtual bool is_auto_commit();

  virtual bool is_active();

  virtual bool changes_visible(Transaction::ID trx_id, const char *table_name);

  virtual Transaction::ID get_id();

 private:
  Transaction(THD *thd = current_thd);
  virtual ~Transaction();

  THD *m_thd;

  // read only trx.
  bool m_read_only{false};

  /**here, we use innodb's trx as ours. in future, we will impl rpl own
   * transaction. But, now that, we use innodb's.*/
  trx_t *m_trx_impl{nullptr};

  ISOLATION_LEVEL m_iso_level{ISOLATION_LEVEL::READ_REPEATABLE};
};

}  // namespace ShannonBase

#endif  //__SHANNONBASE_TRANSACTION_H__