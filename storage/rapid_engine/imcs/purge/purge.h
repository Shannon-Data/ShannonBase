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
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/trx/readview.h"

class ReadView;
namespace ShannonBase {
namespace Purge {

using purge_func_t = std::function<void(void)>;

enum class purge_state_t {
  PURGE_STATE_INIT,    /*!< Purge instance created */
  PURGE_STATE_RUN,     /*!< Purge should be running */
  PURGE_STATE_STOP,    /*!< Purge should be stopped */
  PURGE_STATE_EXIT,    /*!< Purge has been shutdown */
  PURGE_STATE_DISABLED /*!< Purge was never started */
};

// statistics info of purger workers.
struct PurgeStats {
  std::atomic<uint64_t> chunks_processed{0};
  std::atomic<uint64_t> versions_purged{0};
  std::atomic<uint64_t> bytes_freed{0};
  std::atomic<uint64_t> purge_cycles{0};
  std::chrono::steady_clock::time_point last_purge_time;

  void reset() {
    chunks_processed.store(0);
    versions_purged.store(0);
    bytes_freed.store(0);
    purge_cycles.store(0);
    last_purge_time = std::chrono::steady_clock::now();
  }
};

class Purger {
 public:
  // to launch log pop main thread.
  static void start();

  // to stop lop pop main thread.
  static void end();

  // whether the log pop main thread is active or not. true is alive, false dead.
  static bool active();

  // to print thread infos.
  static void print_info(FILE *file);

  // to check whether the specific table are still do populating.
  static bool check_pop_status(std::string &table_name);

  // Enhanced status management
  static inline void set_status(purge_state_t stat) { m_state.store(stat, std::memory_order_release); }

  static inline purge_state_t get_status() { return m_state.load(std::memory_order_acquire); }

  // Get purge statistics
  static const PurgeStats &get_stats() { return m_stats; }

  // Force immediate purge cycle
  static void trigger_purge() {
    std::unique_lock<std::mutex> lock(m_notify_mutex);
    m_immediate_purge = true;
    m_notify_cv.notify_all();
  }

  static std::atomic<purge_state_t> m_state;
  static std::mutex m_notify_mutex;
  static std::condition_variable m_notify_cv;
  static PurgeStats m_stats;
  static std::atomic<bool> m_immediate_purge;
};

// chunk purge logic
struct PurgeCandidate {
  ShannonBase::Imcs::Chunk *chunk;
  size_t version_count;
  size_t estimated_cleanup_bytes;
  double efficiency_score;

  bool operator>(const PurgeCandidate &other) const { return efficiency_score > other.efficiency_score; }
};

class ChunkPurgeOptimizer {
 public:
  // Analyze chunks and prioritize purge candidates
  static std::vector<PurgeCandidate> analyze_purge_candidates(const std::vector<ShannonBase::Imcs::Chunk *> &);

 private:
  static size_t estimate_purgeable_versions(ShannonBase::Imcs::Chunk *, ShannonBase::ReadView::Snapshot_meta_unit *,
                                            const ::ReadView *);
};

}  // namespace Purge
}  // namespace ShannonBase
#endif  // __SHANNONBASE_PURGE_H__
