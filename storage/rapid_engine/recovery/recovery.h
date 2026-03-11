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
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "my_inttypes.h"
#include "storage/rapid_engine/imcs/cu_recovery.h"  // per-table snapshot + WAL I/O
#include "storage/rapid_engine/recovery/recovery_load.h"

class THD;
class TABLE;

namespace ShannonBase {
namespace Imcs {
class Imcu;
class RpdTable;
}  // namespace Imcs

namespace Recovery {
class RecoveryManager {
 public:
  explicit RecoveryManager(std::string base_dir);
  ~RecoveryManager();

  RecoveryManager(const RecoveryManager &) = delete;
  RecoveryManager &operator=(const RecoveryManager &) = delete;

  /** Persist one IMCU snapshot. snapshot_lsn=0 → use current WAL head. */
  bool checkpoint_imcu(const std::string &db, const std::string &tbl, Imcs::Imcu *imcu, uint64_t scn);

  /**
   * Return the latest snapshot LSN for (db, tbl), or 0 if no snapshot exists.
   * A non-zero return means the fast lane is viable for this table.
   */
  uint64_t latest_checkpoint_scn(const std::string &db, const std::string &tbl);

  /**
   * Load all per-IMCU snapshots into an already-created RpdTable, then
   * replay WAL records that post-date the snapshots.
   * Field* are left nullptr; caller patches them via an open TABLE afterwards.
   * @return true if at least one IMCU was recovered.
   */
  bool load_from_snapshots(const std::string &db, const std::string &tbl, Imcs::RpdTable *rpd_table);

  /** fdatasync all open WAL streams. */
  bool sync();

  /** Remove all snapshot + WAL files for a table. */
  void purge_table(const std::string &db, const std::string &tbl);

 private:
  Imcs::CURecoveryManager *get_table_mgr(const std::string &db, const std::string &tbl);
  std::filesystem::path table_dir(const std::string &db, const std::string &tbl) const;

  std::string m_base_dir;
  std::mutex m_mutex;
  // key = db + '\x01' + tbl
  std::unordered_map<std::string, std::unique_ptr<Imcs::CURecoveryManager>> m_per_table;
};

// Background thread. Two triggers:
//   1. Periodic sweep of READ_ONLY IMCUs every `interval` seconds.
//   2. On-demand queue: RecoveryJob::execute() enqueues after a slow-lane
//      reload so the next restart can take the fast lane.
class CheckpointScheduler {
 public:
  struct Config {
    std::string snapshot_base_dir;
    std::chrono::seconds interval{300};
  };

  explicit CheckpointScheduler(Config cfg);
  ~CheckpointScheduler();

  CheckpointScheduler(const CheckpointScheduler &) = delete;
  CheckpointScheduler &operator=(const CheckpointScheduler &) = delete;

  bool start();
  void stop();

  /** Enqueue an on-demand checkpoint. Non-blocking; I/O on bg thread. */
  void enqueue(const std::string &schema_name, const std::string &table_name);

  RecoveryManager *recovery_manager() { return m_mgr.get(); }

  static CheckpointScheduler *global();
  static void set_global(CheckpointScheduler *s);

 private:
  void run();
  void do_periodic_checkpoint();
  void do_ondemand_checkpoints();

  Config m_cfg;
  std::unique_ptr<RecoveryManager> m_mgr;

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::atomic<bool> m_stop{false};

  std::vector<std::pair<std::string, std::string>> m_queue;

  static std::atomic<CheckpointScheduler *> s_global;
};

class RecoveryAdminSession {
 public:
  RecoveryAdminSession();
  ~RecoveryAdminSession();

  RecoveryAdminSession(const RecoveryAdminSession &) = delete;
  RecoveryAdminSession &operator=(const RecoveryAdminSession &) = delete;

  THD *thd() const { return m_thd; }
  bool is_valid() const { return m_thd != nullptr; }

 private:
  THD *m_thd{nullptr};
};

class RecoveryJob {
 public:
  explicit RecoveryJob(const SecondaryLoadedTable &table_info) : m_table_info(table_info) {}
  ~RecoveryJob() = default;

  /**
   * Execute recovery for this table.
   *
   * Two-lane strategy (new):
   *   1. [FAST] try_snapshot_recovery() — .snap + WAL replay.
   *   2. [SLOW] original DDL-replay via reload_normal/partitioned_table().
   *      On success, schedules an async checkpoint so the next restart is fast.
   *
   * @return true if the table was successfully loaded by either lane.
   */
  bool execute();

  const SecondaryLoadedTable &table_info() const { return m_table_info; }

 private:
  // Fast mode.
  bool try_snapshot_recovery(THD *thd);
  bool patch_field_pointers(THD *thd, Imcs::RpdTable *rpd_table, TABLE *&out_source);
  bool register_in_loaded_tables(THD *thd, TABLE *source, Imcs::RpdTable *rpd_table);
  void schedule_checkpoint_async();

  // Slow mode.
  bool reload_normal_table(THD *thd);
  bool reload_partitioned_table(THD *thd);

  SecondaryLoadedTable m_table_info;
};

class DDWorker {
 public:
  DDWorker() = default;
  ~DDWorker() { stop(); }

  DDWorker(const DDWorker &) = delete;
  DDWorker &operator=(const DDWorker &) = delete;

  bool start();
  void stop();

  bool is_done() const { return m_done.load(std::memory_order_acquire); }
  const std::vector<SecondaryLoadedTable> &found_tables() const { return m_found_tables; }

 private:
  void run();

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;

  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_done{false};

  std::vector<SecondaryLoadedTable> m_found_tables;
};

class RecoveryFramework {
 public:
  static RecoveryFramework &instance() {
    static RecoveryFramework inst;
    return inst;
  }

  void startup();
  void shutdown();

  bool global_state_was_empty() const { return m_global_state_empty.load(); }
  size_t reloaded_count() const { return m_reloaded_count.load(); }

 private:
  RecoveryFramework() = default;
  ~RecoveryFramework() = default;
  RecoveryFramework(const RecoveryFramework &) = delete;
  RecoveryFramework &operator=(const RecoveryFramework &) = delete;

  bool is_global_state_empty() const;
  void process_external_global_state();
  void dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables);
  void invalidate_external_global_state();

  std::thread m_monitoring_thread;

  std::atomic<bool> m_global_state_empty{false};
  std::atomic<size_t> m_reloaded_count{0};

  std::unique_ptr<DDWorker> m_dd_worker;
  std::unique_ptr<CheckpointScheduler> m_checkpoint_scheduler;  // NEW

  std::mutex m_jobs_mutex;
  std::condition_variable m_jobs_cv;
  std::atomic<size_t> m_active_jobs{0};

  std::atomic<bool> m_started{false};
  std::atomic<bool> m_stopped{false};

  std::vector<std::thread> m_job_threads;
  std::mutex m_job_threads_mutex;
};

// Plugin entry points
inline void rapid_recovery_startup() { RecoveryFramework::instance().startup(); }
inline void rapid_recovery_shutdown() { RecoveryFramework::instance().shutdown(); }
}  // namespace Recovery
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RECOVERY_H__