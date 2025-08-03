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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Rapid Table.
*/

/**DataTable to mock a table hehaviors. We can use a DataTable to open the IMCS
 * with sepecific table information. After the Cu belongs to this table were found
 * , we can use this DataTable object to read/write, etc., just like a normal innodb
 * table.
 */
#include "storage/rapid_engine/imcs/table.h"

#include <sstream>

#include "include/ut0dbg.h"  //ut_a
#include "sql/field.h"       //field
#include "sql/table.h"       //TABLE
#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/imcs/chunk.h"  //CHUNK
#include "storage/rapid_engine/imcs/cu.h"     //CU

#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/utils/utils.h"  //Blob

namespace ShannonBase {
namespace Imcs {

int Table::create_fields_memo(const Rapid_load_context *context) {
  ut_a(context && context->m_table);
  auto source = context->m_table;

  for (auto index = 0u; index < source->s->fields; index++) {
    auto field = *(source->field + index);
    if (field->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    size_t chunk_size = SHANNON_ROWS_IN_CHUNK * Utils::Util::normalized_length(field);
    if (likely(ShannonBase::rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
      return HA_ERR_GENERIC;
    }

    std::unique_lock<std::shared_mutex> lk(m_fields_mutex);
    m_fields.emplace(field->field_name, std::make_unique<Cu>(this, field, field->field_name));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::create_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  ut_a(source);
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) build_hidden_index_memo(context);

  // no.2: user-defined indexes.
  build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_hidden_index_memo(const Rapid_load_context *context) {
  m_source_keys.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                        std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{SHANNON_DB_ROW_ID}));
  m_indexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                    std::make_unique<Index::Index<uchar, row_id_t>>(ShannonBase::SHANNON_PRIMARY_KEY_NAME));
  m_index_mutexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME, std::make_unique<std::mutex>());
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;
    std::vector<std::string> key_parts_names;
    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      key_parts_names.push_back(key_info->key_part[i].field->field_name);
    }

    m_source_keys.emplace(key_info->name, std::make_pair(key_info->key_length, key_parts_names));
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
    m_index_mutexes.emplace(key_info->name, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

Cu *Table::get_field(std::string field_name) {
  std::shared_lock<std::shared_mutex> lk(m_fields_mutex);
  if (m_fields.find(field_name) == m_fields.end()) return nullptr;
  return m_fields[field_name].get();
}

int Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();

    uint length{0u};
    KEY_PART_INFO *key_part;
    /* Copy the key parts */
    auto key_length = key->key_length;
    for (key_part = key->key_part; (int)key_length > 0; key_part++) {
      if (key_part->null_bit) {
        bool key_is_null = from_record[key_part->null_offset] & key_part->null_bit;
        *to_key++ = (key_is_null ? 1 : 0);
        key_length--;
      }

      if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
        key_length -= HA_KEY_BLOB_LENGTH;
        length = std::min<uint>(key_length, key_part->length);
        key_part->field->get_key_image(to_key, length, Field::itRAW);
        to_key += HA_KEY_BLOB_LENGTH;
      } else {
        length = std::min<uint>(key_length, key_part->length);
        Field *field = key_part->field;
        const CHARSET_INFO *cs = field->charset();
        if (field->type() == MYSQL_TYPE_DOUBLE || field->type() == MYSQL_TYPE_FLOAT ||
            field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
          uchar encoding[8] = {0};
          Utils::Encoder<double>::EncodeFloat(field->val_real(), encoding);
          memcpy(to_key, encoding, length);
        } else {
          const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
          if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
        }
      }
      to_key += length;
      key_length -= length;
    }
  }
  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                     sizeof(rowid));
  }
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

