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
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {

Cu::Cu(const Field *field) {
  ut_a(field && !field->is_flag_set(NOT_SECONDARY_FLAG));
  {
    std::scoped_lock lk(m_header_mutex);
    m_header.reset(new (std::nothrow) Cu_header());
    if (!m_header) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu header allocation failed");
      return;
    }

    // TODO: be aware of freeing the cloned object here.
    m_header->m_source_fld = field->clone(&rapid_mem_root);
    m_header->m_type = field->type();
    m_header->m_charset = field->charset();
    m_header->m_key_len = field->table->file->ref_length;
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

  m_header->m_local_dict.reset(new (std::nothrow) Compress::Dictionary(m_header->m_encoding_type));
  if (!m_header->m_local_dict) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu dictionary allocation failed");
    return;
  }
  // the initial one chunk built.
  auto chunk = new (std::nothrow) Chunk(const_cast<Field *>(field));
  if (!chunk) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
    return;
  }
  m_chunks.emplace_back(chunk);

  m_current_chunk.store(0);

  m_cu_key.append(field->table->s->db.str)
      .append(":")
      .append(field->table->s->table_name.str)
      .append(":")
      .append(field->field_name);
}

Cu::~Cu() {
  if (m_chunks.size()) m_chunks.clear();
}

row_id_t Cu::rows(Rapid_load_context *context) {
  // now, we return the prows, in future, we will return mvcc-versioned row num. using m_chunks[]
  // to get the versioned-rows.
  return m_header ? m_header->m_prows.load() : 0;
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

void Cu::update_meta_info(OPER_TYPE type, uchar *data, uchar *old) {
  // gets the data value.
  auto dict = current_imcs_instance->get_cu(m_cu_key)->header()->m_local_dict.get();
  double data_val = data ? Utils::Util::get_field_value<double>(m_header->m_source_fld, data, dict) : 0;
  double old_val = old ? Utils::Util::get_field_value<double>(m_header->m_source_fld, old, dict) : 0;

  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   *  become complex than before.*/
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      m_header->m_prows.fetch_add(1);
      if (!data) return;  // is null, only update rows count.

      ut_a(m_header->m_prows.load() <= SHANNON_ROWS_IN_CHUNK);
      m_header->m_sum.store(m_header->m_sum + data_val);
      m_header->m_avg.store(m_header->m_sum / m_header->m_prows);

      if (m_header->m_min.load(std::memory_order_relaxed) > data_val) m_header->m_min.store(data_val);
      if (m_header->m_max.load(std::memory_order_relaxed) < data_val) m_header->m_max.store(data_val);

      m_header->m_middle.store(
          (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
      m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));

    } break;
    case ShannonBase::OPER_TYPE::OPER_DELETE: {
      // m_header->m_prows.fetch_sub(1);
      if (!data) return;  // is null, only update rows count.

      ut_a(m_header->m_prows.load() <= SHANNON_ROWS_IN_CHUNK);

      if (m_header->m_prows.load(std::memory_order_relaxed) == 0) {  // empty now.
        m_header->m_avg.store(0);
        m_header->m_sum.store(0);
        m_header->m_middle.store(0);
        m_header->m_median.store(0);
        m_header->m_min.store(SHANNON_MAX_DOUBLE);
        m_header->m_max.store(SHANNON_MIN_DOUBLE);
      } else {
        m_header->m_sum.fetch_sub(data_val);
        m_header->m_avg.store(m_header->m_sum.load(std::memory_order_relaxed) /
                              m_header->m_prows.load(std::memory_order_relaxed));

        if (are_equal(m_header->m_min.load(std::memory_order_relaxed), data_val)) {
          // re-calc the min
        }
        if (are_equal(m_header->m_max.load(std::memory_order_relaxed), data_val)) {
          // re-calc the max
        }

        m_header->m_middle.store(
            (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
        m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));
      }
    } break;
    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
      m_header->m_sum.fetch_sub(old_val);
      m_header->m_sum.fetch_add(data_val);
      m_header->m_avg.store(m_header->m_sum.load(std::memory_order_relaxed) /
                            m_header->m_prows.load(std::memory_order_relaxed));

      if (m_header->m_min.load(std::memory_order_relaxed) > data_val) m_header->m_min.store(data_val);
      if (m_header->m_max.load(std::memory_order_relaxed) < data_val) m_header->m_max.store(data_val);

      m_header->m_middle.store(
          (m_header->m_min.load(std::memory_order_relaxed) + m_header->m_max.load(std::memory_order_relaxed)) / 2);
      m_header->m_median.store(m_header->m_middle.load(std::memory_order_relaxed));
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
    auto chunk = new (std::nothrow) Chunk(const_cast<Field *>(m_header->m_source_fld));
    if (!chunk) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
      return nullptr;
    }
    m_chunks.emplace_back(chunk);

    written_to = m_chunks[m_chunks.size() - 1].get()->write(context, data, len);
  }

  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to, written_to);

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

  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to, written_to);

  // after that should check the size of all fields. they're be same.
  return written_to;
}

// delete the row by rowid.
uchar *Cu::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);

  uchar *del_from{nullptr};
  //  if (rowid >= m_header->m_prows.load())  // out of row range.
  //    return del_from;

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  if (chunk_id > m_chunks.size()) return del_from;  // out of chunk rnage.

  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  if (!(del_from = m_chunks[chunk_id]->remove(context, offset_in_chunk))) {  // ret to deleted row addr.
    return del_from;
  }

  auto is_null = Utils::Util::bit_array_get(m_chunks[chunk_id].get()->header()->m_null_mask.get(), rowid);
  update_meta_info(ShannonBase::OPER_TYPE::OPER_DELETE, is_null ? nullptr : del_from, is_null ? nullptr : del_from);
  return del_from;
  // to update meta data info.
}

uchar *Cu::delete_row_all(const Rapid_load_context *context) {
  auto rows = m_header->m_prows.load();
  for (row_id_t rowid = 0; rowid < rows; rowid++) {
    if (!delete_row(context, rowid))  // errors occur.
      return nullptr;
  }
  // to reset all meta info
  m_header->m_prows.store(0);
  m_header->m_sum.store(0);
  m_header->m_avg.store(0);
  m_header->m_middle.store(0);
  m_header->m_median.store(0);
  m_header->m_max.store(SHANNON_MAX_DOUBLE);
  m_header->m_min.store(SHANNON_MIN_DOUBLE);

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

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(chunk_id < m_chunks.size());
  // auto is_null = Utils::Util::bit_array_get(m_chunks[chunk_id].get()->header()->m_null_mask.get(), rowid);
  auto old = m_chunks[chunk_id].get()->seek((row_id_t)offset_in_chunk) + m_header->m_key_len;

  update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, data, old);

  auto ret = m_chunks[chunk_id]->update(context, offset_in_chunk, data, len);
  return ret;
}

}  // namespace Imcs
}  // namespace ShannonBase