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
#ifndef __SHANNONBASE_IO_SCHEDULER_H__
#define __SHANNONBASE_IO_SCHEDULER_H__
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

class THD;
namespace ShannonBase {
namespace ResMgmt {
// Logical spill-request depth, not kernel IOPS or device throughput.
// Device domains have independent FIFO admission and bounded registry size.
class IoDepthGovernor {
 public:
  struct Snapshot {
    size_t active{0}, waiting{0}, peak{0};
    uint64_t admitted{0}, rejected{0};
  };
  class Lease {
   public:
    Lease() = default;
    ~Lease() { reset(); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept : m_owner(std::exchange(other.m_owner, nullptr)), m_device(other.m_device) {}
    Lease &operator=(Lease &&other) noexcept {
      if (this != &other) {
        reset();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_device = other.m_device;
      }
      return *this;
    }
    explicit operator bool() const { return m_owner != nullptr; }
    void reset() {
      if (auto *owner = std::exchange(m_owner, nullptr)) owner->release(m_device);
    }

   private:
    friend class IoDepthGovernor;
    Lease(IoDepthGovernor *owner, uint64_t device) : m_owner(owner), m_device(device) {}
    IoDepthGovernor *m_owner{nullptr};
    uint64_t m_device{0};
  };
  template <class Cancelled>
  Lease acquire(uint64_t device, size_t limit, std::chrono::milliseconds timeout, Cancelled cancelled) {
    std::unique_lock lock(m_mutex);
    // Reserve domain zero for excess devices instead of growing without bound.
    if (!m_devices.count(device) && m_devices.size() >= 63) device = 0;
    auto &state = m_devices[device];
    const auto ticket = m_next_ticket++;
    state.queue.push_back(ticket);
    ++m_stats.waiting;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remove = [&] {
      state.queue.erase(std::find(state.queue.begin(), state.queue.end(), ticket));
      --m_stats.waiting;
      m_cv.notify_all();
    };
    for (;;) {
      if (cancelled()) {
        remove();
        ++m_stats.rejected;
        return {};
      }
      if (state.queue.front() == ticket && state.active < std::max<size_t>(1, limit)) {
        remove();
        ++state.active;
        ++m_stats.active;
        ++m_stats.admitted;
        m_stats.peak = std::max(m_stats.peak, m_stats.active);
        return Lease(this, device);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        remove();
        ++m_stats.rejected;
        return {};
      }
      m_cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
    }
  }
  Snapshot snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
  }

 private:
  struct Device {
    size_t active{0};
    std::deque<uint64_t> queue;
  };
  void release(uint64_t device) {
    std::lock_guard lock(m_mutex);
    --m_devices.at(device).active;
    --m_stats.active;
    m_cv.notify_all();
  }
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::map<uint64_t, Device> m_devices;
  Snapshot m_stats;
  uint64_t m_next_ticket{0};
};

// RAII around one AP spill request. No source commit/checkpoint paths enter it.
// AUTO chooses conservative depth; OFF/OBSERVE/manual CPU modes preserve I/O.
class SpillIoScope {
 public:
  SpillIoScope(THD *thd, FILE *file);
  explicit operator bool() const { return m_allowed; }

 private:
  IoDepthGovernor::Lease m_lease;
  bool m_allowed{true};
};
IoDepthGovernor::Snapshot spill_io_snapshot();
}  // namespace ResMgmt
}  // namespace ShannonBase
#endif
