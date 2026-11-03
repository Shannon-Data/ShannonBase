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

   Data processing unit: the single dispatch point between the batch layer and a
   hardware backend.
*/

#ifndef __SHANNONBASE_DPU_DISPATCH_H__
#define __SHANNONBASE_DPU_DISPATCH_H__

#include <cstddef>
#include <cstdint>
#include <memory>

#include "storage/rapid_engine/dpu/backend.h"

namespace ShannonBase {
namespace Dpu {

/** Runtime policy. These become the rapid_dpu_* system variables. */
struct DispatchPolicy {
  // Off by default: a device backend is opted into, never picked up silently.
  bool enabled{false};
  // Below this many rows the copy round trip costs more than the kernel saves.
  size_t min_rows{4096};
};

/** Software counters only. Device PMCs stay in the perf harness. */
struct DispatchCounters {
  uint64_t requests{0};
  uint64_t claimed{0};
  uint64_t refused_disabled{0};
  uint64_t refused_small{0};
  uint64_t refused_capability{0};
  uint64_t executed{0};
  uint64_t failed{0};
};

/**
 * The single hardware dispatch point.
 *
 * Backends are tried in registration order and the first one that Supports()
 * the request wins. The host backend is always registered and never claims, so
 * UNCLAIMED always means "run the existing host path" and a build without any
 * accelerator behaves exactly as before.
 */
class DeviceDispatcher {
 public:
  static DeviceDispatcher &Instance();

  void SetPolicy(const DispatchPolicy &policy);
  DispatchPolicy GetPolicy() const;

  /** False when the backend was rejected (null, or the backend limit is hit). */
  bool RegisterBackend(std::unique_ptr<DeviceBackend> backend);
  size_t BackendCount() const;

  /**
   * Planner-facing capability probe. It ignores the runtime policy and the row
   * threshold, so the answer stays stable for a given plan and backend set.
   */
  bool Supports(const KernelRequest &request) const;

  /**
   * Batching hint for SelectBatchRows(): how many rows the claiming backend
   * prefers per call. 0 means no opinion.
   */
  size_t PreferredRows(const KernelRequest &request) const;

  /**
   * Claim and execute. UNCLAIMED means the caller must run the host path and
   * must not inspect `result`.
   */
  Status Execute(const KernelRequest &request, const KernelResult &result);

  DispatchCounters Counters() const;

 private:
  DeviceDispatcher();
  ~DeviceDispatcher();
  DeviceDispatcher(const DeviceDispatcher &) = delete;
  DeviceDispatcher &operator=(const DeviceDispatcher &) = delete;

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace Dpu
}  // namespace ShannonBase

#endif  // __SHANNONBASE_DPU_DISPATCH_H__
