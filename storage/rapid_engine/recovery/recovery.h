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
#ifndef __SHANNONBASE_RECOVERY_H__
#define __SHANNONBASE_RECOVERY_H__

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "my_inttypes.h"
#include "storage/rapid_engine/recovery/load_persist.h"

class THD;

namespace ShannonBase {
namespace Recovery {
/**
 * @class RecoveryAdminSession
 * @brief RAII wrapper that creates an admin-privilege THD for use inside
 *        background recovery threads.
 *
 * Each background worker (DDWorker, RecoveryJob) must operate within its own
 * MySQL thread context so that it can issue SQL queries. This class:
 *   1. Calls my_thread_init() to initialise thread-local state.
 *   2. Allocates a new THD with COM_DAEMON command and skip_grants authority.
 *   3. Stores the THD as the thread global via store_globals().
 *   4. On destruction, restores prior globals, detaches, and cleans up.
 *
 * Usage:
 *   void my_background_func() {
 *     RecoveryAdminSession session;
 *     if (!session.is_valid()) return;
 *     session.thd()->set_query(…);
 *     // use session.thd() for SQL execution …
 *   }
 */
class RecoveryAdminSession {
 public:
  RecoveryAdminSession();
  ~RecoveryAdminSession();

  // Non-copyable, non-movable (RAII resource).
  RecoveryAdminSession(const RecoveryAdminSession &) = delete;
  RecoveryAdminSession &operator=(const RecoveryAdminSession &) = delete;

  /** Returns the created THD, or nullptr if initialisation failed. */
  THD *thd() const { return m_thd; }

  /** Returns true if the session was created successfully. */
  bool is_valid() const { return m_thd != nullptr; }

 private:
  THD *m_thd{nullptr};
};

/**
 * @class DD_KillImmunizer
 * @brief RAII guard that prevents MySQL from killing a THD's query while the
 *        recovery DD worker is executing queries.
 *
 * Problem (Bug#33752387):
 *   A MySQL shutdown sends kill signals to all active connections. If the DD
 *   Worker's query is killed mid-execution, the Ed_connection may be left in
 *   an inconsistent state and crash.
 *
 * Solution:
 *   Before issuing any query, wrap the call with DD_KillImmunizer. This sets
 *   THD::killed = NOT_KILLED for the duration and restores the original value
 *   on destruction.
 *
 * Usage:
 *   {
 *     DD_KillImmunizer guard(thd);
 *     // queries here cannot be killed by MySQL shutdown
 *   } // restores original killed state
 */
class DD_KillImmunizer {
 public:
  explicit DD_KillImmunizer(THD *thd);
  ~DD_KillImmunizer();

  DD_KillImmunizer(const DD_KillImmunizer &) = delete;
  DD_KillImmunizer &operator=(const DD_KillImmunizer &) = delete;

