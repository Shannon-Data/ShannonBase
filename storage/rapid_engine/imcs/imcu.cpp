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
#include <limits.h>

#include "sql/current_thd.h"
#include "sql/sql_class.h"

#include "storage/rapid_engine/imcs/imcu.h"

namespace ShannonBase {
namespace Imcs {
Imcu::Imcu() {

}

Imcu::Imcu(uint num_cus [[maybe_unused]]) {

}
Imcu::~Imcu() {

}

uint Imcu::Build_header(const TABLE& table_arg) {
  m_headers.m_num_cu = 0;

  m_headers.m_max_value = 0;
  m_headers.m_min_value = LLONG_MIN;
  m_headers.m_median = 0;
  m_headers.m_middle = 0;
  m_headers.m_avg = 0;
  m_headers.m_has_vcol = false;

  Field* field_ptr = *table_arg.s->field;
  m_headers.m_db = table_arg.s->db.str;
  m_headers.m_table = table_arg.s->table_name.str;

  std::scoped_lock lk (m_mutex_cus);
  for (uint index =0; index < table_arg.s->fields; index ++) {
    field_ptr = *(table_arg.s->field + index);
    m_headers.m_field.push_back(field_ptr);
    std::string field_name  = field_ptr->field_name;
    //TODO: use own memory.
    Cu* cu_ptr = new (current_thd->mem_root) Cu (field_ptr);
    if (cu_ptr)
      m_cus.insert(std::make_pair(field_name, cu_ptr));
  }
  m_headers.m_fields = table_arg.s->fields;
  return 0;
}
uint Imcu::Release_header() {
  std::scoped_lock lk(m_mutex_cus);
  std::map<std::string, Cu*>::iterator its = m_cus.begin();
  for (; its != m_cus.end(); its ++) {
    if (its->second) {
      delete its->second;
    }
  }
  return 0;
}
/**Gets a CU by field_name (aka: key)*/
Cu* Imcu::Get_cu(std::string& field_name) {
  Cu* cu_ptr {nullptr};
  std::scoped_lock lk(m_mutex_header);
  auto pos =  m_cus.find(field_name);
  if (pos == m_cus.end()) {
    return cu_ptr;
  }

  cu_ptr = pos->second;
  return cu_ptr;
}

} // ns:Imcs
} // ns:ShannonBase
