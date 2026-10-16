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
#ifndef __SHANNONBASE_NOTIFICATION_TRACKER_H__
#define __SHANNONBASE_NOTIFICATION_TRACKER_H__
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
namespace ShannonBase {
namespace Recovery {
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
  // RESET reuses filenames. Keep existing trackers disabled until a fresh load;
  // clearing outstanding transactions here would falsely certify quiescence.
  static void source_reset() noexcept { s_source_epoch.fetch_add(1, std::memory_order_acq_rel); }
  std::recursive_mutex &mutex() { return m_mutex; }
  uint64_t notify(uint64_t transaction) {
    std::lock_guard lock(m_mutex);
    if (disabled()) return 0;
    const auto seq = ++m_last;
    m_pending.insert(seq);
    m_active.try_emplace(transaction, std::chrono::steady_clock::now());
    return seq;
  }
  // Caller holds source MDL excluding writers across load and publication.
  void initialize(const BinlogPosition &position) {
    std::lock_guard lock(m_mutex);
    if (!disabled() && m_active.empty() && m_pending.empty()) m_position = position;
  }
  void committed(uint64_t transaction, const BinlogPosition &position) {
    std::lock_guard lock(m_mutex);
    m_active.erase(transaction);
    if (!position.valid()) {
      m_disabled = true;
      return;
    }
    if (!m_position.valid()) {
      m_position = position;
      return;
    }
    // The decimal suffix grows beyond six digits; lexical order is incorrect.
    auto split = [](const std::string &file, std::string_view &prefix, uint64_t &number) {
      const auto dot = file.rfind('.');
      if (dot == std::string::npos) return false;
      const auto slash = file.find_last_of("/\\");
      const auto begin = slash == std::string::npos ? 0 : slash + 1;
      if (dot < begin) return false;
      prefix = std::string_view(file).substr(begin, dot - begin);
      const auto *end = file.data() + file.size();
      const auto result = std::from_chars(file.data() + dot + 1, end, number);
      return result.ec == std::errc{} && result.ptr == end;
    };
    std::string_view old_prefix, new_prefix;
    uint64_t old_number{}, new_number{};
    if (!split(m_position.file, old_prefix, old_number) || !split(position.file, new_prefix, new_number) ||
        old_prefix != new_prefix) {
      m_disabled = true;  // a different source cannot certify this checkpoint
      return;
    }
    if (new_number > old_number || (new_number == old_number && position.offset > m_position.offset))
      m_position = position;
  }
  void advance_quiescent(const BinlogPosition &position) {
    std::lock_guard lock(m_mutex);
    if (!quiescent()) return;
    const auto previous = m_position;
    committed(0, position);
    if (m_position.file != previous.file || m_position.offset != previous.offset) ++m_last;
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
  bool disabled() const {  // gate held
    return m_disabled || m_source_epoch != s_source_epoch.load(std::memory_order_acquire);
  }
  bool quiescent() const { return !disabled() && m_active.empty() && m_pending.empty(); }
  bool needs_checkpoint() {
    std::lock_guard lock(m_mutex);
    return !disabled() && m_position.valid() && m_last != m_checkpointed;
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
    m_source_epoch = s_source_epoch.load(std::memory_order_acquire);
    m_disabled = false;
  }
  std::string checkpoint_blockers() {
    std::lock_guard lock(m_mutex);
    if (disabled()) return "source tracking disabled";
    if (!m_active.empty()) {
      auto oldest = std::chrono::steady_clock::now();
      for (const auto &[id, started] : m_active) oldest = std::min(oldest, started);
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - oldest);
      return "active source transactions: " + std::to_string(m_active.size()) +
             ", oldest age seconds: " + std::to_string(age.count());
    }
    return !m_pending.empty() ? "notifications pending apply" : "";
  }

 private:
  inline static std::atomic<uint64_t> s_source_epoch{0};
  uint64_t m_source_epoch{s_source_epoch.load(std::memory_order_acquire)};
  std::recursive_mutex m_mutex;
  std::unordered_set<uint64_t> m_pending;
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_active;
  BinlogPosition m_position;
  uint64_t m_last{0}, m_checkpointed{~uint64_t{0}};
  bool m_disabled{false};
};
}  // namespace Recovery
}  // namespace ShannonBase
#endif