 private:
  THD *m_thd{nullptr};
  int m_saved_killed{0};  // stores THD::killed value on entry
};

// ─────────────────────────────────────────────────────────────────────────────
// RecoveryJob – single-table reload unit
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class RecoveryJob
 * @brief Encapsulates a single-table reload task performed during restart
 *        recovery.
 *
 * The job is responsible for:
 *   1. Opening a new admin session in a background thread.
 *   2. Issuing "ALTER TABLE db.tbl SECONDARY_LOAD" through the normal IMCS
 *      load path (Imcs::load_table / load_parttable).
 *   3. Reporting success or failure without marking the entire recovery as
 *      FAILED (because OOM or other transient errors during restart reload
 *      should not block other tables – see Bug#34197659).
 *
 * Patch #5 note:
 *   Before loading, RecoveryJob checks whether the table is already present
 *   in the in-memory Global State (Imcs::get_rpd_table). If it is, the job
 *   is skipped so that a table loaded by a concurrent user request before the
 *   DD Worker finishes is not double-processed.
 */
class RecoveryJob {
 public:
  explicit RecoveryJob(const SecondaryLoadedTable &table_info) : m_table_info(table_info) {}

  ~RecoveryJob() = default;

  /**
   * @brief Execute the reload job synchronously in the calling thread.
   *
   * This is invoked by RecoveryFramework from within the DDWorker thread pool.
   * Errors are logged but do not propagate upward (recovery continues for
   * other tables regardless of this job's outcome).
   *
   * @return true if the table was successfully reloaded, false otherwise.
   */
  bool execute();

  const SecondaryLoadedTable &table_info() const { return m_table_info; }

 private:
  SecondaryLoadedTable m_table_info;

  /**
   * @brief Execute the actual SECONDARY_LOAD for a non-partitioned table.
   * @param thd Admin session THD.
   * @return true on success.
   */
  bool reload_normal_table(THD *thd);

  /**
   * @brief Execute the actual SECONDARY_LOAD for a partitioned table.
   * @param thd Admin session THD.
   * @return true on success.
   */
  bool reload_partitioned_table(THD *thd);
};

// ─────────────────────────────────────────────────────────────────────────────
// DDWorker – Data Dictionary worker thread (Patch #2)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class DDWorker
 * @brief Background thread that queries the Data Dictionary for tables that
 *        need to be reloaded after a MySQL restart.
 *
 * The DDWorker is started by RecoveryFramework during plugin initialisation
 * (before the recovery loop begins, per Patch #4 design). It:
 *   1. Waits until MySQL's server bootup is complete (up to 300s to account
 *      for long MySQL version upgrades – Patch #4).
 *   2. Opens a RecoveryAdminSession.
 *   3. Wraps all queries in a DD_KillImmunizer (Bug#33752387).
 *   4. Calls LoadFlagManager::query_loaded_tables() to enumerate tables with
 *      secondary_load=1 in their CREATE_OPTIONS.
 *   5. For each found table, creates a RecoveryJob and dispatches it.
 *   6. Exits cleanly when m_stop is set (during plugin shutdown).
 *
 * Thread lifecycle:
 *   start()  → spawns m_thread
 *   stop()   → sets m_stop, notifies m_cv, joins m_thread
 */
class DDWorker {
 public:
  DDWorker() = default;
  ~DDWorker() { stop(); }

  DDWorker(const DDWorker &) = delete;
  DDWorker &operator=(const DDWorker &) = delete;

  /**
   * @brief Start the DDWorker background thread.
   * @return true if the thread was started successfully.
   */
  bool start();

  /**
   * @brief Signal the DDWorker to stop and wait for it to exit.
   */
  void stop();

  /** @brief Returns true if the DDWorker has completed its query phase. */
  bool is_done() const { return m_done.load(std::memory_order_acquire); }

  /**
   * @brief Returns the list of tables found during the DD scan.
   *        Safe to call only after is_done() returns true.
   */
  const std::vector<SecondaryLoadedTable> &found_tables() const { return m_found_tables; }

 private:
  void run();  // thread body

  /** @brief Wait for MySQL server bootup to complete. */
  bool wait_for_server_bootup(int timeout_seconds = 300);

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;

  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_done{false};

  std::vector<SecondaryLoadedTable> m_found_tables;
};

// ─────────────────────────────────────────────────────────────────────────────
// RecoveryFramework – top-level orchestrator (Patches #2 / #4)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class RecoveryFramework
 * @brief Top-level singleton that orchestrates ShannonBase restart recovery.
 *
 * Responsibilities:
 *   1. Detect at startup whether the IMCS Global State is empty (indicating a
 *      restart rather than a hot plugin reload).
 *   2. If rapid_reload_on_restart is ON and Global State is empty, start the
 *      DDWorker to find tables that were loaded before the restart.
 *   3. Dispatch RecoveryJob instances for each discovered table.
 *   4. If rapid_reload_on_restart is OFF, invalidate the external Global State
 *      (if any) so stale metadata does not confuse query planning.
 *   5. Provide a clean shutdown path that stops the DDWorker and waits for all
 *      in-flight jobs to complete.
 *
 * Patch #5:
 *   Before dispatching a RecoveryJob, check whether the table is already
 *   present in the in-memory IMCS state (loaded by a concurrent user request).
 *   If so, skip the job.
 *
 * Usage (called from plugin init):
 *   RecoveryFramework::instance().startup();
 *
 * Usage (called from plugin deinit):
 *   RecoveryFramework::instance().shutdown();
 */
class RecoveryFramework {
 public:
  static RecoveryFramework &instance() {
    static RecoveryFramework inst;
    return inst;
  }

  /**
   * @brief Called during ShannonBase plugin initialisation.
   *
   * Determines whether recovery is needed, starts the DDWorker when
   * appropriate, and dispatches recovery jobs.
   *
   * This function returns quickly (the DDWorker runs asynchronously).
   */
  void startup();

  /**
   * @brief Called during ShannonBase plugin de-initialisation.
   *
   * Stops the DDWorker and waits for all pending recovery jobs to finish.
   */
  void shutdown();

  /**
   * @brief Returns true if the framework detected that Global State was empty
   *        at startup (i.e., a restart scenario).
   */
  bool global_state_was_empty() const { return m_global_state_empty.load(); }

  /**
   * @brief Returns the total number of tables that were successfully reloaded
   *        during this recovery session.
   */
  size_t reloaded_count() const { return m_reloaded_count.load(); }

 private:
  RecoveryFramework() = default;
  ~RecoveryFramework() = default;
  RecoveryFramework(const RecoveryFramework &) = delete;
  RecoveryFramework &operator=(const RecoveryFramework &) = delete;

  /**
   * @brief Check whether the current IMCS Global State is empty.
   *        An empty state indicates a restart (no tables loaded in memory yet).
   * @return true if no tables are currently loaded in IMCS.
   */
  bool is_global_state_empty() const;

  /**
   * @brief Process the external Global State before starting the recovery loop.
   *        Patch #4: single entry point for external state processing.
   *
   * - If rapid_reload_on_restart is OFF → invalidate the external state.
   * - If rapid_reload_on_restart is ON  → start the DDWorker.
   */
  void process_external_global_state();

  /**
   * @brief Dispatch RecoveryJob objects for all tables found by the DDWorker.
   *        Called from the DDWorker callback after the DD scan is complete.
   * @param tables List of tables to reload.
   */
  void dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables);

  /**
   * @brief Invalidate the external Global State when reload is disabled.
   *        This prevents stale metadata from affecting query planning.
   */
  void invalidate_external_global_state();

  // ── State ──────────────────────────────────────────────────────────────────

  /** True if Global State was empty when startup() was called. */
  std::atomic<bool> m_global_state_empty{false};

  /** Number of successfully reloaded tables this session. */
  std::atomic<size_t> m_reloaded_count{0};

  /** The Data Dictionary worker thread. */
  std::unique_ptr<DDWorker> m_dd_worker;

  /** Protects m_active_jobs and m_jobs_done. */
  std::mutex m_jobs_mutex;
  std::condition_variable m_jobs_cv;
  std::atomic<size_t> m_active_jobs{0};

  /** Set to true once startup() has been called. */
  std::atomic<bool> m_started{false};

  /** Set to true once shutdown() has been called. */
  std::atomic<bool> m_stopped{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Free helper – called from ha_shannon_rapid.cc plugin init / deinit
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Entry point called from the ShannonBase plugin init function.
 *
 * Delegates to RecoveryFramework::instance().startup().
 * Must be called after Imcs::initialize() so that the Global State check
 * reflects the actual in-memory table count.
 */
inline void rapid_recovery_startup() { RecoveryFramework::instance().startup(); }

/**
 * @brief Entry point called from the ShannonBase plugin deinit function.
 *
 * Delegates to RecoveryFramework::instance().shutdown().
 */
inline void rapid_recovery_shutdown() { RecoveryFramework::instance().shutdown(); }
}  // namespace Recovery
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RECOVERY_H__