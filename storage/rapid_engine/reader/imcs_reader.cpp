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
#include "include/my_base.h"
#include "sql/table.h" //TABLE
#include "sql/field.h" //Field
#include "sql/my_decimal.h"
#include "storage/innobase/include/trx0trx.h"    //read_view
#include "storage/innobase/include/dict0mem.h"   //table_name_t
#include "storage/innobase/include/read0types.h" //ReadView

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/utils/utils.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/chunk.h"
namespace ShannonBase {

CuView::CuView(TABLE* table, Field* field) : m_source_table(table), m_source_field(field){
  ut_a(!m_source_field->is_flag_set(NOT_SECONDARY_FLAG));
  m_key_name = m_source_table->s->db.str;
  m_key_name+= m_source_table->s->table_name.str;
  m_key_name+= m_source_field->field_name;
  m_source_cu = Imcs::Imcs::get_instance()->get_cu(m_key_name);
  if (!m_source_cu) {
    std::unique_ptr<Imcs::Cu> new_cu = std::make_unique<Imcs::Cu>(field);
    Imcs::Imcs::get_instance()->add_cu(m_key_name, new_cu);
  }
  m_source_cu = Imcs::Imcs::get_instance()->get_cu(m_key_name);
  ut_a(m_source_cu);
}

int CuView::open(){
  DBUG_TRACE;
  auto chunk = m_source_cu->get_chunk(m_reader_chunk_id);
  ut_a(chunk);
  m_reader_pos = chunk->get_base();
  m_reader_chunk_id = 0;

  m_writter_chunk_id = m_source_cu->get_chunk_nums() ? m_source_cu->get_chunk_nums() -1: 0;
  m_writter_pos = m_source_cu->get_chunk(m_writter_chunk_id)->get_data();
  return 0;
}

int CuView::close(){
  DBUG_TRACE;
  m_reader_chunk_id = 0;
  m_reader_pos = nullptr;

  m_writter_chunk_id = 0;
  m_writter_pos = nullptr;
  return 0;
}

/**
 * this function is similiar to Chunk::Read_data, you should change these funcs in sync.
*/
int CuView::read(ShannonBaseContext* context, uchar* buffer, size_t length){
  DBUG_TRACE;
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

    uint8 info = *((uint8*)(m_reader_pos + SHANNON_INFO_BYTE_OFFSET));   //info byte
    uint64 trxid = *((uint64*)(m_reader_pos + SHANNON_TRX_ID_BYTE_OFFSET)); //trxid bytes
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
      memcpy(m_row_buff,  &data, SHANNON_DATA_BYTE_LEN);
      return 0;
    #else
      memcpy(m_rec_buff, m_reader_pos, SHANNON_ROW_TOTAL_LEN);
      memcpy(buffer, m_rec_buff, SHANNON_ROW_TOTAL_LEN);
      m_reader_pos.fetch_add(SHANNON_ROW_TOTAL_LEN, std::memory_order_acq_rel); //go to the next.
      return 0;
    #endif
  }
  return HA_ERR_END_OF_FILE;
}

int CuView::get(ShannonBaseContext* context, uchar* buffer, size_t length) {
  memcpy(buffer, m_rec_buff, SHANNON_ROW_TOTAL_LEN);
  return 0;
}

int CuView::records_in_range(ShannonBaseContext* context, unsigned int index, key_range* min_key,
                              key_range *max_key) {
  Imcs::Cu* cu = get_source();
  ut_a(cu);
  double start{0}, end{0};
  Utils::Util::get_range_value(cu->get_header()->m_cu_type, min_key, max_key, start, end);

  ha_rows rows{0};
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  for(size_t idx = 0; idx < cu->get_chunk_nums(); idx ++) {
    auto chunk_max = cu->get_chunk(idx)->get_header()->m_max.load(std::memory_order::memory_order_relaxed);
    auto chunk_min = cu->get_chunk(idx)->get_header()->m_min.load(std::memory_order::memory_order_relaxed);
    //not in [start, end] to next chunk, we dont case flag here.
    if (min_key && is_less_than (chunk_max, start))
      continue;
    else if (max_key && is_greater_than(chunk_min, end))
      continue;
    else
      rows += cu->get_chunk(idx)->records_in_range(rpd_context, start, end);
  }

  return rows;
}

