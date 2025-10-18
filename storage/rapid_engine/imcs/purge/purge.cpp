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
//#include "include/os0event.h"
#include "sql/sql_class.h"
#include "storage/innobase/include/os0thread-create.h"

#include "storage/innobase/include/read0types.h"
#include "storage/innobase/include/trx0sys.h"

#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/purge/purge.h"
#include "storage/rapid_engine/include/rapid_status.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_purge_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
extern ulonglong rpd_min_versions_for_purge;
extern double rpd_purge_efficiency_threshold;
extern ulonglong rpd_max_purger_timeout;

namespace Populate {
extern std::shared_mutex g_processing_table_mutex;
extern std::multiset<std::string> g_processing_tables;
}  // namespace Populate
namespace Purge {
std::atomic<purge_state_t> Purger::m_state{purge_state_t::PURGE_STATE_EXIT};
std::mutex Purger::m_notify_mutex;
std::condition_variable Purger::m_notify_cv;
PurgeStats Purger::m_stats;
std::atomic<bool> Purger::m_immediate_purge{false};

// estimate how many history version can be purged.
size_t ChunkPurgeOptimizer::estimate_purgeable_versions(ShannonBase::Imcs::Chunk *chunk,
                                                        ShannonBase::ReadView::Snapshot_meta_unit *smu,
                                                        const ::ReadView *oldest_view) {
  if (!chunk || !smu || !oldest_view) return 0;

  size_t purgeable_cnt{0};
  for (const auto &[_, smu_items] : smu->version_info()) {
    for (const auto &version : smu_items.items) {
      // Check if this version is visible to the oldest active transaction
      // If visible, it means all active transactions can see it, so older versions can be purged
      auto tb_name = chunk->header()->m_owner->header()->m_owner->name();
      table_name_t table_name{const_cast<char *>(tb_name.c_str())};
      if (oldest_view->changes_visible(version.trxid, table_name)) {
        purgeable_cnt++;
      }
    }
  }
  return purgeable_cnt;
}

// Analyze chunks and prioritize purge candidates
std::vector<PurgeCandidate> ChunkPurgeOptimizer::analyze_purge_candidates(
    const std::vector<ShannonBase::Imcs::Chunk *> &chunks) {
  std::vector<PurgeCandidate> candidates;
  candidates.reserve(chunks.size());

  // Get oldest view once for all chunks to avoid repeated calls
  ::ReadView oldest_view;
  trx_sys->mvcc->clone_oldest_view(&oldest_view);

  for (auto &chunk : chunks) {
    if (!chunk || !chunk->header()) continue;

    auto smu = chunk->header()->m_smu.get();
    if (!smu) continue;

    size_t total_versions_cnt = smu->version_info().size();
    if (total_versions_cnt < ShannonBase::rpd_min_versions_for_purge) continue;

    size_t purgeable_versions_cnt = estimate_purgeable_versions(chunk, smu, &oldest_view);
    if (!purgeable_versions_cnt) continue;

    double efficiency = static_cast<double>(purgeable_versions_cnt) / (!total_versions_cnt) ? 1 : total_versions_cnt;
    if (efficiency < ShannonBase::rpd_purge_efficiency_threshold) continue;

    size_t estimated_bytes = purgeable_versions_cnt * chunk->normalized_pack_length();

    candidates.emplace_back(PurgeCandidate{
        chunk, total_versions_cnt, estimated_bytes,
        efficiency * estimated_bytes  // Score based on efficiency and bytes freed
    });
  }

  // Sort by efficiency score (highest first)
  std::sort(candidates.begin(), candidates.end(), std::greater<PurgeCandidate>());
  return candidates;
}

// Optimized purge worker with batching
static int purger_purge_worker(const std::vector<PurgeCandidate> &candidates) {
#if !defined(_WIN32)
  pthread_setname_np(pthread_self(), "rapid_purge_opt_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_opt_wkr");
#endif

  if (candidates.empty()) return ShannonBase::SHANNON_SUCCESS;

  uint64_t chunks_processed = 0;
  uint64_t versions_purged = 0;
  uint64_t bytes_freed = 0;

  for (const auto &candidate : candidates) {
    if (Purger::get_status() != purge_state_t::PURGE_STATE_RUN) {
      break;
    }

    auto start_time = std::chrono::steady_clock::now();

    // Perform the actual purge
    int result = candidate.chunk->purge();
    if (result == ShannonBase::SHANNON_SUCCESS) {
      chunks_processed++;
      bytes_freed += candidate.estimated_cleanup_bytes;

      // Update chunk's last GC timestamp
      candidate.chunk->header()->m_last_gc_tm = std::chrono::steady_clock::now();
    }

    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();

    // Log slow purges for monitoring
    if (duration > 1000) {  // > 1ms // Could add logging here
    }
  }

  // Update global statistics atomically
  Purger::m_stats.chunks_processed.fetch_add(chunks_processed);
  Purger::m_stats.versions_purged.fetch_add(versions_purged);
  Purger::m_stats.bytes_freed.fetch_add(bytes_freed);

  return ShannonBase::SHANNON_SUCCESS;
}

// main purge coordinator. First collecting all needed chunks, and puts into candidates vector
// , then start the workers to do real purge job.
static void purge_coordinator_main() {
#if !defined(_WIN32)
  pthread_setname_np(pthread_self(), "rapid_purge_coordinator_opt");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_purge_coordinator_opt");
#endif

  auto last_full_scan [[maybe_unused]] = std::chrono::steady_clock::now();
  constexpr auto FULL_SCAN_INTERVAL = std::chrono::minutes(5);

  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         Purger::get_status() == purge_state_t::PURGE_STATE_RUN) {
    {
      std::unique_lock<std::mutex> lk(Purger::m_notify_mutex);
      bool immediate_purge = Purger::m_immediate_purge.exchange(false);
      if (!immediate_purge) {
        Purger::m_notify_cv.wait_for(lk, std::chrono::milliseconds(ShannonBase::rpd_max_purger_timeout));
      }
      if (Purger::get_status() == purge_state_t::PURGE_STATE_STOP) {
        return;
      }
    }

    auto cycle_start = std::chrono::steady_clock::now();

    // Check if IMCS instance exists and has tables
    auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
    ut_a(imcs_instance);

    // Collect all chunks that need analysis
    std::vector<ShannonBase::Imcs::Chunk *> all_chunks;
    for (const auto &[_, table] : imcs_instance->get_tables()) {
      if (!table) continue;

      for (const auto &[_, field] : table->get_fields()) {
        if (!field || !field->chunks()) continue;

        for (size_t idx = 0; idx < field->chunks(); ++idx) {
          if (auto *chunk = field->chunk(idx)) {
            all_chunks.push_back(chunk);
          }
        }
      }
    }

    if (all_chunks.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    // Analyze and prioritize chunks for purging
    auto candidates = ChunkPurgeOptimizer::analyze_purge_candidates(all_chunks);

    if (candidates.empty()) {
      // No chunks need purging, wait longer
      std::this_thread::sleep_for(std::chrono::seconds(5));
      continue;
    }

    // Process candidates in batches using std::async
    const size_t max_workers =
        std::max(1UL, std::min(static_cast<size_t>(std::thread::hardware_concurrency() / 3), candidates.size()));

    std::vector<std::future<int>> futures;
    size_t batch_size = std::max(1UL, (candidates.size() + max_workers - 1) / max_workers);

    for (size_t i = 0; i < max_workers && i * batch_size < candidates.size(); ++i) {
      size_t start = i * batch_size;
      size_t end = std::min(start + batch_size, candidates.size());

      std::vector<PurgeCandidate> batch(candidates.begin() + start, candidates.begin() + end);

      futures.emplace_back(std::async(std::launch::async, purger_purge_worker, std::move(batch)));
    }

    // Wait for all workers to complete
    for (auto &future : futures) {
      future.get();
    }

    // Update statistics
    Purger::m_stats.purge_cycles.fetch_add(1);
    Purger::m_stats.last_purge_time = cycle_start;

    auto cycle_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cycle_start).count();

    // Adaptive timing: if purge cycle was quick, we can afford to run more frequently
    if (cycle_duration < 100) {  // Less than 100ms
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    } else if (cycle_duration < 1000) {  // Less than 1s
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
  }

  ut_a(Purger::get_status() == purge_state_t::PURGE_STATE_STOP);
}

// Implementation of Purger methods
void Purger::start() {
  if (!active() && shannon_loaded_tables->size()) {
    m_stats.reset();
    srv_threads.m_rapid_purg_cordinator = os_thread_create(rapid_purge_thread_key, 0, purge_coordinator_main);
    set_status(purge_state_t::PURGE_STATE_RUN);
    srv_threads.m_rapid_purg_cordinator.start();
    ut_a(active());
  }
}

void Purger::end() {
  if (active()) {
    set_status(purge_state_t::PURGE_STATE_STOP);
    m_notify_cv.notify_all();
    srv_threads.m_rapid_purg_cordinator.join();
    ut_a(!active());
  }
}

bool Purger::active() { return thread_is_active(srv_threads.m_rapid_purg_cordinator); }

void Purger::print_info(FILE *file) {
  const auto &stats = get_stats();
  fprintf(file, "Rapid Purge Statistics:\n");
  fprintf(file, "  Purge cycles: %lu\n", stats.purge_cycles.load());
  fprintf(file, "  Chunks processed: %lu\n", stats.chunks_processed.load());
  fprintf(file, "  Versions purged: %lu\n", stats.versions_purged.load());
  fprintf(file, "  Bytes freed: %lu\n", stats.bytes_freed.load());

  auto now = std::chrono::steady_clock::now();
  auto last_purge_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats.last_purge_time).count();
  fprintf(file, "  Time since last purge: %ldms\n", last_purge_ms);
}

// to check if table is currently being pop. table_name format: "schema_name/table_name"
bool Purger::check_pop_status(std::string &table_name) {
  std::shared_lock lk(ShannonBase::Populate::g_processing_table_mutex);
  if (ShannonBase::Populate::g_processing_tables.find(table_name) != ShannonBase::Populate::g_processing_tables.end())
    return true;
  else
    return false;
}

}  // namespace Purge
}  // namespace ShannonBase
