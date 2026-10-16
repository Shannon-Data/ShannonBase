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
#ifndef __SHANNONBASE_CHECKPOINT_STORE_H__
#define __SHANNONBASE_CHECKPOINT_STORE_H__
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
namespace ShannonBase {
namespace Recovery {
/** Transport for immutable native-format recovery generations.
 * Install before recovery workers start. Implementations must preserve schema,
 * source identity and committed-binlog boundary metadata alongside column data.
 * A generation is visible only after every object and its manifest are durable.
 * restore() stages complete generations locally; normal CRC/schema validation
 * still applies. Missing data/network errors return false and cause source reload.
 * invalidate() must fence all old generations durably, including remote copies.
 * It must serialize with in-flight publish(): no publication started before the
 * fence may subsequently make an invalidated generation visible. Implementations
 * must persist a generation/epoch fence and reject stale publication atomically;
 * deleting objects alone does not satisfy this contract.
 * Incremental implementations may share immutable objects between generations;
 * publish() success always represents a complete recoverable logical state.
 */
class CheckpointStore {
 public:
  virtual ~CheckpointStore() = default;
  virtual bool publish(const std::string &db, const std::string &table, uint64_t generation,
                       const std::filesystem::path &directory) = 0;
  virtual bool restore(const std::string &db, const std::string &table, const std::filesystem::path &directory) = 0;
  virtual bool invalidate(const std::string &db, const std::string &table) = 0;
  virtual bool erase(const std::string &db, const std::string &table) = 0;
};
// Local durability and validation are provided by TablePersistenceManager.
class LocalCheckpointStore final : public CheckpointStore {
 public:
  bool publish(const std::string &, const std::string &, uint64_t, const std::filesystem::path &) override {
    return true;
  }
  bool restore(const std::string &, const std::string &, const std::filesystem::path &) override { return true; }
  bool invalidate(const std::string &, const std::string &) override { return true; }
  bool erase(const std::string &, const std::string &) override { return true; }
};
class CheckpointStores {
 public:
  static std::shared_ptr<CheckpointStore> current() {
    std::lock_guard lock(m_mutex);
    return m_store;
  }
  static void install(std::shared_ptr<CheckpointStore> store) {
    std::lock_guard lock(m_mutex);
    m_store = store ? std::move(store) : std::make_shared<LocalCheckpointStore>();
  }

 private:
  inline static std::mutex m_mutex;
  inline static std::shared_ptr<CheckpointStore> m_store = std::make_shared<LocalCheckpointStore>();
};
}  // namespace Recovery
}  // namespace ShannonBase
#endif
