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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/

#include "storage/rapid_engine/populate/populate.h"

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

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/populate/log_parser.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {
// should be pay more attention to syc relation between this thread
// and log_buffer write. if we stop changes poping, should stop writing
// firstly, then stop this thread.
std::unordered_map<uint64_t, mtr_log_rec> sys_pop_buff;

std::atomic<bool> sys_pop_started{false};
// how many data was in sys_pop_buff?
std::atomic<uint64> sys_pop_data_sz{0};
static uint64 sys_rapid_loop_count{0};

/**
 * return lsn_t, key of processing mtr_log_rec_t.
 */
static uint64_t parse_mtr_log_worker(uint64_t start_lsn, const byte *start, const byte *end, size_t sz) {
  THD *log_pop_thread_thd{nullptr};
  if (current_thd == nullptr) {
    my_thread_init();
    log_pop_thread_thd = create_internal_thd();
    if (!log_pop_thread_thd) {
      my_thread_end();
      return start_lsn;
    }
  }

#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_log_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_log_wkr");
#endif
  SHANNON_THREAD_LOCAL LogParser parse_log;
  SHANNON_THREAD_LOCAL Rapid_load_context context;

  auto parsed_bytes = parse_log.parse_redo(&context, const_cast<byte *>(start), const_cast<byte *>(end));
  ut_a(parsed_bytes == sz);

  if (log_pop_thread_thd) {
    my_thread_end();
    destroy_internal_thd(log_pop_thread_thd);
    log_pop_thread_thd = nullptr;
  }

  return (parsed_bytes == sz) ? start_lsn : 0;
}

/**
 * main entry of pop thread. it monitors sys_pop_buff, a new mtr_log_rect_t
 * is coming, then it starts a new worker to dealing with this mtr_log_rec_t.
 */
static void parse_log_func_main(log_t *log_ptr) {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_log_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_log_coordinator");
#endif

  // here we have a notifiyer, start pop. ref: https://dev.mysql.com/doc/heatwave/en/mys-hw-change-propagation.html
  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         sys_pop_started.load(std::memory_order_acquire)) {
    auto stop_condition = [&](bool wait) {
      if (sys_pop_data_sz.load(std::memory_order_acquire) >= SHANNON_POPULATION_HRESHOLD_SIZE) {
        return true;
      }

      if (unlikely(wait)) {
        os_event_set(log_sys->rapid_events[0]);
        return true;
      }
      return false;
    };

    // waiting until the 200ms reached or the incomming data in buffer more than 64MB.
    os_event_wait_for(log_ptr->rapid_events[0], MAX_LOG_POP_SPINS, std::chrono::microseconds{MAX_WAIT_TIMEOUT},
                      stop_condition);
    os_event_reset(log_sys->rapid_events[0]);

    // thread stopped.
    if (unlikely(!sys_pop_started.load(std::memory_order_acquire))) break;

    // pop buffer is empty, then re-check the condtion.
    if (likely(sys_pop_buff.empty())) continue;

    // we only use a half of threads to do propagation.
    std::vector<std::future<uint64_t>> results;
    size_t thread_num = std::thread::hardware_concurrency() / 2;
    thread_num = (thread_num >= sys_pop_buff.size()) ? sys_pop_buff.size() : thread_num;
    auto curr_iter = sys_pop_buff.begin();
    for (size_t counter = 0; counter < thread_num; counter++) {
      mutex_enter(&log_sys->rapid_populator_mutex);
      byte *from_ptr = curr_iter->second.data.get();
      auto size = curr_iter->second.size;

      // using std thread, not IB_thread, ib_thread has not interface to thread func ret.
      results.emplace_back(
          std::async(std::launch::async, parse_mtr_log_worker, curr_iter->first, from_ptr, from_ptr + size, size));
      curr_iter++;
      mutex_exit(&log_sys->rapid_populator_mutex);
    }

    for (auto &res : results) {  // gets the result from worker thread.
      auto ret_lsn = res.get();
      if (!ret_lsn) {  // propagation failure occure.
        std::stringstream error_message;
        error_message << "Propagation thread occur errors, it makes loaded data to be stale"
                      << ". Please unload loaded tables, and then load tables again manually.";
        push_warning(current_thd, Sql_condition::SL_WARNING, errno, error_message.str().c_str());
      }

      mutex_enter(&log_sys->rapid_populator_mutex);
      auto iter = sys_pop_buff.find(ret_lsn);
      if (iter != sys_pop_buff.end()) {
        sys_pop_data_sz.fetch_sub(iter->second.size);
        sys_pop_buff.erase(ret_lsn);
      }
      mutex_exit(&log_sys->rapid_populator_mutex);
    }

    sys_rapid_loop_count++;
  }

  sys_pop_started.store(false, std::memory_order_seq_cst);
}

bool Populator::active() { return thread_is_active(srv_threads.m_change_pop_cordinator); }

void Populator::send_notify() { os_event_set(log_sys->rapid_events[0]); }

void Populator::start() {
  if (!Populator::active() && shannon_loaded_tables->size()) {
    srv_threads.m_change_pop_cordinator = os_thread_create(rapid_populate_thread_key, 0, parse_log_func_main, log_sys);
    ShannonBase::Populate::sys_pop_started.store(true, std::memory_order_seq_cst);
    srv_threads.m_change_pop_cordinator.start();
    ut_a(Populator::active());
  }
}

void Populator::end() {
  if (Populator::active() && !shannon_loaded_tables->size()) {
    sys_pop_started.store(false, std::memory_order_seq_cst);
    os_event_set(log_sys->rapid_events[0]);
    srv_threads.m_change_pop_cordinator.join();
    sys_rapid_loop_count = 0;

    g_index_cache.clear();
    g_index_names.clear();

    ut_a(Populator::active() == false);
  }
}

void Populator::print_info(FILE *file) { /* in: output stream */
  fprintf(file,
          "rapid log pop thread : %s \n"
          "rapid log pop thread loops: " ULINTPF
          "\n"
          "rapid log data remaining size: " ULINTPF
          " KB\n"
          "rapid log data remaining line: " ULINTPF "\n",
          ShannonBase::Populate::sys_pop_started ? "running" : "stopped", ShannonBase::Populate::sys_rapid_loop_count,
          ShannonBase::Populate::sys_pop_data_sz / 1024, ShannonBase::Populate::sys_pop_buff.size());
}

bool Populator::check_status(std::string &table_name) { return false; }

}  // namespace Populate
}  // namespace ShannonBase