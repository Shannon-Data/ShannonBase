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
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class THD;
namespace ShannonBase {
namespace Utils {
class MemoryPool;
}  // namespace Utils
}  // namespace ShannonBase

namespace ShannonBase {
namespace ResMgmt {
/** V1 separates active AP, SYNC and MAINT work; source DML never enters this governor.
    CPU groups are native MySQL groups. Memory/spill continue using their existing
    budgets. This is not an RSS, I/O or physical worker-count limit. */
enum class WorkClass : size_t { AP, SYNC, MAINT, COUNT };
enum class IsolationMode : unsigned long { OFF, OBSERVE, SOFT, STRICT_CPU, AUTO };
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
  bool configure(IsolationMode mode, std::array<size_t, 3> limits, bool resume = true) {
    std::lock_guard lock(m_mutex);
    for (const auto &state : m_states)
      if (state.stats.active || !state.queue.empty()) return false;
    m_limits = limits;
    m_stopping = !resume;
    m_mode.store(mode, std::memory_order_release);
    return true;
  }
  bool idle() const {
    std::lock_guard lock(m_mutex);
    for (const auto &state : m_states)
      if (state.stats.active || !state.queue.empty()) return false;
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
  size_t limit(WorkClass kind) const {
    std::lock_guard lock(m_mutex);
    return m_limits[index(kind)];
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
  bool enforced() const {
    return mode() == IsolationMode::SOFT || mode() == IsolationMode::STRICT_CPU || mode() == IsolationMode::AUTO;
  }
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

// Conservative physical pool sizing, independent of the number of tables.
// This is a planning estimate, not a second memory reservation or RSS limit.
inline size_t make_apply_pool_size(size_t sync_limit, size_t pool_capacity) {
  constexpr size_t worker_headroom = 8ULL * 1024 * 1024;
  return std::max<size_t>(1, std::min({sync_limit, std::max<size_t>(1, pool_capacity / worker_headroom), size_t{32}}));
}

enum class CpuRole : size_t { TP, AP, SYNC, MAINT, COUNT };
struct CpuTopologyEntry {
  unsigned cpu;
  int package, core;
};
struct AutoCpuPlan {
  std::array<std::vector<unsigned>, static_cast<size_t>(CpuRole::COUNT)> cpus;
  std::array<size_t, 3> limits{};
  bool available{false};
};
// Keep SMT siblings together. Reserve at least two physical cores for TP and
// separate cores for AP/SYNC/MAINT; low-capacity hosts use software admission.
inline AutoCpuPlan make_auto_cpu_plan(const std::vector<CpuTopologyEntry> &topology,
                                      const std::array<size_t, 3> &configured_limits) {
  AutoCpuPlan plan;
  std::map<std::pair<int, int>, std::vector<unsigned>> cores;
  for (const auto &entry : topology) cores[{entry.package, entry.core}].push_back(entry.cpu);
  if (cores.size() < 5) return plan;
  const size_t background = std::max<size_t>(3, cores.size() / 3);
  const size_t tp = cores.size() - background;
  size_t index = 0;
  for (const auto &[key, siblings] : cores) {
    const auto role = index < tp                  ? CpuRole::TP
                      : index < cores.size() - 2  ? CpuRole::AP
                      : index == cores.size() - 2 ? CpuRole::SYNC
                                                  : CpuRole::MAINT;
    auto &cpus = plan.cpus[static_cast<size_t>(role)];
    cpus.insert(cpus.end(), siblings.begin(), siblings.end());
    ++index;
  }
  plan.limits = {std::min(configured_limits[0], background - 2), std::min<size_t>(configured_limits[1], 2),
                 std::min<size_t>(configured_limits[2], 1)};
  plan.available = true;
  return plan;
}
WorkGovernor &work_governor();
bool configure_auto_resources(IsolationMode mode, std::array<size_t, 3> limits);
void start_auto_resources();
void stop_auto_resources();
void shutdown_auto_resources();
std::string auto_resource_status();
bool limits_execution(IsolationMode mode);

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

enum class QueryMemoryOperator : uint8_t {
  kSort,
  kHashAggregate,
  kHashJoin,
  kWindow,
  kNestedLoop,
  kBatchTransport,
  kCount,
};

inline const char *query_memory_operator_name(QueryMemoryOperator op) {
  switch (op) {
    case QueryMemoryOperator::kSort:
      return "sort";
    case QueryMemoryOperator::kHashAggregate:
      return "hash_aggregate";
    case QueryMemoryOperator::kHashJoin:
      return "hash_join";
    case QueryMemoryOperator::kWindow:
      return "window";
    case QueryMemoryOperator::kNestedLoop:
      return "nested_loop";
    case QueryMemoryOperator::kBatchTransport:
      return "batch_transport";
    case QueryMemoryOperator::kCount:
      return "invalid";
  }
  return "unknown";
}

/** An operator that can give memory back at a safe point.

    The budget grows a reservation on demand and, when the pool cannot satisfy
    the growth, asks the cheapest registered operator of the same query to free
    bytes. An operator that is inside its own Read() must not mutate its state
    from another frame, so it arms itself and frees at its next safe point
    instead; it reports how many bytes it actually freed right now. */
class Evictable {
 public:
  virtual ~Evictable() = default;
  virtual size_t Evict(size_t target) = 0;
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
    const void *query() const { return m_query; }
    explicit operator bool() const { return m_bytes != 0; }
    // Ask for more room. Returns the bytes actually added, which may be less
    // than requested (or zero) when the global or per-query pool is busy.
    size_t try_grow(size_t extra);
    // Grow the grant to `want_total` (never shrinks). `available_capacity` is
    // what the pool can spare right now, so a lone operator can grow past the
    // floor without a second operator having to predict it. Returns the grant
    // after growth.
    size_t grow(size_t want_total, size_t available_capacity = SIZE_MAX);
    // Hand back room without dropping the whole grant.
    void release_bytes(size_t bytes);
    void reset() noexcept {
      if (m_owner) m_owner->release(m_query, m_bytes, /*drop_grant=*/true);
      m_owner = nullptr;
      m_bytes = 0;
      m_lifetime.reset();
    }
    void retain(std::shared_ptr<void> lifetime) { m_lifetime = std::move(lifetime); }

   private:
    friend class MemoryBudget;
    Reservation(MemoryBudget *owner, const void *query, size_t bytes)
        : m_owner(owner), m_query(query), m_bytes(bytes) {}
    void swap(Reservation &other) noexcept {
      std::swap(m_owner, other.m_owner);
      std::swap(m_query, other.m_query);
      std::swap(m_bytes, other.m_bytes);
      std::swap(m_lifetime, other.m_lifetime);
    }
    MemoryBudget *m_owner{nullptr};
    const void *m_query{nullptr};
    size_t m_bytes{0};
    std::shared_ptr<void> m_lifetime;
  };

  MemoryBudget(size_t global_limit, size_t query_limit) : m_global_limit(global_limit), m_query_limit(query_limit) {}
  // Publish both limits atomically. A shrink cannot invalidate live grants;
  // callers must release them first. Rejection leaves both limits unchanged.
  bool set_limits(size_t global_limit, size_t query_limit) {
    std::lock_guard lock(m_mutex);
    if (global_limit < m_reserved) return false;
    for (const auto &entry : m_queries)
      if (entry.second > query_limit) return false;
    m_global_limit = global_limit;
    m_query_limit = query_limit;
    return true;
  }
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
    ++m_grants;
    return Reservation(this, query, bytes);
  }
  size_t reserved() const {
    std::lock_guard lock(m_mutex);
    return m_reserved;
  }
  size_t reserved_for(const void *query) const {
    std::lock_guard lock(m_mutex);
    const auto found = m_queries.find(query);
    return found == m_queries.end() ? 0 : found->second;
  }
  // Live grants, i.e. operators currently holding a reservation. Reported for
  // diagnostics; the adaptive ceiling splits free space over these.
  size_t active_grants() const {
    std::lock_guard lock(m_mutex);
    return m_grants;
  }
  // --- Eviction arbitration -------------------------------------------------
  // An operator that can give memory back at a safe point registers here. A
  // grow that the pool cannot satisfy asks the cheapest victims first, which
  // is the same shape as a buffer manager's eviction queue: the budget is not
  // sliced up front, it is reclaimed on pressure.
  void register_evictable(const void *query, QueryMemoryOperator op, Evictable *victim);
  void unregister_evictable(Evictable *victim);
  // Ask every other registered operator of `query` to free `needed` bytes.
  // Returns the bytes the victims actually freed. Victims that cannot free
  // memory at this point return 0 and arm themselves for their next safe
  // allocation site instead of mutating state under a suspended frame.
  size_t evict_for(const void *query, const Evictable *requester, size_t needed);

 private:
  friend class Reservation;
  // Extends one reservation. Returns the bytes granted, 0 when the pool or the
  // per-query limit is exhausted.
  size_t grow(const void *query, size_t extra);

  struct Victim {
    const void *query;
    QueryMemoryOperator op;
    Evictable *victim;
  };
  // Cheapest-to-evict first: a hash aggregate can flush a partition without
  // restarting its input, an ordered hash join has to re-partition its whole
  // build, so it is the last choice.
  static int eviction_rank(QueryMemoryOperator op) {
    switch (op) {
      case QueryMemoryOperator::kHashAggregate:
        return 0;
      case QueryMemoryOperator::kSort:
        return 1;
      case QueryMemoryOperator::kHashJoin:
        return 2;
      default:
        return 3;
    }
  }

  void release(const void *query, size_t bytes, bool drop_grant = false) noexcept {
    std::lock_guard lock(m_mutex);
    if (drop_grant && m_grants != 0) --m_grants;
    auto found = m_queries.find(query);
    if (found == m_queries.end()) return;
    found->second -= bytes;
    m_reserved -= bytes;
    if (found->second == 0) m_queries.erase(found);
  }
  size_t m_global_limit, m_query_limit;
  mutable std::mutex m_mutex;
  size_t m_reserved{0};
  size_t m_grants{0};
  std::vector<Victim> m_victims;
  std::unordered_map<const void *, size_t> m_queries;
};

// Query-side memory is budgeted only for the operators below. This is not a
// whole-mysqld RSS cap: it is a bounded per-query reservation enforced by the
// operation/query/query_total guards in the system pool.
MemoryBudget::Reservation reserve_query_memory(const void *query, QueryMemoryOperator op, size_t requested);

// Grows `grant` on demand, doubling from its current size, capped by what the
// pool can spare right now (minus the scan/checkpoint reserve) and by the
// per-query limit. When the pool is short, other operators of the same query
// are asked to free memory first. Returns the grant after growth; the caller
// decides whether the result is enough and spills otherwise.
//
// This is the execution-side budget. Plan choice keeps reading the calibrated
// floor (`SystemMemoryLimits::operation`) through
// Util::hash_join_memory_budget()/hash_aggregate_memory_budget(), so a plan is
// chosen as if memory were tight and only execution adapts.
size_t grow_query_memory(MemoryBudget::Reservation &grant, QueryMemoryOperator op, size_t want_total,
                         const Evictable *requester = nullptr);
// Process-wide (system pool) registry access, so an operator does not have to
// reach into the pool state to take part in eviction arbitration.
void register_evictable(const void *query, QueryMemoryOperator op, Evictable *victim);
void unregister_evictable(Evictable *victim);

// Internal policy, derived from the actual root pool capacity. These are not
// user variables. Reserve part of sys_pool for scans/checkpoint alongside AP.
// `operation` is the FLOOR of the per-operator budget, not the grant itself:
// see operator_memory_limit().
struct SystemMemoryLimits {
  size_t pool{0}, query_total{0}, query{0}, operation{0};
};
SystemMemoryLimits system_memory_limits(size_t root_capacity);
// Execution-side ceiling for one operator: what the pool could spare right now,
// capped at the per-query `query` limit and floored at `operation`. It is only
// a ceiling for grow_query_memory(); the cost model deliberately does not read
// it, so plan choice cannot drift with pool occupancy. Reported as
// `operator_cap` in the pool status.
size_t operator_memory_limit();
// Choose the initial radix fanout (2..16) from estimated resident bytes and
// the execution-time hash budget. Freeze it before writing any partitions.
// Recursive overflow still uses the bounded 16-way fallback for skew.
size_t initial_spill_fanout(double resident_bytes, size_t hash_budget);
bool initialize_system_pool(const std::shared_ptr<Utils::MemoryPool> &root);
void shutdown_system_pool();
SystemMemoryLimits system_memory_limits();
struct SystemPoolStats {
  size_t capacity{0}, used{0}, peak{0}, reserved{0}, failed_allocations{0};
  size_t operator_cap{0};
};
SystemPoolStats system_pool_stats();
class SystemPoolState;
struct SystemPoolDeleter {
  std::shared_ptr<SystemPoolState> state;
  size_t bytes{0};
  void operator()(uint8_t *pointer) const noexcept;
};
using SystemPoolBuffer = std::unique_ptr<uint8_t[], SystemPoolDeleter>;
SystemPoolBuffer allocate_system_buffer(size_t bytes);

// Captures a pool generation so buffers can safely outlive plugin teardown or
// a quiescent resize. The state holds the parent strongly until its child dies.
class PoolMemoryResource final : public std::pmr::memory_resource {
 public:
  PoolMemoryResource();
  PoolMemoryResource(const PoolMemoryResource &other) : m_state(other.m_state) {}
  PoolMemoryResource &operator=(const PoolMemoryResource &other) {
    m_state = other.m_state;
    return *this;
  }
  void *try_allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));

