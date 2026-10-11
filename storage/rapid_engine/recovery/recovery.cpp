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

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/recovery/recovery.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <thread>

#include "include/my_dbug.h"
#include "include/scope_guard.h"
#include "sql/dd/dd_kill_immunizer.h"  // dd::DD_kill_immunizer
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/handler.h"         // handler::ha_records
#include "sql/log.h"             // sql_print_error
#include "sql/mdl.h"             // MDL_SHARED_NO_WRITE
#include "sql/mysqld.h"          // connection_events_loop_aborted, mysql_real_data_home
#include "sql/partition_info.h"  // partition_info
#include "sql/sql_base.h"        // close_thread_tables
#include "sql/sql_class.h"       // THD
#include "sql/table.h"           // TABLE
#include "sql/transaction.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/recovery/binlog_recovery.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"  // shannon_loaded_tables, RapidShare
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"  // shannon_rpd_engine_cfg
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"  // recovery counters
#include "storage/rapid_engine/populate/log_populate.h"  // Populator::start
#include "storage/rapid_engine/populate/propagation_mode.h"
#include "storage/rapid_engine/recovery/recovery_load.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
#include "storage/rapid_engine/trx/transaction.h"  // Transaction, TransactionCoordinator
#include "storage/rapid_engine/utils/utils.h"      // Util::open_table_by_name

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

