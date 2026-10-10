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
#ifndef SHANNONBASE_NOTIFICATION_TRACKER_H
#define SHANNONBASE_NOTIFICATION_TRACKER_H
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace ShannonBase::Recovery {
struct BinlogPosition {
  std::string file;
  uint64_t offset{0};
  std::string prefix_digest;
  bool valid() const { return !file.empty() && offset >= 4; }
};
// Volatile rendezvous only: no file, append, redo flush or durable commit marker.
// The gate orders notification/enqueue, apply, outcomes and checkpoint freezing.
class NotificationTracker {
 public:
  std::recursive_mutex &mutex() { return m_mutex; }
  uint64_t notify(uint64_t transaction) {
    std::lock_guard lock(m_mutex);
    if (m_disabled) return 0;
    const auto seq = ++m_last;
    m_pending.insert(seq);
    m_active.insert(transaction);
    return seq;
  }
  // Caller holds source MDL excluding writers across load and publication.
  void initialize(const BinlogPosition &position) {
    std::lock_guard lock(m_mutex);
    if (!m_disabled && m_active.empty() && m_pending.empty()) m_position = position;
  }
  void committed(uint64_t transaction, const BinlogPosition &position) {
    std::lock_guard lock(m_mutex);
    m_active.erase(transaction);
    if (!position.valid()) {
      m_disabled = true;
      return;
    }
    if (!m_position.valid() || position.file > m_position.file ||
        (position.file == m_position.file && position.offset > m_position.offset))
      m_position = position;
  }
  void aborted(uint64_t transaction) {
    std::lock_guard lock(m_mutex);
    m_active.erase(transaction);
  }
  void applied(uint64_t sequence) {
    std::lock_guard lock(m_mutex);
    m_pending.erase(sequence);
  }
  void invalidate() {
    std::lock_guard lock(m_mutex);
    m_disabled = true;
  }
  bool disabled() const { return m_disabled; }  // gate held
  bool quiescent() const { return !m_disabled && m_active.empty() && m_pending.empty(); }
  bool needs_checkpoint() {
    std::lock_guard lock(m_mutex);
    return !m_disabled && m_position.valid() && m_last != m_checkpointed;
  }
  BinlogPosition position() const { return m_position; }  // gate held
  uint64_t sequence() const { return m_last; }
  void checkpoint_published(uint64_t boundary) {
    std::lock_guard lock(m_mutex);
    m_checkpointed = boundary;
  }
  void reset() {
    std::lock_guard lock(m_mutex);
    m_active.clear();
    m_pending.clear();
    m_position = {};
    m_last = 0;
    m_checkpointed = ~uint64_t{0};
    m_disabled = false;
  }
  std::string checkpoint_blockers() {
    std::lock_guard lock(m_mutex);
    return !m_active.empty() ? "active source transactions" : (!m_pending.empty() ? "notifications pending apply" : "");
  }

 private:
  std::recursive_mutex m_mutex;
  std::unordered_set<uint64_t> m_pending, m_active;
  BinlogPosition m_position;
  uint64_t m_last{0}, m_checkpointed{~uint64_t{0}};
  bool m_disabled{false};
};
}  // namespace ShannonBase::Recovery
#endif