 private:
  void *do_allocate(size_t bytes, size_t alignment) override;
  void do_deallocate(void *pointer, size_t bytes, size_t alignment) override;
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
  std::shared_ptr<SystemPoolState> m_state;
};

/** Charges allocations, including simultaneous old/new buffers on growth. */
enum class AllocationFailure { kNone, kBudgetExhausted, kPoolExhausted };

class BoundedMemoryResource final : public std::pmr::memory_resource {
 public:
  explicit BoundedMemoryResource(size_t limit, std::pmr::memory_resource *upstream = nullptr)
      : m_limit(limit), m_upstream(upstream ? upstream : &m_pool_memory) {}
  // External buffers (ColumnChunk and SQL row images) reserve before calling
  // their allocator and retain this reservation until the allocation is destroyed.
  class Reservation {
   public:
    Reservation() = default;
    Reservation(BoundedMemoryResource *resource, size_t bytes) : m_resource(resource), m_bytes(bytes) {
      if (m_resource) m_resource->acquire(bytes);
    }
    bool try_acquire(BoundedMemoryResource *resource, size_t bytes) noexcept {
      reset();
      resource->m_last_failure = AllocationFailure::kNone;
      if (bytes > resource->m_limit - resource->m_used) {
        resource->m_last_failure = AllocationFailure::kBudgetExhausted;
        return false;
      }
      resource->m_used += bytes;
      resource->m_peak = std::max(resource->m_peak, resource->m_used);
      m_resource = resource;
      m_bytes = bytes;
      return true;
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
  // Raises the hard boundary after the operator's reservation grew. Never
  // lowers it below what is already charged, so an in-flight buffer survives.
  void set_limit(size_t limit) { m_limit = std::max(limit, m_used); }
  // Non-throwing allocation for explicitly sized buffers; nullptr means the
  // budget or the pool is exhausted; last_failure() distinguishes them.
  // do_allocate() keeps the throwing pmr
  // contract for containers.
  AllocationFailure last_failure() const noexcept { return m_last_failure; }
  void *try_allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept {
    m_last_failure = AllocationFailure::kNone;
    if (bytes > m_limit - m_used) {
      m_last_failure = AllocationFailure::kBudgetExhausted;
      return nullptr;
    }
    void *pointer = nullptr;
    if (m_upstream == &m_pool_memory) {
      pointer = m_pool_memory.try_allocate(bytes, alignment);
    } else {
      // A foreign upstream can only report exhaustion by throwing.
      try {
        pointer = m_upstream->allocate(bytes, alignment);
      } catch (const std::bad_alloc &) {
        m_last_failure = AllocationFailure::kPoolExhausted;
        return nullptr;
      }
    }
    if (pointer == nullptr) {
      m_last_failure = AllocationFailure::kPoolExhausted;
      return nullptr;
    }
    m_used += bytes;
    m_peak = std::max(m_peak, m_used);
    return pointer;
  }

 private:
  void acquire(size_t bytes) {
    m_last_failure = AllocationFailure::kNone;
    if (bytes > m_limit - m_used) {
      m_last_failure = AllocationFailure::kBudgetExhausted;
      throw std::bad_alloc();
    }
    m_used += bytes;
    m_peak = std::max(m_peak, m_used);
  }
  void release(size_t bytes) noexcept {
    assert(bytes <= m_used);
    m_used -= bytes;
  }
  void *do_allocate(size_t bytes, size_t alignment) override {
    if (void *pointer = try_allocate(bytes, alignment)) return pointer;
    throw std::bad_alloc();  // Required by the std::pmr contract.
  }
  void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
    m_upstream->deallocate(pointer, bytes, alignment);
    release(bytes);
  }
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
  size_t m_limit;
  PoolMemoryResource m_pool_memory;
  std::pmr::memory_resource *m_upstream;
  size_t m_used{0}, m_peak{0};
  AllocationFailure m_last_failure{AllocationFailure::kNone};
};
}  // namespace ResMgmt
}  // namespace ShannonBase
#endif
