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

#include "sql/field.h"  //Field
#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/imcs/index/index.h"  //index
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu(Field *field) {
  init_header_info(field);
  init_body_info(field);
}

Cu::~Cu() { m_chunks.clear(); }

bool Cu::init_header_info(const Field *field) {
  std::scoped_lock lk(m_header_mutex);

  m_header = std::make_unique<Cu_header>();
  if (!m_header.get()) return true;

  m_header->m_field_no = field->field_index();
  m_header->m_rows = m_header->m_deleted_mark = 0;
  m_header->m_sum = 0;
  m_header->m_avg = 0;

  m_header->m_max = std::numeric_limits<double>::lowest();
  m_header->m_min = std::numeric_limits<double>::max();
  m_header->m_middle = std::numeric_limits<double>::lowest();
  m_header->m_median = std::numeric_limits<double>::lowest();

  m_header->m_cu_type = field->type();
  m_header->m_charset = field->charset();
  m_header->m_nullable = field->is_real_null();

  m_header->m_smu = std::make_unique<Snapshot_meta_unit>();

  std::string comment(field->comment.str);
  std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
  const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
  std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

  if (std::regex_search(comment.c_str(), column_encoding_patt)) {
    if (comment.find("SORTED") != std::string::npos)
      m_header->m_encoding_type = Compress::Encoding_type::SORTED;
    else if (comment.find("VARLEN") != std::string::npos)
      m_header->m_encoding_type = Compress::Encoding_type::VARLEN;
  } else
    m_header->m_encoding_type = Compress::Encoding_type::NONE;

  m_header->m_local_dict = std::make_unique<Compress::Dictionary>(m_header->m_encoding_type);
  return false;
}

bool Cu::init_body_info(const Field *field) {
  // the initial one chunk built.
  m_chunks.push_back(std::make_unique<Chunk>(const_cast<Field *>(field)));

  std::string comment(field->comment.str);
  std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
  if ((comment.find("INDEXED") != std::string::npos))
    m_index = std::make_unique<Index>(Index::IndexType::ART);
  else
    m_index = std::make_unique<Index>(Index::IndexType::ART);
  return false;
}

bool Cu::update_statistics(double old_v, double new_v, OPER_TYPE type) {
  std::scoped_lock lk(m_header_mutex);
  if (!m_header.get()) return false;

  // update the meta info.
  if (m_header->m_cu_type == MYSQL_TYPE_BLOB || m_header->m_cu_type == MYSQL_TYPE_STRING ||
      m_header->m_cu_type == MYSQL_TYPE_VARCHAR) {  // string type, otherwise,
    return true;
  }

  switch (type) {
    case OPER_TYPE::OPER_INSERT: {
      m_header->m_rows.fetch_add(1, std::memory_order_seq_cst);
      m_header->m_sum = m_header->m_sum + new_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;

      if (is_less_than(m_header->m_max, new_v)) m_header->m_max.store(new_v, std::memory_order::memory_order_relaxed);
      if (is_greater_than(m_header->m_min, new_v))
        m_header->m_min.store(new_v, std::memory_order::memory_order_relaxed);
    } break;

    case OPER_TYPE::OPER_UPDATE: {
      m_header->m_sum = m_header->m_sum - old_v + new_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;

      double value = is_greater_than_or_eq(old_v, new_v) ? old_v : new_v;
      if (is_less_than_or_eq(m_header->m_max, value))
        m_header->m_max.store(value, std::memory_order::memory_order_relaxed);
      if (is_greater_than_or_eq(m_header->m_min, value))
        m_header->m_min.store(value, std::memory_order::memory_order_relaxed);
    } break;

    case OPER_TYPE::OPER_DELETE: {
      m_header->m_rows.fetch_sub(1, std::memory_order_seq_cst);
      m_header->m_deleted_mark.fetch_sub(1, std::memory_order_seq_cst);
      m_header->m_sum = m_header->m_sum - old_v;
      m_header->m_avg = m_header->m_sum / m_header->m_rows;
    } break;
  }

  return true;
}

bool Cu::reset_statistics() {
  std::scoped_lock lk(m_header_mutex);
  if (!m_header.get()) return true;

  m_header->m_field_no = 0;
  m_header->m_rows = 0;
  m_header->m_sum = m_header->m_avg = 0;

  m_header->m_max = std::numeric_limits<double>::lowest();
  m_header->m_min = std::numeric_limits<double>::max();
  m_header->m_middle = std::numeric_limits<double>::lowest();
  m_header->m_median = std::numeric_limits<double>::lowest();
  return true;
}

