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
#ifndef __SHANNONBASE_LOG_BINLOG_REPLAY_H__
#define __SHANNONBASE_LOG_BINLOG_REPLAY_H__
#include <functional>
#include "storage/rapid_engine/include/rapid_change_record.h"
class THD;
struct TABLE;

namespace ShannonBase {
namespace Populate {
// Applies complete source transactions to an unpublished restored table.
// The caller holds write-excluding source MDL and discards the image on failure.
class BinlogReplay {
 public:
  static bool replay(THD *, TABLE *, const BinlogPosition &, const std::function<bool(change_record_buff_t &)> &apply);
};

}  // namespace Populate
}  // namespace ShannonBase
#endif