int CuView::write(ShannonBaseContext* context, uchar*buffer, size_t length) {
  DBUG_TRACE;
  ut_a(context && buffer);
  uchar* pos{nullptr};
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  auto chunk_ptr = m_source_cu->get_chunk(m_writter_chunk_id);
  if (!(pos = chunk_ptr->write_data_direct(rpd_context, buffer, length))) {
    //the prev chunk is full, then allocate a new chunk to write.
    ut_ad(m_source_field);
    auto new_chunk = std::make_unique<Imcs::Chunk>(m_source_field);
    pos = new_chunk->write_data_direct(rpd_context, buffer, length);
    if (pos) {
      m_source_cu->add_chunk(new_chunk);
      m_writter_chunk_id.fetch_add(1, std::memory_order_acq_rel);

      chunk_ptr->get_header()->m_next_chunk = m_source_cu->get_last_chunk();
      m_source_cu->get_last_chunk()->get_header()->m_prev_chunk = chunk_ptr;
      m_writter_pos.store(m_source_cu->get_last_chunk()->get_data(), std::memory_order_acq_rel);
    } else return HA_ERR_GENERIC;
    //To update the metainfo.
  }
  //update the meta info.
  auto header = m_source_cu->get_header();
  if (header->m_cu_type == MYSQL_TYPE_BLOB || header->m_cu_type == MYSQL_TYPE_STRING ||
      header->m_cu_type == MYSQL_TYPE_VARCHAR) { //string type, otherwise, update the meta info.
      return 0;
  }

  double data_val = *(double*) (buffer + SHANNON_DATA_BYTE_OFFSET);
  header->m_rows++;
  header->m_sum = header->m_sum + data_val;
  header->m_avg.store(header->m_sum/header->m_rows, std::memory_order::memory_order_relaxed);
  if (is_greater_than(data_val,header->m_max))
    header->m_max.store(data_val, std::memory_order::memory_order_relaxed);
  if (is_less_than(data_val,header->m_min))
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
    ut_a(field_ptr);
    // Skip columns marked as NOT SECONDARY.
    if (!bitmap_is_set(m_source_table->read_set, field_ptr->field_index()) ||
       field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    key += field_ptr->field_name;
    m_cu_views.emplace(key, std::make_unique<CuView>(table, field_ptr));
  }
}

int ImcsReader::open() {
  DBUG_TRACE;
  for(auto& item : m_cu_views) {
    item.second->open();
  }
  m_start_of_scan = true;
  return 0;
}

int ImcsReader::close() {
  DBUG_TRACE;
  for(auto& item : m_cu_views) {
    item.second->close();
  }
  m_start_of_scan = false;  
  return 0;
}

