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
SHANNON_THREAD_LOCAL Cu::LocalDataBuffer Cu::m_buff;
Cu::Cu(RapidTable *owner, const Field *field) {
  static_assert(alignof(m_buff) >= CACHE_LINE_SIZE, "Alignment failed");
  ut_a(field && !field->is_flag_set(NOT_SECONDARY_FLAG));

  m_header = std::make_unique<Cu_header>();
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu header allocation failed");
    return;
  }

  // TODO: be aware of freeing the cloned object here.
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  init_header(m_header.get(), owner, m_header->m_source_fld);
  init_body(m_header->m_source_fld);
}

Cu::Cu(RapidTable *owner, const Field *field, std::string name) {
  // TODO: be aware of freeing the cloned object here.
  m_header = std::make_unique<Cu_header>();
  if (!m_header) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu header allocation failed");
    return;
  }
  m_header->m_source_fld = field->clone(&rapid_mem_root);
  init_header(m_header.get(), owner, m_header->m_source_fld);
  init_body(m_header->m_source_fld);
  m_cu_key = name;
}

Cu::~Cu() {
  if (m_chunks.size()) m_chunks.clear();
}

void Cu::init_header(Cu_header *header, RapidTable *owner, const Field *field) {
  {
    header->m_owner = owner;
    header->m_type = field->type();
    header->m_width = field->pack_length();
    header->m_charset = field->charset();
    ut_a(header->m_source_fld->table->file->ref_length == field->table->file->ref_length);
    header->m_key_len.store(header->m_source_fld->table->file->ref_length);
  }
  m_cu_key.append(field->table->s->db.str)
      .append(":")
      .append(field->table->s->table_name.str)
      .append(":")
      .append(field->field_name);

  std::string comment(field->comment.str);
  std::transform(comment.begin(), comment.end(), comment.begin(), ::toupper);
  const char *const patt_str = "RAPID_COLUMN\\s*=\\s*ENCODING\\s*=\\s*(SORTED|VARLEN)";
  std::regex column_encoding_patt(patt_str, std::regex_constants::nosubs | std::regex_constants::icase);

  if (std::regex_search(comment.c_str(), column_encoding_patt)) {
    if (comment.find("SORTED") != std::string::npos)
      header->m_encoding_type = Compress::Encoding_type::SORTED;
    else if (comment.find("VARLEN") != std::string::npos)
      header->m_encoding_type = Compress::Encoding_type::VARLEN;
  } else
    header->m_encoding_type = Compress::Encoding_type::NONE;

  header->m_local_dict = std::make_unique<Compress::Dictionary>(header->m_encoding_type);
  if (!header->m_local_dict) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Cu dictionary allocation failed");
    return;
  }
}

void Cu::init_body(const Field *field) {
  // the initial one chunk built.
  auto chunk = std::make_unique<Chunk>(this, const_cast<Field *>(field), m_cu_key);
  if (!chunk) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
    return;
  }
  m_chunks.emplace_back(std::move(chunk));

  m_current_chunk.store(0);
}

uchar *Cu::get_vfield_value(uchar *&data, size_t &len, bool need_pack) {
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
      if (m_header->m_source_fld->real_type() == MYSQL_TYPE_ENUM) {
        return data;
      }
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
    default:
      break;
  }

  return data;
}

void Cu::update_meta_info(OPER_TYPE type, uchar *data, uchar *old, bool row_reserved) {
  // gets the data value.
  // double data_val =
  //    data ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get()) : 0;
  // double old_val =
  //    old ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, old, m_header->m_local_dict.get()) : 0;

  /** TODO: due to the each data has its own version, and the data
   * here is committed. in fact, we support MV, which makes this problem
   *  become complex than before.*/
  switch (type) {
    case ShannonBase::OPER_TYPE::OPER_INSERT: {
      if (!data) return;  // null, only update row counts.

      double data_val =
          Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get());

      m_header->m_sum.fetch_add(data_val, std::memory_order_relaxed);

      update_min_atomic(data_val);
      update_max_atomic(data_val);

      m_stats_dirty.store(true, std::memory_order_relaxed);
    } break;

    case ShannonBase::OPER_TYPE::OPER_DELETE: {
      if (!data) return;  // value is null.

      double data_val =
          Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get());

      m_header->m_sum.fetch_sub(data_val, std::memory_order_relaxed);

      // Note: The min/max update for deletion operations is complex and requires rescanning the data
      // Here, it is simply marked as needing recalculation
      m_stats_dirty.store(true, std::memory_order_relaxed);

    } break;

    case ShannonBase::OPER_TYPE::OPER_UPDATE: {
      if (!old && !data) return;

      double old_val =
          old ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, old, m_header->m_local_dict.get()) : 0;
      double data_val =
          data ? Utils::Util::get_field_numeric<double>(m_header->m_source_fld, data, m_header->m_local_dict.get()) : 0;

      m_header->m_sum.fetch_sub(old_val, std::memory_order_relaxed);
      m_header->m_sum.fetch_add(data_val, std::memory_order_relaxed);

      if (data) {
        update_min_atomic(data_val);
        update_max_atomic(data_val);
      }

      m_stats_dirty.store(true, std::memory_order_relaxed);
    } break;

    default:
      break;
  }
  return;
}

