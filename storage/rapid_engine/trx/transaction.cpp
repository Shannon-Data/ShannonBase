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
   Now that, we use innodb trx as rapid's. But, in future, we will impl
   our own trx implementation, because we use innodb trx id in rapid
   for our visibility check.
*/
#include "storage/rapid_engine/trx/transaction.h"

#include "sql/sql_class.h"                        // THD
#include "storage/innobase/include/read0types.h"  //ReadView
#include "storage/innobase/include/trx0roll.h"    // rollback
#include "storage/innobase/include/trx0trx.h"     // trx_t
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase {

// defined in ha_shannon_rapid.cc
extern handlerton *shannon_rapid_hton_ptr;

static ShannonBase::Rapid_ha_data *&get_ha_data_or_null(THD *const thd) {
  ShannonBase::Rapid_ha_data **ha_data =
      reinterpret_cast<ShannonBase::Rapid_ha_data **>(thd_ha_data(thd, ShannonBase::shannon_rapid_hton_ptr));
  return *ha_data;
}

static ShannonBase::Rapid_ha_data *&get_ha_data(THD *const thd) {
  auto *&ha_data = get_ha_data_or_null(thd);
  if (ha_data == nullptr) {
    ha_data = new ShannonBase::Rapid_ha_data();
  }
  return ha_data;
}

static void destroy_ha_data(THD *const thd) {
  ShannonBase::Rapid_ha_data *&ha_data = get_ha_data(thd);
  delete ha_data;
  ha_data = nullptr;
}

ShannonBase::Transaction::ISOLATION_LEVEL Transaction::get_rpd_isolation_level(THD *thd) {
  ulong const tx_isolation = thd_tx_isolation(thd);

  if (tx_isolation == ISO_READ_UNCOMMITTED) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED;
  } else if (tx_isolation == ISO_READ_COMMITTED) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED;
  } else if (tx_isolation == ISO_REPEATABLE_READ) {
    return ShannonBase::Transaction::ISOLATION_LEVEL::READ_REPEATABLE;
  } else
    return ShannonBase::Transaction::ISOLATION_LEVEL::SERIALIZABLE;
}

Transaction::Transaction(THD *thd) {
  m_thd = thd;
  m_trx_impl = trx_allocate_for_mysql();  // check_trx_exists(thd);
  m_read_only = false;
}

Transaction::~Transaction() {
  release_snapshot();
  rollback();
  trx_free_for_mysql(m_trx_impl);
}

Transaction *Transaction::get_trx_from_thd(THD *const thd) { return get_ha_data(thd)->get_trx(); }

void Transaction::set_trx_on_thd(THD *const thd) { return get_ha_data(thd)->set_trx(this); }

void Transaction::reset_trx_on_thd(THD *const thd) {
  get_ha_data(thd)->set_trx(nullptr);
  return destroy_ha_data(thd);
}

Transaction *Transaction::get_or_create_trx(THD *thd) {
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx == nullptr) {
    trx = new ShannonBase::Transaction(thd);
    trx->set_trx_on_thd(thd);
  }

  return trx;
}

void Transaction::free_trx_from_thd(THD *const thd) {
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx) {
    trx->reset_trx_on_thd(thd);
    delete trx;
    trx = nullptr;
  }
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

  ut_a(m_trx_impl);
  ut_a(m_trx_impl->state.load() == TRX_STATE_NOT_STARTED);
  m_trx_impl->isolation_level = is;

  m_trx_impl->auto_commit =
      m_thd != nullptr && !thd_test_options(m_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) && thd_is_query_block(m_thd);

  trx_start_if_not_started(m_trx_impl, (m_read_only ? false : true), UT_LOCATION_HERE);
  return SHANNON_SUCCESS;
}

int Transaction::begin_stmt(ISOLATION_LEVEL iso_level) { return SHANNON_SUCCESS; }

int Transaction::commit() {
  dberr_t error{DB_SUCCESS};
  if (trx_is_started(m_trx_impl)) {
    error = trx_commit_for_mysql(m_trx_impl);
  }
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback() {
  dberr_t error{DB_SUCCESS};
  if (trx_is_started(m_trx_impl)) {
    error = trx_rollback_for_mysql(m_trx_impl);
  }
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback_stmt() { return SHANNON_SUCCESS; }

int Transaction::rollback_to_savepoint(void *const savepoint) { return SHANNON_SUCCESS; }

void Transaction::set_read_only(bool read_only) { m_read_only = read_only; }

::ReadView *Transaction::acquire_snapshot() {
  if (!MVCC::is_view_active(m_trx_impl->read_view) && (m_trx_impl->isolation_level > TRX_ISO_READ_UNCOMMITTED)) {
    trx_assign_read_view(m_trx_impl);
  }

  return m_trx_impl->read_view;
}

int Transaction::release_snapshot() {
  if (m_trx_impl->read_view && MVCC::is_view_active(m_trx_impl->read_view)) {
    trx_sys->mvcc->view_close(m_trx_impl->read_view, false);
  }

  return SHANNON_SUCCESS;
}

::ReadView *Transaction::get_snapshot() const { return m_trx_impl->read_view; }

bool Transaction::has_snapshot() const { return MVCC::is_view_active(m_trx_impl->read_view); }

bool Transaction::is_auto_commit() { return m_trx_impl->auto_commit; }

bool Transaction::is_active() { return trx_is_started(m_trx_impl); }

bool Transaction::changes_visible(Transaction::ID trx_id, const char *table_name) {
  if (MVCC::is_view_active(m_trx_impl->read_view)) {
    table_name_t name;
    name.m_name = const_cast<char *>(table_name);
    return m_trx_impl->read_view->changes_visible(trx_id, name);
  }

  return false;
}

Transaction::ID Transaction::get_id() { return m_trx_impl->id; }

}  // namespace ShannonBase