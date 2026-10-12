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
#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/resource_management/io_scheduler.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "storage/rapid_engine/resource_management/res_mgmt.h"
#ifdef SHANNON_LINUX_PLATFORM
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ShannonBase::ResMgmt {
namespace {
IoDepthGovernor spill_governor;
}
SpillIoScope::SpillIoScope(THD *thd, FILE *file) {
  if (work_governor().mode() != IsolationMode::AUTO || !file) return;
  uint64_t device = 0;
#ifdef SHANNON_LINUX_PLATFORM
  struct stat state;
  if (!fstat(fileno(file), &state)) device = static_cast<uint64_t>(state.st_dev);
#endif
  // The shared depth follows the effective AP limit, capped conservatively.
  // No throughput is inferred from buffered stdio or uncalibrated storage.
  // AP retains statement MDL: fail fast rather than add a resource wait.
  const size_t depth = std::min<size_t>(8, work_governor().limit(WorkClass::AP));
  m_lease = spill_governor.acquire(device, depth, std::chrono::milliseconds(0),
                                   [thd] { return work_governor().stopping() || (thd && thd->killed); });
  m_allowed = static_cast<bool>(m_lease);
  if (!m_allowed && thd) {
    if (thd->killed)
      thd->send_kill_message();
    else
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid spill I/O admission is busy or stopping");
  }
}
IoDepthGovernor::Snapshot spill_io_snapshot() { return spill_governor.snapshot(); }
}  // namespace ShannonBase::ResMgmt