int ImcsReader::read(ShannonBaseContext* context, uchar* buffer, size_t length) {
  DBUG_TRACE;
  ut_a(context && buffer);
  if (!m_start_of_scan) return HA_ERR_GENERIC;

  std::string key_part1 = m_db_name + m_table_name;
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field_ptr = *(m_source_table->field + idx);
    ut_a(field_ptr);
    // Skip columns marked as NOT SECONDARY.
    if (!bitmap_is_set(m_source_table->read_set, field_ptr->field_index()) ||
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    std::string key(key_part1 + field_ptr->field_name);
    if (auto ret = m_cu_views[key].get()->read(context, m_buff)) return ret;
    #ifdef SHANNON_ONLY_DATA_FETCH
      double data = *(double*) m_buff;
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (field_ptr->type() == MYSQL_TYPE_BLOB || field_ptr->type() == MYSQL_TYPE_STRING
          || field_ptr->type() == MYSQL_TYPE_VARCHAR) { //read the data
        String str;
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        ut_ad(dict);
        dict->get(data, str, *const_cast<CHARSET_INFO*>(field_ptr->charset()));
        field_ptr->store(str.c_ptr(), str.length(), &my_charset_bin);
      } else {
        field_ptr->store (data);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #else
      uint8 info = *(uint8*) m_buff;
      if (info & DATA_DELETE_FLAG_MASK) continue; //deleted rows.
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (info & DATA_NULL_FLAG_MASK)
        field_ptr->set_null();
      else {
        field_ptr->set_notnull();
        double val = *(double*) (m_buff + SHANNON_DATA_BYTE_OFFSET);
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        Utils::Util::store_field_value(m_source_table, field_ptr, dict, val);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #endif
  }
  return 0;
}

int ImcsReader::records_in_range(ShannonBaseContext* context, unsigned int index, key_range * min_key,
                                 key_range * max_key) {
  ut_a (context);
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  TABLE* table = rpd_context->m_table;
  ut_a(table);

  uint16 field_nr{0};
  KEY_PART_INFO* key_inf = (m_source_table->s->key_info + index)->key_part;
  if (key_inf) //here only the first keypart needed. total:m_source_table->s->key_parts
   field_nr = key_inf->field->field_index();

  Field* field_ptr {nullptr};
  std::string key = rpd_context->m_current_db + rpd_context->m_current_table;
  if (table && field_nr < table->s->fields) {
   field_ptr = *(table->s->field + field_nr);
   key += field_ptr->field_name;
  }
  if (m_cu_views.find(key) == m_cu_views.end()) return 0;
  CuView* cu_view = m_cu_views[key].get();
  if (cu_view)
    return cu_view->records_in_range(context, index, min_key, max_key);

  return HA_ERR_END_OF_FILE;
}

int  ImcsReader::get(ShannonBaseContext* context, uchar* buff, size_t length) {
  DBUG_TRACE;
  ut_a(context && buff);
  if (!m_start_of_scan) return HA_ERR_GENERIC;

  std::string key_part1 = m_db_name + m_table_name;
  size_t pos {0}, field_cnt{0};
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field_ptr = *(m_source_table->field + idx);
    ut_a(field_ptr);
    // Skip columns marked as NOT SECONDARY.
    if (!bitmap_is_set(m_source_table->read_set, field_ptr->field_index()) ||
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    std::string key(key_part1 + field_ptr->field_name);
    if (auto ret = m_cu_views[key].get()->get(context, m_buff)) return ret;
    memcpy(buff + pos, m_buff, SHANNON_ROW_TOTAL_LEN);
    pos+= SHANNON_ROW_TOTAL_LEN;
    field_cnt ++;
  }
  ut_a(field_cnt == m_cu_views.size());
  return 0;
}

bool ImcsReader::is_satisfied(ShannonBaseContext* context,double key) {
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  ut_a(rpd_context);

  if (const_cast<ha_rapid*>(rpd_context->m_handler)->is_push_down()) {
    ICP_RESULT result;
    Field* field_ptr  = *(rpd_context->m_table->field + rpd_context->m_extra_info.m_keynr);
    ut_a(field_ptr);
    std::string key_name(rpd_context->m_current_db + rpd_context->m_current_table);
    key_name+= field_ptr->field_name;
    Compress::Dictionary* dict = m_cu_views[key_name].get()->get_source()->local_dictionary();
    Utils::Util::store_field_value(rpd_context->m_table, field_ptr, dict, key);

    result = shannon_rapid_index_cond(
      const_cast<ha_rapid*>(rpd_context->m_handler));
    switch (result) {
      case ICP_MATCH:
        /* Convert the remaining fields to MySQL format.
        If this is a secondary index record, we must defer
        this until we have fetched the clustered index record. */
        return true;
      case ICP_NO_MATCH:
        return false;
      case ICP_OUT_OF_RANGE:
        return false;
    }
  } else {
    if (rpd_context->m_extra_info.m_find_flag ==  HA_READ_KEY_EXACT &&
        are_equal(key, rpd_context->m_extra_info.m_min_key_val))
      return true;
  }
  /*
  if(find_flag == HA_READ_KEY_EXACT && are_equal(key, min_key))                //=
    return true;
  */
  return false;
}

int ImcsReader::index_read(ShannonBaseContext* context, uchar*buff, uchar* key,
                           uint key_len, ha_rkey_function find_flag) {
  ut_a(context);
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  TABLE* table = rpd_context->m_table;
  ut_a(table);

  size_t key_pos = rpd_context->m_extra_info.m_keynr * SHANNON_ROW_TOTAL_LEN;
  std::unique_ptr<uchar[]> buf (new uchar[m_cu_views.size()*SHANNON_ROW_TOTAL_LEN]);
  int ret {0};
  while (!(ret = read(context, buff))) {
    get(context, buf.get());
    double data = *(double*) (buf.get() + key_pos + SHANNON_DATA_BYTE_OFFSET);
    if (is_satisfied(rpd_context, data))
      break;
  }
  if (ret) return ret;

  std::string key_part1 = m_db_name + m_table_name;
  key_pos = 0;
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field_ptr = *(m_source_table->field + idx);
    ut_a(field_ptr);
    // Skip columns marked as NOT SECONDARY.
    if (!bitmap_is_set(m_source_table->read_set, field_ptr->field_index()) ||
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    std::string key(key_part1 + field_ptr->field_name);
    memcpy(m_buff, buf.get() + key_pos, SHANNON_ROW_TOTAL_LEN);
    #ifdef SHANNON_ONLY_DATA_FETCH
      double data = *(double*) m_buff;
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (field_ptr->type() == MYSQL_TYPE_BLOB || field_ptr->type() == MYSQL_TYPE_STRING
          || field_ptr->type() == MYSQL_TYPE_VARCHAR) { //read the data
        String str;
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        ut_ad(dict);
        dict->get(data, str, *const_cast<CHARSET_INFO*>(field_ptr->charset()));
        field_ptr->store(str.c_ptr(), str.length(), &my_charset_bin);
      } else {
        field_ptr->store (data);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #else
      uint8 info = *(uint8*) m_buff;
      if (info & DATA_DELETE_FLAG_MASK) continue; //deleted rows.
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (info & DATA_NULL_FLAG_MASK)
        field_ptr->set_null();
      else {
        field_ptr->set_notnull();
        double val = *(double*) (m_buff + SHANNON_DATA_BYTE_OFFSET);
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        Utils::Util::store_field_value(m_source_table, field_ptr, dict, val);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #endif
    key_pos += SHANNON_ROW_TOTAL_LEN;
  }

  return 0;
}

int ImcsReader::index_next(ShannonBaseContext* context, uchar*buffer, size_t length) {
  assert(false);
  return 0;
}

int ImcsReader::index_general(ShannonBaseContext* context, uchar* buffer, size_t length) {
  ut_a(context);
  RapidContext* rpd_context = dynamic_cast<RapidContext*> (context);
  TABLE* table = rpd_context->m_table;
  ut_a(table);

  size_t key_pos = rpd_context->m_extra_info.m_keynr * SHANNON_ROW_TOTAL_LEN;
  std::unique_ptr<uchar[]> buf (new uchar[m_cu_views.size()*SHANNON_ROW_TOTAL_LEN]);
  int ret {0};
  while (!(ret = read(context, buffer))) {
    get(context, buf.get());
    double data = *(double*) (buf.get() + key_pos + SHANNON_DATA_BYTE_OFFSET);
    if (is_satisfied(rpd_context, data))
      break;
  }
  if (ret) return ret;

  std::string key_part1 = m_db_name + m_table_name;
  key_pos = 0;
  for (uint idx =0; idx < m_source_table->s->fields; idx++) {
    Field* field_ptr = *(m_source_table->field + idx);
    ut_a(field_ptr);
    // Skip columns marked as NOT SECONDARY.
    if (!bitmap_is_set(m_source_table->read_set, field_ptr->field_index()) ||
        field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    std::string key(key_part1 + field_ptr->field_name);
    memcpy(m_buff, buf.get() + key_pos, SHANNON_ROW_TOTAL_LEN);
    #ifdef SHANNON_ONLY_DATA_FETCH
      double data = *(double*) m_buff;
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (field_ptr->type() == MYSQL_TYPE_BLOB || field_ptr->type() == MYSQL_TYPE_STRING
          || field_ptr->type() == MYSQL_TYPE_VARCHAR) { //read the data
        String str;
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        ut_ad(dict);
        dict->get(data, str, *const_cast<CHARSET_INFO*>(field_ptr->charset()));
        field_ptr->store(str.c_ptr(), str.length(), &my_charset_bin);
      } else {
        field_ptr->store (data);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #else
      uint8 info = *(uint8*) m_buff;
      if (info & DATA_DELETE_FLAG_MASK) continue; //deleted rows.
      my_bitmap_map *old_map = tmp_use_all_columns(m_source_table, m_source_table->write_set);
      if (info & DATA_NULL_FLAG_MASK)
        field_ptr->set_null();
      else {
        field_ptr->set_notnull();
        double val = *(double*) (m_buff + SHANNON_DATA_BYTE_OFFSET);
        Compress::Dictionary* dict = m_cu_views[key].get()->get_source()->local_dictionary();
        Utils::Util::store_field_value(m_source_table, field_ptr, dict, val);
      }
      if (old_map) tmp_restore_column_map(m_source_table->write_set, old_map);
    #endif
    key_pos += SHANNON_ROW_TOTAL_LEN;
  }

  return 0;
}

int ImcsReader::write(ShannonBaseContext* context, uchar*buffer, size_t length) {
  DBUG_TRACE;
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
    //start to pack the data, then writes into memory.
    std::unique_ptr<uchar[]> data (new uchar[SHANNON_ROW_TOTAL_LEN]);
    int8 info {0};
    int64 sum_ptr{0};
    if (field->is_real_null())
       info |= DATA_NULL_FLAG_MASK;
    //write info byte
    memcpy(data.get() + SHANNON_INFO_BYTE_OFFSET, &info, SHANNON_INFO_BYTE_LEN);
    //write trxid
    memcpy(data.get() + SHANNON_TRX_ID_BYTE_OFFSET, &rpd_context->m_extra_info.m_trxid,
           SHANNON_TRX_ID_BYTE_LEN);
    //write rowid
    memcpy(data.get() + SHANNON_ROW_ID_BYTE_OFFSET, &rpd_context->m_extra_info.m_pk,
           SHANNON_ROWID_BYTE_LEN);
    //write sumptr
    memcpy(data.get() + SHANNON_SUMPTR_BYTE_OFFSET, &sum_ptr, SHANNON_SUMPTR_BYTE_LEN);
    double data_val {0};
    if (!field->is_real_null()) {//not null
      Compress::Dictionary* dict = m_cu_views[key_name].get()->get_source()->local_dictionary();
      data_val = Utils::Util::get_field_value(field, dict);
      //write data
      memcpy(data.get() + SHANNON_DATA_BYTE_OFFSET, &data_val, SHANNON_DATA_BYTE_LEN);
    }
    auto err = m_cu_views[key_name].get()->write(context, data.get(), SHANNON_ROW_TOTAL_LEN);
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