// using for parallel load.
int Table::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                            ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;
  std::unique_ptr<uchar[]> key_buff{nullptr};
  auto key_len{0u};
  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(false);  // In parallel scan, the primary key must be have, otherwise sequential scan.
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    ut_a(const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len == source->file->ref_length);
    key_len = source->file->ref_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    memcpy(key_buff.get(), source->file->ref, key_len);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = rowdata;

    key_len = key->key_length;
    key_buff.reset(new uchar[key_len]);
    memset(key_buff.get(), 0x0, key_len);
    auto to_key = key_buff.get();

    uint length{0u};
    KEY_PART_INFO *key_part;
    auto key_length = key->key_length;
    { /* Copy the key parts */
      std::unique_lock<std::shared_mutex> ex_lk(m_key_buff_mutex);
      for (key_part = key->key_part; (int)key_length > 0; key_part++) {
        Field *field = key_part->field;
        const CHARSET_INFO *cs = field->charset();
        auto fld_ptr = rowdata + ptrdiff_t(col_offsets[field->field_index()]);
        field->set_field_ptr(fld_ptr);

        if (key_part->null_bit) {
          bool key_is_null = from_record[key_part->null_offset] & key_part->null_bit;
          ut_a(is_field_null(field->field_index(), rowdata, null_byte_offsets, null_bitmasks) == key_is_null);
          *to_key++ = (key_is_null ? 1 : 0);
          key_length--;
        }

        if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
          key_length -= HA_KEY_BLOB_LENGTH;
          length = std::min<uint>(key_length, key_part->length);
          field->get_key_image(to_key, length, Field::itRAW);
          to_key += HA_KEY_BLOB_LENGTH;
        } else {
          length = std::min<uint>(key_length, key_part->length);
          if (field->type() == MYSQL_TYPE_DOUBLE || field->type() == MYSQL_TYPE_FLOAT ||
              field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
            uchar encoding[8] = {0};
            Utils::Encoder<double>::EncodeFloat(field->val_real(), encoding);
            memcpy(to_key, encoding, length);
          } else {
            const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
            if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
          }
        }
        to_key += length;
        key_length -= length;
      }
    }  // scope lock end.
  }

  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[keypart].get());
    m_indexes[keypart].get()->insert(key_buff.get(), key_len, &rowid, sizeof(rowid));
  }
  return SHANNON_SUCCESS;
}

int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

// using for parallel load.
int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                       ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  return build_index_impl(context, key, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks);
}

// IMPORTANT NOTIC: IF YOU CHANGE THE CODE HERE, YOU SHOULD CHANGE THE PARTITIAL TABLE `PartTable::write`
// CORRESPONDINGLY.
int Table::write(const Rapid_load_context *context, uchar *data) {
  /**
   * for VARCHAR type Data in field->ptr is stored as: 1 or 2 bytes length-prefix-header  (from
   * Field_varstring::length_bytes) data. the here we dont use var_xxx to get data, rather getting
   * directly, due to we dont care what real it is. ref to: field.cc:6703
   */

  auto rowid = reserver_rowid();

  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (fld->is_null()) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          // TODO: BLOB data maybe not in the page. stores off the page.
          data_ptr = const_cast<uchar *>(fld->data_ptr());
          data_len = down_cast<Field_blob *>(fld)->get_length();
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          extra_offset = (fld->field_length > 256 ? 2 : 1);
          data_ptr = fld->field_ptr() + extra_offset;
          if (extra_offset == 1)
            data_len = mach_read_from_1(fld->field_ptr());
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(fld->field_ptr());
        } break;
        default: {
          data_ptr = fld->field_ptr();
          data_len = fld->pack_length();
        } break;
      }
    }

    if (!(m_fields[fld->field_name]->write_row(context, rowid, data_ptr, data_len))) {
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid)) return HA_ERR_GENERIC;
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

