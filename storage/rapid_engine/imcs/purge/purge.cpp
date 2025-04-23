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

std::atomic<bool> sys_purge_started{false};
std::mutex Purger::m_state_mutex;
purge_state_t Purger::m_state{Purge::purge_state_t::PURGE_STATE_EXIT};

std::mutex Purger::m_notify_mutex;
std::condition_variable Purger::m_notify_cv;
std::atomic<bool> Purger::m_notify_flag{false};

static uint64_t purger_purge_worker(ShannonBase::Imcs::Cu *cu_ptr) {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_purge_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_wkr");
#endif

  if (!cu_ptr || (cu_ptr && !cu_ptr->chunks())) return 0;
  for (auto idx = 0u; idx < cu_ptr->chunks(); idx++) {
    auto chunk_ptr = cu_ptr->chunk(idx);
    chunk_ptr->GC();
  }

  return 0;
}

// purge coordinator main func entry.
static void purge_func_main() {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_purge_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_coordinator");
#endif

  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         sys_purge_started.load(std::memory_order_acquire)) {
    {  // wait for event or timeout.
      std::unique_lock<std::mutex> lock(Purger::m_notify_mutex);
      Purger::m_notify_cv.wait_for(lock, std::chrono::milliseconds(MAX_PURGER_TIMEOUT),
                                   [] { return Purger::m_notify_flag.load(std::memory_order_acquire); });
      Purger::m_notify_flag.store(false, std::memory_order_release);
    }

    auto start = std::chrono::steady_clock::now();
    auto loaded_cu_sz = ShannonBase::Imcs::Imcs::instance()->get_cus().size();

    // we only use a half of threads to do propagation.
    std::vector<std::future<uint64_t>> results;
    size_t thread_num = std::thread::hardware_concurrency() / 2;
    thread_num = std::min(thread_num, loaded_cu_sz);

    auto cit = ShannonBase::Imcs::Imcs::instance()->get_cus().begin();
    while (cit != ShannonBase::Imcs::Imcs::instance()->get_cus().end()) {
      std::vector<std::future<uint64_t>> results;

      for (size_t thr_ind = 0; thr_ind < thread_num && cit != ShannonBase::Imcs::Imcs::instance()->get_cus().end();
           ++thr_ind, ++cit) {
        results.emplace_back(std::async(std::launch::async, purger_purge_worker, cit->second.get()));
      }

      for (auto &res : results) {
        if (srv_shutdown_state.load(std::memory_order_acquire) != SRV_SHUTDOWN_NONE) break;
        res.get();
      }
    }

    // in duration, we finish the GC operations.
    auto duration [[maybe_unused]] =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // thread stopped.
    if (unlikely(!sys_purge_started.load(std::memory_order_acquire))) break;
  }

  sys_purge_started.store(false, std::memory_order_seq_cst);
}

void Purger::set_status(purge_state_t stat) {
  std::scoped_lock lk(Purger::m_state_mutex);
  Purger::m_state = stat;
}

purge_state_t Purger::get_status() {
  std::scoped_lock lk(Purger::m_state_mutex);
  return Purger::m_state;
}

bool Purger::active() { return thread_is_active(srv_threads.m_rapid_purg_cordinator); }

void Purger::send_notify() {
  {
    std::lock_guard<std::mutex> lock(m_notify_mutex);
    m_notify_flag.store(true, std::memory_order_release);
  }
  m_notify_cv.notify_one();
}

void Purger::start() {
  if (!Purger::active() && shannon_loaded_tables->size()) {
    srv_threads.m_rapid_purg_cordinator = os_thread_create(rapid_purge_thread_key, 0, purge_func_main);
    ShannonBase::Purge::sys_purge_started.store(true, std::memory_order_seq_cst);
    srv_threads.m_rapid_purg_cordinator.start();
    Purger::set_status(purge_state_t::PURGE_STATE_RUN);
    ut_a(Purger::active());
  }
}

void Purger::end() {
  if (Purger::active() && shannon_loaded_tables->size()) {
    sys_purge_started.store(false, std::memory_order_seq_cst);
    srv_threads.m_rapid_purg_cordinator.join();
    Purge::Purger::set_status(purge_state_t::PURGE_STATE_STOP);
    assert(Purger::active() == false);
  }
}

void Purger::print_info(FILE *file) { /* in: output stream */
}

bool Purger::check_pop_status(std::string &table_name) { return false; }

}  // namespace Purge
}  // namespace ShannonBase