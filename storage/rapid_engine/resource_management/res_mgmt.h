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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_RES_MGMT_H__
#define __SHANNONBASE_RES_MGMT_H__
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

class THD;

namespace ShannonBase::ResMgmt {
/** V1 separates active AP, SYNC and MAINT work; source DML never enters this governor.
    CPU groups are native MySQL groups. Memory/spill continue using their existing
    budgets. This is not an RSS, I/O or physical worker-count limit. */
enum class WorkClass : size_t { AP, SYNC, MAINT, COUNT };
enum class IsolationMode : unsigned long { OFF, OBSERVE, SOFT, STRICT_CPU };
inline constexpr unsigned long DEFAULT_AP_LIMIT = 4;
inline constexpr unsigned long DEFAULT_SYNC_LIMIT = 2;
inline constexpr unsigned long DEFAULT_MAINT_LIMIT = 1;

class WorkGovernor {
 public:
  struct Snapshot {
    size_t active{0}, waiting{0}, peak{0};
    uint64_t admitted{0}, rejected{0}, cancelled{0};
  };
  class Lease {
   public:
    Lease() = default;
    ~Lease() { reset(); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept { swap(other); }
    Lease &operator=(Lease &&other) noexcept {
      if (this != &other) {
        reset();
        swap(other);
      }
      return *this;
    }
    explicit operator bool() const { return m_valid; }
    void reset() noexcept {
      if (m_owner) m_owner->release(m_kind);
      m_owner = nullptr;
      m_valid = false;
    }

   private:
    friend class WorkGovernor;
    Lease(WorkGovernor *owner, WorkClass kind) : m_owner(owner), m_kind(kind), m_valid(true) {}
    void swap(Lease &other) noexcept {
      std::swap(m_owner, other.m_owner);
      std::swap(m_kind, other.m_kind);
      std::swap(m_valid, other.m_valid);
    }
    WorkGovernor *m_owner{nullptr};
    WorkClass m_kind{WorkClass::AP};
    bool m_valid{false};
  };
  bool configure(IsolationMode mode, std::array<size_t, 3> limits) {
    std::lock_guard lock(m_mutex);
    for (const auto &state : m_states)
      if (state.stats.active || !state.queue.empty()) return false;
    m_limits = limits;
    m_stopping = false;
    m_mode.store(mode, std::memory_order_release);
    return true;
  }
  IsolationMode mode() const { return m_mode.load(std::memory_order_acquire); }
  Lease try_acquire(WorkClass kind) {
    std::lock_guard lock(m_mutex);
    auto &state = m_states[index(kind)];
    if (m_stopping || (enforced() && (state.stats.active >= m_limits[index(kind)] || !state.queue.empty()))) {
      ++state.stats.rejected;
      return {};
    }
    return admit(kind);
  }
  template <class Cancelled>
  Lease acquire(WorkClass kind, Cancelled cancelled) {
    std::unique_lock lock(m_mutex);
    auto &state = m_states[index(kind)];
    const uint64_t ticket = m_next_ticket++;
    state.queue.push_back(ticket);
    state.stats.waiting = state.queue.size();
    auto erase = [&] {
      state.queue.erase(std::find(state.queue.begin(), state.queue.end(), ticket));
      state.stats.waiting = state.queue.size();
      m_cv.notify_all();
    };
    for (;;) {
      // Cancellation must be a nonblocking atomic predicate: no external locks.
      if (m_stopping || cancelled()) {
        erase();
        ++state.stats.cancelled;
        return {};
      }
      if (!enforced() || (state.queue.front() == ticket && state.stats.active < m_limits[index(kind)])) {
        erase();
        return admit(kind);
      }
      m_cv.wait_for(lock, std::chrono::milliseconds(20));
    }
  }
  Snapshot snapshot(WorkClass kind) const {
    std::lock_guard lock(m_mutex);
    return m_states[index(kind)].stats;
  }
  bool stopping() const {
    std::lock_guard lock(m_mutex);
    return m_stopping;
  }
  void stop() {
    std::lock_guard lock(m_mutex);
    m_stopping = true;
    m_cv.notify_all();
  }