bool RecoveryManager::checkpoint_imcu(const std::string &db, const std::string &tbl, Imcs::Imcu *imcu,
                                      uint64_t /*scn*/) {
  if (!imcu) return false;
  auto mgr = get_table_mgr(db, tbl);
  if (!mgr->checkpoint(imcu, 0)) return false;

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
                      " before its restart recovery finished; its on-disk image is revoked and the table will be "
                      "reloaded from the primary";
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

void note_unregistered_source_change(const char *db, const char *tbl) noexcept {
  try {
    auto *sched = CheckpointScheduler::global();
    auto *mgr = sched ? sched->recovery_manager() : nullptr;
    if (mgr) mgr->note_unregistered_change(db, tbl);
  } catch (...) {
  }
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

      // Calculate remaining time until the next scheduled periodic sweep
      auto now = std::chrono::steady_clock::now();
      auto elapsed = now - last_periodic_time;
      auto timeout = (elapsed >= m_cfg.interval)
                         ? std::chrono::seconds(0)
                         : std::chrono::duration_cast<std::chrono::seconds>(m_cfg.interval - elapsed);

      m_cv.wait_for(lk, timeout, [this] { return m_stop.load() || !m_queue.empty(); });
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
    if (!m_mgr->checkpoint_imcu(db, tbl, imcus.front().get(), scn)) {
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

RecoveryAdminSession::RecoveryAdminSession() {
  my_thread_init();

  m_thd = new (std::nothrow) THD;
  if (!m_thd) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryAdminSession: failed to allocate THD");
    return;
  }
  m_thd->set_new_thread_id();
  m_thd->thread_stack = reinterpret_cast<char *>(this);
  m_thd->set_command(COM_DAEMON);
  m_thd->security_context()->skip_grants();
  m_thd->system_thread = NON_SYSTEM_THREAD;
  m_thd->store_globals();
  m_thd->lex->sql_command = SQLCOM_SELECT;
}

RecoveryAdminSession::~RecoveryAdminSession() {
  if (!m_thd) {
    my_thread_end();
    return;
  }

  trans_rollback_stmt(m_thd);
  trans_rollback(m_thd);
  ShannonBase::Transaction::free_trx_from_thd(m_thd);

  if (m_thd->open_tables) {
    if (!connection_events_loop_aborted() && !m_thd->killed)
      close_thread_tables(m_thd);
    else {
      for (TABLE *t = m_thd->open_tables; t; t = t->next) {
        if (t->file) t->file->ha_external_lock(m_thd, F_UNLCK);
        MDL_ticket *mdl_ticket = t->mdl_ticket;
        m_thd->mdl_context.release_all_locks_for_name(mdl_ticket);
      }

      m_thd->open_tables = nullptr;
    }
  }
  m_thd->mdl_context.release_statement_locks();
  m_thd->mdl_context.release_transactional_locks();
  m_thd->release_resources();
  delete m_thd;
  m_thd = nullptr;
  my_thread_end();
}

bool RecoveryJob::execute() {
  std::shared_lock mode_lock(Populate::propagation_mode_mutex);
  if (!Populate::propagation_backend_available(Populate::configured_change_propagation_mode.load())) return false;
  const auto &info = m_table_info;

  // Once the table is registered, or its previous image has been discarded, capture
  // (or the absence of an image) covers it and the pending-recovery fence can go.
  auto settle_pending = [&info]() {
    auto *sched = CheckpointScheduler::global();
    auto *recovery = sched ? sched->recovery_manager() : nullptr;
    if (recovery) recovery->clear_recovery_pending(info.schema_name, info.table_name);
  };

  DBUG_PRINT("recovery", ("RecoveryJob::execute - %s.%s (partitioned=%d)", info.schema_name.c_str(),
                          info.table_name.c_str(), info.is_partitioned ? 1 : 0));

  // Skip if already present in IMCS (idempotent).
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
    settle_pending();
    DBUG_PRINT("recovery",
               ("RecoveryJob: skip %s.%s - already in IMCS", info.schema_name.c_str(), info.table_name.c_str()));
    return true;
  }

  RecoveryAdminSession session;
  if (!session.is_valid()) {
    std::string log_msg = "RecoveryJob: cannot create admin session for " + info.schema_name + "." + info.table_name;
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  THD *thd = session.thd();

  DBUG_SIGNAL_WAIT_FOR(thd, "rapid_pause_restart_recovery", "rapid_restart_recovery_paused",
                       "rapid_restart_recovery_continue");

  // Source state remains authoritative. Fast recovery requires a matching
  // column checkpoint and complete committed MySQL binlog history.
  // Block source DML and DDL until the primary scan, registration and
  // propagation startup are complete. Otherwise a source commit can fall
  // between the scan and registration, when capture still ignores this table.
  // RecoveryAdminSession releases this transaction-duration lock on all exits.
  MDL_request source_lock;
  MDL_REQUEST_INIT(&source_lock, MDL_key::TABLE, info.schema_name.c_str(), info.table_name.c_str(), MDL_SHARED_NO_WRITE,
                   MDL_TRANSACTION);
  if (thd->mdl_context.acquire_lock(&source_lock, thd->variables.lock_wait_timeout)) {
    std::string log_msg =
        "RecoveryJob: cannot lock " + info.schema_name + "." + info.table_name + " for a consistent primary reload";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  // A user SECONDARY_LOAD may have completed while we waited for the lock.
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
    settle_pending();
    return true;
  }

  if (try_snapshot_recovery(thd)) {
    settle_pending();
    RapidMonitor::rapid_counters.recovery_storage_restores.fetch_add(1, std::memory_order_relaxed);
    start_change_propagation();
    schedule_checkpoint_async();
    std::string log_msg = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                          " recovered from checkpoint and committed MySQL binlog";
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return true;
  }

  // A reload renumbers every row from InnoDB, so nothing on disk for this
  // table describes the layout that is about to exist.  Start a fresh LSN epoch
  // before the reload so the first DML after publication belongs to the new
  // epoch, once the source write lock is released.
  //
  // This throws away whatever checkpoint state the table had. Say so, with the
  // reason, before it happens, including in release builds.
  {
    std::string log_msg = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                          " has no complete, compatible binlog checkpoint anchor; rebuilding from InnoDB "
                          "and discarding its previous recovery epoch";
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
  }
  if (!discard_stale_recovery_state()) {
    std::string log_msg = "RecoveryJob: refusing to reload " + info.schema_name + "." + info.table_name +
                          " because its previous recovery epoch could not be safely discarded";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  settle_pending();  // nothing on disk describes the old image any more

  const auto t1 = std::chrono::steady_clock::now();
  bool ok = info.is_partitioned ? reload_partitioned_table(thd) : reload_normal_table(thd);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  if (ok) {
    RapidMonitor::rapid_counters.recovery_primary_reloads.fetch_add(1, std::memory_order_relaxed);
    DBUG_PRINT("recovery", ("RecoveryJob: [RELOAD] successfully reloaded %s.%s in %ld ms — snapshot scheduled",
                            info.schema_name.c_str(), info.table_name.c_str(), ms));
  } else {
    std::string log_msg = "RecoveryJob: [RELOAD] FAILED to reload " + info.schema_name + "." + info.table_name +
                          " after " + std::to_string(ms) + " ms (recovery continues)";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
  }

  // Checkpoint the new epoch for subsequent binlog recovery.
  // Partitioned tables are excluded:
  // they have no per-partition checkpoint identity to snapshot against.
  if (ok && !info.is_partitioned) schedule_checkpoint_async();
  if (ok) start_change_propagation();

  return ok;
}

/**
  Bring change propagation up for a table this job just restored.

  ALTER TABLE ... SECONDARY_LOAD does this at the end of ha_rapid::load_table(),
  but recovery loads a table without going through the handler. Without this
  call the coordinator thread never starts, and the first DML after the restart
  takes the "captured a change while propagation is stopped" branch in
  PopulatorImpl::write_buffer_impl(): the table is quarantined for good, its
  contents freeze at the restored state, and Rapid reads it as stale until
  someone reloads it by hand.

  start() is idempotent -- it no-ops once the coordinator is running -- so
  calling it per recovered table costs nothing after the first.
*/
void RecoveryJob::start_change_propagation() const { ShannonBase::Populate::Populator::start(); }

bool RecoveryJob::try_snapshot_recovery(THD *thd) {
  // A partitioned table has no usable snapshot: its partitions never wrote one
  // (see RpdTable::recovery_supported()), and any generation left behind by an
  // older build describes only whichever partition triggered it.  Rebuild from
  // InnoDB.
  if (m_table_info.is_partitioned) return false;

  auto *sched = CheckpointScheduler::global();
  if (!sched) return false;

  auto *mgr = sched->recovery_manager();
  if (!mgr) return false;

  const auto &info = m_table_info;
  auto table_manager = mgr->table_manager(info.schema_name, info.table_name);
  auto *capture = table_manager ? table_manager->notifications() : nullptr;
  if (!capture) return false;  // no notification tracker
  // Bail before creating anything: this lane must not publish a table whose
  // previous image was revoked. load_from_snapshots() repeats the check, which
  // is what actually guards every caller.
  if (table_manager->recovery_tainted()) return false;
  std::lock_guard capture_gate(capture->mutex());

  // Is there a snapshot on disk?
  if (!mgr->has_durable_checkpoint(info.schema_name, info.table_name)) return false;

  // Create empty in-memory table structure (schema metadata).
  TABLE *source = Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!source) {
    DBUG_PRINT("recovery",
               ("RecoveryJob(snapshot): cannot open %s.%s", info.schema_name.c_str(), info.table_name.c_str()));
    return false;
  }

  Rapid_load_context ctx;
  ctx.m_thd = thd;
  ctx.m_schema_name = info.schema_name;
  ctx.m_table_name = info.table_name;
  ctx.m_sch_tb_name = info.schema_name + "." + info.table_name;
  ctx.m_table = source;
  ctx.m_table_id = source->file->get_table_id();

  const auto table_id = ctx.m_table_id;
  const int rc = Imcs::Imcs::instance()->create_table_memo(&ctx, source);
  Utils::Util::close_table(thd, source);
  source = nullptr;

  if (rc != SHANNON_SUCCESS) {
    DBUG_PRINT("recovery", ("RecoveryJob(snapshot): create_table_memo failed for %s.%s", info.schema_name.c_str(),
                            info.table_name.c_str()));
    return false;
  }

  // From here on the table has an entry in IMCS.  Every failure below has to
  // take it back out again: execute() falls through to the slow lane, whose
  // guard_load() sees a pre-existing entry and therefore does not clean up,
  // create_table_memo()'s emplace() is a no-op on an existing key, and
  // load_table_impl() then appends the whole InnoDB table on top of the rows
  // the fast lane had already restored -- duplicate rows, duplicate ART keys.
  bool published{false};
  auto drop_partial_table = create_scope_guard([&]() {
    if (!published) Imcs::Imcs::instance()->cleanup(table_id);
  });

  auto rpd_table = Imcs::Imcs::instance()->get_rpd_table_by_name(info.schema_name, info.table_name);
  if (!rpd_table) return false;

  // Restore committed column state before applying committed source changes.
  uint64_t generation = 0;
  if (!mgr->load_from_snapshots(info.schema_name, info.table_name, rpd_table.get(), &generation)) return false;
  auto manifest = table_manager->load_manifest(generation);
  if (!manifest.ok()) return false;

  // Reconnect Field* (cannot be serialised; patch from live TABLE).
  TABLE *patched_src = nullptr;
  if (!reconstruct_field_pointers(thd, rpd_table.get(), patched_src)) return false;

  // Rebuild the ART indexes from the restored column cells before binlog
  // mutations run through normal row/index maintenance. Without this the table
  // comes back with empty indexes, and an empty ART does not fall back to a
  // scan -- an indexed lookup just finds nothing. The full scan stays correct,
  // so the damage is silent: SELECT ... WHERE pk = ? answers zero rows on a
  // table that plainly holds the row.
  //
  // It runs after reconstruct_field_pointers() because the key codec encodes out of a
  // record image, which needs Fields bound to this TABLE.
  auto *table_impl = dynamic_cast<Imcs::Table *>(rpd_table.get());
  if (table_impl == nullptr) {
    Utils::Util::close_table(thd, patched_src);
    return false;
  }
  if (table_impl->rebuild_indexes(&ctx, patched_src) != SHANNON_SUCCESS) {
    // Fall back to the slow lane rather than publish a table whose indexes
    // disagree with its rows. drop_partial_table takes the IMCS entry back out.
    sql_print_error("RecoveryJob(snapshot): %s could not rebuild its indexes; reloading from InnoDB instead",
                    ctx.m_sch_tb_name.c_str());
    Utils::Util::close_table(thd, patched_src);
    return false;
  }

  rpd_table->foreach_imcu([](Imcs::Imcu *imcu) {
    if (imcu) TransactionCoordinator::instance().observe_commit_scn(imcu->get_max_scn());
  });
  ShannonBase::Populate::DML::CopyInfoParser parser;
  ctx.m_table = patched_src;
  bool replayed =
      BinlogRecovery::replay(thd, patched_src, manifest.value.binlog, [&](Populate::change_record_buff_t &record) {
        record.m_table_id = table_id;
        record.m_source_trx_id = 0;  // recovered committed system version, not a live writer
        return parser.apply_change(ctx, record, 0).status == Populate::ChangeApplyResult::Status::APPLIED;
      });
  if (!replayed) {
    Utils::Util::close_table(thd, patched_src);
    return false;
  }
  capture->initialize(BinlogRecovery::current_position());
  rpd_table->update_statistics(true);
  published = register_in_loaded_tables(thd, patched_src, rpd_table.get());
  if (published) Utils::Util::update_rpd_meta_info(&ctx, patched_src, Utils::Util::STAGE::END);
  Utils::Util::close_table(thd, patched_src);
  return published;
}

bool RecoveryJob::reconstruct_field_pointers(THD *thd, Imcs::RpdTable *rpd_table, TABLE *&out_source) {
  const auto &info = m_table_info;

  out_source = Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!out_source) {
    std::string log_msg =
        "RecoveryJob(snapshot): cannot open " + info.schema_name + "." + info.table_name + " to patch Field*";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  // Bind the Field clones the table itself owns (Table::Table clones them into its
  // mem_root, and the normal load path uses those). The Fields of out_source belong
  // to a TABLE that the caller closes right after publication; a CU holding one of
  // them would dangle from then on.
  const auto &fields = rpd_table->meta().fields;
  rpd_table->foreach_imcu([&](Imcs::Imcu *imcu) {
    if (!imcu) return;
    for (uint i = 0; i < fields.size(); ++i) {
      Field *f = fields[i].source_fld;
      if (f) imcu->reconstruct_cu_field(i, f, f->charset());
    }
  });
  return true;
}

bool RecoveryJob::register_in_loaded_tables(THD *thd, TABLE *source, Imcs::RpdTable *rpd_table) {
  const auto &info = m_table_info;
  (void)thd;

  rpd_table->meta().total_rows.store(rpd_table->count_total_rows(), std::memory_order_relaxed);

  auto m_share = std::make_shared<RapidShare>(*source);
  m_share->is_partitioned = info.is_partitioned;
  m_share->m_tableid = source->file->get_table_id();

  shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
  if (!shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str)) {
    // Runs on the recovery admin session: there is no client to raise this to.
    std::string log_msg = "RecoveryJob(snapshot): " + info.schema_name + "." + info.table_name +
                          " did not register in IMCS after restore";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  return true;
}

bool RecoveryJob::discard_stale_recovery_state() {
  auto *sched = CheckpointScheduler::global();
  if (!sched) return true;
  auto *mgr = sched->recovery_manager();
  if (!mgr) return true;
  auto tbl_mgr = mgr->table_manager(m_table_info.schema_name, m_table_info.table_name);
  if (!tbl_mgr) return false;

  if (!tbl_mgr->reset_epoch()) {
    std::string log_msg = "RecoveryJob: could not reset the recovery epoch for " + m_table_info.schema_name + "." +
                          m_table_info.table_name + "; checkpointing is disabled for this table until restart";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  // The table is about to be rebuilt from InnoDB, so the earlier "must reload"
  // decision has been discharged.
  tbl_mgr->clear_recovery_taint();
  return true;
}

void RecoveryJob::schedule_checkpoint_async() {
  if (auto *sched = CheckpointScheduler::global()) sched->enqueue(m_table_info.schema_name, m_table_info.table_name);
}

bool RecoveryJob::reload_normal_table(THD *thd) {
  const auto &info = m_table_info;

  TABLE *source =
      ShannonBase::Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!source) {
    std::string log_msg = "RecoveryJob: cannot open table " + info.schema_name + "." + info.table_name + " for reading";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  Rapid_load_context context;
  context.m_schema_name = info.schema_name;
  context.m_table_name = info.table_name;
  context.m_thd = thd;
  context.m_sch_tb_name = info.schema_name + "." + info.table_name;
  context.m_table = source;
  context.m_table_id = source->file->get_table_id();
  // Bulk source loads create committed baseline rows without DML journals.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_table(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);
  if (result == SHANNON_SUCCESS) {
    const std::string db_name(source->s->db.str, source->s->db.length);
    const std::string tbl_name(source->s->table_name.str, source->s->table_name.length);
    auto m_share = std::make_shared<RapidShare>(*source);
    m_share->is_partitioned = false;
    m_share->m_tableid = context.m_table_id;

    ShannonBase::Utils::Util::close_table(thd, source);
    source = nullptr;

    shannon_loaded_tables->add(db_name.c_str(), tbl_name.c_str(), m_share);
    if (!shannon_loaded_tables->get(db_name.c_str(), tbl_name.c_str())) {
      // A non-zero error code here used to be returned from this bool function,
      // which made the failure read as success. There is no client to report it
      // to either -- this runs on the recovery admin session -- so log it.
      std::string log_msg = "RecoveryJob: " + db_name + "." + tbl_name + " did not register in IMCS after reload";
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
      return false;
    }
  } else {
    ShannonBase::Utils::Util::close_table(thd, source);
    source = nullptr;
  }
  return (result == SHANNON_SUCCESS);
}

bool RecoveryJob::reload_partitioned_table(THD *thd) {
  const auto &info = m_table_info;

  TABLE *source =
      ShannonBase::Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!source) {
    std::string log_msg =
        "RecoveryJob: cannot open partitioned table " + info.schema_name + "." + info.table_name + " for reading";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  Rapid_load_context context;
  context.m_schema_name = info.schema_name;
  context.m_table_name = info.table_name;
  context.m_thd = thd;
  context.m_sch_tb_name = info.schema_name + "." + info.table_name;
  context.m_table = source;
  context.m_table_id = source->file->get_table_id();
  // Bulk source loads create committed baseline rows without DML journals.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  // Enumerate every partition, the way ha_rapidpart::load_table() does for
  // ALTER TABLE ... SECONDARY_LOAD. Both PartTable::build_partitions() and
  // load_innodbpart() are driven entirely by this map: leaving it empty builds
  // no partitions, copies no rows, and still reports success -- the table comes
  // back from a restart empty while InnoDB still has every row.
  if (source->part_info == nullptr) {
    sql_print_error("RecoveryJob: %s has no partition info; cannot reload it", context.m_sch_tb_name.c_str());
    ShannonBase::Utils::Util::close_table(thd, source);
    return false;
  }
  for (uint index = 0; index < source->part_info->get_tot_partitions(); ++index) {
    context.m_extra_info.m_partition_infos.emplace(source->part_info->partitions[index]->partition_name, index);
  }

  // The load path stamps every row with the loading transaction's id and SCN.
  context.m_trx = Transaction::get_or_create_trx(thd);
  if (context.m_trx == nullptr) {
    sql_print_error("RecoveryJob: cannot start a Rapid transaction for %s", context.m_sch_tb_name.c_str());
    ShannonBase::Utils::Util::close_table(thd, source);
    return false;
  }
  context.m_trx->begin_stmt();
  context.m_extra_info.m_trxid = context.m_trx->get_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_parttable(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);

  bool success = (result == SHANNON_SUCCESS);
  if (success)
    context.m_trx->commit();
  else
    context.m_trx->rollback_stmt();
  DBUG_PRINT("recovery", ("reload_partitioned_table: load_parttable %s for %s.%s%s", success ? "succeeded" : "failed",
                          info.schema_name.c_str(), info.table_name.c_str(),
                          success ? "" : (std::string(" (err=") + std::to_string(result) + ")").c_str()));

  if (success) {
    const std::string db_name(source->s->db.str, source->s->db.length);
    const std::string tbl_name(source->s->table_name.str, source->s->table_name.length);
    auto m_share = std::make_shared<RapidShare>(*source);
    m_share->is_partitioned = true;
    m_share->m_tableid = context.m_table_id;

    ShannonBase::Utils::Util::close_table(thd, source);
    source = nullptr;

    shannon_loaded_tables->add(db_name.c_str(), tbl_name.c_str(), m_share);
    if (!shannon_loaded_tables->get(db_name.c_str(), tbl_name.c_str())) {
      // A non-zero error code here used to be returned from this bool function,
      // which made the failure read as success. There is no client to report it
      // to either -- this runs on the recovery admin session -- so log it.
      std::string log_msg = "RecoveryJob: " + db_name + "." + tbl_name + " did not register in IMCS after reload";
      LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
      return false;
    }
  } else {
    ShannonBase::Utils::Util::close_table(thd, source);
    source = nullptr;
  }

  return success;
}

bool DDWorker::start() {
  if (m_thread.joinable()) return true;

  m_stop.store(false, std::memory_order_release);
  m_done.store(false, std::memory_order_release);

  m_thread = std::thread(&DDWorker::run, this);
  DBUG_PRINT("recovery", ("DDWorker: background thread started"));
  return true;
}

void DDWorker::stop() {
  {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_stop.store(true, std::memory_order_release);
  }
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
    DBUG_PRINT("recovery", ("DDWorker: background thread stopped"));
  }
}

void DDWorker::run() {
  if (!Utils::Util::wait_for_server_bootup(300, [this] { return m_stop.load(std::memory_order_acquire); })) {
    m_done.store(true, std::memory_order_release);
    return;
  }
  if (m_stop.load(std::memory_order_acquire)) {
    m_done.store(true, std::memory_order_release);
    return;
  }

  RecoveryAdminSession session;
  if (!session.is_valid()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "DDWorker: failed to create admin session");
    m_done.store(true, std::memory_order_release);
    return;
  }
  THD *thd = session.thd();

  {
    const dd::DD_kill_immunizer kill_immunizer(thd);
    std::vector<SecondaryLoadedTable> found;
    bool incomplete = false;
    int ret = LoadFlagManager::instance().query_loaded_tables(thd, found, &incomplete);
    if (ret == 0 && incomplete)
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
             "DDWorker: the data dictionary walk skipped at least one schema; its loaded tables are not recovered");
    if (ret != 0) {
      std::string warning_str = "DDWorker: query_loaded_tables failed with error code " + std::to_string(ret);
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, warning_str.c_str());
      m_done.store(true, std::memory_order_release);
      return;
    }
    DBUG_PRINT("recovery", ("DDWorker: found %zu table(s) with secondary_load=1 in Data "
                            "Dictionary",
                            found.size()));
    m_found_tables = std::move(found);
  }
  m_done.store(true, std::memory_order_release);
}

