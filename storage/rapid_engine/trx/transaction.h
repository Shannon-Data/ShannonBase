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
#include "sql/current_thd.h"
#include "storage/rapid_engine/include/rapid_object.h"

class THD;
class trx_t;
namespace ShannonBase {

/**This class is used for an interface of real implementation of trans.
Here, is used as an interface of innobase transaction. In future we can
use any transaction impl to replace innobase's trx used here.
*/

class Transaction : public MemoryObject {
 public:
  // same order with trx_t::isolation_level_t::
  enum class ISOLATION_LEVEL : uint8 { READ_UNCOMMITTED, READ_COMMITTED, READ_REPEATABLE, SERIALIZABLE };
  enum class STATUS : uint8 { NOT_START, ACTIVE, PREPARED, COMMITTED_IN_MEMORY };

  Transaction(THD *thd = current_thd);
  virtual ~Transaction() = default;

  static Transaction *get_or_create_tx(THD *);

  virtual int begin(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);

  virtual int commit();

  virtual int rollback();

  virtual void set_trx_read_only(bool read_only);

  virtual bool acquire_snapshot(bool create);

  virtual inline bool is_auto_commit();

  virtual inline bool is_active();

 private:
  THD *m_thd;
  /**here, we use innodb's trx as ours. in future, we will impl rpl own transaction.
   * But, now that, we use innodb's.*/
  trx_t *m_trx_impl{nullptr};
  ISOLATION_LEVEL m_iso_level{ISOLATION_LEVEL::READ_REPEATABLE};
};

}  // namespace ShannonBase