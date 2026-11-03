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

   Data processing unit: the backend interface every accelerator implements.
*/

#ifndef __SHANNONBASE_DPU_BACKEND_H__
#define __SHANNONBASE_DPU_BACKEND_H__

#include <cstddef>
#include <cstdint>

#include "storage/rapid_engine/dpu/kernel.h"

namespace ShannonBase {
namespace Dpu {

enum class BackendKind : uint8_t { HOST = 0, CUDA, HIP, SYCL, OTHER };

/**
 * Outcome of one dispatch.
 *
 *   UNCLAIMED  the host path must run. Nothing was written.
 *   OK         the result is complete.
 *   ERROR      a claimed kernel failed. The statement fails; it is not retried
 *              on the host, because a partially written buffer cannot be told
 *              apart from a real answer.
 */
enum class Status : uint8_t { UNCLAIMED = 0, OK = 1, ERROR = 2 };

struct Capabilities {
  BackendKind kind{BackendKind::HOST};
  const char *name{"none"};
  uint64_t device_memory_bytes{0};  // visible to this process
  uint64_t max_allocation_bytes{0};
  size_t max_rows_per_dispatch{0};  // 0 means no limit
  bool asynchronous{false};
};

/**
 * One accelerator.
 *
 * A backend is validated once per kind of column, not per row: it must answer
 * Supports() exactly as it will behave in Execute(), or the plan the optimizer
 * chose and the operator that runs will disagree.
 */
class DeviceBackend {
 public:
  DeviceBackend() = default;
  virtual ~DeviceBackend() = default;
  DeviceBackend(const DeviceBackend &) = delete;
  DeviceBackend &operator=(const DeviceBackend &) = delete;

  virtual const Capabilities &capabilities() const = 0;

  /**
   * Pure capability question, side-effect free and callable from the optimizer
   * thread. The planner asks this to decide which physical operator to put in
   * the plan: a plan that picks a device operator and is refused at run time
   * aborts the query instead of falling back.
   */
  virtual bool Supports(const KernelRequest &request) const = 0;

  /** Fill `result` completely, or return UNCLAIMED and leave it untouched. */
  virtual Status Execute(const KernelRequest &request, const KernelResult &result) = 0;

  /**
   * Device-visible memory.
   *
   * Every byte is charged by the caller through ResMgmt::MemoryBudget before it
   * is requested here. A backend must never allocate device memory behind the
   * memory governor's back: that bypasses the query limit and eviction
   * arbitration, and the accounting tests depend on it.
   */
  virtual void *Allocate(size_t bytes) = 0;
  virtual void Deallocate(void *device_memory) = 0;
  virtual bool CopyIn(void *device_memory, const void *host_memory, size_t bytes) = 0;
  virtual bool CopyOut(void *host_memory, const void *device_memory, size_t bytes) = 0;
  virtual bool Synchronize() = 0;
};

}  // namespace Dpu
}  // namespace ShannonBase

#endif  // __SHANNONBASE_DPU_BACKEND_H__
