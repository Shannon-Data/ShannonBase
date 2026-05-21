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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs rapid.
*/
#ifndef __SHANNONBASE_RAPID_MONITOR_H__
#define __SHANNONBASE_RAPID_MONITOR_H__

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ShannonBase {
namespace Populate {
class PopulatorImpl;
}
namespace RapidMonitor {
struct Metrics {
  bool rapid_pop_thread_running{false};
  uint64_t rapid_pop_loop_counter{0};
  uint64_t rapid_pop_data_sz{0};
  size_t total_buffer_tables{0};
  size_t tables_in_progress{0};
  size_t total_worker_threads{0};
  uint64_t worker_pending_bytes{0};
  size_t loaded_tables{0};
  size_t bg_pool_queue_size{0};
  size_t bg_active_workers{0};
  size_t bg_total_workers{0};
  uint32_t bg_concurrent_gc{0};
  uint32_t bg_concurrent_compact{0};
  uint32_t bg_concurrent_stats{0};
};

void collect_rapid_monitor_metrics(Metrics &metrics);
void print_rapid_monitor_info(FILE *file);
}  // namespace RapidMonitor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_MONITOR_H__
