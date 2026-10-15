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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#ifndef SHANNON_RECOVERY_SERVICE_H
#define SHANNON_RECOVERY_SERVICE_H
#include "storage/rapid_engine/recovery/recovery.h"
#include "storage/rapid_engine/recovery/recovery_load.h"
#include "storage/rapid_engine/recovery/source_validation.h"

namespace ShannonBase {
namespace Service {
class RecoveryJob {
 public:
  explicit RecoveryJob(const Recovery::SecondaryLoadedTable &table_info,
                       Recovery::SourceValidationPolicy validation = Recovery::SourceRecoveryValidation::current())
      : m_table_info(table_info), m_source_validation(std::move(validation)) {}
  ~RecoveryJob() = default;

  /**
   * Execute recovery for this table.
   *
   * Restore a certified columnar checkpoint and committed MySQL binlog when
   * source history is complete. Otherwise discard the old epoch and reload
   * from recovered InnoDB. Both paths fence source writes through publication.
   *
   * @return true only if recovering and registering the table succeeded.
   */
  bool execute();

  const Recovery::SecondaryLoadedTable &table_info() const { return m_table_info; }

 private:
  bool try_snapshot_recovery(THD *thd);
  bool reconstruct_field_pointers(THD *thd, Imcs::RpdTable *rpd_table, TABLE *&out_source);
  bool register_in_loaded_tables(THD *thd, TABLE *source, Imcs::RpdTable *rpd_table);
  void schedule_checkpoint_async();

  /** Start the change-propagation coordinator for a table this job restored. */
  void start_change_propagation() const;

  /**
   * Drop checkpoint generations left by the previous epoch before
   * rebuilding this table from InnoDB (see TablePersistenceManager::reset_epoch()).
   */
  bool discard_stale_recovery_state();

  // Authoritative primary-engine reload.
  bool reload_normal_table(THD *thd);
  bool reload_partitioned_table(THD *thd);

  Recovery::SecondaryLoadedTable m_table_info;
  Recovery::SourceValidationPolicy m_source_validation;
  bool m_source_validation_failed{false};
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
  const std::vector<Recovery::SecondaryLoadedTable> &found_tables() const { return m_found_tables; }

 private:
  void run();

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;

  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_done{false};

  std::vector<Recovery::SecondaryLoadedTable> m_found_tables;
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
  bool restart_in_progress() const { return m_restart_in_progress.load(std::memory_order_acquire); }

 private:
  RecoveryFramework() = default;
  ~RecoveryFramework() = default;
  RecoveryFramework(const RecoveryFramework &) = delete;
  RecoveryFramework &operator=(const RecoveryFramework &) = delete;

  bool is_global_state_empty() const;
  void process_external_global_state();
  void dispatch_jobs(const std::vector<Recovery::SecondaryLoadedTable> &tables);
  void invalidate_external_global_state();
  static std::string resolve_snapshot_dir();

  std::thread m_monitoring_thread;

  std::atomic<bool> m_global_state_empty{false};
  std::atomic<size_t> m_reloaded_count{0};

  std::unique_ptr<DDWorker> m_dd_worker;
  std::unique_ptr<Recovery::CheckpointScheduler> m_checkpoint_scheduler;

  std::mutex m_jobs_mutex;
  std::condition_variable m_jobs_cv;
  std::atomic<size_t> m_active_jobs{0};

  std::atomic<bool> m_started{false};
  std::atomic<bool> m_restart_in_progress{false};
  std::atomic<bool> m_stopped{false};

  std::vector<std::thread> m_job_threads;
  std::mutex m_job_threads_mutex;
};

// Plugin entry points
inline void rapid_recovery_startup() { RecoveryFramework::instance().startup(); }
inline void rapid_recovery_shutdown() { RecoveryFramework::instance().shutdown(); }

}  // namespace Service
}  // namespace ShannonBase
#endif
