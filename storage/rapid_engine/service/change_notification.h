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
#ifndef __SHANNON_CHANGE_NOTIFICATION_H__
#define __SHANNON_CHANGE_NOTIFICATION_H__
#include "my_inttypes.h"
class THD;
struct TABLE;
class COPY_INFO;

namespace ShannonBase {
namespace Service {
// Source facts from after-write hooks. This service owns routing to capture,
// cascade fencing and persistence eligibility; callers know none of those policies.
void notify_insert(THD *, TABLE *, COPY_INFO *);
void notify_update(THD *, TABLE *, const uchar *old_row, const uchar *new_row);
void notify_delete(THD *, TABLE *, const uchar *old_row);
}  // namespace Service
}  // namespace ShannonBase
#endif