// using for parallel load. change the parttable correspondingly.
int Table::write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                 ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);
  uchar *data_ptr{nullptr};
  uint data_len{0};

  auto rowid = reserver_rowid();

  for (auto col_ind = 0u; col_ind < context->m_table->s->fields; col_ind++) {
    auto fld = *(context->m_table->field + col_ind);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    data_ptr = rowdata + col_offsets[col_ind];
    auto is_null = (fld->is_nullable()) ? is_field_null(col_ind, rowdata, null_byte_offsets, null_bitmasks) : false;

    if (is_null) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          // TODO: BLOB data maybe not in the page. stores off the page.
          auto bfld = down_cast<Field_blob *>(fld);
          switch (bfld->pack_length_no_ptr()) {
            case 1:
              data_len = *data_ptr;
              break;
            case 2:
              data_len = uint2korr(data_ptr);
              break;
            case 3:
              data_len = uint3korr(data_ptr);
              break;
            case 4:
              data_len = uint4korr(data_ptr);
              break;
          }
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          auto extra_offset = (fld->field_length > 256 ? 2 : 1);
          if (extra_offset == 1)
            data_len = mach_read_from_1(data_ptr);
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(data_ptr);
          data_ptr = data_ptr + ptrdiff_t(extra_offset);
        } break;
        default: {
          data_len = fld->pack_length();
        } break;
      }
    }

    if (!(m_fields[fld->field_name]->write_row(context, rowid, data_ptr, data_len))) {
      // TODO: mark this row to be junk.
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::rollback_changes_by_trxid(Transaction::ID trxid) {
  for (auto &cu : m_fields) {
    auto chunk_sz = cu.second.get()->chunks();
    for (auto index = 0u; index < chunk_sz; index++) {
      auto &version_infos = cu.second.get()->chunk(index)->header()->m_smu->version_info();
      if (!version_infos.size()) continue;

      for (auto &ver : version_infos) {
        std::lock_guard<std::mutex> lock(ver.second.vec_mutex);
        auto rowid = ver.first;

        std::for_each(ver.second.items.begin(), ver.second.items.end(), [&](ReadView::SMU_item &item) {
          if (item.trxid == trxid) {
            // To update rows status.
            if (item.oper_type == OPER_TYPE::OPER_INSERT) {                      //
              if (!cu.second.get()->chunk(index)->header()->m_del_mask.get()) {  // the del mask not exists now.
                cu.second.get()->chunk(index)->header()->m_del_mask =
                    std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
              }
              Utils::Util::bit_array_set(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            if (item.oper_type == OPER_TYPE::OPER_DELETE) {
              Utils::Util::bit_array_reset(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            item.tm_committed = ShannonBase::SHANNON_MAX_STMP;  // reset commit timestamp to max, mean it rollbacked.
                                                                // has been rollbacked, invisible to all readview.
          }
        });
      }
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                              std::unordered_map<std::string, mysql_field_t> &fields) {
  for (auto &field_val : fields) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->write_row(context, rowid, field_val.second.data.get(), len)) {
      // TODO: mark this row to be junk.
      return HA_ERR_WRONG_IN_RECORD;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                      const uchar *new_field_data, size_t nlen) {
  if (m_fields.find(field_key) == m_fields.end()) return HA_ERR_GENERIC;

  auto ret = m_fields[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return ShannonBase::SHANNON_SUCCESS;
}

int Table::update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                               std::unordered_map<std::string, mysql_field_t> &upd_recs) {
  for (auto &field_val : upd_recs) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->update_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Table::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  for (auto it = m_fields.begin(); it != m_fields.end();) {
    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }

  m_prows.fetch_sub(1);
  return SHANNON_SUCCESS;
}

int Table::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) {
  if (!m_fields.size()) return SHANNON_SUCCESS;

  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return ShannonBase::SHANNON_SUCCESS;
  }

  for (auto &rowid : rowids) {
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
  }
  // TODO: remove the index item.

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::create_fields_memo(const Rapid_load_context *context) {
  ut_a(context && context->m_table);
  auto source = context->m_table;

  for (auto index = 0u; index < source->s->fields; index++) {
    auto field = *(source->field + index);
    for (auto &part : context->m_extra_info.m_partition_infos) {
      size_t chunk_size = SHANNON_ROWS_IN_CHUNK * Utils::Util::normalized_length(field);
      if (likely(ShannonBase::rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
        return HA_ERR_GENERIC;
      }
      auto part_name = part.first;
      part_name.append("#").append(std::to_string(part.second)).append(":").append(field->field_name);
      m_fields.emplace(part_name, std::make_unique<Cu>(this, field, part_name));
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

Cu *PartTable::get_field(std::string field_name) {
  std::shared_lock<std::shared_mutex> lk(m_fields_mutex);
  if (m_fields.find(field_name) == m_fields.end()) return nullptr;
  return m_fields[field_name].get();
}

int PartTable::create_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  ut_a(source);
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) build_hidden_index_memo(context);

  // no.2: user-defined indexes.
  build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_hidden_index_memo(const Rapid_load_context *context) {
  for (auto &part : context->m_extra_info.m_partition_infos) {
    auto partkey = part.first;
    partkey.append("#").append(std::to_string(part.second)).append(":");
    partkey.append(ShannonBase::SHANNON_PRIMARY_KEY_NAME);

    m_source_keys.emplace(partkey,
                          std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{SHANNON_DB_ROW_ID}));
    m_indexes.emplace(partkey, std::make_unique<Index::Index<uchar, row_id_t>>(SHANNON_DB_ROW_ID));
    m_index_mutexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME, std::make_unique<std::mutex>());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;
  for (auto &part : context->m_extra_info.m_partition_infos) {
    auto keypart = part.first;
    keypart.append("#").append(std::to_string(part.second)).append(":");

    for (auto ind = 0u; ind < source->s->keys; ind++) {
      auto key_info = source->key_info + ind;
      auto keyname(keypart);
      keyname.append(key_info->name);

      std::vector<std::string> key_parts_names;
      for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
        key_parts_names.push_back(key_info->key_part[i].field->field_name);
      }
      m_source_keys.emplace(keyname, std::make_pair(key_info->key_length, key_parts_names));
      m_indexes.emplace(keyname, std::make_unique<Index::Index<uchar, row_id_t>>(keyname));
      m_index_mutexes.emplace(keyname, std::make_unique<std::mutex>());
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;
  auto active_partkey = context->m_extra_info.m_active_part_key;

  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);

    ut_a(const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len == source->file->ref_length);
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = source->file->ref_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff =
        std::make_unique<uchar[]>(source->file->ref_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, source->file->ref_length);
    memcpy(context->m_extra_info.m_key_buff.get(), source->file->ref, source->file->ref_length);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = source->record[0];

    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = key->key_length;
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
    memset(context->m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    auto to_key = context->m_extra_info.m_key_buff.get();

    uint length{0u};
    KEY_PART_INFO *key_part;
    /* Copy the key parts */
    auto key_length = key->key_length;
    for (key_part = key->key_part; (int)key_length > 0; key_part++) {
      if (key_part->null_bit) {
        bool key_is_null = from_record[key_part->null_offset] & key_part->null_bit;
        *to_key++ = (key_is_null ? 1 : 0);
        key_length--;
      }

      if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
        key_length -= HA_KEY_BLOB_LENGTH;
        length = std::min<uint>(key_length, key_part->length);
        key_part->field->get_key_image(to_key, length, Field::itRAW);
        to_key += HA_KEY_BLOB_LENGTH;
      } else {
        length = std::min<uint>(key_length, key_part->length);
        Field *field = key_part->field;
        const CHARSET_INFO *cs = field->charset();
        if (field->type() == MYSQL_TYPE_DOUBLE || field->type() == MYSQL_TYPE_FLOAT ||
            field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
          uchar encoding[8] = {0};
          Utils::Encoder<double>::EncodeFloat(field->val_real(), encoding);
          memcpy(to_key, encoding, length);
        } else {
          const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
          if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
        }
      }
      to_key += length;
      key_length -= length;
    }
  }

  auto keyname = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  auto active_key = active_partkey.append(":").append(keyname);
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[active_key].get());
    m_indexes[active_key].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                        sizeof(rowid));
  }
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

// using for parallel load.
int PartTable::build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                                ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  // this is come from ha_innodb.cc postion(), when postion() changed, the part should be changed respondingly.
  // why we dont not change the impl of postion() directly? because the postion() is impled in innodb engine.
  // we want to decouple with innodb engine.
  auto source = context->m_table;
  auto active_partkey = context->m_extra_info.m_active_part_key;

  std::unique_ptr<uchar[]> key_buff{nullptr};
  auto key_len{0u};
  if (key == nullptr) {
    /* No primary key was defined for the table and we generated the clustered index
     from row id: the row reference will be the row id, not any key value that MySQL
     knows of */
    ut_a(source->file->ref_length == ShannonBase::SHANNON_DATA_DB_ROW_ID_LEN);
    ut_a(const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len == source->file->ref_length);
    key_len = source->file->ref_length;
    key_buff = std::make_unique<uchar[]>(key_len);
    memset(key_buff.get(), 0x0, key_len);
    memcpy(key_buff.get(), source->file->ref, key_len);
  } else {
    /* Copy primary key as the row reference */
    auto from_record = rowdata;

    key_len = key->key_length;
    key_buff = std::make_unique<uchar[]>(key_len);
    memset(key_buff.get(), 0x0, key_len);
    auto to_key = key_buff.get();

    uint length{0u};
    KEY_PART_INFO *key_part;
    /* Copy the key parts */
    auto key_length = key->key_length;
    {
      std::unique_lock<std::shared_mutex> ex_lk(m_key_buff_mutex);
      for (key_part = key->key_part; (int)key_length > 0; key_part++) {
        Field *field = key_part->field;
        const CHARSET_INFO *cs = field->charset();
        auto fld_ptr = rowdata + ptrdiff_t(col_offsets[field->field_index()]);
        field->set_field_ptr(fld_ptr);

        if (key_part->null_bit) {
          bool key_is_null = from_record[key_part->null_offset] & key_part->null_bit;
          ut_a(is_field_null(field->field_index(), rowdata, null_byte_offsets, null_bitmasks) == key_is_null);
          *to_key++ = (key_is_null ? 1 : 0);
          key_length--;
        }

        if (key_part->key_part_flag & HA_BLOB_PART || key_part->key_part_flag & HA_VAR_LENGTH_PART) {
          key_length -= HA_KEY_BLOB_LENGTH;
          length = std::min<uint>(key_length, key_part->length);
          field->get_key_image(to_key, length, Field::itRAW);
          to_key += HA_KEY_BLOB_LENGTH;
        } else {
          length = std::min<uint>(key_length, key_part->length);
          if (field->type() == MYSQL_TYPE_DOUBLE || field->type() == MYSQL_TYPE_FLOAT ||
              field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
            uchar encoding[8] = {0};
            Utils::Encoder<double>::EncodeFloat(field->val_real(), encoding);
            memcpy(to_key, encoding, length);
          } else {
            const size_t bytes = field->get_key_image(to_key, length, Field::itRAW);
            if (bytes < length) cs->cset->fill(cs, (char *)to_key + bytes, length - bytes, ' ');
          }
        }
        to_key += length;
        key_length -= length;
      }
    }
  }

  auto keyname = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  auto active_key = active_partkey.append(":").append(keyname);
  {
    std::lock_guard<std::mutex> lock(*m_index_mutexes[active_key].get());
    m_indexes[active_key].get()->insert(key_buff.get(), key_len, &rowid, sizeof(rowid));
  }

  return SHANNON_SUCCESS;
}

int PartTable::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

// using for parallel load.
int PartTable::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                           ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) {
  return build_index_impl(context, key, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks);
}

// IMPORTANT NOTIC: IF YOU CHANGE THE CODE HERE, YOU SHOULD CHANGE THE PARTITIAL TABLE `Table::write` CORRESPONDINGLY.
int PartTable::write(const Rapid_load_context *context, uchar *data) {
  ut_a(context && data);

  auto rowid = reserver_rowid();
  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (fld->is_null()) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          data_ptr = const_cast<uchar *>(fld->data_ptr());
          data_len = down_cast<Field_blob *>(fld)->get_length();
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          extra_offset = (fld->field_length > 256 ? 2 : 1);
          data_ptr = fld->field_ptr() + extra_offset;
          if (extra_offset == 1)
            data_len = mach_read_from_1(fld->field_ptr());
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(fld->field_ptr());
        } break;
        default: {
          data_ptr = fld->field_ptr();
          data_len = fld->pack_length();
        } break;
      }
    }

    auto active_part = context->m_extra_info.m_active_part_key;
    auto key = active_part.append(":").append(fld->field_name);
    if (!(m_fields[key]->write_row(context, rowid, data_ptr, data_len))) {
      // TODO: mark this row to be junk.
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid)) return HA_ERR_GENERIC;
  }
  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                     ulong *null_byte_offsets, ulong *null_bitmasks) {
  ut_a(context->m_table->s->fields == n_cols);
  uchar *data_ptr{nullptr};
  uint data_len{0};

  auto rowid = reserver_rowid();
  for (auto col_ind = 0u; col_ind < context->m_table->s->fields; col_ind++) {
    auto fld = *(context->m_table->field + col_ind);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    data_ptr = rowdata + col_offsets[col_ind];
    auto is_null = (fld->is_nullable()) ? is_field_null(col_ind, rowdata, null_byte_offsets, null_bitmasks) : false;
    if (is_null) {
      data_len = UNIV_SQL_NULL;
      data_ptr = nullptr;
    } else {
      switch (fld->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          auto bfld = down_cast<Field_blob *>(fld);
          switch (bfld->pack_length_no_ptr()) {
            case 1:
              data_len = *data_ptr;
              break;
            case 2:
              data_len = uint2korr(data_ptr);
              break;
            case 3:
              data_len = uint3korr(data_ptr);
              break;
            case 4:
              data_len = uint4korr(data_ptr);
              break;
          }
        } break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING: {
          auto extra_offset = (fld->field_length > 256 ? 2 : 1);
          if (extra_offset == 1)
            data_len = mach_read_from_1(data_ptr);
          else if (extra_offset == 2)
            data_len = mach_read_from_2_little_endian(data_ptr);
          data_ptr = data_ptr + ptrdiff_t(extra_offset);
        } break;
        default: {
          data_len = fld->pack_length();
        } break;
      }
    }

    if (!(m_fields[fld->field_name]->write_row(context, rowid, data_ptr, data_len))) {
      // TODO: mark this row to be junk.
      return HA_ERR_GENERIC;
    }
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)rowdata);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
  }

  for (auto index = 0u; index < context->m_table->s->keys; index++) {
    auto key_info = context->m_table->key_info + index;
    if (build_index(context, key_info, rowid, rowdata, col_offsets, null_byte_offsets, null_bitmasks))
      return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::rollback_changes_by_trxid(Transaction::ID trxid) {
  for (auto &cu : m_fields) {
    auto chunk_sz = cu.second.get()->chunks();
    for (auto index = 0u; index < chunk_sz; index++) {
      auto &version_infos = cu.second.get()->chunk(index)->header()->m_smu->version_info();
      if (!version_infos.size()) continue;

      for (auto &ver : version_infos) {
        std::lock_guard<std::mutex> lock(ver.second.vec_mutex);
        auto rowid = ver.first;

        std::for_each(ver.second.items.begin(), ver.second.items.end(), [&](ReadView::SMU_item &item) {
          if (item.trxid == trxid) {
            // To update rows status.
            if (item.oper_type == OPER_TYPE::OPER_INSERT) {                      //
              if (!cu.second.get()->chunk(index)->header()->m_del_mask.get()) {  // the del mask not exists now.
                cu.second.get()->chunk(index)->header()->m_del_mask =
                    std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
              }
              Utils::Util::bit_array_set(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            if (item.oper_type == OPER_TYPE::OPER_DELETE) {
              Utils::Util::bit_array_reset(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
            }
            item.tm_committed = ShannonBase::SHANNON_MAX_STMP;  // reset commit timestamp to max, mean it rollbacked.
                                                                // has been rollbacked, invisible to all readview.
          }
        });
      }
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                  std::unordered_map<std::string, mysql_field_t> &fields) {
  for (auto &field_val : fields) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->write_row(context, rowid, field_val.second.data.get(), len)) {
      // TODO: mark this row to be junk.
      return HA_ERR_WRONG_IN_RECORD;
    }
  }

  m_prows.fetch_add(1);
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                          const uchar *new_field_data, size_t nlen) {
  if (m_fields.find(field_key) == m_fields.end()) return HA_ERR_GENERIC;

  auto ret = m_fields[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                   std::unordered_map<std::string, mysql_field_t> &upd_recs) {
  for (auto &field_val : upd_recs) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || m_fields.find(key_name) == m_fields.end()) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!m_fields[key_name]->update_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  for (auto it = m_fields.begin(); it != m_fields.end();) {
    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }

  return SHANNON_SUCCESS;
}

int PartTable::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) {
  if (!m_fields.size()) return SHANNON_SUCCESS;

  auto deleted_cnt{0};
  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return ShannonBase::SHANNON_SUCCESS;
  }

  for (auto &rowid : rowids) {  // delete some rows.
    for (auto &cu : m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
    deleted_cnt++;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

}  // namespace Imcs
}  // namespace ShannonBase
