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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_WRITABLE_ACCESS_PATH_H__
#define __SHANNONBASE_WRITABLE_ACCESS_PATH_H__

#include "sql/join_optimizer/access_path.h"

namespace ShannonBase {
namespace Optimizer {
struct SHANNON_ALIGNAS RapidScanParameters {
  Imcs::RpdTable *rpd_table{nullptr};
  bool use_storage_index{false};
  std::shared_ptr<Imcs::Predicate> prune_predicate;
  std::vector<uint32_t> projected_columns;
  ha_rows limit{HA_POS_ERROR};
  ha_rows offset{0};
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_WRITABLE_ACCESS_PATH_H__