bool RecoveryFramework::is_global_state_empty() const {
  std::atomic<size_t> count{0};
  Imcs::Imcs::instance()->for_each_table([&count](Imcs::RpdTable *) { count++; });
  return count.load() == 0;
}

std::string RecoveryFramework::resolve_snapshot_dir() {
  if (!ShannonBase::shannon_rpd_engine_cfg.snapshot_dir.empty())
    return ShannonBase::shannon_rpd_engine_cfg.snapshot_dir;
  return std::string(mysql_real_data_home) + "/rapid_snapshots";
}

void RecoveryFramework::invalidate_external_global_state() {
  // No scheduler runs this session, so nothing captures source changes. Every
  // image and journal left on disk by an earlier session would keep a valid
  // certificate while going stale; revoke them so a later restart with the flag ON
  // reloads from the primary instead of fast-restoring them.
  RecoveryManager on_disk(resolve_snapshot_dir());
  const size_t revoked = on_disk.revoke_all_on_disk();
  if (revoked > 0) {
    std::string msg = "RecoveryFramework: rapid_reload_on_restart=OFF - revoked the on-disk recovery images of " +
                      std::to_string(revoked) + " table(s); they will be reloaded if the flag is turned back on";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
  }
}

void RecoveryFramework::process_external_global_state() {
  if (!ShannonBase::shannon_rpd_engine_cfg.reload_on_restart) {
    if (m_global_state_empty.load()) invalidate_external_global_state();
    return;
  }

  const std::string snap_dir = resolve_snapshot_dir();

  std::error_code ec;
  std::filesystem::create_directories(snap_dir, ec);
  if (ec) {
    std::string warning_str = "RecoveryFramework: cannot create snapshot dir '" + snap_dir + "': " + ec.message() +
                              " - snapshots will be unavailable this session";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, warning_str.c_str());
  }

  const uint32_t interval_secs = (ShannonBase::shannon_rpd_engine_cfg.snapshot_interval_secs > 0)
                                     ? ShannonBase::shannon_rpd_engine_cfg.snapshot_interval_secs
                                     : 300u;

  CheckpointScheduler::Config chk_cfg;
  chk_cfg.snapshot_base_dir = snap_dir;
  chk_cfg.interval = std::chrono::seconds(interval_secs);

  m_checkpoint_scheduler = std::make_unique<CheckpointScheduler>(chk_cfg);
  if (m_checkpoint_scheduler->start()) {
    CheckpointScheduler::set_global(m_checkpoint_scheduler.get());
    // Runs in plugin init, before connections: from here until each table's
    // RecoveryJob settles it, a source change to a table with an on-disk image
    // revokes that image (see RecoveryManager::note_unregistered_change).
    const size_t pending = m_checkpoint_scheduler->recovery_manager()->scan_pending_recovery();
    DBUG_PRINT("recovery", ("RecoveryFramework: CheckpointScheduler started, %zu table(s) awaiting recovery", pending));
  } else {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: CheckpointScheduler failed to start — "
           "checkpoint maintenance unavailable this session");
    m_checkpoint_scheduler.reset();
  }

  m_dd_worker = std::make_unique<DDWorker>();
  if (!m_dd_worker->start()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: failed to start DDWorker - "
           "restart reload will not be performed");
    m_dd_worker.reset();
  }
}

