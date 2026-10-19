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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for recovery.
*/
#include "storage/rapid_engine/recovery/recovery.h"
#include <algorithm>
#include <cctype>
#include "include/my_dbug.h"
#include "sql/log.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/utils/try_operation.h"

namespace ShannonBase {
namespace Recovery {
RecoveryManager::RecoveryManager(std::string base_dir) : m_base_dir(std::move(base_dir)) {}

RecoveryManager::~RecoveryManager() {
  std::lock_guard lk(m_mutex);
  for (auto &[k, slot] : m_per_table)
    if (slot && slot->mgr) slot->mgr->close();
}

std::filesystem::path RecoveryManager::table_dir(const std::string &db, const std::string &tbl) const {
  return std::filesystem::path(m_base_dir) / db / tbl;
}

std::shared_ptr<Imcs::TablePersistenceManager> RecoveryManager::get_table_mgr(const std::string &db,
                                                                              const std::string &tbl) {
  const std::string key = db + '\x01' + tbl;
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard lk(m_mutex);
    auto &entry = m_per_table[key];
    if (!entry) {
      entry = std::make_shared<Slot>();
      entry->mgr = std::make_shared<Imcs::TablePersistenceManager>(m_base_dir, db, tbl);
    }
    slot = entry;
  }
  // open() reads both journals end to end; do it without the global mutex.
  std::call_once(slot->opened, [&] {
    if (!slot->mgr->open()) {
      slot->mgr->require_recovery();
      std::string log_msg = "RecoveryManager: could not open checkpoint storage for " + db + "." + tbl +
                            "; Rapid writes are disabled until recovery succeeds";
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    }
  });
  return slot->mgr;
}

std::shared_ptr<Imcs::TablePersistenceManager> RecoveryManager::table_manager(const std::string &db,
                                                                              const std::string &tbl) {
  return get_table_mgr(db, tbl);
}

bool RecoveryManager::checkpoint_imcu(const std::string &db, const std::string &tbl, Imcs::Imcu *imcu, uint64_t /*scn*/,
                                      bool *deferred) {
  if (deferred) *deferred = false;
  if (!imcu) return false;
  auto mgr = get_table_mgr(db, tbl);
  if (!mgr->checkpoint(imcu, 0, deferred)) return false;

  return true;
}

bool RecoveryManager::has_durable_checkpoint(const std::string &db, const std::string &tbl) {
  std::error_code ec;
  const auto dir = table_dir(db, tbl) / "checkpoints";
  if (!std::filesystem::is_directory(dir, ec)) return false;
  for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
    if (!ec && e.path().extension() == ".manifest") return true;
  }
  return false;
}

