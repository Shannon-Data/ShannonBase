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
#include "storage/rapid_engine/reader/imcs_reader.h"

#include <string>

#include "include/ut0dbg.h" //ut_a
#include "include/template_utils.h"
#include "sql/table.h" //TABLE
#include "sql/field.h" //Field
#include "sql/my_decimal.h"
#include "storage/innobase/include/trx0trx.h"    //read_view
#include "storage/innobase/include/dict0mem.h"   //table_name_t
#include "storage/innobase/include/read0types.h" //ReadView

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/chunk.h"
namespace ShannonBase {

CuView::CuView(TABLE* table, Field* field) : m_source_table(table), m_source_field(field){
  m_key_name = m_source_table->s->db.str;
  m_key_name+= m_source_table->s->table_name.str;
  m_key_name+= m_source_field->field_name;
  m_source_cu = Imcs::Imcs::get_instance()->get_Cu(m_key_name);
  ut_a(m_source_cu);
}

int CuView::open(){
  auto chunk = m_source_cu->get_chunk(m_current_chunk_id);
  ut_a(chunk);
  m_current_pos = chunk->get_base();
  return 0;
}
int CuView::close(){
  m_current_chunk_id = 0;
  m_current_pos = nullptr;
  return 0;
}
/**
 * this function is similiar to Chunk::Read_data, you should change these funcs in sync.
*/
int CuView::read(ShannonBaseContext* context, uchar* buffer, size_t length)
{
  ut_a(context && buffer);
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  if (!m_source_cu) return HA_ERR_END_OF_FILE;

  //gets the chunks belongs to this cu.
  auto chunk = m_source_cu->get_chunk(m_current_chunk_id);
  while (chunk) {
    ptrdiff_t diff = m_current_pos - chunk->get_Data();
    if (diff >= 0) { //to the next
      m_current_chunk_id.store(m_current_chunk_id + 1, std::memory_order::memory_order_seq_cst),
      chunk = m_source_cu->get_chunk(m_current_chunk_id);
      if (!chunk) return HA_ERR_END_OF_FILE;
      m_current_pos = chunk->get_base();
      continue;
    }
    uint8 offset{0};
    uint8 info = *((uint8*)(m_current_pos + offset));   //info byte
    offset += SHANNON_INFO_BYTE_LEN;
    uint64 trxid = *((uint64*)(m_current_pos + offset)); //trxid bytes
    offset += SHANNON_TRX_ID_BYTE_LEN;
    //visibility check at firt.
    table_name_t name{const_cast<char*>(m_source_table->s->table_name.str)};
    ReadView* read_view = trx_get_read_view(rpd_context->m_trx);
    ut_ad(read_view);
    if (!read_view->changes_visible(trxid, name) || (info & DATA_DELETE_FLAG_MASK)) {//invisible and deleted
      //TODO: travel the change link to get the visibile version data.
      m_current_pos += SHANNON_ROW_TOTAL_LEN; //to the next value.
      diff = m_current_pos - chunk->get_Data();
      if (diff >= 0) {
        m_current_chunk_id.store(m_current_chunk_id + 1, std::memory_order::memory_order_seq_cst),
        chunk = m_source_cu->get_chunk(m_current_chunk_id);
        if (!chunk) return HA_ERR_END_OF_FILE;
        m_current_pos = chunk->get_base();
        continue;
      } //no data here to the next.
    }
    #ifdef SHANNON_ONLY_DATA_FETCH
      uint32 sum_ptr_off;
      uint64 pk, data;
      //reads info field
      info [[maybe_unused]] = *(m_current_pos ++);
      m_current_pos += SHANNON_TRX_ID_BYTE_LEN;
      //reads PK field
      memcpy(&pk, m_current_pos, SHANNON_ROWID_BYTE_LEN);
      m_current_pos += SHANNON_ROWID_BYTE_LEN;
      //reads sum_ptr field
      memcpy(&sum_ptr_off, m_current_pos, SHANNON_SUMPTR_BYTE_LEN);
      m_current_pos +=SHANNON_SUMPTR_BYTE_LEN;
      if (!sum_ptr_off) {
      //To be impled.
      }
      //reads real data field. if it string type stores strid otherwise, real data.
      memcpy(&data, m_current_pos, SHANNON_DATA_BYTE_LEN);
      m_current_pos +=SHANNON_DATA_BYTE_LEN;
      //cpy the data into buffer.
      memcpy(buffer, &data, SHANNON_DATA_BYTE_LEN);
      return 0;
    #else
      memcpy(buffer, m_current_pos, SHANNON_ROW_TOTAL_LEN);
      m_current_pos += SHANNON_ROW_TOTAL_LEN; //go to the next.
      return 0;
    #endif
  }
  return HA_ERR_END_OF_FILE;
}
ImcsReader::ImcsReader(TABLE* table) :
                      m_source_table(table),
                      m_db_name(table->s->db.str),
                      m_table_name(table->s->table_name.str) {
  m_rows_read = 0;
  m_start_of_scan = false;
  for (uint idx =0; idx < m_source_table->s->fields; idx ++) {
    std::string key = m_db_name + m_table_name;
    Field* field_ptr = *(m_source_table->s->field + idx);
    key += field_ptr->field_name;
    m_cu_views.insert({key, std::make_unique<CuView>(table, field_ptr)});
  }
}
int ImcsReader::open() {
  for(auto& item : m_cu_views) {
    item.second->open();
  }
  m_start_of_scan = true;
  return 0;
}
int ImcsReader::close() {
  for(auto& item : m_cu_views) {
    item.second->close();
  }
  m_start_of_scan = false;  
  return 0;
}
int ImcsReader::read(ShannonBaseContext* context, uchar* buffer, size_t length) {
  ut_a(context && buffer);
  if (!m_start_of_scan) return HA_ERR_GENERIC;
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field_ptr = *(m_source_table->field + idx);
    ut_a(field_ptr);
    if (field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    std::string key = m_db_name + m_table_name + field_ptr->field_name;
    if (m_cu_views.find(key) == m_cu_views.end()) return HA_ERR_END_OF_FILE;

    uchar buff[SHANNON_ROW_TOTAL_LEN + 1] = {0};
    if (auto ret = m_cu_views[key].get()->read(context, buff)) return ret;
    #ifdef SHANNON_ONLY_DATA_FETCH
      long long data = *(long long*) buff;
      my_bitmap_map *old_map = 0;
      old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (field_ptr->type() == MYSQL_TYPE_BLOB || field_ptr->type() == MYSQL_TYPE_STRING
          || field_ptr->type() == MYSQL_TYPE_VARCHAR) { //read the data
        String str;
        Compress::Dictionary* dict = m_cu_views[key].get()->local_dictionary();
        ut_ad(dict);
        dict->get(data, str, *const_cast<CHARSET_INFO*>(field_ptr->charset()));
        if (str.length())
          field_ptr->set_notnull();
        field_ptr->store(str.c_ptr(), str.length(), &my_charset_bin);
      } else {
        field_ptr->store (data);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #else
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      uint8 info = *(uint8*) buff;
      if (info & DATA_NULL_FLAG_MASK)
        field_ptr->set_null();
      else {
        field_ptr->set_notnull();
        uint8 data_offset = SHANNON_INFO_BYTE_LEN + SHANNON_TRX_ID_BYTE_LEN + SHANNON_ROWID_BYTE_LEN;
              data_offset += SHANNON_SUMPTR_BYTE_LEN;
        double val = *(double*) (buff + data_offset);
        switch (field_ptr->type()) {
          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_STRING:
          case MYSQL_TYPE_VARCHAR: { //if string, stores its stringid, and gets from local dictionary.
            String str;
            Compress::Dictionary* dict =m_cu_views[key].get()->get_source()->local_dictionary();
            ut_ad(dict);
            dict->get(val, str, *const_cast<CHARSET_INFO*>(field_ptr->charset()));
            field_ptr->store(str.c_ptr(), str.length(), &my_charset_bin);
          }break;
          case MYSQL_TYPE_INT24:
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE: {
            field_ptr->store(val);
          } break;
          case MYSQL_TYPE_DECIMAL:
          case MYSQL_TYPE_NEWDECIMAL:{
            field_ptr->store(val);
          }break;
          case MYSQL_TYPE_DATE:
          case MYSQL_TYPE_DATETIME2:
          case MYSQL_TYPE_DATETIME:{
            field_ptr->store (val);
          } break;
          default: field_ptr->store (val);
        }
        if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
      }
    #endif
  }
  return 0;
}
int ImcsReader::write(ShannonBaseContext* context, uchar*buffer, size_t lenght) {
  return 0;
}
uchar* ImcsReader::tell() {
  return nullptr;
}
uchar* ImcsReader::seek(uchar* pos) {
  return nullptr;
}
uchar* ImcsReader::seek(size_t offset) {
  return nullptr;
}

} //ns:shannonbase