void RecoveryFramework::dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables) {
  if (tables.empty()) {
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: no tables to reload");
    return;
  }

  DBUG_PRINT("recovery", ("RecoveryFramework: dispatching reload jobs for %zu table(s)", tables.size()));

  // Each job holds a THD, an MDL lock and (on the slow lane) a full primary scan;
  // one thread per table would start thousands at once on a large instance.
  constexpr size_t kMaxParallelRecoveryJobs = 8;

  for (const auto &tbl : tables) {
    {
      std::unique_lock<std::mutex> lk(m_jobs_mutex);
      m_jobs_cv.wait(lk, [this] {
        return m_stopped.load(std::memory_order_acquire) || m_active_jobs.load() < kMaxParallelRecoveryJobs;
      });
      if (m_stopped.load(std::memory_order_acquire)) break;
      m_active_jobs.fetch_add(1, std::memory_order_relaxed);
    }

    std::thread job_thread([this, tbl]() mutable {
      RecoveryJob job(tbl);
      bool ok = job.execute();
      if (ok) m_reloaded_count.fetch_add(1, std::memory_order_relaxed);

      {
        std::lock_guard<std::mutex> lk(m_jobs_mutex);  // under the lock: no lost wake-up
        m_active_jobs.fetch_sub(1, std::memory_order_acq_rel);
      }
      m_jobs_cv.notify_all();
    });

    {
      std::lock_guard<std::mutex> lock(m_job_threads_mutex);
      m_job_threads.push_back(std::move(job_thread));
    }
  }
}

