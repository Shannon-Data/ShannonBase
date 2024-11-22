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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/cu.h"

#include <limits.h>
#include <regex>

#include "sql/field.h"  //Field
#include "storage/innobase/include/univ.i"
#include "storage/innobase/include/ut0new.h"

#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu(const Field *field) {
  ut_a(field && !field->is_flag_set(NOT_SECONDARY_FLAG));
  {
    std::scoped_lock lk(m_header_mutex);
    m_header = std::make_unique<Cu_header>();
    if (!m_header.get()) return;

    // TODO: be aware of freeing the cloned object here.
    m_header->m_source_fld = field->clone(&rapid_mem_root);
    m_header->m_type = field->type();
    m_header->m_charset = field->charset();
  }

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

  // the initial one chunk built.
  m_chunks.emplace_back(std::make_unique<Chunk>(const_cast<Field *>(field)));

  m_current_chunk.store(0);

  m_name = field->field_name;
}

Cu::~Cu() { m_chunks.clear(); }

uchar *Cu::base() {
  if (!m_chunks.size()) return nullptr;
  return m_chunks[0].get()->base();
}

uchar *Cu::last() {
  if (!m_chunks.size()) return nullptr;
  return m_chunks[m_chunks.size() - 1]->tell();
}

Chunk *Cu::chunk(uint id) {
  if (id >= m_chunks.size()) return nullptr;
  return m_chunks[id].get();
}

row_id_t Cu::prows() {
#ifndef NDEBUG
  auto total_rows_in_chunk{0u};
  for (auto sz = 0u; sz < m_chunks.size(); sz++) {
    total_rows_in_chunk += m_chunks[sz]->get_header()->m_prows.load();
  }
  ut_a(total_rows_in_chunk == m_header->m_prows.load());
#endif
  return m_header->m_prows.load(std::memory_order_seq_cst);
}

row_id_t Cu::rows(Rapid_load_context *context) {
  // now, we return the prows, in future, we will return mvcc-versioned row num.
  ut_a(context->m_trx);
  size_t rows{0u};
  for (auto idx = 0u; idx < m_chunks.size(); idx++) {
    rows += m_chunks[idx]->rows(context);
  }
  return m_header->m_prows.load(std::memory_order_seq_cst);
}

size_t Cu::normalized_pack_length() {
  ut_a(m_chunks.size());
  return m_chunks[0]->normalized_pack_length();
}

size_t Cu::pack_length() {
  ut_a(m_chunks.size());
  return m_chunks[0]->pack_length();
}

uchar *Cu::get_field_value(uchar *&data, size_t &len, bool need_pack) {
  ut_a(len != UNIV_SQL_NULL);

  uint32 dict_val{0u};
  switch (m_header->m_type) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      if (need_pack) {
        auto to = std::make_unique<uchar[]>(m_header->m_source_fld->pack_length());
        auto to_ptr = Utils::Util::pack_str(data, len, &my_charset_bin, to.get(), m_header->m_source_fld->pack_length(),
                                            m_header->m_source_fld->charset());
        dict_val =
            m_header->m_local_dict->store(to_ptr, m_header->m_source_fld->pack_length(), m_header->m_encoding_type);
        *reinterpret_cast<uint32 *>(data) = dict_val;
        len = sizeof(uint32);
      } else {
        dict_val = m_header->m_local_dict->store(data, len, m_header->m_encoding_type);
        *reinterpret_cast<uint32 *>(data) = dict_val;
        len = sizeof(uint32);
      }
    } break;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      break;
    case MYSQL_TYPE_DOUBLE:
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
    } break;
    default:
      break;
  }

  return data;
}

void Cu::update_meta_info(OPER_TYPE type, uchar *data) {
  // gets the data value.
  double data_val{0};
  switch (m_header->m_type) {
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      data_val = *reinterpret_cast<double *>(data);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      data_val = *reinterpret_cast<double *>(data);
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      data_val = *reinterpret_cast<int *>(data);
    } break;
    default:
      break;
  }

  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   *  become complex than before.*/
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      m_header->m_prows.fetch_add(1);
      m_header->m_sum.store(m_header->m_sum + data_val);
      m_header->m_avg = m_header->m_sum / m_header->m_prows;
    } break;
    case ShannonBase::OPER_TYPE::OPER_DELETE: {
    } break;
    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
    } break;
    default:
      break;
  }
  return;
}

