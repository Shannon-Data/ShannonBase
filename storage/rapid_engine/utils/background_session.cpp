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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/utils/background_session.h"
#include <new>
#include "sql/log.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/transaction.h"
#include "storage/rapid_engine/trx/transaction.h"

namespace ShannonBase::Utils {
BackgroundSession::BackgroundSession() {
  my_thread_init();

  m_thd = new (std::nothrow) THD;
  if (!m_thd) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "BackgroundSession: failed to allocate THD");
    return;
  }
  m_thd->set_new_thread_id();
  m_thd->thread_stack = reinterpret_cast<char *>(this);
  m_thd->set_command(COM_DAEMON);
  m_thd->security_context()->skip_grants();
  m_thd->system_thread = NON_SYSTEM_THREAD;
  m_thd->store_globals();
  m_thd->lex->sql_command = SQLCOM_SELECT;
}

BackgroundSession::~BackgroundSession() {
  if (!m_thd) {
    my_thread_end();
    return;
  }

  // Internal readers lock handlers outside MYSQL_LOCK. Release each lock
  // once, then close every TABLE even during shutdown: discarding open_tables
  // leaks InnoDB dictionary references. pre_dd_shutdown joins these sessions
  // before dictionary/cache destruction.
  for (TABLE *table = m_thd->open_tables; table; table = table->next)
    if (table->file && table->file->get_lock_type() != F_UNLCK) table->file->ha_external_lock(m_thd, F_UNLCK);
  trans_rollback_stmt(m_thd);
  trans_rollback(m_thd);
  ShannonBase::Transaction::free_trx_from_thd(m_thd);
  if (m_thd->open_tables) close_thread_tables(m_thd);
  m_thd->mdl_context.release_statement_locks();
  m_thd->mdl_context.release_transactional_locks();
  m_thd->release_resources();
  delete m_thd;
  m_thd = nullptr;
  my_thread_end();
}

}  // namespace ShannonBase::Utils