uchar *Cu::get_base() {
  DBUG_TRACE;
  if (!m_chunks.size()) return nullptr;
  return m_chunks[0].get()->get_base();
}

void Cu::add_chunk(std::unique_ptr<Chunk> &chunk) {
  DBUG_TRACE;
  m_chunks.push_back(std::move(chunk));
}

uint Cu::rnd_init(bool scan) {
  DBUG_TRACE;
  if (!m_chunks.size()) return 0;
  for (size_t index = 0; index < m_chunks.size(); index++) {
    if (m_chunks[index].get()->rnd_init(scan)) return HA_ERR_INITIALIZATION;
  }
  m_current_chunk_id.store(0, std::memory_order::memory_order_relaxed);
  return 0;
}

uint Cu::rnd_end() {
  DBUG_TRACE;
  if (!m_chunks.size()) return 0;
  for (size_t index = 0; index < m_chunks.size(); index++) {
    if (m_chunks[index].get()->rnd_end()) return HA_ERR_INITIALIZATION;
  }
  return 0;
}

uchar *Cu::write_data_direct(ShannonBase::RapidContext *context, const uchar *pos, const uchar *data, uint length) {
  return nullptr;
}

uchar *Cu::write_data_direct(ShannonBase::RapidContext *context, const uchar *data, uint length) {
  DBUG_TRACE;
  ut_ad(m_header.get() && m_chunks.size() && data);

  auto index_pos = m_index->lookup(reinterpret_cast<uchar *>(context->m_extra_info.m_key_buff.get()),
                                   context->m_extra_info.m_key_len);
  if (index_pos) {  // found, return now.
    return 0;
  }

  // the last available chunk.
  Chunk *chunk_ptr = m_chunks[m_chunks.size() - 1].get();
  uchar *pos = chunk_ptr->write_data_direct(context, data, length);
  if (unlikely(!pos)) {
    // the prev chunk is full, then allocate a new chunk to write.
    std::scoped_lock lk(m_header_mutex);
    Field *field = *(context->m_table->field + m_header->m_field_no);
    ut_ad(field);

    m_chunks.push_back(std::make_unique<Chunk>(field));
    chunk_ptr->get_header()->m_next_chunk = m_chunks[m_chunks.size() - 1].get();
    m_chunks[m_chunks.size() - 1].get()->get_header()->m_prev_chunk = chunk_ptr;
    chunk_ptr = m_chunks[m_chunks.size() - 1].get();

    pos = chunk_ptr->write_data_direct(context, data, length);
    ut_a(pos);
  }

  if (unlikely(m_index->insert(reinterpret_cast<uchar *>(context->m_extra_info.m_key_buff.get()),
                               context->m_extra_info.m_key_len, pos)))
    return nullptr;

  double data_val{0};
  if (!m_header->m_nullable)
    data_val = *reinterpret_cast<double *>(const_cast<uchar *>(data) + SHANNON_DATA_BYTE_OFFSET);

  // update the meta info.
  update_statistics(data_val, data_val, OPER_TYPE::OPER_INSERT);
  return pos;
}

uchar *Cu::read_data_direct(ShannonBase::RapidContext *context, uchar *buff) {
  DBUG_TRACE;
  if (!m_chunks.size()) return nullptr;
#ifdef SHANNON_GET_NTH_CHUNK
  // Gets the last chunk data.
  m_current_chunk_id = m_chunks.size() - 1;
  Chunk *chunk = m_chunks[m_current_chunk_id].get();
  if (!chunk) return nullptr;
  return chunk->Read_data(context, buffer);
#else
  if (m_current_chunk_id >= m_chunks.size()) return nullptr;

  Chunk *chunk = m_chunks[m_current_chunk_id].get();
  if (unlikely(!chunk)) return nullptr;

  auto ret = chunk->read_data_direct(context, buff);
  // to the end of this chunk, then start to read the next chunk.
  if (unlikely(!ret)) {
    m_current_chunk_id.fetch_add(1, std::memory_order::memory_order_seq_cst);
    if (m_current_chunk_id >= m_chunks.size()) return nullptr;

    chunk = m_chunks[m_current_chunk_id].get();
    if (unlikely(!chunk)) return nullptr;

    ret = chunk->read_data_direct(context, buff);
  }
  return ret;
#endif
}

uchar *Cu::seek(ShannonBase::RapidContext *context, size_t offset) {
  auto chunk_id = (offset / SHANNON_ROWS_IN_CHUNK);
  auto offset_in_chunk = offset % SHANNON_ROWS_IN_CHUNK;

  return m_chunks[chunk_id]->seek(context, offset_in_chunk);
}

