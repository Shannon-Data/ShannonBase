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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The purge is used to gc the unused data by any inactive
   transaction. it's usually deleted data.
*/
#if !defined(_WIN32)
#include <pthread.h>  // For pthread_setname_np
#else
#include <Windows.h>  // For SetThreadDescription
#endif
#include <chrono>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

#include "current_thd.h"
#include "include/os0event.h"
#include "sql/sql_class.h"
#include "storage/innobase/include/os0thread-create.h"

#include "storage/innobase/include/srv0shutdown.h"
#include "storage/innobase/include/srv0srv.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/populate/purge.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_purge_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Purge {

std::atomic<bool> sys_purge_started{false};

static void purge_func_main() {
  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         sys_purge_started.load(std::memory_order_acquire)) {
    // thread stopped.
    if (unlikely(!sys_purge_started.load(std::memory_order_acquire))) break;
  }

  sys_purge_started.store(false, std::memory_order_seq_cst);
}

void start_rapid_purge_threads() {
  if (!Purger::active()) {
    srv_threads.m_rapid_purg_cordinator = os_thread_create(rapid_purge_thread_key, 0, purge_func_main);
    ShannonBase::Purge::sys_purge_started.store(false, std::memory_order_seq_cst);
    srv_threads.m_rapid_purg_cordinator.start();
  }
}

bool Purger::active() { return thread_is_active(srv_threads.m_rapid_purg_cordinator); }

void Purger::send_notify() { os_event_set(log_sys->rapid_events[0]); }

void Purger::start() {
  if (!Purger::active()) {
    srv_threads.m_rapid_purg_cordinator = os_thread_create(rapid_purge_thread_key, 0, purge_func_main);
    ShannonBase::Purge::sys_purge_started.store(false, std::memory_order_seq_cst);
    srv_threads.m_rapid_purg_cordinator.start();
  }
}

void Purger::end() {
  if (Purger::active()) {
    sys_purge_started.store(false, std::memory_order_seq_cst);
    os_event_set(log_sys->rapid_events[0]);
    srv_threads.m_rapid_purg_cordinator.join();

    assert(Purger::active() == false);

    srv_threads.m_rapid_purg_cordinator.join();
  }
}

void Purger::print_info(FILE *file) { /* in: output stream */
}

bool Purger::check_status(std::string &table_name) { return false; }

}  // namespace Purge
}  // namespace ShannonBase