void Cu::recalculate_statistics() {
  std::lock_guard<std::mutex> lock(m_stats_mutex);

  size_t total_rows = m_header->m_owner->rows(nullptr);

  if (total_rows == 0) {
    m_header->m_avg.store(0, std::memory_order_relaxed);
    m_header->m_middle.store(0, std::memory_order_relaxed);
    m_header->m_median.store(0, std::memory_order_relaxed);
    m_header->m_min.store(SHANNON_MAX_DOUBLE, std::memory_order_relaxed);
    m_header->m_max.store(SHANNON_MIN_DOUBLE, std::memory_order_relaxed);
  } else {
    // re-calc avg
    double sum = m_header->m_sum.load(std::memory_order_relaxed);
    m_header->m_avg.store(sum / total_rows, std::memory_order_relaxed);

    // calc medium（simple impl：middle of min, max）
    double min_val = m_header->m_min.load(std::memory_order_relaxed);
    double max_val = m_header->m_max.load(std::memory_order_relaxed);
    double middle = (min_val + max_val) / 2.0;

    m_header->m_middle.store(middle, std::memory_order_relaxed);
    m_header->m_median.store(middle, std::memory_order_relaxed);
  }

  m_stats_dirty.store(false, std::memory_order_relaxed);
}

void Cu::ensure_new_chunks(size_t required_chunk_id) {
  std::scoped_lock lk(m_mutex);
  while (required_chunk_id >= m_chunks.size()) {
    auto chunk = std::make_unique<Chunk>(this, m_header->m_source_fld);
    if (!chunk) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Chunk allocation failed");
      throw std::runtime_error("Chunk allocation failed");
    }
    m_chunks.emplace_back(std::move(chunk));
  }
}

uchar *Cu::write_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));
  auto dlen = (len == UNIV_SQL_NULL) ? sizeof(uint32) : ((len < sizeof(uint32)) ? sizeof(uint32) : len);
  std::unique_ptr<uchar[]> datum(dlen < MAX_FIELD_WIDTH ? nullptr : new uchar[dlen]);
  uchar *pdatum{nullptr};
  if (data) {  // not null.
    pdatum = (dlen < MAX_FIELD_WIDTH) ? m_buff.data : datum.get();
    std::memset(pdatum, 0x0, (dlen < MAX_FIELD_WIDTH) ? MAX_FIELD_WIDTH : dlen);
    std::memcpy(pdatum, data, len);
  }

  /** if the field type is text type then the value will be encoded by local dictionary.
   * otherwise, the values stores in plain format.*/
  uchar *written_to{nullptr};
  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset = rowid % SHANNON_ROWS_IN_CHUNK;

  ensure_new_chunks(chunk_id);

  auto chunk_ptr = m_chunks[chunk_id].get();
  ut_a(chunk_ptr);

  auto wlen{len};
  auto wdata = (wlen == UNIV_SQL_NULL) ? nullptr : get_vfield_value(pdatum, wlen, false);
  if (!(written_to = chunk_ptr->write(context, offset, wdata, wlen))) return nullptr;

  update_meta_info(ShannonBase::OPER_TYPE::OPER_INSERT, written_to, written_to, true);
  return written_to;
}

// delete the row by rowid.
uchar *Cu::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);

  uchar *del_from{nullptr};
  if (rowid >= m_header->m_owner->rows(context))  // out of row range.
    return del_from;

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  if (chunk_id > m_chunks.size()) return del_from;  // out of chunk rnage.

  std::scoped_lock lk(m_mutex);
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
  auto rows = m_header->m_owner->rows(nullptr);
  for (row_id_t rowid = 0; rowid < rows; rowid++) {
    if (!delete_row(context, rowid))  // errors occur.
      return nullptr;
  }

  // to reset all meta info. just mark. GC will do the real space reclaim.
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

  auto old = m_chunks[chunk_id].get()->seek((row_id_t)offset_in_chunk);

  update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, data, old);

  auto ret = m_chunks[chunk_id]->update(context, offset_in_chunk, data, len);
  return ret;
}

uchar *Cu::update_row_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len) {
  ut_a(context);
  ut_a((data && len != UNIV_SQL_NULL) || (!data && len == UNIV_SQL_NULL));

  auto dlen = (len < sizeof(uint32)) ? sizeof(uint32) : len;
  std::unique_ptr<uchar[]> datum((dlen < MAX_FIELD_WIDTH) ? nullptr : new uchar[dlen]);
  uchar *pdatum{nullptr};
  if (data) {
    pdatum = (dlen < MAX_FIELD_WIDTH) ? m_buff.data : datum.get();
    std::memset(pdatum, 0x0, (dlen < MAX_FIELD_WIDTH) ? MAX_FIELD_WIDTH : dlen);
    std::memcpy(pdatum, data, len);
  }
  auto wdata = (len == UNIV_SQL_NULL) ? nullptr : get_vfield_value(pdatum, len, false);

  auto chunk_id = rowid / SHANNON_ROWS_IN_CHUNK;
  auto offset_in_chunk = rowid % SHANNON_ROWS_IN_CHUNK;
  ut_a(chunk_id < m_chunks.size());

  auto ret = m_chunks[chunk_id]->update(context, offset_in_chunk, wdata, len);
  if (!ret) {
    auto old = m_chunks[chunk_id].get()->seek((row_id_t)offset_in_chunk);
    update_meta_info(ShannonBase::OPER_TYPE::OPER_UPDATE, data, old);
  }

  return ret;
}

}  // namespace Imcs
}  // namespace ShannonBase