 private:
  static size_t index(WorkClass kind) { return static_cast<size_t>(kind); }
  bool enforced() const { return mode() == IsolationMode::SOFT || mode() == IsolationMode::STRICT_CPU; }
  Lease admit(WorkClass kind) {
    auto &stats = m_states[index(kind)].stats;
    ++stats.active;
    ++stats.admitted;
    stats.peak = std::max(stats.peak, stats.active);
    return Lease(this, kind);
  }
  void release(WorkClass kind) noexcept {
    std::lock_guard lock(m_mutex);
    auto &stats = m_states[index(kind)].stats;
    assert(stats.active);
    --stats.active;
    m_cv.notify_all();
  }
  struct State {
    Snapshot stats;
    std::deque<uint64_t> queue;
  };
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::array<State, 3> m_states;
  std::array<size_t, 3> m_limits{DEFAULT_AP_LIMIT, DEFAULT_SYNC_LIMIT, DEFAULT_MAINT_LIMIT};
  std::atomic<IsolationMode> m_mode{IsolationMode::OFF};
  uint64_t m_next_ticket{0};
  bool m_stopping{false};
};

WorkGovernor &work_governor();
/** Owns a native group MDL pin, original binding and a governor lease.
    Destruction precedes THD/table cleanup and primary fallback. */
class WorkScope {
 public:
  WorkScope();
  ~WorkScope();
  WorkScope(const WorkScope &) = delete;
  WorkScope &operator=(const WorkScope &) = delete;
  const char *enter(
      THD *thd, WorkClass kind, bool wait, const std::function<bool()> &cancelled = [] { return false; });
  void reset();

 private:
  struct State;
  std::unique_ptr<State> m_state;
};
// Native names are startup-only. DBA creates USER AP and SYSTEM SYNC/MAINT groups.
extern char *tp_resource_group, *ap_resource_group, *sync_resource_group, *maint_resource_group,
    *maint_user_resource_group;
class WorkerRegistration {
 public:
  WorkerRegistration();
  ~WorkerRegistration();
  WorkerRegistration(const WorkerRegistration &) = delete;
  WorkerRegistration &operator=(const WorkerRegistration &) = delete;

 private:
  unsigned long long m_tid{0};
};
extern std::atomic<uint64_t> cpu_restore_failures;
const char *validate_cpu_plan(THD *thd);

/** Divide a plan's query allowance before bottom-up iterator construction.
    Otherwise early children can reserve the entire allowance and starve their
    parent, even though all operators could execute with smaller grants. */
class PlanningMemoryScope {
 public:
  PlanningMemoryScope(const void *query, size_t query_limit, size_t operators)
      : m_query(query), m_share(query_limit / std::max<size_t>(operators, 1)), m_previous(s_current) {
    s_current = this;
  }
  ~PlanningMemoryScope() { s_current = m_previous; }
  PlanningMemoryScope(const PlanningMemoryScope &) = delete;
  PlanningMemoryScope &operator=(const PlanningMemoryScope &) = delete;
  static size_t limit_request(const void *query, size_t requested) {
    for (auto *scope = s_current; scope; scope = scope->m_previous)
      if (scope->m_query == query) requested = std::min(requested, scope->m_share);
    return requested;
  }

 private:
  const void *m_query;
  size_t m_share;
  PlanningMemoryScope *m_previous;
  inline static thread_local PlanningMemoryScope *s_current{nullptr};
};

/** Atomic global/query reservations. A grant is owned until its reservation is destroyed. */
class MemoryBudget {
 public:
  class Reservation {
   public:
    Reservation() = default;
    ~Reservation() { reset(); }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    Reservation(Reservation &&other) noexcept { swap(other); }
    Reservation &operator=(Reservation &&other) noexcept {
      if (this != &other) {
        reset();
        swap(other);
      }
      return *this;
    }
    size_t bytes() const { return m_bytes; }
    explicit operator bool() const { return m_bytes != 0; }
    void reset() noexcept {
      if (m_owner) m_owner->release(m_query, m_bytes);
      m_owner = nullptr;
      m_bytes = 0;
    }