bool RecoveryManager::load_from_snapshots(const std::string &db, const std::string &tbl, Imcs::RpdTable *rpd_table,
                                          uint64_t *restored_generation) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_snapshot_load_error", { return false; });

  if (!rpd_table) return false;

  std::error_code ec;
  const auto dir = table_dir(db, tbl);
  if (!std::filesystem::is_directory(dir, ec)) return false;

  auto mgr = get_table_mgr(db, tbl);
  // get_table_mgr retains a failed-open manager to block writes. Do not bypass
  // that failure by reading checkpoints directly (including a failed power cut).
  if (mgr->recovery_required()) return false;
  // A reload-required marker outranks any certificate on disk: the instance
  // that wrote it could not prove the image safe, and the marker lives outside
  // the capture journal so it is still readable when that journal is unusable.
  if (mgr->recovery_tainted()) {
    DBUG_PRINT("recovery", ("%s.%s is marked reload-required; rebuilding from InnoDB", db.c_str(), tbl.c_str()));
    return false;
  }
  auto &meta = rpd_table->meta();
  auto mem_pool = rpd_table->get_memory_pool();

  // Authoritative IMCU topology comes from the newest valid manifest; fall
  // back to older generations when the newest is corrupt (recover() selects
  // the actual generation to load from).
  std::vector<uint32_t> imcu_ids;
  {
    const auto gens = mgr->list_manifest_generations();  // ascending
    for (auto it = gens.rbegin(); it != gens.rend() && imcu_ids.empty(); ++it) {
      auto mres = mgr->load_manifest(*it);
      if (mres.ok())
        for (const auto &e : mres.value.imcus) imcu_ids.push_back(e.imcu_id);
    }
    if (imcu_ids.empty()) return false;  // no valid checkpoint generation → slow lane
  }

  std::sort(imcu_ids.begin(), imcu_ids.end());
  imcu_ids.erase(std::unique(imcu_ids.begin(), imcu_ids.end()), imcu_ids.end());

  std::vector<std::shared_ptr<Imcs::Imcu>> imcu_holders;
  std::vector<Imcs::Imcu *> imcu_ptrs;
  imcu_holders.reserve(imcu_ids.size());
  imcu_ptrs.reserve(imcu_ids.size());
  for (const uint32_t imcu_id : imcu_ids) {
    auto imcu = rpd_table->locate_imcu(imcu_id);
    if (!imcu) {
      const row_id_t start = static_cast<row_id_t>(imcu_id) * meta.rows_per_imcu;
      imcu = std::make_shared<Imcs::Imcu>(rpd_table, meta, start, meta.rows_per_imcu, mem_pool);
      rpd_table->add_imcu(imcu);
    }
    imcu_holders.push_back(imcu);
    imcu_ptrs.push_back(imcu.get());
  }

  // recover() validates and loads the entire native column generation.
  auto recover_result = mgr->recover(imcu_ptrs, restored_generation);

  if (!recover_result.ok()) {
    DBUG_PRINT("recovery",
               ("RecoveryManager: %s.%s — checkpoint validation failed, recovery failed", db.c_str(), tbl.c_str()));
    return false;
  }

  // Replay wrote cells straight into the CUs, so the IMCU zone maps and the
  // table-level column statistics still describe the checkpoint rather than the
  // recovered state. Rebuild them before the table serves a query, otherwise
  // the optimizer would prune against min/max values that predate every
  // mutation just replayed.
  rpd_table->update_statistics(true);

  DBUG_PRINT("recovery", ("RecoveryManager: %s.%s — %zu IMCU(s), %zu snapshot(s) restored", db.c_str(), tbl.c_str(),
                          imcu_ptrs.size(), recover_result.value));
  return true;
}

namespace {
// Directory names are whatever the server handed us; with lower_case_table_names=2 a
// DML can use a different case than the directory, so match case-insensitively.
std::string pending_key(const std::string &db, const std::string &tbl) {
  std::string key = db + '\x01' + tbl;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return key;
}
}  // namespace

void RecoveryManager::purge_table(const std::string &db, const std::string &tbl) {
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard lk(m_mutex);
    const std::string key = db + '\x01' + tbl;
    auto it = m_per_table.find(key);
    if (it != m_per_table.end()) {
      slot = std::move(it->second);
      m_per_table.erase(it);
    }
  }
  if (slot && slot->mgr) slot->mgr->close();
  clear_recovery_pending(db, tbl);

  // The reload-required marker has a second copy outside the table directory;
  // removing the directory alone would leave it behind to force a pointless
  // reload of whatever table is later created under the same name.
  Imcs::TablePersistenceManager scratch(m_base_dir, db, tbl);
  if (!scratch.erase_store()) {
    (void)scratch.mark_recovery_taint();
    return;
  }
  scratch.clear_recovery_taint();

  const auto dir = table_dir(db, tbl);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  if (!ec) (void)DurableFileSystem::sync_directory(dir);
}

size_t RecoveryManager::scan_pending_recovery() {
  std::unordered_map<std::string, std::pair<std::string, std::string>> found;
  std::error_code ec;
  for (std::filesystem::directory_iterator db_it(m_base_dir, ec), end; !ec && db_it != end; db_it.increment(ec)) {
    std::error_code e2;
    if (!db_it->is_directory(e2) || e2) continue;
    for (std::filesystem::directory_iterator t_it(db_it->path(), e2), end2; !e2 && t_it != end2; t_it.increment(e2)) {
      std::error_code e3;
      if (!std::filesystem::exists(t_it->path() / "checkpoints", e3) || e3) continue;
      const std::string db = db_it->path().filename().string();
      const std::string tbl = t_it->path().filename().string();
      found.emplace(pending_key(db, tbl), std::make_pair(db, tbl));
    }
  }
  std::lock_guard lk(m_pending_mutex);
  m_pending = std::move(found);
  m_pending_count.store(m_pending.size(), std::memory_order_release);
  return m_pending.size();
}

