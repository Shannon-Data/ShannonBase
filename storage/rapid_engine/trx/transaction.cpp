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
#include "storage/rapid_engine/trx/transaction.h"

#include "sql/sql_class.h"                     // THD
#include "storage/innobase/include/trx0trx.h"  // trx_t

namespace ShannonBase {

Transaction::Transaction(THD *thd) {
  m_thd = thd;
  m_trx_impl = check_trx_exists(current_thd);
}

Transaction *Transaction::get_or_create_tx(THD *thd) {
  auto trx = new (thd->mem_root) Transaction(thd);

  return trx;
}

int Transaction::begin(ISOLATION_LEVEL iso_level) {
  m_iso_level = iso_level;
  trx_t::isolation_level_t is = trx_t::isolation_level_t::REPEATABLE_READ;

  switch (iso_level) {
    case ISOLATION_LEVEL::READ_UNCOMMITTED:
      is = trx_t::isolation_level_t::READ_UNCOMMITTED;
      break;
    case ISOLATION_LEVEL::READ_COMMITTED:
      is = trx_t::isolation_level_t::READ_COMMITTED;
      break;
    case ISOLATION_LEVEL::READ_REPEATABLE:
      is = trx_t::isolation_level_t::REPEATABLE_READ;
      break;
    case ISOLATION_LEVEL::SERIALIZABLE:
      is = trx_t::isolation_level_t::SERIALIZABLE;
      break;
    default:
      break;
  }

  m_trx_impl->isolation_level = is;
  trx_start_if_not_started(m_trx_impl, false, UT_LOCATION_HERE);
  if (m_trx_impl->isolation_level > TRX_ISO_READ_UNCOMMITTED) {
    trx_assign_read_view(m_trx_impl);
  }

  m_trx_impl->auto_commit =
      m_thd != nullptr && !thd_test_options(m_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) && thd_is_query_block(m_thd);
  return 0;
}

int Transaction::commit() {
  dberr_t error;
  if (trx_is_started(m_trx_impl)) {
    error = trx_commit_for_mysql(m_trx_impl);
  }
  return (DB_SUCCESS == error);
}

int Transaction::rollback() { return 0; }

void Transaction::set_trx_read_only(bool read_only) { m_trx_impl->read_only = read_only; }

bool Transaction::acquire_snapshot(bool create) { return true; }

bool Transaction::is_auto_commit() { return m_trx_impl->auto_commit; }

bool Transaction::is_active() { return trx_is_started(m_trx_impl); }

}  // namespace ShannonBase