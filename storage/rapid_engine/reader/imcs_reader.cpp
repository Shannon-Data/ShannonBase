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
  ut_a(!m_source_field->is_flag_set(NOT_SECONDARY_FLAG));
  m_key_name = m_source_table->s->db.str;
  m_key_name+= m_source_table->s->table_name.str;
  m_key_name+= m_source_field->field_name;
  m_source_cu = Imcs::Imcs::get_instance()->get_Cu(m_key_name);
  if (!m_source_cu) {
    std::unique_ptr<Imcs::Cu> new_cu = std::make_unique<Imcs::Cu>(field);
    Imcs::Imcs::get_instance()->add_cu(m_key_name, new_cu);
  }
  m_source_cu = Imcs::Imcs::get_instance()->get_Cu(m_key_name);
  ut_a(m_source_cu);
}
int CuView::open(){
  auto chunk = m_source_cu->get_chunk(m_reader_chunk_id);
  ut_a(chunk);
  m_reader_pos = chunk->get_base();
  m_reader_chunk_id = 0;

  m_writter_chunk_id = m_source_cu->get_chunk_nums() ? m_source_cu->get_chunk_nums() -1: 0;
  m_writter_pos = m_source_cu->get_chunk(m_writter_chunk_id)->get_data();
  return 0;
}
int CuView::close(){
  m_reader_chunk_id = 0;
  m_reader_pos = nullptr;

  m_writter_chunk_id = 0;
  m_writter_pos = nullptr;
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
  auto chunk = m_source_cu->get_chunk(m_reader_chunk_id);
  while (chunk) {
    ptrdiff_t diff = m_reader_pos - chunk->get_data();
    if (diff >= 0) { //to the next
      m_reader_chunk_id.fetch_add(1, std::memory_order::memory_order_acq_rel);
      chunk = m_source_cu->get_chunk(m_reader_chunk_id);
      if (!chunk) return HA_ERR_END_OF_FILE;
      m_reader_pos.store(chunk->get_base(), std::memory_order_acq_rel);
      continue;
    }
    uint8 offset{0};
    uint8 info = *((uint8*)(m_reader_pos + offset));   //info byte
    offset += SHANNON_INFO_BYTE_LEN;
    uint64 trxid = *((uint64*)(m_reader_pos + offset)); //trxid bytes
    offset += SHANNON_TRX_ID_BYTE_LEN;
    //visibility check at firt.
    table_name_t name{const_cast<char*>(m_source_table->s->table_name.str)};
    ReadView* read_view = trx_get_read_view(rpd_context->m_trx);
    ut_ad(read_view);
    if (!read_view->changes_visible(trxid, name) || (info & DATA_DELETE_FLAG_MASK)) {//invisible and deleted
      //TODO: travel the change link to get the visibile version data.
      m_reader_pos.fetch_add (SHANNON_ROW_TOTAL_LEN, std::memory_order_acq_rel); //to the next value.
      diff = m_reader_pos - chunk->get_data();
      if (diff >= 0) {
        m_reader_chunk_id.fetch_add(1, std::memory_order::memory_order_seq_cst);
        chunk = m_source_cu->get_chunk(m_reader_chunk_id);
        if (!chunk) return HA_ERR_END_OF_FILE;
        m_reader_pos.store(chunk->get_base(), std::memory_order_acq_rel);
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
      memcpy(buffer, m_reader_pos, SHANNON_ROW_TOTAL_LEN);
      m_reader_pos.fetch_add(SHANNON_ROW_TOTAL_LEN, std::memory_order_acq_rel); //go to the next.
      return 0;
    #endif
  }
  return HA_ERR_END_OF_FILE;
}
int CuView::write(ShannonBaseContext* context, uchar*buffer, size_t length) {
  ut_a(context && buffer);
  uchar* pos{nullptr};
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  auto chunk_ptr = m_source_cu->get_chunk(m_writter_chunk_id);
  auto header = m_source_cu->get_header();
  if (!(pos = chunk_ptr->write_data_direct(rpd_context, buffer, length))) {
    //the prev chunk is full, then allocate a new chunk to write.
    ut_ad(m_source_field);
    auto new_chunk = std::make_unique<Imcs::Chunk>(m_source_field);
    pos = new_chunk->write_data_direct(rpd_context, buffer, length);
    if (pos){
      m_source_cu->add_chunk(new_chunk);
      m_writter_chunk_id.fetch_add(1, std::memory_order_acq_rel);

      chunk_ptr->get_header().m_next_chunk = m_source_cu->get_last_chunk();
      m_source_cu->get_last_chunk()->get_header().m_prev_chunk = chunk_ptr;
      m_writter_pos.store(m_source_cu->get_last_chunk()->get_data(), std::memory_order_acq_rel);
    } else return HA_ERR_GENERIC;
    //To update the metainfo.
  }
  //update the meta info.
  if (header->m_cu_type == MYSQL_TYPE_BLOB || header->m_cu_type == MYSQL_TYPE_STRING ||
      header->m_cu_type == MYSQL_TYPE_VARCHAR) { //string type, otherwise, update the meta info.
      return 0;
  }
  uint8 data_offset = SHANNON_INFO_BYTE_LEN + SHANNON_TRX_ID_BYTE_LEN + SHANNON_ROWID_BYTE_LEN;
        data_offset += SHANNON_SUMPTR_BYTE_LEN;
  double data_val = *(double*) (buffer + data_offset);
  header->m_rows++;
  header->m_sum += data_val;
  header->m_avg.store(header->m_sum/header->m_rows, std::memory_order::memory_order_relaxed);
  if (data_val > header->m_max)
    header->m_max.store(data_val, std::memory_order::memory_order_relaxed);
  if (data_val < header->m_min)
    header->m_min.store(data_val, std::memory_order::memory_order_relaxed);
  return 0;
}
ImcsReader::ImcsReader(TABLE* table) :
                      m_source_table(table),
                      m_db_name(table->s->db.str),
                      m_table_name(table->s->table_name.str) {
  m_rows_read = 0;
  m_start_of_scan = false;
  for (uint idx =0; idx < m_source_table->s->fields; idx ++) {
    std::string key = m_db_name + m_table_name;
    Field* field_ptr = *(m_source_table->field + idx);
    // Skip columns marked as NOT SECONDARY.
    if ((field_ptr)->is_flag_set(NOT_SECONDARY_FLAG)) continue;
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
    // Skip columns marked as NOT SECONDARY.
    if ((field_ptr)->is_flag_set(NOT_SECONDARY_FLAG)) continue;

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
  ut_a(context && buffer);
  if (!m_start_of_scan) return HA_ERR_GENERIC;
  RapidContext* rpd_context = dynamic_cast<RapidContext*>(context);
  /** before insertion, should to check whether there's spare space to store the new data.
      or not. If no extra sapce left, allocate a new imcu. After a new imcu allocated, the
      meta info is stored into 'm_imcus'.
  */
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field = *(m_source_table->field + idx);
    ut_a(field);
    // Skip columns marked as NOT SECONDARY.
    if ((field)->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    std::string key_name = field->table->s->db.str;
    key_name += *field->table_name;
    key_name += field->field_name;
    if (m_cu_views.find(key_name) == m_cu_views.end()) return HA_ERR_END_OF_FILE;

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
    memcpy(data.get() + offset, &info, SHANNON_INFO_BYTE_LEN);
    offset += SHANNON_INFO_BYTE_LEN;
    memcpy(data.get() + offset, &rpd_context->m_extra_info.m_trxid, SHANNON_TRX_ID_BYTE_LEN);
    offset += SHANNON_TRX_ID_BYTE_LEN;
    memcpy(data.get() + offset, &rpd_context->m_extra_info.m_pk, SHANNON_ROWID_BYTE_LEN);
    offset += SHANNON_ROWID_BYTE_LEN;
    memcpy(data.get() + offset, &sum_ptr, SHANNON_SUMPTR_BYTE_LEN);
    offset += SHANNON_SUMPTR_BYTE_LEN;
    double data_val {0};
    if (!field->is_real_null()) {//not null
      switch (field->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VARCHAR: {
          String buf;
          buf.set_charset(field->charset());
          field->val_str(&buf);
          Compress::Dictionary* dict = m_cu_views[key_name].get()->get_source()->local_dictionary();
          ut_ad(dict);
          data_val = dict->store(buf);
        } break;
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
          data_val = field->val_real();
        }break;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
          my_decimal dval;
          field->val_decimal(&dval);
          my_decimal2double(10, &dval, &data_val);
        } break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME: {
          data_val = field->val_real();
        } break;
        default: data_val = field->val_real();
      }
      memcpy(data.get() + offset, &data_val, SHANNON_DATA_BYTE_LEN);
    }
    auto err = m_cu_views[key_name].get()->write(context, data.get(), data_len);
    if (err) return err;
  }
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