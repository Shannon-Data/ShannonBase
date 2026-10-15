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
#ifndef __SHANNONBASE_RECOVERY_H__
#define __SHANNONBASE_RECOVERY_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "my_inttypes.h"
#include "storage/rapid_engine/imcs/table0persistence.h"  // per-table checkpoint storage

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

  /**
   * Checkpoint the whole table that @a imcu belongs to into one new generation
   * (a table-level operation; @a imcu only identifies the table). @a scn is
   * informational. Each generation records its committed-binlog boundary.
   */
  bool checkpoint_imcu(const std::string &db, const std::string &tbl, Imcs::Imcu *imcu, uint64_t scn);

  /**
   * Return the shared per-table checkpoint and notification manager.
   */
  std::shared_ptr<Imcs::TablePersistenceManager> table_manager(const std::string &db, const std::string &tbl);

  /**
   * True when at least one checkpoint generation is on disk for this table,
   * This establishes file availability, not source transaction consistency.
   */
  bool has_durable_checkpoint(const std::string &db, const std::string &tbl);

  /**
   * Load per-IMCU snapshots into an unpublished RpdTable. Before publication,
   * RecoveryJob must validate and replay committed MySQL binlog; otherwise
   * it must discard this reconstruction and reload from InnoDB.
   * Field* are left nullptr; caller patches them via an open TABLE afterwards.
   * @return true if at least one IMCU was recovered.
   */
  bool load_from_snapshots(const std::string &db, const std::string &tbl, Imcs::RpdTable *rpd_table,
                           uint64_t *restored_generation = nullptr);

  /** Remove all checkpoint files for a table, including its reload-required marker. */
  void purge_table(const std::string &db, const std::string &tbl);

  /**
   * Fence for tables whose on-disk image outlives the process.
   *
   * Capture only runs for tables registered in IMCS, and a restored table is
   * registered last. Any source commit between process start and that
   * registration is absent from volatile notification tracking, while the old checkpoint
   * certificate stays valid -- a fast restore would then publish a stale image.
   *
   * scan_pending_recovery() lists, synchronously and before connections are
   * accepted, every table directory that holds checkpoints.
   * note_unregistered_change() is called by the source-DML hook for a table that
   * is not registered; if the table is pending it durably revokes fast recovery
   * (so the restart path reloads from the primary). RecoveryJob calls
   * clear_recovery_pending() once the table's state is settled.
   *
   * @return number of pending tables.
   */
  size_t scan_pending_recovery();
  void note_unregistered_change(const char *db, const char *tbl);
  void clear_recovery_pending(const std::string &db, const std::string &tbl);

  /**
   * For sessions in which capture will not run (rapid_reload_on_restart=OFF): the
   * on-disk images cannot be certified for the changes this session will make, so
   * revoke every one of them.
   * @return number of tables revoked.
   */
  size_t revoke_all_on_disk();

 private:
  std::shared_ptr<Imcs::TablePersistenceManager> get_table_mgr(const std::string &db, const std::string &tbl);
  std::filesystem::path table_dir(const std::string &db, const std::string &tbl) const;

  // One slot per table. The manager is opened outside m_mutex (open() scans the
  // whole physical journal and the capture journal), exactly once, so a slow
  // table no longer blocks table_manager() calls for every other table.
  struct Slot {
    std::shared_ptr<Imcs::TablePersistenceManager> mgr;
    std::once_flag opened;
  };

  std::string m_base_dir;
  std::mutex m_mutex;
  // key = db + '\x01' + tbl
  std::unordered_map<std::string, std::shared_ptr<Slot>> m_per_table;

  // Pending-recovery fence. key = lower-cased db + '\x01' + tbl; value = names as on disk.
  std::mutex m_pending_mutex;
  std::unordered_map<std::string, std::pair<std::string, std::string>> m_pending;
  std::atomic<size_t> m_pending_count{0};
};

/**
 * Source-DML hook for a table not registered in IMCS: durably revoke the
 * restart-pending on-disk image so a restart reloads from the primary.
 *
 * No-throw: the checkpoint scheduler may be absent during startup/shutdown.
 */
void revoke_unregistered_table_image(const char *db, const char *tbl) noexcept;

// Background thread. Two triggers:
//   1. Periodic sweep of READ_ONLY IMCUs every `interval` seconds.
//   2. On-demand queue: RecoveryJob::execute() enqueues after a primary reload
//      to start checkpoint maintenance in the new epoch.
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

}  // namespace Recovery
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RECOVERY_H__
