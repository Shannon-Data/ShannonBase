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

int RapidTable::build_field_memo(const Rapid_load_context *context, Field *field) {
  ut_a(field);
  size_t chunk_size = SHANNON_ROWS_IN_CHUNK * Utils::Util::normalized_length(field);
  if (likely(ShannonBase::rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
    return HA_ERR_GENERIC;
  }

  std::unique_lock<std::shared_mutex> lk(m_fields_mutex);
  m_fields.emplace(field->field_name, std::make_unique<Cu>(field, field->field_name));
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidTable::build_hidden_index_memo(const Rapid_load_context *context) {
  m_source_keys.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                        std::make_pair(SHANNON_DATA_DB_ROW_ID_LEN, std::vector<std::string>{SHANNON_DB_ROW_ID}));
  m_indexes.emplace(ShannonBase::SHANNON_PRIMARY_KEY_NAME,
                    std::make_unique<Index::Index<uchar, row_id_t>>(ShannonBase::SHANNON_PRIMARY_KEY_NAME));
  return ShannonBase::SHANNON_SUCCESS;
}

int RapidTable::build_user_defined_index_memo(const Rapid_load_context *context) {
  auto source = context->m_table;

  for (auto ind = 0u; ind < source->s->keys; ind++) {
    auto key_info = source->key_info + ind;
    std::vector<std::string> key_parts_names;
    for (uint i = 0u; i < key_info->user_defined_key_parts /**actual_key_parts*/; i++) {
      key_parts_names.push_back(key_info->key_part[i].field->field_name);
    }

    m_source_keys.emplace(key_info->name, std::make_pair(key_info->key_length, key_parts_names));
    m_indexes.emplace(key_info->name, std::make_unique<Index::Index<uchar, row_id_t>>(key_info->name));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

Cu *RapidTable::get_field(std::string field_name) {
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
  auto keypart = key ? key->name : ShannonBase::SHANNON_PRIMARY_KEY_NAME;
  m_indexes[keypart].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                   sizeof(rowid));
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

int Table::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

int Table::write(const Rapid_load_context *context, uchar *data) {
  /**
   * for VARCHAR type Data in field->ptr is stored as: 1 or 2 bytes length-prefix-header  (from
   * Field_varstring::length_bytes) data. the here we dont use var_xxx to get data, rather getting
   * directly, due to we dont care what real it is. ref to: field.cc:6703
   */
  row_id_t rowid{0};
  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (Utils::Util::is_blob(fld->type())) {
      data_ptr = const_cast<uchar *>(fld->data_ptr());
      data_len = down_cast<Field_blob *>(fld)->get_length();
    } else {
      extra_offset = Utils::Util::is_varstring(fld->type()) ? (fld->field_length > 256 ? 2 : 1) : 0;
      data_ptr = fld->is_null() ? nullptr : fld->field_ptr() + extra_offset;
      if (fld->is_null()) {
        data_len = UNIV_SQL_NULL;
        data_ptr = nullptr;
      } else {
        if (extra_offset == 1)
          data_len = mach_read_from_1(fld->field_ptr());
        else if (extra_offset == 2)
          data_len = mach_read_from_2_little_endian(fld->field_ptr());
        else
          data_len = fld->pack_length();
      }
    }

    if (!m_fields[fld->field_name]->write_row(context, data_ptr, data_len)) {
      return HA_ERR_GENERIC;
    }
    rowid = m_fields[fld->field_name]->header()->m_prows.load(std::memory_order_relaxed) - 1;
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid)) return HA_ERR_GENERIC;

    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
    }
  } else {
    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int PartTable::build_field_memo(const Rapid_load_context *context, Field *field) {
  for (auto &part : context->m_extra_info.m_partition_infos) {
    size_t chunk_size = SHANNON_ROWS_IN_CHUNK * Utils::Util::normalized_length(field);
    if (likely(ShannonBase::rapid_allocated_mem_size + chunk_size > ShannonBase::rpd_mem_sz_max)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid allocated memory exceeds over the maximum");
      return HA_ERR_GENERIC;
    }
    auto part_name = part.first;
    part_name.append("#").append(std::to_string(part.second)).append(":").append(field->field_name);
    m_fields.emplace(part_name, std::make_unique<Cu>(field, part_name));
  }

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
  m_indexes[active_key].get()->insert(context->m_extra_info.m_key_buff.get(), context->m_extra_info.m_key_len, &rowid,
                                      sizeof(rowid));
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_len = 0;
  const_cast<Rapid_load_context *>(context)->m_extra_info.m_key_buff.reset(nullptr);
  return SHANNON_SUCCESS;
}

int PartTable::build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) {
  return build_index_impl(context, key, rowid);
}

int PartTable::write(const Rapid_load_context *context, uchar *data) {
  row_id_t rowid{0};
  for (auto index = 0u; index < context->m_table->s->fields; index++) {
    auto fld = *(context->m_table->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto data_len{0u}, extra_offset{0u};
    uchar *data_ptr{nullptr};
    if (Utils::Util::is_blob(fld->type())) {
      data_ptr = const_cast<uchar *>(fld->data_ptr());
      data_len = down_cast<Field_blob *>(fld)->get_length();
    } else {
      extra_offset = Utils::Util::is_varstring(fld->type()) ? (fld->field_length > 256 ? 2 : 1) : 0;
      data_ptr = fld->is_null() ? nullptr : fld->field_ptr() + extra_offset;
      if (fld->is_null()) {
        data_len = UNIV_SQL_NULL;
        data_ptr = nullptr;
      } else {
        if (extra_offset == 1)
          data_len = mach_read_from_1(fld->field_ptr());
        else if (extra_offset == 2)
          data_len = mach_read_from_2_little_endian(fld->field_ptr());
        else
          data_len = fld->pack_length();
      }
    }
    auto active_part = context->m_extra_info.m_active_part_key;
    auto key = active_part.append(":").append(fld->field_name);
    if (!m_fields[key]->write_row(context, data_ptr, data_len)) {
      return HA_ERR_GENERIC;
    }
    rowid = m_fields[key]->header()->m_prows.load(std::memory_order_relaxed) - 1;
  }

  if (context->m_table->s->is_missing_primary_key()) {
    context->m_table->file->position((const uchar *)context->m_table->record[0]);  // to set DB_ROW_ID.
    if (build_index(context, nullptr, rowid)) return HA_ERR_GENERIC;

    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
    }
  } else {
    for (auto index = 0u; index < context->m_table->s->keys; index++) {
      auto key_info = context->m_table->key_info + index;
      if (build_index(context, key_info, rowid)) return HA_ERR_GENERIC;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

}  // namespace Imcs
}  // namespace ShannonBase
