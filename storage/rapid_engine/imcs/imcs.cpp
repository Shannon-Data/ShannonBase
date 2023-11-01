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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include <mutex>

#include "sql/field.h"

#include "storage/rapid_engine/imcs/imcs.h"

namespace ShannonBase{
namespace Imcs{

unsigned long rapid_memory_size {0};

Imcs* Imcs::m_instance {nullptr};
std::once_flag Imcs::one;

Imcs::Imcs() {

}
Imcs::~Imcs() {
  
}
Imcu* Imcs::New_imcu(const TABLE& table_arg) {
  std::string db_name = table_arg.s->db.str;
  std::string table_name = table_arg.s->table_name.str;
  std::scoped_lock lk(m_imcu_mtx);
  std::string key_name = db_name + table_name;
  key_name += m_cu_id;
  Imcu* imcu = new (current_thd->mem_root) Imcu(table_arg);
  m_imcus.insert(std::make_pair(key_name, imcu));
  m_cu_id += 1;
  return imcu;
}

uint Imcs::Write(ShannonBaseContext* context, TransactionID trxid, Field* fields) {
  assert(context);
  assert(trxid != 0);
  assert(fields);
  /** before insertion, should to check whether there's spare space to store the new data.
      or not. If no extra sapce left, allocate a new imcu. After a new imcu allocated, the
      meta info is stored into 'm_imcus'.
  */
  //the last imcu key_name.
  std::string key_name = *fields->table_name;
  key_name += fields->orig_db_name;
  key_name += (m_cu_id > 0 ) ? (m_cu_id -1) : 0;

  Imcu* curr_imcu {nullptr};
  if (!m_imcus.size()) {
    curr_imcu = New_imcu(*fields->table);
  } else {
    curr_imcu = m_imcus[key_name];
  }
  assert(curr_imcu);

  Field* field = fields;
  while (field) {
 
    field ++;
  }
  return 0;
}
uint Imcs::Read (ShannonBaseContext* context, Field* field) {
  assert(context);
  assert(field);

  return 0;
}

} //ns:imcs 
} //ns:shannonbase