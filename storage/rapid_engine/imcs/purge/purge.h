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
#ifndef __SHANNONBASE_PURGE_H__
#define __SHANNONBASE_PURGE_H__
#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "storage/innobase/include/os0thread.h"  //IBThread
namespace ShannonBase {
namespace Purge {

/** Default value of spin delay (in spin rounds)
 * 1000 spin round takes 4us,  25000 takes 1ms for busy waiting. therefore, 200ms means
 * 5000000 spin rounds. for the more detail infor ref to : comment of
 * `innodb_log_writer_spin_delay`.
 */
constexpr uint64_t MAX_PURGER_SPINS = 5000000;
constexpr uint64_t MAX_PURGER_TIMEOUT = 10000;

extern std::atomic<bool> sys_purge_started;
using purge_func_t = std::function<void(void)>;

enum class purge_state_t {
  PURGE_STATE_INIT,    /*!< Purge instance created */
  PURGE_STATE_RUN,     /*!< Purge should be running */
  PURGE_STATE_STOP,    /*!< Purge should be stopped */
  PURGE_STATE_EXIT,    /*!< Purge has been shutdown */
  PURGE_STATE_DISABLED /*!< Purge was never started */
};

class Purger {
 public:
  // whether the log pop main thread is active or not. true is alive, false dead.
  static bool active();

  // to launch log pop main thread.
  static void start();

  // to stop lop pop main thread.
  static void end();

  // to print thread infos.
  static void print_info(FILE *file);

  // to check whether the specific table are still do populating.
  static bool check_pop_status(std::string &table_name);

  // to send notify to populator main thread to start do propagation.
  static void send_notify();

  static void wait_for_notify();

  static void set_status(purge_state_t stat);

  static purge_state_t get_status();

  static std::mutex m_notify_mutex;
  static std::condition_variable m_notify_cv;
  static std::atomic<bool> m_notify_flag;

 private:
  static std::mutex m_state_mutex;
  static purge_state_t m_state;

  // purge workers.
  IB_thread *m_purge_workers;
};

}  // namespace Purge
}  // namespace ShannonBase
#endif  // __SHANNONBASE_PURGE_H__