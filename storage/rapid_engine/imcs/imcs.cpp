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
#include "storage/rapid_engine/imcs/imcs.h"

#include <mutex>
#include "sql/field.h"
#include "sql/my_decimal.h"
#include "storage/innobase/include/ut0dbg.h"   //ut_ad

#include "storage/rapid_engine/utils/utils.h" //Utils
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/cu.h"

namespace ShannonBase{
namespace Imcs{

unsigned long rapid_memory_size {SHANNON_MAX_MEMRORY_SIZE};
unsigned long rapid_chunk_size {SHANNON_CHUNK_SIZE};
Imcs* Imcs::m_instance {nullptr};
std::once_flag Imcs::one;

Imcs::Imcs() {
}

Imcs::~Imcs() { 
}

uint Imcs::initialize() {
  DBUG_TRACE;
  return 0;
}

uint Imcs::deinitialize() {
  DBUG_TRACE;
  return 0;
}

Cu* Imcs::get_cu(std::string& key) {
  DBUG_TRACE;
  if (m_cus.find(key) != m_cus.end()) {
    return m_cus[key].get();
  }
  return nullptr;
}

void Imcs::add_cu(std::string key, std::unique_ptr<Cu>& cu) {
  DBUG_TRACE;
  m_cus.insert({key, std::move(cu)});
  return;
}

ha_rows Imcs::get_rows(TABLE* source_table) {
  ha_rows row_count{0};
  std::string key_part (source_table->s->db.str);
  key_part += source_table->s->table_name.str;

  for (uint index =0; index < source_table->s->fields; index++) {
    Field* field_ptr = *(source_table->field + index);
    ut_ad(field_ptr);
    if (!bitmap_is_set(source_table->read_set, field_ptr->field_index()) ||
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    std::string key = key_part + field_ptr->field_name;
    if (m_cus.find(key) == m_cus.end()) continue; //not found this field.
    row_count = m_cus[key].get()->get_header()->m_rows;
    break;
  }

  return row_count;
}

uint Imcs::rnd_init(bool scan) {
  DBUG_TRACE;
  //ut::new_withkey<Compress::Dictionar>(UT_NEW_THIS_FILE_PSI_KEY);
  for(auto &cu: m_cus) {
    auto ret = cu.second.get()->rnd_init(scan);
    if (ret) return ret;
  }
  m_inited = handler::RND;
  return 0;
}

uint Imcs::rnd_end() {
  DBUG_TRACE;
  for(auto &cu: m_cus) {
    auto ret = cu.second.get()->rnd_end();
    if (ret) return ret;
  }
  m_inited = handler::NONE;
  return 0;
}

uint Imcs::write_direct(ShannonBase::RapidContext* context, Field* field) {
  DBUG_TRACE;
  ut_ad(context && field);
  /** before insertion, should to check whether there's spare space to store the new data.
      or not. If no extra sapce left, allocate a new imcu. After a new imcu allocated, the
      meta info is stored into 'm_imcus'.
  */
  //the last imcu key_name.
  if (!Utils::Util::is_support_type(field->type())) {
    std::ostringstream err;
    err << field->field_name << " type not allowed";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }
  std::string key_name = field->table->s->db.str;
  key_name += *field->table_name;
  key_name += field->field_name;

  auto elem = m_cus.find(key_name);
  if ( elem == m_cus.end()) { //a new field. not found
    auto [it, sucess] = m_cus.insert(std::pair{key_name, std::make_unique<Cu>(field)});
    if (!sucess) return HA_ERR_GENERIC;
  }
  //start writing the data, at first, assemble the data we want to write. the layout of data
  //pls ref to: issue #8.[info | trx id | rowid(pk)| smu_ptr| data]. And the string we dont
  //store the string but using string id instead. offset[] = {0, 1, 9, 17, 21, 29}
  uint data_len = SHANNON_INFO_BYTE_LEN + SHANNON_TRX_ID_BYTE_LEN + SHANNON_ROWID_BYTE_LEN;
  data_len += SHANNON_SUMPTR_BYTE_LEN + SHANNON_DATA_BYTE_LEN;
  //start to pack the data, then writes into memory. 
  std::unique_ptr<uchar[]> data (new uchar[data_len]);
  int8 info {0};
  int64 sum_ptr{0}, offset{0};
  if (field->is_real_null())
   info |= DATA_NULL_FLAG_MASK;

  double rowid{0};
  memcpy(data.get() + offset, &info, SHANNON_INFO_BYTE_LEN);
  offset += SHANNON_INFO_BYTE_LEN;
  memcpy(data.get() + offset, &context->m_extra_info.m_trxid, SHANNON_TRX_ID_BYTE_LEN);
  offset += SHANNON_TRX_ID_BYTE_LEN;
  memcpy(data.get() + offset, &rowid, SHANNON_ROWID_BYTE_LEN);
  offset += SHANNON_ROWID_BYTE_LEN;
  memcpy(data.get() + offset, &sum_ptr, SHANNON_SUMPTR_BYTE_LEN);
  offset += SHANNON_SUMPTR_BYTE_LEN;
  Compress::Dictionary* dict = m_cus[key_name]->local_dictionary();
  double data_val = Utils::Util::get_field_value(field, dict);
  memcpy(data.get() + offset, &data_val, SHANNON_DATA_BYTE_LEN);

  if (!m_cus[key_name]->write_data_direct(context, data.get(), data_len)) return 1;
  return 0;
}

uint Imcs::read_direct(ShannonBase::RapidContext* context, Field* field) {
  ut_ad(context && field);
  return 0;
}

uint Imcs::read_direct(ShannonBase::RapidContext* context, uchar* buffer) {
  DBUG_TRACE;
  ut_ad(context && buffer);
  if (!m_cus.size()) return HA_ERR_END_OF_FILE;

  for (uint index =0; index < context->m_table->s->fields; index++) {
    Field* field_ptr = *(context->m_table->field + index);
    ut_ad(field_ptr);
    if (!bitmap_is_set(context->m_table->read_set, field_ptr->field_index()) || 
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    std::string key = context->m_current_db + context->m_current_table;
    key += field_ptr->field_name;

    if (m_cus.find(key) == m_cus.end()) continue; //not found this field.
    uchar buff [SHANNON_ROW_TOTAL_LEN] = {0};
    if (!m_cus[key].get()->read_data_direct(context, buff))
       return HA_ERR_END_OF_FILE;

    uint8 info = *(uint8*) buff;
    my_bitmap_map *old_map = tmp_use_all_columns(context->m_table, context->m_table->write_set);
    if (info & DATA_NULL_FLAG_MASK)
      field_ptr->set_null();
    else {
      field_ptr->set_notnull();
      uint8 data_offset = SHANNON_INFO_BYTE_LEN + SHANNON_TRX_ID_BYTE_LEN + SHANNON_ROWID_BYTE_LEN;
            data_offset += SHANNON_SUMPTR_BYTE_LEN;
      double val = *(double*) (buff + data_offset);
      Compress::Dictionary* dict = m_cus[key]->local_dictionary();
      Utils::Util::store_field_value(context->m_table, field_ptr, dict, val);
    }
    if (old_map) tmp_restore_column_map(context->m_table->write_set, old_map);
  }
  return 0;
}

uint read_batch_direct(ShannonBase::RapidContext* context, uchar* buffer){
  ut_ad(context && buffer);
  return 0;
}

uint Imcs::delete_direct(ShannonBase::RapidContext* context, Field* field, uchar* rowid) {
  if (!context || !field) return HA_ERR_GENERIC;

  std::string key = context->m_current_db + context->m_current_table;
  key += field->field_name;
  m_cus.erase(key);
  return 0;
}

uint Imcs::delete_all_direct(ShannonBase::RapidContext* context) {

  std::string key = context->m_current_db;
  key += context->m_current_table;

  for (auto& item : m_cus){
    if (item.first.compare(0, key.length(), key, 0, key.length()) == 0)
      m_cus.erase(item.first);
  }

  return 0;
}
} //ns:imcs 
} //ns:shannonbase