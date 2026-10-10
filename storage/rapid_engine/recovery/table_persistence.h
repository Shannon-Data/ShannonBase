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

   Copyright (c) 2023 - 2026, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_TABLE_PERSISTENCE_H__
#define __SHANNONBASE_TABLE_PERSISTENCE_H__
#include <atomic>
#include <filesystem>
#include <istream>
#include <mutex>
#include <string>
#include <vector>
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/recovery/checkpoint_store.h"
#include "storage/rapid_engine/recovery/durable_fs.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"
namespace ShannonBase::Imcs {
class Imcu;
static constexpr uint32_t SNAP_MAGIC = 0x50414E53u;
static constexpr uint16_t SNAP_FORMAT_VER = 2u;
static constexpr uint32_t MANIFEST_MAGIC = 0x4E414D52u;
static constexpr uint16_t MANIFEST_FORMAT_VER = 2u;
static constexpr uint64_t MAX_CU_SNAPSHOT_SIZE = (1ull << 40);
static constexpr size_t kMaxRetainedGenerations = 2;
static constexpr size_t SNAP_FILE_HEADER_SIZE = 36;
enum class ManifestImcuState : uint8_t {
  NEVER_CHECKPOINTED = 0,
  CHECKPOINTED = 1,
};

struct ManifestImcuEntry {
  uint32_t imcu_id{0};
  ManifestImcuState state{ManifestImcuState::NEVER_CHECKPOINTED};
  uint64_t snapshot_sequence{0};
  uint64_t snapshot_size{0};
  uint32_t snapshot_crc{0};
  std::string snapshot_file;
};

/**
 * Per-table recovery manifest.  Authoritatively records which IMCUs exist,
 * whether each has a durable checkpoint, the schema fingerprint the snapshot
 * was written under, and the committed MySQL binlog boundary.
 */
struct RecoveryManifest {
  uint64_t table_id{0};
  uint64_t generation{0};
  uint64_t schema_fingerprint{0};
  Recovery::BinlogPosition binlog;
  std::vector<ManifestImcuEntry> imcus;
};

// Owns immutable committed checkpoints. MySQL binlog is the only replay log.
// Legacy manifests are rejected and rebuilt from InnoDB; there is no WAL I/O.
class TablePersistenceManager {
 public:
  TablePersistenceManager(const std::string &data_dir, const std::string &db, const std::string &table);
  ~TablePersistenceManager() = default;
  bool open();
  void close() {}
  bool reset_epoch();
  bool erase_store() { return m_store->erase(m_db_name, m_tbl_name); }
  bool enable_notifications() { return reset_epoch(); }
  Recovery::NotificationTracker *notifications() { return &m_notifications; }
  bool checkpoint(Imcu *trigger, uint64_t sequence = 0);
  Result<uint64_t> load_snapshot(Imcu *imcu, uint64_t generation);
  Result<size_t> recover(const std::vector<Imcu *> &imcus, uint64_t *restored_generation = nullptr);
  std::filesystem::path snap_path(uint64_t generation, uint32_t imcu_id) const;
  std::filesystem::path manifest_path(uint64_t generation) const;
  uint64_t latest_generation() const;
  std::vector<uint64_t> list_manifest_generations() const;
  Result<RecoveryManifest> load_manifest(uint64_t generation) const;
  bool remove_generation(uint64_t generation);
  bool recovery_required() const { return m_recovery_required.load(); }
  void require_recovery() { m_recovery_required.store(true); }
  bool revoke_fast_recovery();
  bool mark_recovery_taint();
  bool recovery_tainted() const;
  void clear_recovery_taint();
  std::filesystem::path recovery_taint_path() const { return m_partition_dir / "reload_required"; }
  std::filesystem::path recovery_taint_alt_path() const {
    return m_recovery_taint_root / m_db_name / m_tbl_name / "reload_required";
  }

 private:
  bool persist_manifest(const RecoveryManifest &manifest);
  bool write_snap_header(std::ostream &out, uint32_t imcu_id, uint32_t cols, uint64_t sequence) const;
  bool read_snap_header(std::istream &in, uint32_t &imcu_id, uint32_t &cols, uint64_t &sequence) const;
  bool write_imcu_metadata(std::ostream &out, const Imcu *imcu) const;
  bool read_imcu_metadata(std::istream &in, Imcu *imcu) const;
  bool serialize_imcu(Imcu *imcu, uint64_t sequence, std::string &out) const;
  void gc_old_generations();
  std::string m_db_name, m_tbl_name;
  std::filesystem::path m_partition_dir, m_recovery_taint_root;
  mutable std::mutex m_checkpoint_mutex;
  std::shared_ptr<Recovery::CheckpointStore> m_store;
  Recovery::NotificationTracker m_notifications;
  std::atomic<bool> m_recovery_required{false};
};
}  // namespace ShannonBase::Imcs
#endif
