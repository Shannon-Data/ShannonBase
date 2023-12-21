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

uint Imcs::Write(RapidContext* context, Field* field) {
  assert(context);
  assert(field);
  /** before insertion, should to check whether there's spare space to store the new data.
      or not. If no extra sapce left, allocate a new imcu. After a new imcu allocated, the
      meta info is stored into 'm_imcus'.
  */
  //the last imcu key_name.
  std::string key_name = *field->table_name;
  key_name += field->table->s->db.str;
  key_name += field->field_name;

  auto elem = m_cus.find(key_name);
  if ( elem == m_cus.end()) { //a new field. not found
    auto [it, sucess] = m_cus.insert(std::pair{key_name, std::make_unique<Cu>(field)});
    if (!sucess) return 1;
  }
  //start writing the data, at first, assemble the data we want to write. the layout of data
  //pls ref to: issue #8.[info | trx id | rowid(pk)| smu_ptr| data]
  uint data_len = 1 + 8 + 8 + 4;
  if (!field->is_real_null())
    data_len += field->pack_length();

  std::unique_ptr<uchar> data(new uchar[data_len]);
  int8 info {0};
  int64 sum_ptr{0};
  if (field->is_real_null())
   info |= DATA_NULL_FLAG_MASK;

  memcpy(data.get(), &info, 1);
  memcpy(data.get() + 1, &context->m_extra_info.m_trxid, 8);
  memcpy(data.get() + 9, &context->m_extra_info.m_pk, 8);
  memcpy(data.get() + 17, &sum_ptr, 4);
  //value is null, then return.
  if (!field->is_real_null())
    memcpy(data.get() + 21, field->data_ptr(), field->pack_length());
  
  if (!m_cus[key_name]->Write_data(context, data.get(), data_len)) return 1;
  return 0;
}
uint Imcs::Read (RapidContext* context, Field* field) {
  assert(context && field);
  return 0;
}
uint Imcs::Read(RapidContext* context, uchar* buffer) {
  assert(context && buffer);
  return 0;
}
uint Read_batch(RapidContext* context, uchar* buffer){
  assert(context && buffer);
  return 0;
}
uint Imcs::Delete(RapidContext* context, Field* field, uchar* rowid) {
  return 0;
}
uint Imcs::Delete_all(RapidContext* context) {
  return 0;
}
} //ns:imcs 
} //ns:shannonbase