void RecoveryManager::clear_recovery_pending(const std::string &db, const std::string &tbl) {
  std::lock_guard lk(m_pending_mutex);
  m_pending.erase(pending_key(db, tbl));
  m_pending_count.store(m_pending.size(), std::memory_order_release);
}

void RecoveryManager::note_unregistered_change(const char *db, const char *tbl) {
  if (m_pending_count.load(std::memory_order_acquire) == 0 || db == nullptr || tbl == nullptr) return;
  std::pair<std::string, std::string> names;
  const std::string key = pending_key(db, tbl);
  {
    std::lock_guard lk(m_pending_mutex);
    auto it = m_pending.find(key);
    if (it == m_pending.end()) return;
    names = it->second;
    m_pending.erase(it);
    m_pending_count.store(m_pending.size(), std::memory_order_release);
  }
  auto mgr = get_table_mgr(names.first, names.second);
  if (mgr->revoke_fast_recovery()) {
    std::string msg = "RecoveryManager: a source change reached " + names.first + "." + names.second +
                      " before its restart recovery finished; its on-disk image is revoked; "
                      "run ALTER TABLE SECONDARY_LOAD to reload from the primary";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
    return;
  }
  // Neither revocation channel worked. Keep the table pending so the next change retries.
  std::string msg = "RecoveryManager: could not revoke the on-disk image of " + names.first + "." + names.second +
                    " after a source change; it may be stale";
  LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
  std::lock_guard lk(m_pending_mutex);
  m_pending.emplace(key, names);
  m_pending_count.store(m_pending.size(), std::memory_order_release);
}

void revoke_unregistered_table_image(const char *db, const char *tbl) noexcept {
  try {
    auto *scheduler = CheckpointScheduler::global();
    auto *manager = scheduler ? scheduler->recovery_manager() : nullptr;
    if (manager) manager->note_unregistered_change(db, tbl);
  } catch (...) {
  }
}

size_t RecoveryManager::revoke_all_on_disk() {
  scan_pending_recovery();
  std::vector<std::pair<std::string, std::string>> tables;
  {
    std::lock_guard lk(m_pending_mutex);
    for (const auto &[key, names] : m_pending) tables.push_back(names);
  }
  size_t revoked = 0;
  for (const auto &[db, tbl] : tables) {
    if (get_table_mgr(db, tbl)->revoke_fast_recovery()) {
      ++revoked;
      clear_recovery_pending(db, tbl);
    } else {
      std::string msg = "RecoveryManager: could not revoke the on-disk image of " + db + "." + tbl;
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
    }
  }
  return revoked;
}

/*static*/ std::atomic<CheckpointScheduler *> CheckpointScheduler::s_global{nullptr};

/*static*/ CheckpointScheduler *CheckpointScheduler::global() { return s_global.load(std::memory_order_acquire); }

/*static*/ void CheckpointScheduler::set_global(CheckpointScheduler *s) {
  s_global.store(s, std::memory_order_release);
}

CheckpointScheduler::CheckpointScheduler(Config cfg) : m_cfg(std::move(cfg)) {
  m_mgr = std::make_unique<RecoveryManager>(m_cfg.snapshot_base_dir);
}

CheckpointScheduler::~CheckpointScheduler() { stop(); }

bool CheckpointScheduler::start() {
  if (m_thread.joinable()) return true;
  m_stop.store(false);
  m_thread = std::thread(&CheckpointScheduler::run, this);
  DBUG_PRINT("recovery", ("CheckpointScheduler: started (interval=%lld s, dir=%s)",
                          static_cast<long long>(m_cfg.interval.count()), m_cfg.snapshot_base_dir.c_str()));
  return true;
}

