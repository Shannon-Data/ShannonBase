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

#include <chrono>
#include <filesystem>
#include <thread>

#include "include/my_dbug.h"
#include "sql/dd/dd_kill_immunizer.h"  // dd::DD_kill_immunizer
#include "sql/field.h"                 // Field
#include "sql/handler.h"               // handler::ha_records
#include "sql/mysqld.h"                // connection_events_loop_aborted, mysql_real_data_home
#include "sql/sql_base.h"              // close_thread_tables
#include "sql/sql_class.h"             // THD
#include "sql/table.h"                 // TABLE

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"  // shannon_loaded_tables, RapidShare
#include "storage/rapid_engine/imcs/cu_recovery.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"  // shannon_rpd_engine_cfg
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/recovery/recovery_load.h"
#include "storage/rapid_engine/utils/utils.h"  // Util::open_table_by_name

namespace ShannonBase {
namespace Recovery {
RecoveryManager::RecoveryManager(std::string base_dir) : m_base_dir(std::move(base_dir)) {}

RecoveryManager::~RecoveryManager() {
  std::lock_guard lk(m_mutex);
  for (auto &[k, mgr] : m_per_table)
    if (mgr) mgr->close();
}

std::filesystem::path RecoveryManager::table_dir(const std::string &db, const std::string &tbl) const {
  return std::filesystem::path(m_base_dir) / db / tbl;
}

Imcs::CURecoveryManager *RecoveryManager::get_table_mgr(const std::string &db, const std::string &tbl) {
  const std::string key = db + '\x01' + tbl;
  std::lock_guard lk(m_mutex);
  auto it = m_per_table.find(key);
  if (it != m_per_table.end()) return it->second.get();

  auto mgr = std::make_unique<Imcs::CURecoveryManager>(m_base_dir, db, tbl);
  mgr->open();
  auto *raw = mgr.get();
  m_per_table.emplace(key, std::move(mgr));
  return raw;
}

bool RecoveryManager::checkpoint_imcu(const std::string &db, const std::string &tbl, Imcs::Imcu *imcu,
                                      uint64_t /*scn*/) {
  if (!imcu) return false;
  return get_table_mgr(db, tbl)->checkpoint(imcu, 0);
}

uint64_t RecoveryManager::latest_checkpoint_scn(const std::string &db, const std::string &tbl) {
  std::error_code ec;
  const auto dir = table_dir(db, tbl);
  if (!std::filesystem::is_directory(dir, ec)) return 0;
  for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
    if (!ec && e.path().extension() == ".snap") return 1;  // ≥1 snap found
  }
  return 0;
}

bool RecoveryManager::load_from_snapshots(const std::string &db, const std::string &tbl, Imcs::RpdTable *rpd_table) {
  if (!rpd_table) return false;

  std::error_code ec;
  const auto dir = table_dir(db, tbl);
  if (!std::filesystem::is_directory(dir, ec)) return false;

  // Collect snap files, sorted by embedded imcu_id.
  std::vector<std::pair<uint32_t, std::filesystem::path>> snaps;
  for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    const auto &p = e.path();
    const auto stem = p.stem().string();
    if (p.extension() == ".snap" && stem.substr(0, 5) == "imcu_") {
      snaps.emplace_back(static_cast<uint32_t>(std::stoul(stem.substr(5))), p);
    }
  }
  if (snaps.empty()) return false;