uchar *Cu::lookup(ShannonBase::RapidContext *context, const uchar *pk, uint pk_len) {
  if (!m_index || !pk || !pk_len) return nullptr;

  return reinterpret_cast<uchar *>(m_index->lookup(const_cast<uchar *>(pk), pk_len));
}

uchar *Cu::read_data_direct(ShannonBase::RapidContext *context, const uchar *rowid, uchar *buffer) {
  if (!m_chunks.size()) return nullptr;
  // Chunk* chunk = m_chunks [m_chunks.size() - 1].get();
  // chunk data. if (!chunk) return nullptr;
  ut_a(false);
  return nullptr;
}

uchar *Cu::delete_data_direct(ShannonBase::RapidContext *context, const uchar *rowid) {
  ut_a(false);
  return nullptr;
}

uchar *Cu::delete_data_direct(ShannonBase::RapidContext *context, const uchar *pk, uint pk_len) {
  uchar *data_pos{nullptr};
  if (!pk && !pk_len) {  // delete all
    data_pos = delete_all_direct();
  } else {
    data_pos = reinterpret_cast<uchar *>(m_index->lookup(const_cast<uchar *>(pk), pk_len));
    if (!data_pos) return nullptr;

    uint8 info = *reinterpret_cast<uint8 *>(data_pos + SHANNON_INFO_BYTE_OFFSET);
    info |= DATA_DELETE_FLAG_MASK;
    *reinterpret_cast<uint8 *>(data_pos + SHANNON_INFO_BYTE_OFFSET) = info;
    *reinterpret_cast<uint64 *>(data_pos + SHANNON_TRX_ID_BYTE_OFFSET) = context->m_extra_info.m_trxid;
    auto data_val = *reinterpret_cast<double *>(data_pos + SHANNON_DATA_BYTE_OFFSET);
    update_statistics(data_val, data_val, OPER_TYPE::OPER_DELETE);
  }

  return data_pos;
}

uchar *Cu::delete_all_direct() {
  ut_a(m_chunks.size() >= 1);

  if (m_chunks.size() > 1) {
    m_chunks.erase(m_chunks.begin() + 1, m_chunks.end());
  }

  m_chunks[0]->set_empty();
  reset_statistics();
  return (uchar *)m_header.get();
}

uchar *Cu::update_data_direct(ShannonBase::RapidContext *context, const uchar *rowid, const uchar *data, uint length) {
  const uchar *data_pos{nullptr};
  if (rowid)
    data_pos = rowid;
  else
    data_pos = (uchar *)m_index->lookup(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len);

  if (!data_pos) return nullptr;
  // in future, the old data will be added to version link.
  memcpy(const_cast<uchar *>(data_pos), data, length);

  return const_cast<uchar *>(data_pos);
}

uint Cu::flush_direct(ShannonBase::RapidContext *context, const uchar *from, const uchar *to) {
  assert(from || to);
  return 0;
}

uchar *Cu::GC(ShannonBase::RapidContext *context) {
  // step 1: all chunks do GC, to rebuild its index.
  if (m_chunks.empty()) return nullptr;
  for (auto index = 0u; index < m_chunks.size(); index++) {
    m_chunks[index]->GC(context);
  }

  // step 2: merge all the chunks.
  for (auto index = 1u; index < m_chunks.size();) {
    auto mv_size = m_chunks[index - 1]->free_size();
    // the next data size is larger than pre free space.
    if (m_chunks[index]->data_size() > mv_size) {
      std::memcpy(m_chunks[index - 1]->get_data(), m_chunks[index]->get_base(), mv_size);
      m_chunks[index - 1]->set_full();
      m_chunks[index]->reshift(context, m_chunks[index]->get_base(), m_chunks[index]->get_base() + mv_size);
      index++;
    } else {
      mv_size = m_chunks[index]->data_size();
      std::memcpy(m_chunks[index - 1]->get_data(), m_chunks[index]->get_base(), mv_size);
      m_chunks[index]->reshift(context, m_chunks[index]->get_base(), m_chunks[index]->get_base() + mv_size);
      if (m_chunks[index]->is_empty()) {
        m_chunks[index].reset(nullptr);
        m_chunks.erase(m_chunks.begin() + index - 1);
      } else
        index++;
    }
  }
  // step 3: rebuild the index.

  return nullptr;
}

}  // namespace Imcs
}  // namespace ShannonBase