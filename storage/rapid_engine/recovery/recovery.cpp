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
#include <thread>

#include "include/my_dbug.h"
#include "sql/dd/dd_kill_immunizer.h"  // dd::DD_kill_immunizer
#include "sql/handler.h"               // handler::ha_records
#include "sql/mysqld.h"                // connection_events_loop_aborted
#include "sql/sql_base.h"              // close_thread_tables
#include "sql/sql_class.h"             // THD
#include "sql/table.h"                 // TABLE

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"  // shannon_loaded_tables
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"  // shannon_rpd_engine_cfg
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/recovery/recovery_load.h"
#include "storage/rapid_engine/utils/utils.h"  // Util::open_table_by_name

namespace ShannonBase {
namespace Recovery {
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
    if (!connection_events_loop_aborted() && !m_thd->killed) {
      close_thread_tables(m_thd);
    } else {
      for (TABLE *t = m_thd->open_tables; t; t = t->next)
        if (t->file) t->file->ha_external_lock(m_thd, F_UNLCK);
      m_thd->open_tables = nullptr;
    }
  }
  m_thd->mdl_context.release_transactional_locks();
  m_thd->release_resources();
  delete m_thd;
  m_thd = nullptr;
  my_thread_end();
}

bool RecoveryJob::execute() {
  //  Ed_connection + "ALTER TABLE ... SECONDARY_LOAD"
  const auto &info = m_table_info;

  DBUG_PRINT("recovery", ("RecoveryJob::execute - %s.%s (partitioned=%d)", info.schema_name.c_str(),
                          info.table_name.c_str(), info.is_partitioned ? 1 : 0));

  // Skip if already present in IMCS
  {
    auto share = shannon_loaded_tables->get(info.schema_name, info.table_name);
    if (share) {
      DBUG_PRINT("recovery",
                 ("RecoveryJob: skip %s.%s - already in IMCS", info.schema_name.c_str(), info.table_name.c_str()));
      return true;
    }
  }

  // Create admin session for performing the reload. This is necessary to avoid interference with user sessions, and
  // also to ensure the session has the necessary privileges to read from the source table and write to IMCS.
  RecoveryAdminSession session;
  if (!session.is_valid()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryJob: cannot create admin session for %s.%s",
           info.schema_name.c_str(), info.table_name.c_str());
    return false;
  }

  // Perform the reload
  bool ok = info.is_partitioned ? reload_partitioned_table(session.thd()) : reload_normal_table(session.thd());
  LogErr(ok ? INFORMATION_LEVEL : WARNING_LEVEL, ER_LOG_PRINTF_MSG,
         ok ? "RecoveryJob: successfully reloaded %s.%s" : "RecoveryJob: FAILED to reload %s.%s (recovery continues)",
         info.schema_name.c_str(), info.table_name.c_str());
  return ok;
}

bool RecoveryJob::reload_normal_table(THD *thd) {
  const auto &info = m_table_info;

  TABLE *source =
      ShannonBase::Utils::Util::open_table_by_name(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  if (!source) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryJob: cannot open table %s.%s for reading", info.schema_name.c_str(),
           info.table_name.c_str());
    return false;
  }

  Rapid_load_context context;
  context.m_schema_name = info.schema_name;
  context.m_table_name = info.table_name;
  context.m_thd = thd;
  context.m_sch_tb_name = info.schema_name + "." + info.table_name;
  context.m_table = source;
  context.m_table_id = source->file->get_table_id();

  // Query row count (used for progress tracking internally by IMCS).
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
    // m_share->file = this;
    m_share->m_tableid = context.m_table_id;

    shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
    if (shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str) == nullptr) {
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
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryJob: cannot open partitioned table %s.%s for reading",
           info.schema_name.c_str(), info.table_name.c_str());
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
    // m_share->file = this;
    m_share->m_tableid = context.m_table_id;

    shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
    if (shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str) == nullptr) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), source->s->db.str, source->s->table_name.str);
      return HA_ERR_KEY_NOT_FOUND;
    }
  }
  return success;
}

bool DDWorker::start() {
  if (m_thread.joinable()) return true;  // already running

  m_stop.store(false, std::memory_order_release);
  m_done.store(false, std::memory_order_release);

  try {
    m_thread = std::thread(&DDWorker::run, this);
  } catch (const std::system_error &e) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "DDWorker::start failed: %s", e.what());
    return false;
  }

  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "DDWorker: background thread started");
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
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "DDWorker: background thread stopped");
  }
}

