// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef SHANNONBASE_CHECKPOINT_STORE_H
#define SHANNONBASE_CHECKPOINT_STORE_H
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
namespace ShannonBase::Recovery {
/** Transport for immutable native-format recovery generations.
 * Install before recovery workers start. Implementations must preserve schema,
 * source identity and committed-binlog boundary metadata alongside column data.
 * A generation is visible only after every object and its manifest are durable.
 * restore() stages complete generations locally; normal CRC/schema validation
 * still applies. Missing data/network errors return false and cause source reload.
 * invalidate() must fence all old generations durably, including remote copies.
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
}  // namespace ShannonBase::Recovery
#endif
