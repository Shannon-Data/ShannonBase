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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/imcs/imcu.h"

#include <limits.h>

#include "sql/sql_class.h"
#include "sql/field.h"  //Field

namespace ShannonBase {
namespace Imcs {
Imcu::Imcu(const TABLE& table_arg) {
  m_version_num = SHANNON_MAGIC_IMCU;
  m_magic_num = SHANNON_MAGIC_IMCU;

  m_headers.m_avg = 0;
  m_headers.m_fields = 0;
  m_headers.m_max_value = std::numeric_limits<double>::lowest();
  m_headers.m_min_value = std::numeric_limits<double>::max();
  m_headers.m_median = 0;
  m_headers.m_middle = 0;

  m_headers.m_db = table_arg.s->db.str;
  m_headers.m_table = table_arg.s->table_name.str;
  m_headers.m_has_vcol = false;

  uint fields {table_arg.s->fields};
  std::scoped_lock lk(m_mutex_cus);
  for (uint index =0; index < fields; index ++) {
    Field* col  = *(table_arg.s->field + index);
    if (col->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    m_headers.m_fields ++;
    m_headers.m_field.push_back(col);

    Cu* cu = new Cu (col);
    auto ret = m_cus.insert (std::make_pair(col, cu));
    if (!ret.second) { //if insert failed. the clear up all the inserted items.
      m_cus.clear();
      m_headers.m_fields =0;
      m_headers.m_field.clear();
    }
  }

  m_prev = nullptr;
  m_next = nullptr;
}

Imcu::~Imcu() {
  std::scoped_lock lk(m_mutex_cus);
}

/**Gets a CU by field_name (aka: key)*/
Cu* Imcu::get_cu(const Field* field) {
  Cu* cu_ptr {nullptr};
  assert(field);
  return cu_ptr;
}

/**
 * Whether this imcu is full or not. Before wirte it will check the capacity of a imuc
 * 
*/
bool Imcu::is_full() {
  //if there're not imcu in, the we consider that it's full. return true.
  if (!m_cus.size()) return true;
  return false;
}

} // ns:Imcs
} // ns:ShannonBase