void DDWorker::run() {
  // Wait for MySQL bootup
  // Query DD before the recovery loop, not after node connect.
  if (!Utils::Util::wait_for_server_bootup(300 /* 5 minutes */)) {
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

  // Query Data Dictionary with kill immunity
  {
    /* that prevents MySQL from killing a THD's query while the recovery DD worker is executing queries.
     * A MySQL shutdown sends kill signals to all active connections. If the DD
     * Worker's query is killed mid-execution, the Ed_connection may be left in an inconsistent state and crash. */
    const dd::DD_kill_immunizer kill_immunizer(thd);

    std::vector<SecondaryLoadedTable> found;
    int ret = LoadFlagManager::instance().query_loaded_tables(thd, found);
    if (ret != 0) {
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
             "DDWorker: query_loaded_tables returned error %d - skipping restart reload", ret);
      return;
    }

    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
           "DDWorker: found %zu table(s) with secondary_load=1 in Data Dictionary", found.size());
    m_found_tables = std::move(found);
  }

  m_done.store(true, std::memory_order_release);
  return;
}

bool RecoveryFramework::is_global_state_empty() const {
  std::atomic<size_t> count{0};
  ShannonBase::Imcs::Imcs::instance()->for_each_table([&count](ShannonBase::Imcs::RpdTable *) { count++; });
  return count.load() == 0;
}

void RecoveryFramework::invalidate_external_global_state() {
  // When rapid_reload_on_restart is OFF, we skip table reload. If there was
  // any stale external state (e.g., from a previous run), we invalidate it so
  // query planning does not reference tables that are not in memory.
  //
  // In the current ShannonBase design the "external global state" is the
  // in-memory table registry m_rpd_tables / m_rpd_parttables. Since it is
  // empty at startup (confirmed by is_global_state_empty()), there is nothing
  // to invalidate. This function exists as an extension point for future
  // integrations (e.g., Object Store recovery).
  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
         "RecoveryFramework: rapid_reload_on_restart=OFF - external state invalidated");
}

void RecoveryFramework::process_external_global_state() {
  if (!ShannonBase::shannon_rpd_engine_cfg.reload_on_restart) {
    // If reload is disabled AND global state is empty, invalidate.
    if (m_global_state_empty.load()) invalidate_external_global_state();
    return;
  }

  // rapid_reload_on_restart is ON → start DDWorker.
  m_dd_worker = std::make_unique<DDWorker>();
  if (!m_dd_worker->start()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: failed to start DDWorker - restart reload will not be performed");
    m_dd_worker.reset();
  }
}

void RecoveryFramework::dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables) {
  if (tables.empty()) {
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: no tables to reload");
    return;
  }

  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: dispatching reload jobs for %zu table(s)",
         tables.size());

  for (const auto &tbl : tables) {
    if (m_stopped.load(std::memory_order_acquire)) break;

    m_active_jobs.fetch_add(1, std::memory_order_relaxed);

    // Each job runs in its own detached thread to parallelize reloads and
    // ensure that a single slow/OOM table does not block other tables.
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
  if (m_started.exchange(true, std::memory_order_acq_rel)) {
    return;  // idempotent
  }

  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: startup initiated");

  bool empty = is_global_state_empty();
  m_global_state_empty.store(empty, std::memory_order_release);

  if (!empty) {
    // Tables are already in memory (hot reload scenario, not a restart).
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: IMCS Global State is not empty - skipping restart recovery");
    return;
  }

  process_external_global_state();

  if (!m_dd_worker) {
    return;  // reload is OFF or DDWorker failed to start
  }

  // Spawn monitoring thread
  // The monitoring thread waits for DDWorker to complete its DD scan, then
  // dispatches reload jobs. This keeps startup() non-blocking.
  m_monitoring_thread = std::thread([this]() {
    while (m_dd_worker && !m_dd_worker->is_done()) {
      if (m_stopped.load(std::memory_order_acquire)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!m_stopped.load(std::memory_order_acquire) && m_dd_worker) {
      dispatch_jobs(m_dd_worker->found_tables());
    }
  });
}

void RecoveryFramework::shutdown() {
  m_stopped.store(true, std::memory_order_release);
  if (m_monitoring_thread.joinable()) m_monitoring_thread.join();

  if (m_dd_worker) m_dd_worker->stop();

  {
    std::unique_lock<std::mutex> lk(m_jobs_mutex);
    m_jobs_cv.wait_for(lk, std::chrono::seconds(30), [this] { return m_active_jobs.load() == 0; });
  }

  {
    std::lock_guard<std::mutex> lock(m_job_threads_mutex);
    for (auto &thread : m_job_threads) {
      if (thread.joinable()) thread.join();
    }
    m_job_threads.clear();
  }

  if (m_dd_worker) m_dd_worker.reset();

  size_t total = m_reloaded_count.load();
  LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
         "RecoveryFramework: shutdown complete - %zu table(s) reloaded this session", total);
}
}  // namespace Recovery
}  // namespace ShannonBase