void CheckpointScheduler::stop() {
  {
    std::lock_guard lk(m_mutex);
    m_stop.store(true);
  }
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "CheckpointScheduler: stopped");
  }
}

void CheckpointScheduler::enqueue(const std::string &schema_name, const std::string &table_name) {
  {
    std::lock_guard lk(m_mutex);
    m_queue.emplace_back(schema_name, table_name);
  }
  m_cv.notify_one();
}

void CheckpointScheduler::run() {
  auto last_periodic_time = std::chrono::steady_clock::now();
  while (!m_stop.load(std::memory_order_acquire)) {
    {
      std::unique_lock lk(m_mutex);

      m_cv.wait_until(lk, last_periodic_time + m_cfg.interval, [this] { return m_stop.load() || !m_queue.empty(); });
    }
    if (m_stop.load()) break;

    // 1. Always process on-demand checkpoints if any were queued
    do_ondemand_checkpoints();

    // 2. Only run the global periodic checkpoint if the interval has actually expired
    auto now = std::chrono::steady_clock::now();
    if (now - last_periodic_time >= m_cfg.interval) {
      do_periodic_checkpoint();
      last_periodic_time = now;  // Reset periodic baseline
    }
  }
}

void CheckpointScheduler::do_ondemand_checkpoints() {
  std::vector<std::pair<std::string, std::string>> todo;
  {
    std::lock_guard lk(m_mutex);
    todo.swap(m_queue);
  }

  for (const auto &[db, tbl] : todo) {
    auto rpd_table = Imcs::Imcs::instance()->get_rpd_table_by_name(db, tbl);
    if (!rpd_table) continue;
    // Partitioned tables do not participate: see RpdTable::recovery_supported().
    if (!rpd_table->recovery_supported()) continue;

    uint64_t scn = 0;
    rpd_table->foreach_imcu([&scn](Imcs::Imcu *imcu) {
      if (imcu) scn = std::max(scn, imcu->get_max_scn());
    });
    auto imcus = rpd_table->get_imcus();
    if (imcus.empty()) continue;
    // checkpoint() snapshots the whole table into one generation; trigger once.
    bool deferred = false;
    if (!m_mgr->checkpoint_imcu(db, tbl, imcus.front().get(), scn, &deferred) && !deferred) {
      std::string log_msg = "CheckpointScheduler: on-demand checkpoint failed " + db + "." + tbl;
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    }
  }
}

void CheckpointScheduler::do_periodic_checkpoint() {
  auto *imcs = Imcs::Imcs::instance();
  if (!imcs) return;

  imcs->for_each_table([this](Imcs::RpdTable *rpd_table) {
    if (!rpd_table) return;
    const auto &metadata = rpd_table->meta();
    if (!Utils::try_bool_operation([&] {
          auto state_manager = m_mgr->table_manager(metadata.db_name, metadata.table_name);
          return state_manager->checkpoint_table_state(metadata.table_id);
        })) {
      const std::string message =
          "CheckpointScheduler: table metadata checkpoint failed " + metadata.db_name + "." + metadata.table_name;
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, message.c_str());
    }
    // Partitioned tables do not participate: see RpdTable::recovery_supported().
    if (!rpd_table->recovery_supported()) return;
    const auto db = rpd_table->meta().db_name;
    const auto tbl = rpd_table->meta().table_name;

    uint64_t scn = 0;
    rpd_table->foreach_imcu([&scn](Imcs::Imcu *imcu) {
      if (imcu) scn = std::max(scn, imcu->get_max_scn());
    });
    auto *persistence = rpd_table->recovery_manager();
    auto *tracker = persistence ? persistence->notifications() : nullptr;
    if (!tracker || !tracker->needs_checkpoint()) return;
    for (const auto &im : rpd_table->get_imcus()) {
      if (im) {
        (void)m_mgr->checkpoint_imcu(db, tbl, im.get(), scn);
        break;
      }
    }
  });
}

}  // namespace Recovery
}  // namespace ShannonBase
