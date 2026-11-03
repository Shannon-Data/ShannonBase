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

   Data processing unit: the no-device backend, built on every platform.
*/

#ifndef __SHANNONBASE_DPU_HOST_BACKEND_H__
#define __SHANNONBASE_DPU_HOST_BACKEND_H__

#include <cstddef>

#include "storage/rapid_engine/dpu/backend.h"

namespace ShannonBase {
namespace Dpu {

/**
 * The backend that is always present and never claims anything.
 *
 * It exists for three reasons:
 *   1. the dispatcher, its counters and the capability probe are exercised in
 *      every build, including one with no accelerator compiled in;
 *   2. UNCLAIMED therefore always means "the existing host path", never "no
 *      backend is registered";
 *   3. it is the natural home for a scalar reference implementation later, used
 *      to validate a device kernel against the host answer.
 */
class HostBackend final : public DeviceBackend {
 public:
  const Capabilities &capabilities() const override;

  bool Supports(const KernelRequest &) const override { return false; }
  Status Execute(const KernelRequest &, const KernelResult &) override { return Status::UNCLAIMED; }

  void *Allocate(size_t) override { return nullptr; }
  void Deallocate(void *) override {}
  bool CopyIn(void *, const void *, size_t) override { return false; }
  bool CopyOut(void *, const void *, size_t) override { return false; }
  bool Synchronize() override { return true; }
};

}  // namespace Dpu
}  // namespace ShannonBase

#endif  // __SHANNONBASE_DPU_HOST_BACKEND_H__
