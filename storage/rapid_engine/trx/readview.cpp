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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. for readview.
   Now that, we use innodb trx as rapid's. But, in future, we will impl
   our own trx implementation, because we use innodb trx id in rapid
   for our visibility check.
*/
#include "storage/rapid_engine/trx/readview.h"

#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase {
namespace ReadView {
uchar *smu_item_vec_t::get_data(Rapid_load_context *context) {
  std::lock_guard<std::mutex> lock(vec_mutex);
  for (auto it = items.rbegin(); it < items.rend(); it++) {
    if (!context->m_trx->changes_visible(it->trxid, context->m_table_name)) {
      return it->data.get();
    }
  }

  return nullptr;
}

}  // namespace ReadView
}  // namespace ShannonBase