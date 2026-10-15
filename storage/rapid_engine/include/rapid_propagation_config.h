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
#ifndef SHANNON_PROPAGATION_CONFIG_H
#define SHANNON_PROPAGATION_CONFIG_H
#include <atomic>
#include <shared_mutex>
#include "my_inttypes.h"

namespace ShannonBase {
// Source selects the record format/parser; COPY_INFO has two producer modes.
enum class Source : uint8 {
  UN_KNOWN = 0,
  REDO_LOG, /** serialized InnoDB redo/mtr records */
  COPY_INFO /** detached MySQL row images from notification or committed binlog */
};

enum class CopyInfoMode : uint8 { NOTIFICATION = 0, COMMITTED_BINLOG };

// Loads and unloads hold shared ownership through publication or worker shutdown.
// A mode change takes exclusive ownership and requires no loaded tables or active
// propagation. try_lock in the sysvar callbacks avoids waiting while holding
// MySQL global system-variable locks.
inline std::shared_mutex propagation_mode_mutex;

// The configured backend is sampled at load admission and stays fixed until
// all tables have been unloaded and propagation has stopped.
inline std::atomic<CopyInfoMode> configured_change_propagation_mode{CopyInfoMode::NOTIFICATION};

constexpr bool propagation_query_supported(CopyInfoMode mode, bool autocommit, bool explicit_transaction) {
  return mode == CopyInfoMode::NOTIFICATION || (autocommit && !explicit_transaction);
}
constexpr bool propagation_backend_available(CopyInfoMode mode) {
  return mode == CopyInfoMode::NOTIFICATION || mode == CopyInfoMode::COMMITTED_BINLOG;
}

}  // namespace ShannonBase
#endif
