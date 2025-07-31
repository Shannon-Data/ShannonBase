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

   The fundmental code for imcs.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/imcs/imcu.h"

#include <limits.h>

#include "sql/field.h"  //Field
#include "sql/sql_class.h"

namespace ShannonBase {
namespace Imcs {
Imcu::Imcu(const TABLE &table_arg) {}

Imcu::~Imcu() { std::scoped_lock lk(m_mutex_cus); }

/**Gets a CU by field_name (aka: key)*/
Cu *Imcu::get_cu(const Field *field) {
  Cu *cu_ptr{nullptr};
  assert(field);
  return cu_ptr;
}

/**
 * Whether this imcu is full or not. Before wirte it will check the capacity of
 * a imuc
 *
 */
bool Imcu::is_full() {
  // if there're not imcu in, the we consider that it's full. return true.
  if (!m_cus.size()) return true;
  return false;
}

}  // namespace Imcs
}  // namespace ShannonBase