void RecoveryFramework::startup() {
  if (m_started.exchange(true, std::memory_order_acq_rel)) return;
  m_restart_in_progress.store(true, std::memory_order_release);
  auto finish_startup = create_scope_guard([this]() { m_restart_in_progress.store(false, std::memory_order_release); });

  DBUG_PRINT("recovery", ("RecoveryFramework: startup initiated"));

  bool empty = is_global_state_empty();
  m_global_state_empty.store(empty, std::memory_order_release);

  if (!empty) {
    // Not an error -- a non-empty global state means something already
    // populated IMCS -- but it silently skips ALL restart recovery, so a
    // release build gave no clue why no table came back.
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: IMCS global state is not empty; skipping restart recovery");
    return;
  }

  process_external_global_state();

  if (!m_dd_worker) return;

  m_monitoring_thread = std::thread([this]() {
    auto finish_recovery =
        create_scope_guard([this]() { m_restart_in_progress.store(false, std::memory_order_release); });
    while (m_dd_worker && !m_dd_worker->is_done()) {
      if (m_stopped.load(std::memory_order_acquire)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!m_stopped.load(std::memory_order_acquire) && m_dd_worker) dispatch_jobs(m_dd_worker->found_tables());
    // Dispatch finishing is insufficient: the last jobs still own table state.
    std::unique_lock<std::mutex> lock(m_jobs_mutex);
    m_jobs_cv.wait(lock, [this]() {
      return m_stopped.load(std::memory_order_acquire) || m_active_jobs.load(std::memory_order_acquire) == 0;
    });
  });
  finish_startup.release();
}

void RecoveryFramework::shutdown() {
  // 1. Signal shutdown flags
  m_stopped.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(m_jobs_mutex);  // pair with the waiters' predicate
  }
  m_jobs_cv.notify_all();  // wake a dispatcher waiting for a free job slot

  // 2. Stop the DD worker FIRST so that m_dd_worker->is_done() becomes true
  if (m_dd_worker) m_dd_worker->stop();

  // 3. Now it is completely safe to wait for the monitoring thread to complete
  if (m_monitoring_thread.joinable()) m_monitoring_thread.join();

  {
    std::unique_lock<std::mutex> lk(m_jobs_mutex);
    bool drained = m_jobs_cv.wait_for(lk, std::chrono::seconds(5), [this] { return m_active_jobs.load() == 0; });
    if (!drained) {
      // The join below waits for these jobs; say so instead of claiming otherwise.
      sql_print_warning(
          "RecoveryFramework: %zu job(s) still in flight after 5 s; waiting for them to finish before "
          "shutdown continues",
          m_active_jobs.load());
    }
  }
  {
    std::lock_guard<std::mutex> lock(m_job_threads_mutex);
    for (auto &thread : m_job_threads) {
      if (thread.joinable()) {
        if (thread.get_id() != std::this_thread::get_id())
          thread.join();
        else
          thread.detach();
      }
    }
    m_job_threads.clear();
  }

  // Stop scheduler AFTER jobs drain. Unpublish it first: tables and hooks reach it
  // through global(), and it must not be handed out once it is stopping.
  if (m_checkpoint_scheduler) {
    CheckpointScheduler::set_global(nullptr);
    m_checkpoint_scheduler->stop();
  }
}
}  // namespace Recovery
}  // namespace ShannonBase