  std::sort(snaps.begin(), snaps.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

  auto *mgr = get_table_mgr(db, tbl);
  auto &meta = rpd_table->meta();
  auto mem_pool = rpd_table->get_memory_pool();

  std::vector<Imcs::Imcu *> imcu_ptrs;
  for (size_t i = 0; i < snaps.size(); ++i) {
    const uint32_t snap_id = snaps[i].first;
    Imcs::Imcu *imcu = rpd_table->locate_imcu(snap_id);
    if (!imcu) {
      const row_id_t start = static_cast<row_id_t>(snap_id) * meta.rows_per_imcu;
      auto new_imcu = std::make_shared<Imcs::Imcu>(rpd_table, meta, start, meta.rows_per_imcu, mem_pool);
      imcu = new_imcu.get();
      rpd_table->add_imcu(std::move(new_imcu));
    }
    imcu_ptrs.push_back(imcu);
  }

  for (auto *imcu : imcu_ptrs) mgr->load_snapshot(imcu);

  const size_t replayed [[maybe_unused]] = mgr->recover(imcu_ptrs, [&](const Imcs::WalRecord &rec) {
    auto *target = rpd_table->locate_imcu(rec.imcu_id);
    if (!target) return;
    // TODO: route WAL mutations to CU write API
    (void)rec;
  });

  DBUG_PRINT("recovery", ("RecoveryManager: %s.%s — %zu IMCU(s) from snapshot, "
                          "%zu WAL record(s) replayed",
                          db.c_str(), tbl.c_str(), imcu_ptrs.size(), replayed));
  return true;
}

bool RecoveryManager::sync() {
  std::lock_guard lk(m_mutex);
  bool ok = true;
  for (auto &[k, mgr] : m_per_table)
    if (mgr) ok &= mgr->sync();
  return ok;
}

void RecoveryManager::purge_table(const std::string &db, const std::string &tbl) {
  {
    std::lock_guard lk(m_mutex);
    const std::string key = db + '\x01' + tbl;
    auto it = m_per_table.find(key);
    if (it != m_per_table.end()) {
      if (it->second) it->second->close();
      m_per_table.erase(it);
    }
  }
  std::error_code ec;
  std::filesystem::remove_all(table_dir(db, tbl), ec);
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
  while (!m_stop.load(std::memory_order_acquire)) {
    {
      std::unique_lock lk(m_mutex);
      m_cv.wait_for(lk, m_cfg.interval, [this] { return m_stop.load() || !m_queue.empty(); });
    }
    if (m_stop.load()) break;
    do_ondemand_checkpoints();
    do_periodic_checkpoint();
  }
}

void CheckpointScheduler::do_ondemand_checkpoints() {
  std::vector<std::pair<std::string, std::string>> todo;
  {
    std::lock_guard lk(m_mutex);
    todo.swap(m_queue);
  }

  for (const auto &[db, tbl] : todo) {
    auto *rpd_table = Imcs::Imcs::instance()->get_rpd_table_by_name(db, tbl);
    if (!rpd_table) continue;

    uint64_t scn = 0;
    rpd_table->foreach_imcu([&scn](Imcs::Imcu *imcu) {
      if (imcu) scn = std::max(scn, imcu->get_max_scn());
    });
    rpd_table->foreach_imcu([&](Imcs::Imcu *imcu) {
      if (!imcu) return;
      if (!m_mgr->checkpoint_imcu(db, tbl, imcu, scn)) {
        std::string log_msg = "CheckpointScheduler: on-demand checkpoint failed " + db + "." + tbl +
                              " imcu=" + std::to_string(imcu->get_imcu_id());
        LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
      }
    });
  }
}

void CheckpointScheduler::do_periodic_checkpoint() {
  auto *imcs = Imcs::Imcs::instance();
  if (!imcs) return;

  imcs->for_each_table([this](Imcs::RpdTable *rpd_table) {
    if (!rpd_table) return;
    const auto db = rpd_table->meta().db_name;
    const auto tbl = rpd_table->meta().table_name;

    uint64_t scn = 0;
    rpd_table->foreach_imcu([&scn](Imcs::Imcu *imcu) {
      if (imcu) scn = std::max(scn, imcu->get_max_scn());
    });
    rpd_table->foreach_imcu([&](Imcs::Imcu *imcu) {
      if (!imcu) return;
      if (imcu->get_status() != Imcs::Imcu::imcu_header_t::READ_ONLY) return;
      m_mgr->checkpoint_imcu(db, tbl, imcu, scn);
    });
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
  const auto &info = m_table_info;

  DBUG_PRINT("recovery", ("RecoveryJob::execute - %s.%s (partitioned=%d)", info.schema_name.c_str(),
                          info.table_name.c_str(), info.is_partitioned ? 1 : 0));

  // Skip if already present in IMCS (idempotent).
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
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

  // Fast mode.
  {
    const auto t0 = std::chrono::steady_clock::now();
    if (try_snapshot_recovery(thd)) {
      const auto ms [[maybe_unused]] =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      DBUG_PRINT("recovery", ("RecoveryJob: [FAST] successfully recovered %s.%s in %ld ms", info.schema_name.c_str(),
                              info.table_name.c_str(), ms));
      return true;
    }
  }

  // Slow mode.
  DBUG_PRINT("recovery", ("RecoveryJob: [SLOW] %s.%s — no valid snapshot, falling back to "
                          "DDL replay",
                          info.schema_name.c_str(), info.table_name.c_str()));

  const auto t1 = std::chrono::steady_clock::now();
  bool ok = info.is_partitioned ? reload_partitioned_table(thd) : reload_normal_table(thd);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  if (ok) {
    DBUG_PRINT("recovery", ("RecoveryJob: [SLOW] successfully reloaded %s.%s in %ld ms — snapshot scheduled",
                            info.schema_name.c_str(), info.table_name.c_str(), ms));
  } else {
    std::string log_msg = "RecoveryJob: [SLOW] FAILED to reload " + info.schema_name + "." + info.table_name +
                          " after " + std::to_string(ms) + " ms (recovery continues)";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
  }

  // After a successful slow-lane reload, queue a snapshot so the NEXT
  // restart can take the fast lane.
  if (ok) schedule_checkpoint_async();

  return ok;
}

bool RecoveryJob::try_snapshot_recovery(THD *thd) {
  auto *sched = CheckpointScheduler::global();
  if (!sched) return false;

  auto *mgr = sched->recovery_manager();
  if (!mgr) return false;

  const auto &info = m_table_info;

  // Is there a snapshot on disk?
  if (mgr->latest_checkpoint_scn(info.schema_name, info.table_name) == 0) return false;

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

  const int rc = Imcs::Imcs::instance()->create_table_memo(&ctx, source);
  Utils::Util::close_table(thd, source);
  source = nullptr;

  if (rc != SHANNON_SUCCESS) {
    DBUG_PRINT("recovery", ("RecoveryJob(snapshot): create_table_memo failed for %s.%s", info.schema_name.c_str(),
                            info.table_name.c_str()));
    return false;
  }

  Imcs::RpdTable *rpd_table = Imcs::Imcs::instance()->get_rpd_table_by_name(info.schema_name, info.table_name);
  if (!rpd_table) return false;

  // Load snapshot data + WAL replay.
  if (!mgr->load_from_snapshots(info.schema_name, info.table_name, rpd_table)) return false;

  // Reconnect Field* (cannot be serialised; patch from live TABLE).
  TABLE *patched_src = nullptr;
  if (!patch_field_pointers(thd, rpd_table, patched_src)) return false;

  const bool ok = register_in_loaded_tables(thd, patched_src, rpd_table);
  Utils::Util::close_table(thd, patched_src);
  return ok;
}

bool RecoveryJob::patch_field_pointers(THD *thd, Imcs::RpdTable *rpd_table, TABLE *&out_source) {
  const auto &info = m_table_info;

  out_source = Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!out_source) {
    std::string log_msg =
        "RecoveryJob(snapshot): cannot open " + info.schema_name + "." + info.table_name + " to patch Field*";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  rpd_table->foreach_imcu([&](Imcs::Imcu *imcu) {
    if (!imcu) return;
    for (uint i = 0; i < out_source->s->fields; ++i) {
      Field *f = out_source->field[i];
      if (f) imcu->patch_cu_field(i, f, f->charset());
    }
  });
  return true;
}

bool RecoveryJob::register_in_loaded_tables(THD *thd, TABLE *source, Imcs::RpdTable *rpd_table) {
  const auto &info = m_table_info;
  (void)thd;

  rpd_table->meta().total_rows.store(rpd_table->count_total_rows(), std::memory_order_relaxed);

  auto *m_share = new RapidShare(*source);
  m_share->m_source_table = source;
  m_share->is_partitioned = info.is_partitioned;
  m_share->m_tableid = source->file->get_table_id();

  shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
  if (!shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), source->s->db.str, source->s->table_name.str);
    return false;
  }
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

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_table(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);
  ShannonBase::Utils::Util::close_table(thd, source);

  DBUG_PRINT("recovery",
             ("reload_normal_table: load_table %s for %s.%s%s", (result == SHANNON_SUCCESS) ? "succeeded" : "failed",
              info.schema_name.c_str(), info.table_name.c_str(),
              (result != SHANNON_SUCCESS) ? (std::string(" (err=") + std::to_string(result) + ")").c_str() : ""));

  if (result == SHANNON_SUCCESS) {
    auto m_share = new RapidShare(*source);
    m_share->m_source_table = source;
    m_share->is_partitioned = false;
    m_share->m_tableid = context.m_table_id;

    shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
    if (!shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str)) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), source->s->db.str, source->s->table_name.str);
      return HA_ERR_KEY_NOT_FOUND;
    }
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

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_parttable(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);
  ShannonBase::Utils::Util::close_table(thd, source);

  bool success = (result == SHANNON_SUCCESS);
  DBUG_PRINT("recovery", ("reload_partitioned_table: load_parttable %s for %s.%s%s", success ? "succeeded" : "failed",
                          info.schema_name.c_str(), info.table_name.c_str(),
                          success ? "" : (std::string(" (err=") + std::to_string(result) + ")").c_str()));

  if (success) {
    auto m_share = new RapidShare(*source);
    m_share->m_source_table = source;
    m_share->is_partitioned = true;
    m_share->m_tableid = context.m_table_id;

    shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
    if (!shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str)) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), source->s->db.str, source->s->table_name.str);
      return HA_ERR_KEY_NOT_FOUND;
    }
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
  if (!Utils::Util::wait_for_server_bootup(300)) {
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
    int ret = LoadFlagManager::instance().query_loaded_tables(thd, found);
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

void RecoveryFramework::invalidate_external_global_state() {
  DBUG_PRINT("recovery", ("RecoveryFramework: rapid_reload_on_restart=OFF - "
                          "invalidating external state to prevent stale data access"));
}

void RecoveryFramework::process_external_global_state() {
  if (!ShannonBase::shannon_rpd_engine_cfg.reload_on_restart) {
    if (m_global_state_empty.load()) invalidate_external_global_state();
    return;
  }

  std::string snap_dir;
  if (!ShannonBase::shannon_rpd_engine_cfg.snapshot_dir.empty())
    snap_dir = ShannonBase::shannon_rpd_engine_cfg.snapshot_dir;
  else
    snap_dir = std::string(mysql_real_data_home) + "/rapid_snapshots";

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
    DBUG_PRINT("recovery", ("RecoveryFramework: CheckpointScheduler started"));
  } else {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: CheckpointScheduler failed to start — "
           "fast-lane recovery unavailable this session");
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

/**
 * dispatch_jobs  (EXTENDED: log expected lane per table)
 * Core dispatch logic is unchanged from original.
 */
void RecoveryFramework::dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables) {
  if (tables.empty()) {
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: no tables to reload");
    return;
  }

  // Log which lane each table is expected to take.
  if (auto *sched = CheckpointScheduler::global()) {
    auto *rmgr = sched->recovery_manager();
    for (const auto &tbl : tables) {
      const uint64_t scn [[maybe_unused]] = rmgr ? rmgr->latest_checkpoint_scn(tbl.schema_name, tbl.table_name) : 0;
      DBUG_PRINT("recovery", ("RecoveryFramework: %s.%s → %s (checkpoint_scn=%llu)", tbl.schema_name.c_str(),
                              tbl.table_name.c_str(), scn > 0 ? "FAST (snapshot+WAL)" : "SLOW (DDL replay)",
                              static_cast<unsigned long long>(scn)));
    }
  }

  DBUG_PRINT("recovery", ("RecoveryFramework: dispatching reload jobs for %zu table(s)", tables.size()));

  for (const auto &tbl : tables) {
    if (m_stopped.load(std::memory_order_acquire)) break;

    m_active_jobs.fetch_add(1, std::memory_order_relaxed);

    std::thread job_thread([this, tbl]() mutable {
      RecoveryJob job(tbl);
      bool ok = job.execute();
      if (ok) m_reloaded_count.fetch_add(1, std::memory_order_relaxed);

      size_t remaining = m_active_jobs.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining == 0) {
        std::unique_lock<std::mutex> lk(m_jobs_mutex);
        m_jobs_cv.notify_all();
      }
    });

    {
      std::lock_guard<std::mutex> lock(m_job_threads_mutex);
      m_job_threads.push_back(std::move(job_thread));
    }
  }
}

