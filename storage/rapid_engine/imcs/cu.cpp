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
#include <limits.h>
#include "storage/rapid_engine/imcs/cu.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu(Field* field) {
  std::scoped_lock lk(m_header_mutex);
  m_header.reset (new (current_thd->mem_root) Cu_header());

  m_header->m_field = field;
  m_header->m_avg = m_header->m_sum = m_header->m_rows = 0;
  m_header->m_max = std::numeric_limits<long long>::lowest();
  m_header->m_min = std::numeric_limits<long long>::max();
  m_header->m_middle = std::numeric_limits<long long>::lowest();
  m_header->m_median = std::numeric_limits<long long>::lowest();

  m_header->m_cu_type = field->type();
  m_header->m_nullable = field->is_nullable();
  m_header->m_compress_algo = Compress::enum_compress_algos::NONE;
  m_header->m_local_dict.reset(new (current_thd->mem_root) Compress::Dictionary());
  //the initial one chunk built.
  m_chunks.push_back(std::make_unique<Chunk>(field));
}
Cu::~Cu() {
}

uchar* Cu::Write_data(RapidContext* context, uchar* data, uint length) {
  assert(m_header);
  uchar* pos{nullptr};
  if (!m_chunks.size()) { //empty.
    m_chunks.push_back(std::make_unique<Chunk>(m_header->m_field));
  }

  Chunk* chunk_ptr = m_chunks[m_chunks.size()-1].get();
  if (!(pos = chunk_ptr->Write_data(context, data, length))) { //the chunk is full.
    //allocate a new chunk to write.
    m_chunks.push_back(std::make_unique<Chunk>(m_header->m_field));
    chunk_ptr = m_chunks[m_chunks.size()-1].get();
    pos = chunk_ptr->Write_data(context, data, length);
    if (!pos) return nullptr;
    //To update the metainfo.
  }

  longlong data_val = m_header->m_field->val_real();
  if (m_header->m_cu_type == MYSQL_TYPE_BLOB || m_header->m_cu_type == MYSQL_TYPE_STRING) {
    String buf;
    String *val1[[maybe_unused]] = m_header->m_field->val_str(&buf);  
    m_header->m_local_dict->GetStringId(val1);
  }
  m_header->m_rows++;
  m_header->m_sum += data_val;
  m_header->m_avg.store(m_header->m_sum/m_header->m_rows, std::memory_order::memory_order_relaxed);

  if (data_val > m_header->m_max)
    m_header->m_max.store(data_val, std::memory_order::memory_order_relaxed);
  if (data_val < m_header->m_min)
    m_header->m_min.store(data_val, std::memory_order::memory_order_relaxed);

  return pos;
}

uchar* Cu::Read_data(RapidContext* context, uchar* from, uchar* to, uint length) {
  if (!m_chunks.size()) return nullptr;
  Chunk* chunk = m_chunks [m_chunks.size() - 1].get();
  return chunk->Read_data(context, from, length);
}

uchar* Cu::Read_data(RapidContext* context, uchar* rowid, uint length) {
  if (!m_chunks.size()) return nullptr;
  Chunk* chunk = m_chunks [m_chunks.size() - 1].get();
  return  chunk->Read_data(context, rowid, length);
}
uchar* Cu::Delete_data(RapidContext* context, uchar* rowid) {
  return nullptr;
}
uchar* Cu::Delete_all(){
  uchar* base{nullptr};
  for(size_t index = 0; index < m_chunks.size(); index++) {
    if (index == 0) base = m_chunks[index]->Get_base();
    m_chunks[index]->Delete_all();
  }
  return base;
}
uchar* Cu::Update_data(RapidContext* context, uchar* rowid, uchar* data, uint length){
  
  return nullptr;
}
uint Cu::flush(RapidContext* context, uchar* from, uchar* to) {
  assert(from ||to);

  return 0;
}

} // ns:Imcs
} // ns:ShannonBase
