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
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace ShannonBase::ResMgmt {
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
  static size_t LimitRequest(const void *query, size_t requested) {
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

/** Atomic global/query reservations. A grant is owned until its lease dies. */
class MemoryBudget {
 public:
  class Lease {
   public:
    Lease() = default;
    ~Lease() { Reset(); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept { Swap(other); }
    Lease &operator=(Lease &&other) noexcept {
      if (this != &other) {
        Reset();
        Swap(other);
      }
      return *this;
    }
    size_t bytes() const { return m_bytes; }
    explicit operator bool() const { return m_bytes != 0; }
    void Reset() noexcept {
      if (m_owner) m_owner->Release(m_query, m_bytes);
      m_owner = nullptr;
      m_bytes = 0;
    }

   private:
    friend class MemoryBudget;
    Lease(MemoryBudget *owner, const void *query, size_t bytes) : m_owner(owner), m_query(query), m_bytes(bytes) {}
    void Swap(Lease &other) noexcept {
      std::swap(m_owner, other.m_owner);
      std::swap(m_query, other.m_query);
      std::swap(m_bytes, other.m_bytes);
    }
    MemoryBudget *m_owner{nullptr};
    const void *m_query{nullptr};
    size_t m_bytes{0};
  };

  MemoryBudget(size_t global_limit, size_t query_limit) : m_global_limit(global_limit), m_query_limit(query_limit) {}
  Lease Reserve(const void *query, size_t requested, size_t minimum = 1, size_t available_capacity = SIZE_MAX) {
    std::lock_guard lock(m_mutex);
    const auto found = m_queries.find(query);
    const size_t query_used = found == m_queries.end() ? 0 : found->second;
    const size_t available = available_capacity > m_reserved ? available_capacity - m_reserved : 0;
    const size_t bytes = std::min({requested, m_global_limit - m_reserved, m_query_limit - query_used, available});
    if (bytes == 0 || bytes < minimum) return {};
    // Insert may throw; no counters have changed until it succeeds.
    m_queries[query] = query_used + bytes;
    m_reserved += bytes;
    return Lease(this, query, bytes);
  }
  size_t reserved() const {
    std::lock_guard lock(m_mutex);
    return m_reserved;
  }

 private:
  void Release(const void *query, size_t bytes) noexcept {
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

MemoryBudget::Lease ReserveQueryMemory(const void *query, size_t requested);

/** Charges allocations, including simultaneous old/new buffers on growth. */
class BoundedMemoryResource final : public std::pmr::memory_resource {
 public:
  explicit BoundedMemoryResource(size_t limit) : m_limit(limit) {}
  size_t used() const { return m_used; }
  size_t peak() const { return m_peak; }

 private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    if (bytes > m_limit - m_used) throw std::bad_alloc();
    void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    m_used += bytes;
    m_peak = std::max(m_peak, m_used);
    return result;
  }
  void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    m_used -= bytes;
  }
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
  const size_t m_limit;
  size_t m_used{0}, m_peak{0};
};
}  // namespace ShannonBase::ResMgmt
#endif
