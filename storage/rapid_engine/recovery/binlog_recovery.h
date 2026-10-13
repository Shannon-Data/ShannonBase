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
#ifndef __SHANNONBASE_BINLOG_RECOVERY_H__
#define __SHANNONBASE_BINLOG_RECOVERY_H__
#include <functional>
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"
class THD;
struct TABLE;
namespace ShannonBase {
namespace Recovery {
// Recovery-only reader of local committed MySQL transactions. Never executes
// replication events against the primary. Failure discards the private restore.
class BinlogRecovery {
 public:
  static BinlogPosition current_position();
  static bool certify(BinlogPosition &position);
  // Caller pins/locks the binlog index when validating an already resolved file.
  static bool certify_file(const char *path, BinlogPosition &position);
  static bool replay(THD *thd, TABLE *table, const BinlogPosition &start,
                     const std::function<bool(Populate::change_record_buff_t &)> &apply);
};
}  // namespace Recovery
}  // namespace ShannonBase
#endif