void RecoveryFramework::startup() {
  if (m_started.exchange(true, std::memory_order_acq_rel)) return;

  DBUG_PRINT("recovery", ("RecoveryFramework: startup initiated"));

  bool empty = is_global_state_empty();
  m_global_state_empty.store(empty, std::memory_order_release);

  if (!empty) {
    DBUG_PRINT("recovery", ("RecoveryFramework: IMCS Global State is not empty - "
                            "skipping restart recovery"));
    return;
  }

  process_external_global_state();

  if (!m_dd_worker) return;

  m_monitoring_thread = std::thread([this]() {
    while (m_dd_worker && !m_dd_worker->is_done()) {
      if (m_stopped.load(std::memory_order_acquire)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!m_stopped.load(std::memory_order_acquire) && m_dd_worker) dispatch_jobs(m_dd_worker->found_tables());
  });
}

void RecoveryFramework::shutdown() {
  m_stopped.store(true, std::memory_order_release);
  if (m_monitoring_thread.joinable()) m_monitoring_thread.join();

  if (m_dd_worker) m_dd_worker->stop();

  // Drain in-flight recovery jobs (max 30 s).
  {
    std::unique_lock<std::mutex> lk(m_jobs_mutex);
    m_jobs_cv.wait_for(lk, std::chrono::seconds(30), [this] { return m_active_jobs.load() == 0; });
  }
  {
    std::lock_guard<std::mutex> lock(m_job_threads_mutex);
    for (auto &thread : m_job_threads)
      if (thread.joinable()) thread.join();
    m_job_threads.clear();
  }

  // Stop scheduler AFTER jobs drain (jobs may enqueue snapshots).
  CheckpointScheduler::set_global(nullptr);
  if (m_checkpoint_scheduler) m_checkpoint_scheduler->stop();

  if (m_dd_worker) m_dd_worker.reset();

  size_t total [[maybe_unused]] = m_reloaded_count.load();
  DBUG_PRINT("recovery", ("RecoveryFramework: shutdown complete - %zu table(s) reloaded "
                          "this session",
                          total));
}
}  // namespace Recovery
}  // namespace ShannonBase