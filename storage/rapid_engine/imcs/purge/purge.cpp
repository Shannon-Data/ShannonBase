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

#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/purge/purge.h"
#include "storage/rapid_engine/include/rapid_status.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_purge_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Purge {

std::atomic<purge_state_t> Purger::m_state{Purge::purge_state_t::PURGE_STATE_EXIT};
std::mutex Purger::m_notify_mutex;
std::condition_variable Purger::m_notify_cv;

static int purger_purge_worker(ShannonBase::Imcs::Cu *cu_ptr) {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_purge_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_wkr");
#endif

  if (!cu_ptr || (cu_ptr && !cu_ptr->chunks()))  // empty cu.
    return ShannonBase::SHANNON_SUCCESS;

  for (auto idx = 0u; idx < cu_ptr->chunks(); idx++) {
    auto chunk_ptr = cu_ptr->chunk(idx);
    if (!chunk_ptr) continue;
    chunk_ptr->purge();
  }

  return ShannonBase::SHANNON_SUCCESS;
}

// purge coordinator main func entry.
static void purge_func_main() {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_purge_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_coordinator");
#endif

  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         ShannonBase::Purge::Purger::get_status() == purge_state_t::PURGE_STATE_RUN) {
    std::unique_lock<std::mutex> lk(Purger::m_notify_mutex);
    Purger::m_notify_cv.wait_for(lk, std::chrono::milliseconds(MAX_PURGER_TIMEOUT));
    if (ShannonBase::Purge::Purger::get_status() == purge_state_t::PURGE_STATE_STOP) return;

    auto start = std::chrono::steady_clock::now();
    auto loaded_cu_sz = ShannonBase::Imcs::Imcs::instance()->get_cus().size();

    // we only use a third of threads to do purge opers.
    std::vector<std::future<int>> results;
    size_t thread_num = std::thread::hardware_concurrency() / 3;
    thread_num = std::min(thread_num, loaded_cu_sz);

    size_t thr_cnt = 0;
    std::shared_lock<std::shared_mutex> lk_cu(ShannonBase::Imcs::Imcs::instance()->get_cu_mutex());
    for (auto &cu : ShannonBase::Imcs::Imcs::instance()->get_cus()) {
      results.emplace_back(std::async(std::launch::async, purger_purge_worker, cu.second.get()));
      thr_cnt++;

      if (thr_cnt % thread_num == 0) {
        for (auto &fut : results) fut.get();
        results.clear();
      }
    }
    for (auto &fut : results) fut.get();

    // in duration, we finish the GC operations.
    auto duration [[maybe_unused]] =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  }

  ut_a(ShannonBase::Purge::Purger::get_status() == purge_state_t::PURGE_STATE_STOP);
}

bool Purger::active() { return thread_is_active(srv_threads.m_rapid_purg_cordinator); }

void Purger::start() {
  if (!Purger::active() && shannon_loaded_tables->size()) {
    srv_threads.m_rapid_purg_cordinator = os_thread_create(rapid_purge_thread_key, 0, purge_func_main);
    Purger::set_status(purge_state_t::PURGE_STATE_RUN);
    srv_threads.m_rapid_purg_cordinator.start();

    ut_a(Purger::active());
  }
}

void Purger::end() {
  if (Purger::active() && !shannon_loaded_tables->size()) {
    Purge::Purger::set_status(purge_state_t::PURGE_STATE_STOP);
    Purger::m_notify_cv.notify_all();
    srv_threads.m_rapid_purg_cordinator.join();
    ut_a(Purger::active() == false);
  }
}

void Purger::print_info(FILE *file) { /* in: output stream */
}

bool Purger::check_pop_status(std::string &table_name) { return false; }

}  // namespace Purge
}  // namespace ShannonBase