uchar *Cu::write_row(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  /** if the field type is text type then the value will be encoded by local dictionary.
   * otherwise, the values stores in plain format.*/
  uchar *written_to{nullptr};
  auto chunk_ptr = m_chunks[m_chunks.size() - 1].get();
  ut_a(chunk_ptr);

  data = (len == UNIV_SQL_NULL) ? nullptr : get_field_value(data, len, false);
  if (!(written_to = chunk_ptr->write(context, data, len))) {  // current chunk is full.
    // then build a new one, and re-try to write the data.
    m_chunks.emplace_back(std::make_unique<Chunk>(m_header->m_source_fld));
    if (!m_chunks[m_chunks.size() - 1].get()) return nullptr;  // runs out of mem.

    written_to = m_chunks[m_chunks.size() - 1].get()->write(context, data, len);
  }

  if (written_to) update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to);

  // after that should check the size of all fields. they're be same
  return written_to;
}

/**
 * It seems that the string value was not padded, so we convert it here to MySQL format.
 */
uchar *Cu::write_row_from_log(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  /** if the field type is text type then the value will be encoded by local dictionary.
   * otherwise, the values stores in plain format.*/
  uchar *written_to{nullptr};
  auto chunk_ptr = m_chunks[m_chunks.size() - 1].get();
  ut_a(chunk_ptr);

  // data = (len == UNIV_SQL_NULL) ? nullptr : get_field_value(data, len, true);
  if (!(written_to = chunk_ptr->write(context, data, len))) {  // current chunk is full.
    // then build a new one, and re-try to write the data.
    m_chunks.emplace_back(std::make_unique<Chunk>(m_header->m_source_fld));
    if (!m_chunks[m_chunks.size() - 1].get()) return nullptr;  // runs out of mem.

    written_to = m_chunks[m_chunks.size() - 1].get()->write(context, data, len);
  }
  if (written_to) update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to);

  // after that should check the size of all fields. they're be same.
  return written_to;
}

uchar *Cu::delete_row(const Rapid_load_context *context, uchar *data, size_t len) {
  // TODO:
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));
  if (!m_chunks.size()) return nullptr;

  return data;
}

// delete the row by rowid.
uchar *Cu::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);

  uchar *del_from{nullptr};
  if (rowid >= m_header->m_prows.load())  // out of row range.
    return del_from;

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  if (chunk_id > m_chunks.size()) return del_from;  // out of chunk rnage.

  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  if (!(del_from = m_chunks[chunk_id]->del(context, offset_in_chunk))) {  // ret to deleted row addr.
    return del_from;
  }
  if (del_from) update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, del_from);
  return del_from;
  // to update meta data info.
}

uchar *Cu::delete_row_from_log(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));
  return data;
}

uchar *Cu::delete_row_all(const Rapid_load_context *context) {
  auto rows = m_header->m_prows.load();
  for (row_id_t rowid = 0; rowid < rows; rowid++) {
    if (!delete_row(context, rowid))  // errors occur.
      return nullptr;
  }

  ut_a(m_chunks.size());
  return m_chunks[0]->base();
}

uchar *Cu::read_row(const Rapid_load_context *context, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  uchar *ret{nullptr};
  while (m_current_chunk < m_chunks.size()) {
    if (!(ret = m_chunks[m_current_chunk].get()->read(context, data, len))) {
      // at then end of chunk, then to next
      m_current_chunk.fetch_add(1);
    } else
      break;
  }

  return ret;
}

uchar *Cu::update_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));
  ut_a(rowid < m_header->m_prows.load());

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(chunk_id < m_chunks.size());

  return m_chunks[chunk_id]->update(context, offset_in_chunk, data, len);
}

}  // namespace Imcs
}  // namespace ShannonBase