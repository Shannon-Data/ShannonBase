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

   Data processing unit: backend registry and dispatch policy.
*/

#include "storage/rapid_engine/dpu/dispatch.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "storage/rapid_engine/dpu/host/host_backend.h"

namespace ShannonBase {
namespace Dpu {

namespace {
// A handful of accelerators is the realistic maximum; a device is never added
// or removed while a statement runs.
constexpr size_t kMaxBackends = 4;

// Rows one request covers: a sparse selection renumbers them, so the selection
// length, not the column length, is the work size.
inline size_t RequestRows(const KernelRequest &request) {
  if (request.selection.Sparse()) return request.selection.count;
  if (request.inputs == nullptr || request.input_count == 0) return 0;
  return request.inputs[0].rows;
}
}  // namespace

struct DeviceDispatcher::Impl {
  mutable std::mutex mutex;  // guards `backends` and `policy`
  std::vector<std::unique_ptr<DeviceBackend>> backends;
  DispatchPolicy policy;

  // Hot-path counters live outside the mutex.
  std::atomic<uint64_t> requests{0};
  std::atomic<uint64_t> claimed{0};
  std::atomic<uint64_t> refused_disabled{0};
  std::atomic<uint64_t> refused_small{0};
  std::atomic<uint64_t> refused_capability{0};
  std::atomic<uint64_t> executed{0};
  std::atomic<uint64_t> failed{0};
};

DeviceDispatcher &DeviceDispatcher::Instance() {
  // Function-local static: constructed on first use, no static-init order risk.
  static DeviceDispatcher instance;
  return instance;
}

DeviceDispatcher::DeviceDispatcher() : m_impl(std::make_unique<Impl>()) {
  // Registered first and kept last: it never claims, and it keeps the no-device
  // path exercised in every build.
  m_impl->backends.push_back(std::make_unique<HostBackend>());
}

DeviceDispatcher::~DeviceDispatcher() = default;

void DeviceDispatcher::SetPolicy(const DispatchPolicy &policy) {
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  m_impl->policy = policy;
}

DispatchPolicy DeviceDispatcher::GetPolicy() const {
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->policy;
}

bool DeviceDispatcher::RegisterBackend(std::unique_ptr<DeviceBackend> backend) {
  if (!backend) return false;
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  if (m_impl->backends.size() >= kMaxBackends) return false;
  // Keep the host backend last, so it is always the final fallback.
  m_impl->backends.insert(m_impl->backends.empty() ? m_impl->backends.end() : m_impl->backends.end() - 1,
                          std::move(backend));
  return true;
}

size_t DeviceDispatcher::BackendCount() const {
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->backends.size();
}

bool DeviceDispatcher::Supports(const KernelRequest &request) const {
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  for (const auto &backend : m_impl->backends) {
    if (backend->Supports(request)) return true;
  }
  return false;
}

size_t DeviceDispatcher::PreferredRows(const KernelRequest &request) const {
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  for (const auto &backend : m_impl->backends) {
    if (!backend->Supports(request)) continue;
    return backend->capabilities().max_rows_per_dispatch;
  }
  return 0;
}

Status DeviceDispatcher::Execute(const KernelRequest &request, const KernelResult &result) {
  m_impl->requests.fetch_add(1, std::memory_order_relaxed);

  // Snapshot the claimant, then run it without the registry lock: a kernel must
  // not serialise registration, and backends are never unregistered while a
  // statement can still reference them.
  DeviceBackend *claimant = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->policy.enabled) {
      m_impl->refused_disabled.fetch_add(1, std::memory_order_relaxed);
      return Status::UNCLAIMED;
    }
    const size_t min_rows = m_impl->policy.min_rows;
    if (!request.selection.Sparse() && RequestRows(request) < min_rows) {
      m_impl->refused_small.fetch_add(1, std::memory_order_relaxed);
      return Status::UNCLAIMED;
    }
    for (const auto &backend : m_impl->backends) {
      if (!backend->Supports(request)) continue;
      claimant = backend.get();
      break;
    }
    if (claimant == nullptr) {
      m_impl->refused_capability.fetch_add(1, std::memory_order_relaxed);
      return Status::UNCLAIMED;
    }
    m_impl->claimed.fetch_add(1, std::memory_order_relaxed);
  }

  const Status status = claimant->Execute(request, result);
  if (status == Status::OK) {
    m_impl->executed.fetch_add(1, std::memory_order_relaxed);
  } else {
    // UNCLAIMED after a claim and a hard failure are both errors here: the
    // caller was told this backend would run the kernel.
    m_impl->failed.fetch_add(1, std::memory_order_relaxed);
  }
  return status;
}

DispatchCounters DeviceDispatcher::Counters() const {
  DispatchCounters counters;
  counters.requests = m_impl->requests.load(std::memory_order_relaxed);
  counters.claimed = m_impl->claimed.load(std::memory_order_relaxed);
  counters.refused_disabled = m_impl->refused_disabled.load(std::memory_order_relaxed);
  counters.refused_small = m_impl->refused_small.load(std::memory_order_relaxed);
  counters.refused_capability = m_impl->refused_capability.load(std::memory_order_relaxed);
  counters.executed = m_impl->executed.load(std::memory_order_relaxed);
  counters.failed = m_impl->failed.load(std::memory_order_relaxed);
  return counters;
}

}  // namespace Dpu
}  // namespace ShannonBase
