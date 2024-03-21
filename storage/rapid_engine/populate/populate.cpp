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

#include "storage/rapid_engine/populate/log_parser.h"
#include "storage/innobase/include/os0thread-create.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {

IB_thread Populator::log_rapid_thread;
uint64 population_buffer_size{SHANNON_MAX_POPULATION_BUFFER_SIZE};
static LogParser parse_log;
static void parse_log_func (log_t *log_ptr, lsn_t rapid_start_lsn) {

   parse_log.parse_redo(log_ptr, rapid_start_lsn, log_ptr->flushed_to_disk_lsn);
}

void Populator::start_change_populate_threads(log_t &log) {
  Populator::log_rapid_thread =
      os_thread_create(rapid_populate_thread_key, 0, parse_log_func, &log, 0);

  Populator::log_rapid_thread.start();
}

}  // namespace Populate
}  // namespace ShannonBase