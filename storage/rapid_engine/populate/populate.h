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

#ifndef __SHANNONBASE_POPULATE_H__
#define __SHANNONBASE_POPULATE_H__

#include <memory>
#include "my_inttypes.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/innobase/include/os0thread-create.h"

class log_t;
namespace ShannonBase {
namespace Populate {

#define log_rapid_pop_mutex_enter(log) mutex_enter(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_enter_nowait(log) \
  mutex_enter_nowait(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_exit(log) mutex_exit(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_own(log) \
  (mutex_own(&((log).rapid_populator_mutex)) || !Populator::log_rapid_is_active())


extern uint64 population_buffer_size;
extern std::unique_ptr<Ringbuffer<byte>> population_buffer;
extern std::atomic<bool> pop_started;

class Populator {
 public:
  static bool log_rapid_is_active();
  static void start_change_populate_threads(log_t* log);
  static IB_thread log_rapid_thread;
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_POPULATE_H__