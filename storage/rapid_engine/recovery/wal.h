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
#ifndef SHANNONBASE_RAPID_WAL_H
#define SHANNONBASE_RAPID_WAL_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "storage/rapid_engine/recovery/durable_fs.h"
namespace ShannonBase {
namespace Recovery {
// Durable, pre-propagation half of Rapid WAL. Physical ROW_COMMIT is deliberately
// not a source outcome. Only the server's successful after-commit hook may call
// committed(), after making the primary redo durable. An unresolved source
// transaction requires primary reload; absence of ABORT never means COMMIT.
class WAL {
 public:
  explicit WAL(std::filesystem::path directory);
  bool open();
  bool reset();
  uint64_t capture(uint64_t transaction, const std::string &payload);
  bool committed(uint64_t transaction);
  bool aborted(uint64_t transaction);
  bool invalidate();
  bool disabled() const { return m_disabled; }  // caller holds mutex()
  void applied(uint64_t sequence);
  bool needs_checkpoint();
  bool quiescent() const;  // caller holds mutex()
  bool checkpoint(uint64_t generation);
  void checkpoint_published() {
    std::lock_guard lock(m_mutex);
    m_checkpointed = m_last;
  }
  bool checkpoint_cut(uint64_t generation, uint64_t *cut) const;
  bool replay(uint64_t cut, const std::function<bool(uint64_t, uint64_t, const std::string &)> &apply);
  bool compact(uint64_t cut);

  // One gate orders capture/enqueue, apply, terminal publication and snapshot
  // cuts. Recursive so a composed operation can call the individually safe API.
  std::recursive_mutex &mutex() { return m_mutex; }

 private:
  enum class Kind : uint8_t { CHANGE = 1, COMMIT = 2, ABORT = 3, INVALID = 4 };
  struct Record {
    Kind kind;
    uint64_t sequence{0}, transaction{0};
    std::string payload;
  };
  struct Transaction {
    Kind outcome{Kind::CHANGE};
    uint64_t count{0}, last{0};
    uint32_t digest{0};
  };
  bool append(Kind kind, uint64_t transaction, const std::string &payload, uint64_t *sequence);
  bool terminal(Kind kind, uint64_t transaction);
  bool account(const Record &record);
  bool scan(const std::function<bool(const Record &)> &visit) const;
  std::string header(uint64_t base) const;
  std::filesystem::path checkpoint_path(uint64_t generation) const;

  std::filesystem::path m_directory, m_path;
  DurableFile m_file;
  mutable std::recursive_mutex m_mutex;
  bool m_good{false}, m_disabled{false};
  uint64_t m_epoch{0}, m_base{0}, m_last{0}, m_checkpointed{UINT64_MAX};
  std::unordered_map<uint64_t, Transaction> m_transactions;
  std::unordered_set<uint64_t> m_pending;
};
}  // namespace Recovery
}  // namespace ShannonBase
#endif