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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/

#include "storage/rapid_engine/populate/populate.h"

#include "current_thd.h"
#include "sql/sql_class.h"
#include "include/os0event.h"

#include "storage/rapid_engine/populate/log_parser.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {

std::atomic<bool> pop_started {false};
IB_thread Populator::log_rapid_thread;
uint64 population_buffer_size = m_pop_buff_size;
std::unique_ptr<Ringbuffer<byte>> population_buffer {nullptr};

static void parse_log_func (log_t *log_ptr) {
  current_thd = (current_thd == nullptr) ? new THD(false) : current_thd;
  THR_MALLOC = (THR_MALLOC == nullptr) ? &current_thd->mem_root : THR_MALLOC;

  os_event_reset(log_ptr->rapid_events[0]);
   //here we have a notifiyer, when checkpoint_lsn/flushed_lsn > rapid_lsn to start pop
  while (pop_started.load(std::memory_order_seq_cst)) {
    auto stop_condition = [&](bool wait) {
      if (population_buffer->readAvailable()) {
        return true;
      }
      if (wait) { //do somthing in waiting
      }
      return false;
    };

    os_event_wait_for(log_ptr->rapid_events[0], MAX_LOG_POP_SPIN_COUNT,
                      std::chrono::microseconds{100}, stop_condition);

    auto size = population_buffer->readAvailable();
    byte* from_ptr = population_buffer->peek();
    LogParser parse_log;
    uint parsed_bytes = parse_log.parse_redo(from_ptr, from_ptr + size);
    population_buffer->remove(parsed_bytes);
  } //wile(pop_started)

  pop_started.store(!pop_started, std::memory_order_seq_cst);
  THR_MALLOC = nullptr;
  if (current_thd) {
    delete current_thd;
    current_thd = nullptr;
  }
}

bool Populator::log_rapid_is_active() {
   return (thread_is_active(Populator::log_rapid_thread)); 
}

void Populator::start_change_populate_threads(log_t* log) {
  Populator::log_rapid_thread =
      os_thread_create(rapid_populate_thread_key, 0, parse_log_func, log);

  ShannonBase::Populate::pop_started = true;
  Populator::log_rapid_thread.start();
}

}  // namespace Populate
}  // namespace ShannonBase