   private:
    friend class MemoryBudget;
    Reservation(MemoryBudget *owner, const void *query, size_t bytes)
        : m_owner(owner), m_query(query), m_bytes(bytes) {}
    void swap(Reservation &other) noexcept {
      std::swap(m_owner, other.m_owner);
      std::swap(m_query, other.m_query);
      std::swap(m_bytes, other.m_bytes);
    }
    MemoryBudget *m_owner{nullptr};
    const void *m_query{nullptr};
    size_t m_bytes{0};
  };

  MemoryBudget(size_t global_limit, size_t query_limit) : m_global_limit(global_limit), m_query_limit(query_limit) {}
  Reservation reserve(const void *query, size_t requested, size_t minimum = 1, size_t available_capacity = SIZE_MAX) {
    std::lock_guard lock(m_mutex);
    const auto found = m_queries.find(query);
    const size_t query_used = found == m_queries.end() ? 0 : found->second;
    const size_t available = available_capacity > m_reserved ? available_capacity - m_reserved : 0;
    const size_t bytes = std::min({requested, m_global_limit - m_reserved, m_query_limit - query_used, available});
    if (bytes == 0 || bytes < minimum) return {};
    // Insert may throw; no counters have changed until it succeeds.
    m_queries[query] = query_used + bytes;
    m_reserved += bytes;
    return Reservation(this, query, bytes);
  }
  size_t reserved() const {
    std::lock_guard lock(m_mutex);
    return m_reserved;
  }

 private:
  void release(const void *query, size_t bytes) noexcept {
    std::lock_guard lock(m_mutex);
    auto found = m_queries.find(query);
    if (found == m_queries.end()) return;
    found->second -= bytes;
    m_reserved -= bytes;
    if (found->second == 0) m_queries.erase(found);
  }
  const size_t m_global_limit, m_query_limit;
  mutable std::mutex m_mutex;
  size_t m_reserved{0};
  std::unordered_map<const void *, size_t> m_queries;
};

MemoryBudget::Reservation reserve_query_memory(const void *query, size_t requested);

/** Charges allocations, including simultaneous old/new buffers on growth. */
class BoundedMemoryResource final : public std::pmr::memory_resource {
 public:
  explicit BoundedMemoryResource(size_t limit, std::pmr::memory_resource *upstream = std::pmr::new_delete_resource())
      : m_limit(limit), m_upstream(upstream) {}
  // External buffers (ColumnChunk and SQL row images) reserve before calling
  // their allocator and retain this reservation until the allocation is destroyed.
  class Reservation {
   public:
    Reservation() = default;
    Reservation(BoundedMemoryResource *resource, size_t bytes) : m_resource(resource), m_bytes(bytes) {
      if (m_resource) m_resource->acquire(bytes);
    }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    Reservation(Reservation &&other) noexcept
        : m_resource(std::exchange(other.m_resource, nullptr)), m_bytes(other.m_bytes) {}
    Reservation &operator=(Reservation &&other) noexcept {
      if (this != &other) {
        reset();
        m_resource = std::exchange(other.m_resource, nullptr);
        m_bytes = other.m_bytes;
      }
      return *this;
    }
    ~Reservation() { reset(); }
    void reset() noexcept {
      if (m_resource) {
        m_resource->release(m_bytes);
        m_resource = nullptr;
      }
    }
    BoundedMemoryResource *resource() const { return m_resource; }

   private:
    BoundedMemoryResource *m_resource{nullptr};
    size_t m_bytes{0};
  };
  size_t limit() const { return m_limit; }
  size_t used() const { return m_used; }
  size_t peak() const { return m_peak; }

 private:
  void acquire(size_t bytes) {
    if (bytes > m_limit - m_used) throw std::bad_alloc();
    m_used += bytes;
    m_peak = std::max(m_peak, m_used);
  }
  void release(size_t bytes) noexcept {
    assert(bytes <= m_used);
    m_used -= bytes;
  }
  void *do_allocate(size_t bytes, size_t alignment) override {
    acquire(bytes);
    try {
      return m_upstream->allocate(bytes, alignment);
    } catch (...) {
      release(bytes);
      throw;
    }
  }
  void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
    m_upstream->deallocate(pointer, bytes, alignment);
    release(bytes);
  }
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
  const size_t m_limit;
  std::pmr::memory_resource *m_upstream;
  size_t m_used{0}, m_peak{0};
};
}  // namespace ShannonBase::ResMgmt
#endif
