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
#include "storage/rapid_engine/imcs/cu.h"

#include <limits.h>
#include <regex>

#include "sql/field.h"      //Field
#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/chunk.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu(Field* field) {
  std::scoped_lock lk(m_header_mutex);
  //m_header = ut::new_withkey<Cu_header>(UT_NEW_THIS_FILE_PSI_KEY);
  m_header = std::make_unique<Cu_header>();
  if (!m_header.get()) return;

  m_header->m_field_no = field->field_index();
  m_header->m_rows = 0;
  m_header->m_sum = 0;
  m_header->m_avg = 0;

  m_header->m_max = std::numeric_limits<double>::lowest();
  m_header->m_min = std::numeric_limits<double>::max();
  m_header->m_middle = std::numeric_limits<double>::lowest();
  m_header->m_median = std::numeric_limits<double>::lowest();

  m_header->m_cu_type = field->type();
  m_header->m_nullable = field->is_real_null();

  std::string comment (field->comment.str);
  std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
  const char* const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
  std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs |
                                                   std::regex_constants::icase);
  if (std::regex_search(comment.c_str(), column_encoding_patt)) {
    if (comment.find("SORTED") != std::string::npos)
      m_header->m_encoding_type = Compress::Encoding_type::SORTED;
    else if (comment.find ("VARLEN") != std::string::npos)
      m_header->m_encoding_type = Compress::Encoding_type::VARLEN;
  } else
    m_header->m_encoding_type = Compress::Encoding_type::NONE;
  m_header->m_local_dict = std::make_unique<Compress::Dictionary>(m_header->m_encoding_type);
  //the initial one chunk built.
  m_chunks.push_back(std::make_unique<Chunk>(field));
}

Cu::~Cu() {
  m_chunks.clear();
}

uchar* Cu::get_base() {
  DBUG_TRACE;
  if (!m_chunks.size()) return nullptr;
  return m_chunks[0].get()->get_base();
}

void Cu::add_chunk(std::unique_ptr<Chunk>& chunk) {
  DBUG_TRACE;
  m_chunks.push_back(std::move(chunk));
}

uint Cu::rnd_init(bool scan) {
  DBUG_TRACE;
  if (!m_chunks.size()) return 0;
  for (size_t index =0; index < m_chunks.size(); index++) {
    if (m_chunks[index].get()->rnd_init(scan))
      return HA_ERR_INITIALIZATION;
  }
  m_current_chunk_id.store(0, std::memory_order::memory_order_relaxed);
  return 0;
}

uint Cu::rnd_end() {
  DBUG_TRACE;
  if (!m_chunks.size()) return 0;
  for (size_t index =0; index < m_chunks.size(); index++) {
    if (m_chunks[index].get()->rnd_end())
      return HA_ERR_INITIALIZATION;
  }
  return 0;
}

uchar* Cu::write_data_direct(ShannonBase::RapidContext* context, uchar* data, uint length) {
  DBUG_TRACE;
  ut_ad(m_header.get() && m_chunks.size());
  uchar* pos{nullptr};
  Chunk* chunk_ptr = m_chunks[m_chunks.size()-1].get();
  if (!(pos = chunk_ptr->write_data_direct(context, data, length))) {
    //the prev chunk is full, then allocate a new chunk to write.
    std::scoped_lock lk(m_header_mutex);
    Field* field = *(context->m_table->field + m_header->m_field_no);
    ut_ad(field);
    m_chunks.push_back(std::make_unique<Chunk>(field));
    chunk_ptr->get_header()->m_next_chunk = m_chunks[m_chunks.size()-1].get();
    m_chunks[m_chunks.size()-1].get()->get_header()->m_prev_chunk = chunk_ptr;
    chunk_ptr = m_chunks[m_chunks.size()-1].get();
    pos = chunk_ptr->write_data_direct(context, data, length);
    //To update the metainfo.
  }
  //update the meta info.
  if (m_header->m_cu_type == MYSQL_TYPE_BLOB || m_header->m_cu_type == MYSQL_TYPE_STRING ||
      m_header->m_cu_type == MYSQL_TYPE_VARCHAR) { //string type, otherwise, update the meta info.
      return pos;
  }

  double data_val{0};
  if (m_header->m_nullable)
    data_val = *(double*) (data + SHANNON_DATA_BYTE_OFFSET);
  m_header->m_rows++;
  m_header->m_sum = m_header->m_sum + data_val;
  m_header->m_avg.store(m_header->m_sum/m_header->m_rows, std::memory_order::memory_order_relaxed);
  if (data_val > m_header->m_max)
    m_header->m_max.store(data_val, std::memory_order::memory_order_relaxed);
  if (data_val < m_header->m_min)
    m_header->m_min.store(data_val, std::memory_order::memory_order_relaxed);
  return pos;
}

uchar* Cu::read_data_direct(ShannonBase::RapidContext* context, uchar* buffer) {
  DBUG_TRACE;
  if (!m_chunks.size()) return nullptr;
#ifdef SHANNON_GET_NTH_CHUNK
  //Gets the last chunk data.
  m_current_chunk_id = m_chunks.size() - 1;
  Chunk* chunk = m_chunks [m_current_chunk_id].get();
  if (!chunk) return nullptr;
  return chunk->Read_data(context, buffer);
#else
  if (m_current_chunk_id >= m_chunks.size())  return nullptr;
  Chunk* chunk = m_chunks [m_current_chunk_id].get();
  if (!chunk) return nullptr;
  auto ret = chunk->read_data_direct(context, buffer);
  if (!ret) {//to the end of this chunk, then start to read the next chunk.
    m_current_chunk_id.fetch_add(1, std::memory_order::memory_order_seq_cst);
    if (m_current_chunk_id >= m_chunks.size()) return nullptr;
    chunk = m_chunks [m_current_chunk_id].get();
    if (!chunk) return nullptr;
    ret = chunk->read_data_direct(context, buffer);
  }
  return ret;
#endif
}

uchar* Cu::read_data_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* buffer) {
  if (!m_chunks.size()) return nullptr;
  //Chunk* chunk = m_chunks [m_chunks.size() - 1].get(); //to get the last chunk data.
  //if (!chunk) return nullptr;
  return  nullptr;
}

uchar* Cu::delete_data_direct(ShannonBase::RapidContext* context, uchar* rowid) {
  return nullptr;
}

uchar* Cu::delete_all_direct(){
  uchar* base{nullptr};
  for(size_t index = 0; index < m_chunks.size(); index++) {
    if (index == 0) base = m_chunks[index]->get_base();
    m_chunks[index]->delete_all_direct();
  }
  return base;
}

uchar* Cu::update_data_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* data, uint length){
  
  return nullptr;
}

uint Cu::flush_direct(ShannonBase::RapidContext* context, uchar* from, uchar* to) {
  assert(from ||to);
  return 0;
}

} // ns:Imcs
} // ns:ShannonBase
