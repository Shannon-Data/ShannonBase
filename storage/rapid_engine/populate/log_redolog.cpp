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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   IMPORTANT: If you chage the redo log format, the recv proc and pop change
   proc should be also changed correspondingly.
*/

#include "storage/rapid_engine/populate/log_redolog.h"
#include <algorithm>
#include <set>

#include "current_thd.h"
#include "sql/table.h"

#include "storage/innobase/include/btr0pcur.h"   //for btr_pcur_t
#include "storage/innobase/include/data0type.h"  //DATA_BLOB, etc.
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/dict0mem.h"  //for dict_index_t, etc.
#include "storage/innobase/include/lob0lob.h"   //ext
#include "storage/innobase/include/log0log.h"
#include "storage/innobase/include/log0test.h"
#include "storage/innobase/include/row0mysql.h"
#include "storage/innobase/include/row0sel.h"
#include "storage/innobase/include/row0upd.h"

#include "storage/rapid_engine/imcs/cu.h"    //cu
#include "storage/rapid_engine/imcs/imcs.h"  //imcs
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/table.h"             //RapidTable
#include "storage/rapid_engine/include/rapid_context.h"  //Rapid_load_context
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
extern int shannon_rpd_async_column_threshold;
namespace Populate {
// to cache the found index_t usd by log parser, and used for next time.
std::unordered_map<uint64, const dict_index_t *> g_index_cache;

// to cache the which tables are processing. in populating queue. In query stage, we will check `g_propagating_tables`
// to find out the rapid table is updated or not. If tables in query statement are still in do populating, then query
// should go to innnodb or go to rapid.
std::shared_mutex g_propagating_table_mutex;
std::multiset<std::string> g_propagating_tables;

// if using dict_index_t->table->get_table_name, it seems to slow, using cache
// to accelerate it.
std::unordered_map<uint64, std::pair<std::string, std::string>> g_index_names;
std::shared_mutex g_index_cache_mutex;

std::unordered_map<std::string, SYS_FIELD_TYPE_ID> current_sys_field_map = {
    {SHANNON_DB_TRX_ID, SYS_FIELD_TYPE_ID::SYS_DB_TRX_ID},
    {SHANNON_DB_ROW_ID, SYS_FIELD_TYPE_ID::SYS_DB_ROW_ID},
    {SHANNON_DB_ROLL_PTR, SYS_FIELD_TYPE_ID::DB_ROLL_PTR}};

alignas(CACHE_LINE_SIZE) SHANNON_THREAD_LOCAL uchar LogParser::mysql_field_data_1[MAX_FIELD_WIDTH] = {0};

ShannonBase::Utils::SimpleRatioAdjuster LogParser::m_adaptive_ratio(0.3);

static inline bool is_system_tablespace(space_id_t space_id) {
  auto is_sys_space = (space_id == 0) ? true : false;
  auto is_dict = (space_id == dict_sys_t::s_dict_space_id) ? true : false;
  auto is_sys_temp =
      fsp_is_global_temporary(space_id) || fsp_is_session_temporary(space_id) || fsp_is_system_temporary(space_id);
  auto is_undo = fsp_is_undo_tablespace(space_id);
  return is_sys_space || is_dict || is_sys_temp || is_undo;
}

uint LogParser::get_trxid(const rec_t *rec, const dict_index_t *index, const ulint *offsets, uchar *trx_id) {
  uint pk_len{0};
  auto trx_id_pos = index->get_sys_col_pos(DATA_TRX_ID);

  for (auto idx = 0; idx < index->n_fields; idx++) {
    auto idx_col = index->get_col(idx);
    if (idx < index->n_uniq) {  // this is part of PK.
      ulint len{0};
      rec_get_nth_field(index, rec, offsets, idx, &len);
      pk_len += len;
    }

    auto col_mask = idx_col->prtype & DATA_SYS_PRTYPE_MASK;
    if (idx_col->mtype == DATA_SYS && col_mask == DATA_TRX_ID) {
      mysql_row_templ_t templ;
      templ.type = DATA_SYS;
      templ.is_virtual = false;
      templ.is_multi_val = false;
      templ.mysql_col_len = DATA_TRX_ID_LEN;
      ulint len{0};
      byte *data = rec_get_nth_field(index, const_cast<rec_t *>(rec), offsets, trx_id_pos, &len);
      ut_a(len == DATA_TRX_ID_LEN);
      row_sel_field_store_in_mysql_format(trx_id, &templ, index, trx_id_pos, data, len, ULINT_UNDEFINED);
      break;
    }
  }

  return pk_len;
}

bool LogParser::check_key_field(std::unordered_map<std::string, ShannonBase::key_meta_t> &keys,
                                const char *field_name) {
  std::string fld_name(field_name);
  for (auto &key : keys) {
    for (auto &fld : key.second.second) {
      size_t pos = fld.rfind(':');
      if (pos != std::string::npos) {
        std::string last_part = fld.substr(pos + 1);
        if (last_part == fld_name) return true;
      } else
        return false;
    }
  }
  return false;
}

int LogParser::store_field_in_mysql_format(const dict_index_t *index, const dict_col_t *col, const byte *dest,
                                           const byte *src, ulint mlen, ulint plen) {
  mysql_row_templ_t templ;
  templ.col_no = col->ind;
  templ.rec_field_no = col->get_phy_pos();
  templ.clust_rec_field_no = col->get_phy_pos();

  dtype_t ctype;
  col->copy_type(&ctype);
  templ.mysql_col_len = mlen;
  templ.type = col->mtype;
  templ.mysql_type = dtype_get_mysql_type(&ctype);

  templ.is_virtual = col->is_virtual();
  templ.is_multi_val = col->is_multi_value();

  if (templ.mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
    templ.mysql_length_bytes = (mlen > 255) ? 2 : 1;
  } else {
    templ.mysql_length_bytes = 1;
  }
  templ.charset = dtype_get_charset_coll(col->prtype);
  templ.is_unsigned = col->prtype & DATA_UNSIGNED;

  // here, we dont care about the mb length.
  templ.mbminlen = col->get_mbminlen();
  templ.mbmaxlen = col->get_mbmaxlen();
  /**
   * Here we dont use col->ind as the index pass into `row_sel_field_store_in_mysql_format`,
   * due to that seems col->ind dont correct. for exm, index of DB_TRX_ID should be 1, but
   * it's 6. so sad. * */
  row_sel_field_store_in_mysql_format(const_cast<byte *>(dest), &templ, index, col->get_col_phy_pos(), src, plen,
                                      ULINT_UNDEFINED);
  return ShannonBase::SHANNON_SUCCESS;
}

byte *LogParser::advance_parseMetadataLog(table_id_t id, uint64_t version, byte *ptr, byte *end) {
  if (ptr + 2 > end) {
    /* At least we should get type byte and another one byte for data,
       if not, it's an incomplete log */
    return nullptr;
  }

  persistent_type_t type = static_cast<persistent_type_t>(ptr[0]);

  ut_ad(dict_persist->persisters != nullptr);

  Persister *persister = dict_persist->persisters->get(type);
  if (persister == nullptr) {
    return ptr;
  }

  ptr++;

  PersistentTableMetadata new_entry{id, version};
  bool corrupt;
  ulint consumed = persister->read(new_entry, ptr, end - ptr, &corrupt);

  if (corrupt) {
    return ptr + consumed;
  }

  if (consumed == 0) {
    return nullptr;
  }

  return ptr + consumed;
}

byte *LogParser::parse_tablespace_redo_rename(byte *ptr, const byte *end, const page_id_t &page_id, ulint parsed_bytes,
                                              bool parse_only [[maybe_unused]]) {
  ut_a(parse_only);
  // in mlog pageid == 0. see `fil_op_write_log`
  /* We never recreate the system tablespace. */
  ut_a(page_id.space() != TRX_SYS_SPACE);

  ut_a(parsed_bytes != ULINT_UNDEFINED);

  /* Where 2 = from name len (uint16_t). */
  if (end <= ptr + 2) {
    return nullptr;
  }

  /* Read and check the RENAME FROM_NAME. */
  ulint from_len = mach_read_from_2(ptr);
  ptr += 2;
  char *from_name = reinterpret_cast<char *>(ptr);

  /* Check if the 'from' file name is valid. */
  if (end < ptr + from_len) {
    return nullptr;
  }

  std::string whats_wrong;
  constexpr char more_than_five[] = "The length must be >= 5.";
  constexpr char end_with_ibd[] = "The file suffix must be '.ibd'.";
  bool err_found{false};
  if (from_len < 5) {
    err_found = true;
    whats_wrong.assign(more_than_five);
  } else {
    std::string name{from_name};

    if (!Fil_path::has_suffix(IBD, name)) {
      err_found = true;
      whats_wrong.assign(end_with_ibd);
    }
  }

  if (err_found) {
    ib::info(ER_IB_MSG_357) << "MLOG_FILE_RENAME: Invalid {from} file name: '" << from_name << "'. " << whats_wrong;

    return nullptr;
  }

  ptr += from_len;
  Fil_path::normalize(from_name);

  /* Read and check the RENAME TO_NAME. */
  ulint to_len = mach_read_from_2(ptr);
  ptr += 2;
  char *to_name = reinterpret_cast<char *>(ptr);

  /* Check if the 'to' file name is valid. */
  if (end < ptr + to_len) {
    return nullptr;
  }

  if (to_len < 5) {
    err_found = true;
    whats_wrong.assign(more_than_five);
  } else {
    std::string name{to_name};

    if (!Fil_path::has_suffix(IBD, name)) {
      err_found = true;
      whats_wrong.assign(end_with_ibd);
    }
  }

  if (err_found) {
    ib::info(ER_IB_MSG_357) << "MLOG_FILE_RENAME: Invalid {to} file name: '" << to_name << "'. " << whats_wrong;

    return nullptr;
  }

  ptr += to_len;
  Fil_path::normalize(to_name);

  /* Update filename with correct partition case, if needed. */
  std::string to_name_str(to_name);
  std::string space_name;

  if (from_len == to_len && strncmp(to_name, from_name, to_len) == 0) {
    ib::error(ER_IB_MSG_360) << "MLOG_FILE_RENAME: The from and to name are the"
                             << " same: '" << from_name << "', '" << to_name << "'";

    return nullptr;
  }

  return ptr;
}

byte *LogParser::parse_tablespace_redo_delete(byte *ptr, const byte *end, const page_id_t &page_id, ulint parsed_bytes,
                                              bool parse_only) {
  ut_a(parse_only);
  /* We never recreate the system tablespace. */
  ut_a(page_id.space() != TRX_SYS_SPACE);

  ut_a(parsed_bytes != ULINT_UNDEFINED);

  /* Where 2 =  len (uint16_t). */
  if (end <= ptr + 2) {
    return nullptr;
  }

  ulint len = mach_read_from_2(ptr);

  ptr += 2;

  /* Do we have the full/valid file name. */
  if (end < ptr + len || len < 5) {
    if (len < 5) {
      char name[6];

      snprintf(name, sizeof(name), "%.*s", (int)len, ptr);

      ib::error(ER_IB_MSG_362) << "MLOG_FILE_DELETE : Invalid file name."
                               << " Length (" << len << ") must be >= 5"
                               << " and end in '.ibd'. File name in the"
                               << " redo log is '" << name << "'";
    }

    return nullptr;
  }

  char *name = reinterpret_cast<char *>(ptr);

  Fil_path::normalize(name);

  ptr += len;

  if (!(Fil_path::has_suffix(IBD, name) || fsp_is_undo_tablespace(page_id.space()))) {
    return nullptr;
  }

  return ptr;
}

byte *LogParser::parse_tablespace_redo_create(byte *ptr, const byte *end, const page_id_t &page_id, ulint parsed_bytes,
                                              bool parse_only) {
  ut_a(parse_only);
  /* We never recreate the system tablespace. */
  ut_a(page_id.space() != TRX_SYS_SPACE);

  ut_a(parsed_bytes != ULINT_UNDEFINED);

  /* Where 6 = flags (uint32_t) + name len (uint16_t). */
  if (end <= ptr + 6) {
    return nullptr;
  }

  ptr += 4;

  ulint len = mach_read_from_2(ptr);

  ptr += 2;

  /* Do we have the full/valid file name. */
  if (end < ptr + len || len < 5) {
    if (len < 5) {
      char name[6];

      snprintf(name, sizeof(name), "%.*s", (int)len, ptr);

      ib::error(ER_IB_MSG_355) << "MLOG_FILE_CREATE : Invalid file name."
                               << " Length (" << len << ") must be >= 5"
                               << " and end in '.ibd'. File name in the"
                               << " redo log is '" << name << "'";
    }

    return nullptr;
  }

  char *name = reinterpret_cast<char *>(ptr);

  Fil_path::normalize(name);

  ptr += len;

  if (!(Fil_path::has_suffix(IBD, name))) {
    return nullptr;
  }

  return ptr;
}

byte *LogParser::parse_tablespace_redo_extend(byte *ptr, const byte *end, const page_id_t &page_id, ulint parsed_bytes,
                                              bool parse_only) {
  ut_a(parse_only);
  /* We never recreate the system tablespace. */
  ut_a(page_id.space() != TRX_SYS_SPACE);

  ut_a(parsed_bytes != ULINT_UNDEFINED);

  /* Check for valid offset and size values */
  if (end < ptr + 16) {
    return nullptr;
  }

  /* Offset within the file to start writing zeros */
  os_offset_t offset [[maybe_unused]] = mach_read_from_8(ptr);
  ptr += 8;

  /* Size of the space which needs to be initialized by
  writing zeros */
  os_offset_t size = mach_read_from_8(ptr);
  ptr += 8;

  if (size == 0) {
    ib::error(ER_IB_MSG_INCORRECT_SIZE) << "MLOG_FILE_EXTEND: Incorrect value for size encountered."
                                        << "Redo log corruption found.";
    return nullptr;
  }

  return ptr;
}

bool LogParser::rec_to_mysql_format(Rapid_load_context *context, mem_heap_t *heap, const rec_t *rec,
                                    const dict_index_t *index, const ulint *offsets, uchar **mysql_rec_out,
                                    size_t *rec_len_out) {
  // 1. calc MySQL record length
  size_t mysql_rec_len = 0;
  for (ulint i = 0; i < index->n_fields; i++) {
    const dict_col_t *col = index->get_col(i);
    mysql_rec_len += col->get_max_size();
    if (col->mtype == DATA_VARCHAR) mysql_rec_len += (col->len > 255) ? 2 : 1;
  }

  // NULL bitmap space.
  mysql_rec_len += UT_BITS_IN_BYTES(index->n_nullable);

  // 2. allocate MySQL format rec buffer.
  uchar *mysql_rec = static_cast<uchar *>(mem_heap_alloc(heap, mysql_rec_len));
  memset(mysql_rec, 0, mysql_rec_len);

  // 3. create row_prebuilt_t
  row_prebuilt_t *prebuilt = row_create_prebuilt(index->table, mysql_rec_len);
  if (!prebuilt) return false;

  // 4. convert
  mem_heap_t *blob_heap = nullptr;
  bool success = row_sel_store_mysql_rec(mysql_rec,  // MySQL format rec
                                         prebuilt,   // prebuilt
                                         rec,        // InnoDB format rec
                                         nullptr,    // vrow
                                         true,       // rec_clust (cluster or not)
                                         index,      // index
                                         index,      // prebuilt
                                         offsets,    // offsets arrary
                                         false,      // clust_templ_for_sec
                                         nullptr,    // lob_undo
                                         blob_heap   // BLOB heap
  );

  if (!success) {
    row_prebuilt_free(prebuilt, true);
    if (blob_heap) mem_heap_free(blob_heap);
    return false;
  }

  // 5. get TRX_ID(if needed)
  ulint trx_id_col = dict_table_get_sys_col_no(index->table, DATA_TRX_ID);
  if (trx_id_col != ULINT_UNDEFINED) {
    // get TRX_ID from MySQL rec
    const uchar *trx_id_ptr = mysql_rec + prebuilt->mysql_prefix_len;
    for (ulint i = 0; i < trx_id_col; i++) {
      trx_id_ptr += prebuilt->mysql_template[i].mysql_col_len;
    }
    context->m_extra_info.m_trxid = mach_read_from_6(trx_id_ptr);
  }

  // 6. results
  *mysql_rec_out = mysql_rec;
  *rec_len_out = mysql_rec_len;

  row_prebuilt_free(prebuilt, true);
  if (blob_heap) mem_heap_free(blob_heap);
  return true;
}

bool LogParser::rec_field_parse(Rapid_load_context *context, mem_heap_t *heap, Imcs::RpdTable *rpd_table,
                                const rec_t *rec, const dict_index_t *index, const ulint *offsets,
                                const dict_index_t *real_index, const ulint *real_offsets, size_t idx,
                                std::mutex &field_mutex, std::unordered_map<std::string, mysql_field_t> &field_values) {
  /**
   * in mlog_parse_index_v1, we can found that it does not bring the true type of that col
   * into, just only use tow types: DATA_BINARY : DATA_FIXBINARY (`parse_index_fields(...)`)
   * and `MLOG_LIST_END_COPY_CREATED` will move some recs to a new page, and this also gen
   *  MLOG_REC_INSERT logs. And, here, we dont care about DB_ROW_ID, DB_ROLL_PTR, ONLY care
   * about `DB_TRX_ID`.
   */
  auto field_name = real_index->get_field(idx) ? real_index->get_field(idx)->name() : nullptr;
  if (!field_name) {
    return false;
  }
  auto fld_type = current_sys_field_map.find(field_name);
  auto is_trx_id = (fld_type != current_sys_field_map.end() && fld_type->second == SYS_FIELD_TYPE_ID::SYS_DB_TRX_ID);
  auto is_row_id = (fld_type != current_sys_field_map.end() && fld_type->second == SYS_FIELD_TYPE_ID::SYS_DB_ROW_ID);
  auto is_roll_ptr = (fld_type != current_sys_field_map.end() && fld_type->second == SYS_FIELD_TYPE_ID::DB_ROLL_PTR);

  // clang-format off
    // due to innodb col has not NOT_SECONDARY property, so check it with IMCS.
    if (is_roll_ptr || (!is_trx_id && !is_row_id)) {
      return true;
    }
  // clang-format on

  /**index parsed from mlog[name:DUMMY_LOG], is not consitent with real_index.*/
  dtype_t col_type;
  real_index->get_col(idx)->copy_type(&col_type);
  dict_col_t *col = (dict_col_t *)index->get_col(idx);
  col->ind = real_index->get_col(idx)->ind;
  col->set_phy_pos(real_index->get_col(idx)->get_phy_pos());
  col->mtype = col_type.mtype;
  col->prtype = col_type.prtype;
  col->len = col_type.len;
  col->mbminmaxlen = col_type.mbminmaxlen;

  mysql_field_t field_value;
  field_value.has_nullbit = col->is_nullable();
  field_value.mtype = col->mtype;

  auto mysql_fld_len = (is_trx_id) ? SHANNON_DATA_DB_TRX_ID_LEN
                                   : ((is_row_id) ? SHANNON_DATA_DB_ROW_ID_LEN
                                                  : 0); /*rpd_table->get_field(field_name)->header()->m_width)*/
  ;
  DBUG_PRINT("parse_rec_fields", ("Extracted field: %s, mtype=%u, len=%lu", field_name, col->mtype, mysql_fld_len));

  std::unique_ptr<uchar[]> mysql_field_data((mysql_fld_len <= MAX_FIELD_WIDTH) ? nullptr : new uchar[mysql_fld_len]);
  uchar *field_data_ptr = (mysql_fld_len <= MAX_FIELD_WIDTH) ? mysql_field_data_1 : mysql_field_data.get();
  ut_a(mysql_fld_len <= MAX_FIELD_WIDTH || mysql_field_data);
  std::memset(field_data_ptr, 0x0, mysql_fld_len <= MAX_FIELD_WIDTH ? MAX_FIELD_WIDTH : mysql_fld_len);

  auto physical_fld_len{0lu};
  auto mtype = real_index->get_col(idx)->mtype;
  auto data = rec_get_nth_field_instant(const_cast<rec_t *>(rec), offsets, idx, index, &physical_fld_len);
  field_value.is_null = (physical_fld_len == UNIV_SQL_NULL) ? true : false;

  if (UNIV_UNLIKELY(rec_offs_nth_extern(index, offsets, idx))) {  // store external.
    // TODO: deal with off-page scenario. see comment at blob0blob.cc:385.
    ut_a(strncmp(field_name, SHANNON_DB_TRX_ID, SHANNON_DB_TRX_ID_LEN));

    ut_a(physical_fld_len >= BTR_EXTERN_FIELD_REF_SIZE);
    ut_a(DATA_LARGE_MTYPE(index->get_col(idx)->mtype));

    rec_offs_make_valid(rec, index, const_cast<ulint *>(offsets));
    ut_ad(rec_offs_validate(rec, index, offsets));

    ulint local_len = physical_fld_len;

    const page_size_t page_size = dict_table_page_size(index->table);
    auto field_ref = const_cast<byte *>(lob::btr_rec_get_field_ref(real_index, rec, real_offsets, idx));
    lob::ref_t blobref(field_ref);
    lob::ref_mem_t mem_obj;
    blobref.parse(mem_obj);

    data = lob::btr_rec_copy_externally_stored_field_func(nullptr, real_index, rec, real_offsets, page_size, idx,
                                                          &local_len, nullptr,
                                                          IF_DEBUG(dict_index_is_sdi(real_index), ) heap, true);
  } else {                                                 // store internal.
    if (UNIV_LIKELY(physical_fld_len != UNIV_SQL_NULL)) {  // Not null
      // 1 : gets the data from innodb format to mysql format.
      if ((mysql_fld_len < physical_fld_len) && (physical_fld_len > MAX_FIELD_WIDTH)) {
        mysql_field_data.reset(new uchar[physical_fld_len]);
        memset(mysql_field_data.get(), 0x0, physical_fld_len);
        mysql_fld_len = physical_fld_len;
        field_data_ptr = mysql_field_data.get();
      }

      // if it's blob type. cp the data directly. ref to: row0sel.cc:2610.[DATA_BLOB]
      if (UNIV_UNLIKELY(mtype == DATA_BLOB)) {
        mysql_fld_len = physical_fld_len;
        std::memcpy(field_data_ptr, data, physical_fld_len);
      } else
        store_field_in_mysql_format(index, col, field_data_ptr, data, mysql_fld_len, physical_fld_len);

      // 2: to set trx id if it is DB_TRX_ID column, then goes to next col.
      if (is_trx_id) {
        context->m_extra_info.m_trxid = mach_read_from_6(field_data_ptr);
        return true;
      }

      // 3: assemble the string type data in mysql format to mysql_field_data.
      if (mtype == DATA_MYSQL || mtype == DATA_VARCHAR || mtype == DATA_VARMYSQL || mtype == DATA_FIXBINARY ||
          mtype == DATA_BINARY) {  // string or text type, then store the string id not the real content.
        auto len_offset{0u};
        if (dtype_get_mysql_type(&col_type) == DATA_MYSQL_TRUE_VARCHAR) {
          len_offset = (mysql_fld_len > 255) ? 2 : 1;
        }

        mysql_fld_len -= len_offset;
        std::memmove(field_data_ptr, field_data_ptr + len_offset, mysql_fld_len);
      }

      field_value.mlength = mysql_fld_len;
      field_value.plength = physical_fld_len;
      if (mysql_field_data.get() == nullptr) {
        mysql_field_data.reset(new uchar[mysql_fld_len]);
        std::memcpy(mysql_field_data.get(), field_data_ptr, mysql_fld_len);
      }
      field_value.data = std::move(mysql_field_data);
    } else {  // if value of this field is null, then set field_data to nullptr.
      mysql_field_data.reset(nullptr);
      field_value.mlength = UNIV_SQL_NULL;
      field_value.data = nullptr;
    }
  }

  {
    std::scoped_lock lock(field_mutex);
    field_values.emplace(field_name, std::move(field_value));
  }

  return true;
}

// identical to parse_rec_field impl, but it used in co-routine.
boost::asio::awaitable<void> LogParser::co_parse_field(Rapid_load_context *context, const rec_t *rec,
                                                       const dict_index_t *index, const ulint *offsets,
                                                       const dict_index_t *real_index, std::mutex &field_mutex,
                                                       size_t idx,
                                                       std::unordered_map<std::string, mysql_field_t> &field_values) {
  // switch async context.
  co_await boost::asio::post(boost::asio::use_awaitable);

  // same with parse_rec_field. [TODO: to check it part table or not]
  auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
  table_id_t table_id = real_index->table->id;
  auto rpd_table = imcs_instance->get_rpd_table(table_id);

  std::unique_ptr<mem_heap_t, decltype(&mem_heap_free)> heap(mem_heap_create(UNIV_PAGE_SIZE, UT_LOCATION_HERE),
                                                             mem_heap_free);
  auto heap_ptr = heap.get();
  auto real_offsets = rec_get_offsets(rec, real_index, nullptr, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap_ptr);

  rec_field_parse(context, heap.get(), rpd_table, rec, index, offsets, real_index, real_offsets, idx, field_mutex,
                  field_values);
  co_return;
}

boost::asio::awaitable<int> LogParser::parse_rec_fields_async(
    Rapid_load_context *context, const rec_t *rec, const dict_index_t *index, const ulint *offsets,
    const dict_index_t *real_index, std::unordered_map<std::string, mysql_field_t> &field_values) {
  auto log_parser_pool = ShannonBase::Imcs::Imcs::pool();
  auto field_mutex = std::make_shared<std::mutex>();
  auto executor = co_await boost::asio::this_coro::executor;
  auto counter = std::make_shared<std::atomic<size_t>>(index->n_fields);
  auto result_promise = std::make_shared<ShannonBase::Utils::shared_promise<int>>();
  auto field_values_ptr = &field_values;

  uint max_batch_sz = m_adaptive_ratio.getMaxBatchSize();
  for (auto idx = 0u; idx < index->n_fields; idx += max_batch_sz) {
    size_t end = std::min(idx + max_batch_sz, index->n_fields);

    boost::asio::co_spawn(
        *log_parser_pool,
        [this, end, context, rec, index, offsets, real_index, field_mutex, idx, field_values_ptr, counter,
         result_promise]() mutable -> boost::asio::awaitable<void> {
          try {
            for (size_t sub_idx = idx; sub_idx < end; ++sub_idx) {
              co_await co_parse_field(context, rec, index, offsets, real_index, *field_mutex, sub_idx,
                                      *field_values_ptr);
              if (counter->fetch_sub(1) == 1) {
                result_promise->set_value(ShannonBase::SHANNON_SUCCESS);
              }
            }
          } catch (...) {
            result_promise->set_value(HA_ERR_GENERIC);
            DBUG_PRINT("parse_rec_fields_async", ("co_parse_field failed."));
            co_return;
          }

          co_return;
        },
        boost::asio::detached);
  }

  int result = co_await result_promise->get_awaitable(executor);
  co_return result;
}

int LogParser::parse_rec_fields(Rapid_load_context *context, const rec_t *rec, const dict_index_t *index,
                                const ulint *offsets, const dict_index_t *real_index,
                                std::unordered_map<std::string, mysql_field_t> &field_values) {
  ut_ad(rec);
  ut_ad(rec_validate(rec, offsets));
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(rec_offs_size(offsets));

  ut_a(offsets);
  ut_a(rec == nullptr || rec_get_n_fields(rec, index) >= rec_offs_n_fields(offsets));

  context->m_extra_info.m_key_buff = std::make_unique<uchar[]>(context->m_extra_info.m_key_len);
  memset(context->m_extra_info.m_key_buff.get(), 0x0, context->m_extra_info.m_key_len);

  auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
  ut_a(imcs_instance);
  auto rpd_table = dict_table_is_partition(real_index->table) ? imcs_instance->get_rpd_parttable(real_index->table->id)
                                                              : imcs_instance->get_rpd_table(real_index->table->id);
  auto field_mutex = std::make_shared<std::mutex>();
  std::unique_ptr<mem_heap_t, decltype(&mem_heap_free)> heap(mem_heap_create(UNIV_PAGE_SIZE, UT_LOCATION_HERE),
                                                             mem_heap_free);
  auto heap_ptr = heap.get();
  auto real_offsets = rec_get_offsets(rec, real_index, nullptr, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap_ptr);
  for (auto idx = 0u; idx < index->n_fields; idx++) {
    /**
     * in mlog_parse_index_v1, we can found that it does not bring the true type of that col
     * into, just only use tow types: DATA_BINARY : DATA_FIXBINARY (`parse_index_fields(...)`)
     * and `MLOG_LIST_END_COPY_CREATED` will move some recs to a new page, and this also gen
     *  MLOG_REC_INSERT logs. And, here, we dont care about DB_ROW_ID, DB_ROLL_PTR, ONLY care
     * about `DB_TRX_ID`.
     */
    memset(mysql_field_data_1, 0x0, MAX_FIELD_WIDTH);
    mem_heap_empty(heap_ptr);

    rec_field_parse(context, heap_ptr, rpd_table, rec, index, offsets, real_index, real_offsets, idx, *field_mutex,
                    field_values);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

row_id_t LogParser::find_matched_row(Rapid_load_context *context, std::map<std::string, key_info_t> &keys) {
  // now that, it use sequentail scan all the chunks to find the match rows.
  // in next, ART will be introduced to accelerate the row finding via index scan.
  return INVALID_ROW_ID;
}

int LogParser::parse_cur_rec_change_apply_low(Rapid_load_context *context, const rec_t *rec, const dict_index_t *index,
                                              const dict_index_t *real_index, const ulint *offsets, mlog_id_t type,
                                              bool all, page_zip_des_t *page_zip, const upd_t *upd, trx_id_t trxid) {
  ut_ad(rec);
  ut_ad(rec_validate(rec, offsets));
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(rec_offs_size(offsets));

  ut_a(offsets);
  ut_a(rec == nullptr || rec_get_n_fields(rec, index) >= rec_offs_n_fields(offsets));

  // only leave nodes.
  if (rec_get_status(rec) == REC_STATUS_NODE_PTR) return ShannonBase::SHANNON_SUCCESS;
#ifdef UNIV_DEBUG_VALGRIND
  {
    const void *rec_start = rec - rec_offs_extra_size(offsets);
    ulint extra_size =
        rec_offs_extra_size(offsets) - (rec_offs_comp(offsets) ? REC_N_NEW_EXTRA_BYTES : REC_N_OLD_EXTRA_BYTES);

    /* All data bytes of the record must be valid. */
    UNIV_MEM_ASSERT_RW(rec, rec_offs_data_size(offsets));
    /* The variable-length header must be valid. */
    UNIV_MEM_ASSERT_RW(rec_start, extra_size);
  }
#endif /* UNIV_DEBUG_VALGRIND */
  auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
  ut_a(imcs_instance);
  auto rpd_tb = dict_table_is_partition(real_index->table) ? imcs_instance->get_rpd_parttable(real_index->table->id)
                                                           : imcs_instance->get_rpd_table(real_index->table->id);
  // no user defined index.
  if (!strncmp(real_index->name(), innobase_index_reserve_name, strlen(innobase_index_reserve_name))) {
    context->m_extra_info.m_key_buff = std::make_unique<uchar[]>(SHANNON_DATA_DB_ROW_ID_LEN);
    context->m_extra_info.m_key_len = SHANNON_DATA_DB_ROW_ID_LEN;
  } else {
    auto key_len = context->m_extra_info.m_key_len;
    context->m_extra_info.m_key_buff = std::make_unique<uchar[]>(key_len);
    context->m_extra_info.m_key_len = key_len;
  }

  auto res{ShannonBase::SHANNON_SUCCESS};
  std::unique_ptr<mem_heap_t, decltype(&mem_heap_free)> heap(mem_heap_create(UNIV_PAGE_SIZE, UT_LOCATION_HERE),
                                                             mem_heap_free);
  auto heap_ptr = heap.get();
  uchar *mysql_rec{nullptr};
  size_t mysql_rec_len{0};
  if (rec_to_mysql_format(context, heap_ptr, rec, index, offsets, &mysql_rec, &mysql_rec_len)) return HA_ERR_GENERIC;

  switch (type) {
    case MLOG_REC_DELETE: {
      if (all) return rpd_tb->delete_rows(context, {});

      auto rowid = rpd_tb->locate_row(context, mysql_rec);
      if (rowid == INVALID_ROW_ID) return HA_ERR_GENERIC;
      res = rpd_tb->delete_row(context, rowid);
    } break;
    case MLOG_REC_INSERT: {
      res = rpd_tb->insert_row(context, mysql_rec);
    } break;
    case MLOG_REC_UPDATE_IN_PLACE: {
      ut_a(upd);
      auto global_row_id = rpd_tb->locate_row(context, mysql_rec);

      // step 1: to parse the changed fields. <changed col id, new_value>
      auto n_cols = rpd_tb->meta().num_columns;
      ShannonBase::Imcs::RowBuffer new_row_data(n_cols);
      new_row_data.copy_from_mysql_fields(context, const_cast<uchar *>(mysql_rec), rpd_tb->meta().fields,
                                          rpd_tb->meta().col_offsets.data(), rpd_tb->meta().null_byte_offsets.data(),
                                          rpd_tb->meta().null_bitmasks.data());

      std::unordered_map<uint32_t, ShannonBase::Imcs::RowBuffer::ColumnValue> updates;
      for (size_t idx = 0; idx < n_cols; idx++) {
        Field *field = rpd_tb->meta().fields[idx].source_fld;

        ptrdiff_t offset = rpd_tb->meta().col_offsets[idx];
        size_t field_length = field->pack_length();

        // comp field is changed or not.
        if (std::memcmp(mysql_rec + offset, mysql_rec + offset, field_length) != 0) {  // record has been changed.
          // read the new value.
          auto col_val = new_row_data.get_column_mutable(idx);
          updates.emplace(idx, std::move(*col_val));
        }
      }

      // step 2: update row.
      res = rpd_tb->update_row(context, global_row_id, updates);
    } break;
    default:
      assert(false);
      break;
  }
  return res;
}

byte *LogParser::parse_cur_and_apply_delete_mark_rec(Rapid_load_context *context, byte *ptr, /*!< in: buffer */
                                                     byte *end_ptr,                          /*!< in: buffer end */
                                                     buf_block_t *block,                     /*!< in: page or NULL */
                                                     dict_index_t *index, /*!< in: record descriptor */
                                                     mtr_t *mtr) {        /*!< in: mtr or NULL */
  ulint pos;
  trx_id_t trx_id;
  roll_ptr_t roll_ptr;
  ulint offset;
  rec_t *rec;
  // may the page in this block not used by any one, it could be evicted.???
  page_t *page = block ? ((buf_frame_t *)block->frame) : nullptr;
  ut_ad(!page || page_is_comp(page) == dict_table_is_comp(index->table));

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  auto flags = mach_read_from_1(ptr);
  ptr++;
  auto val [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;

  ptr = row_upd_parse_sys_vals(ptr, end_ptr, &pos, &trx_id, &roll_ptr);

  if (ptr == nullptr) {
    return (nullptr);
  }

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  offset = mach_read_from_2(ptr);
  ptr += 2;

  ut_a(offset <= UNIV_PAGE_SIZE);

  if (index->table->is_system_schema()) return (ptr);

  if (page) {
    auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
    const dict_index_t *real_tb_index = find_index(index_id, context->m_schema_name, context->m_table_name);
    rec = page + offset;

    /* We do not need to reserve search latch, as the page
    is only being recovered, and there cannot be a hash index to
    it. Besides, these fields are being updated in place
    and the adaptive hash index does not depend on them. */
    // btr_rec_set_deleted_flag(rec, page_zip, val);

    if (!(flags & BTR_KEEP_SYS_FLAG) && real_tb_index) {
      mem_heap_t *heap = nullptr;
      ulint offsets_[REC_OFFS_NORMAL_SIZE];
      rec_offs_init(offsets_);
      context->m_extra_info.m_trxid = trx_id;

      /**
       * If all rows were deleted from a page, and those were not used by any other transaction.
       * This page will be purged, otherwise, it's there.
       */
      auto all = (page[PAGE_HEADER + PAGE_N_HEAP + 1] == PAGE_HEAP_NO_USER_LOW) ? true : false;
      auto offsets = rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
      {
        std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
        g_propagating_tables.emplace(context->m_schema_name + "/" + context->m_table_name);
      }
      parse_cur_rec_change_apply_low(context, rec, index, real_tb_index, offsets, MLOG_REC_DELETE, all, nullptr,
                                     nullptr, trx_id);
      {
        std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
        auto it = g_propagating_tables.find(context->m_schema_name + "/" + context->m_table_name);
        if (it != g_propagating_tables.end()) {
          g_propagating_tables.erase(it);
        }
      }
      if (UNIV_LIKELY_NULL(heap)) {
        mem_heap_free(heap);
      }
    }
  }

  return (ptr);
}

byte *LogParser::parse_cur_and_apply_delete_rec(Rapid_load_context *context, byte *ptr, /*!< in: buffer */
                                                byte *end_ptr,                          /*!< in: buffer end */
                                                buf_block_t *block,                     /*!< in: page or NULL */
                                                dict_index_t *index,                    /*!< in: record descriptor */
                                                mtr_t *mtr) {                           /*!< in: mtr or NULL */

  ulint offset;
  page_cur_t cursor;

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  /* Read the cursor rec offset as a 2-byte ulint */
  offset = mach_read_from_2(ptr);
  ptr += 2;

  ut_a(offset <= UNIV_PAGE_SIZE);

  if (index->table->is_system_schema()) return (ptr);

  if (block) {
    // may the page in this block not used by any one, it could be evicted.???
    page_t *page = ((buf_frame_t *)block->frame);
    ut_ad(!page || page_is_comp(page) == dict_table_is_comp(index->table));

    mem_heap_t *heap = nullptr;
    ulint offsets_[REC_OFFS_NORMAL_SIZE];
    rec_t *rec = page + offset;
    rec_offs_init(offsets_);

    if (page) {
      auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
      const dict_index_t *real_tb_index = find_index(index_id, context->m_schema_name, context->m_table_name);
      if (real_tb_index) {  // was not loaded table and not leaf
        /**
         * If all rows were deleted from a page, and those were not used by any other transaction.
         * This page will be purged, otherwise, it's there.
         */
        auto all = (page[PAGE_HEADER + PAGE_N_HEAP + 1] == PAGE_HEAP_NO_USER_LOW) ? true : false;
        auto offsets = rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
        {
          std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
          g_propagating_tables.emplace(context->m_schema_name + "/" + context->m_table_name);
        }
        parse_cur_rec_change_apply_low(context, rec, index, real_tb_index, offsets, MLOG_REC_DELETE, all, nullptr,
                                       nullptr);
        {
          std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
          auto it = g_propagating_tables.find(context->m_schema_name + "/" + context->m_table_name);
          if (it != g_propagating_tables.end()) {
            g_propagating_tables.erase(it);
          }
        }
      }
    }

    if (UNIV_LIKELY_NULL(heap)) {
      mem_heap_free(heap);
    }
  }

  return (ptr);
}

byte *LogParser::parse_cur_and_apply_insert_rec(Rapid_load_context *context,
                                                bool is_short,            /*!< in: true if short inserts */
                                                const byte *ptr,          /*!< in: buffer */
                                                const byte *end_ptr,      /*!< in: buffer end */
                                                buf_block_t *block,       /*!< in: block or NULL */
                                                page_t *page,             /*!< in: page or NULL */
                                                page_zip_des_t *page_zip, /*!< in: page or NULL */
                                                dict_index_t *index,      /*!< in: record descriptor */
                                                mtr_t *mtr) {             /*!< in: mtr or NULL */

  ulint origin_offset = 0; /* remove warning */
  ulint end_seg_len;
  ulint mismatch_index = 0; /* remove warning */
  rec_t *cursor_rec{nullptr};
  byte buf1[1024];
  byte *buf;
  ulint info_and_status_bits = 0; /* remove warning */
  page_cur_t cursor;
  mem_heap_t *heap = nullptr;
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint *offsets = offsets_;
  const dict_index_t *real_tb_index{nullptr};
  rec_offs_init(offsets_);

  if (is_short) {
    cursor_rec = page_rec_get_prev(page_get_supremum_rec(page));
  } else {
    ulint offset;

    /* Read the cursor rec offset as a 2-byte ulint */

    if (UNIV_UNLIKELY(end_ptr < ptr + 2)) {
      return (nullptr);
    }

    offset = mach_read_from_2(ptr);
    ptr += 2;

    if (page != nullptr) cursor_rec = page + offset;

    if (offset >= UNIV_PAGE_SIZE) {
      return (nullptr);
    }
  }

  end_seg_len = mach_parse_compressed(&ptr, end_ptr);

  if (ptr == nullptr) {
    return (nullptr);
  }

  if (end_seg_len >= UNIV_PAGE_SIZE << 1) {
    return (nullptr);
  }

  if (end_seg_len & 0x1UL) {  // with extra storage.
    /* Read the info bits */

    if (end_ptr < ptr + 1) {
      return (nullptr);
    }

    info_and_status_bits = mach_read_from_1(ptr);
    ptr++;

    origin_offset = mach_parse_compressed(&ptr, end_ptr);

    if (ptr == nullptr) {
      return (nullptr);
    }

    ut_a(origin_offset < UNIV_PAGE_SIZE);

    mismatch_index = mach_parse_compressed(&ptr, end_ptr);

    if (ptr == nullptr) {
      return (nullptr);
    }

    ut_a(mismatch_index < UNIV_PAGE_SIZE);
  }

  if (end_ptr < ptr + (end_seg_len >> 1)) {
    return (nullptr);
  }

  /**real_b_index 0 means it's system dict table, otherwise, users. or the record status is
   * NOT REC_STATUS_ORDINARY, means it can be leave nodes. dict_index_is_spatial(index) not support. */
  std::string db_name, tb_name;
  auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
  real_tb_index = page ? find_index(index_id, db_name, tb_name) : nullptr;
  if (!block || !real_tb_index || !is_data_rec(cursor_rec)) {
    return (const_cast<byte *>(ptr + (end_seg_len >> 1)));
  }

  ut_ad(page_is_comp(page) == dict_table_is_comp(index->table));
  ut_ad(!buf_block_get_page_zip(block) || page_is_comp(page));

  /* Read from the log the inserted index record end segment which  differs from the cursor record */

  if ((end_seg_len & 0x1UL) && mismatch_index == 0) {
    /* This is a record has nothing common to cursor record. */
  } else {
    offsets = rec_get_offsets(cursor_rec, index, offsets, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

    if (!(end_seg_len & 0x1UL)) {
      info_and_status_bits = rec_get_info_and_status_bits(cursor_rec, page_is_comp(page));
      origin_offset = rec_offs_extra_size(offsets);
      mismatch_index = rec_offs_size(offsets) - (end_seg_len >> 1);
    }
  }

  if (UNIV_UNLIKELY(mismatch_index >= UNIV_PAGE_SIZE)) {
    return (const_cast<byte *>(ptr + (end_seg_len >> 1)));
  }

  end_seg_len >>= 1;

  if (mismatch_index + end_seg_len < sizeof buf1) {
    buf = buf1;
  } else {
    buf = static_cast<byte *>(ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, mismatch_index + end_seg_len));
  }

  /* Build the inserted record to buf */
  if (mismatch_index) {
    ut_memcpy(buf, rec_get_start(cursor_rec, offsets), mismatch_index);
  }
  ut_memcpy(buf + mismatch_index, ptr, end_seg_len);

  if (page_is_comp(page)) {
    rec_set_info_and_status_bits(buf + origin_offset, info_and_status_bits);
  } else {
    rec_set_info_bits_old(buf + origin_offset, info_and_status_bits);
  }

  if (rec_get_status(buf + origin_offset) != REC_STATUS_ORDINARY) goto finish;

  offsets = rec_get_offsets(buf + origin_offset, index, offsets, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

  {
    std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
    g_propagating_tables.emplace(db_name + "/" + tb_name);
  }

  {
    auto context = std::make_unique<Rapid_load_context>();
    context->m_schema_name = db_name;
    context->m_table_name = tb_name;
    parse_cur_rec_change_apply_low(context.get(), buf + origin_offset, index, real_tb_index, offsets, MLOG_REC_INSERT,
                                   false, page_zip);
  }

  {
    std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
    auto it = g_propagating_tables.find(db_name + "/" + tb_name);
    if (it != g_propagating_tables.end()) {
      g_propagating_tables.erase(it);
    }
  }
finish:
  if (buf != buf1) {
    ut::free(buf);
  }

  if (UNIV_LIKELY_NULL(heap)) {
    mem_heap_free(heap);
  }

  return (const_cast<byte *>(ptr + end_seg_len));
}

byte *LogParser::parse_and_apply_upd_rec_in_place(
    Rapid_load_context *context,    /*!< in: context */
    rec_t *rec,                     /*!< in/out: record where replaced */
    const dict_index_t *index,      /*!< in: the index the record belongs to */
    const dict_index_t *real_index, /*!< in: the index the record belongs to */
    const ulint *offsets,           /*!< in: array returned by rec_get_offsets() */
    const upd_t *update,            /*!< in: update vector */
    page_zip_des_t *page_zip,       /*!< in: compressed page with enough space
                                       available, or NULL */
    trx_id_t trx_id) {
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(!index->table->skip_alter_undo);
  {
    std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
    g_propagating_tables.emplace(context->m_schema_name + "/" + context->m_table_name);
  }
  auto ret = parse_cur_rec_change_apply_low(context, rec, index, real_index, offsets, MLOG_REC_UPDATE_IN_PLACE, false,
                                            page_zip, update, trx_id);
  {
    std::unique_lock<std::shared_mutex> lk(g_propagating_table_mutex);
    auto it = g_propagating_tables.find(context->m_schema_name + "/" + context->m_table_name);
    if (it != g_propagating_tables.end()) {
      g_propagating_tables.erase(it);
    }
  }
  /*now, we dont want to support zipped page now. how to deal with pls ref to:
    page_zip_write_rec(page_zip, rec, index, offsets, 0); */
  if (ret) return nullptr;

  return rec;
}

byte *LogParser::parse_cur_update_in_place_and_apply(Rapid_load_context *context, byte *ptr, /*!< in: buffer */
                                                     byte *end_ptr,                          /*!< in: buffer end */
                                                     buf_block_t *block,                     /*!< in: block or NULL */
                                                     page_t *page,                           /*!< in: page or NULL */
                                                     page_zip_des_t *page_zip, /*!< in/out: compressed page, or NULL */
                                                     dict_index_t *index) {    /*!< in: index corresponding to page */
  ulint flags{0};
  rec_t *rec{nullptr};
  upd_t *update{nullptr};
  ulint pos{0};
  trx_id_t trx_id{0};
  roll_ptr_t roll_ptr{0};
  ulint rec_offset{0};
  mem_heap_t *heap{nullptr};
  ulint *offsets{nullptr};
  uint64_t index_id{0};
  const dict_index_t *tb_index{nullptr};
  std::string db_name, tb_name;

  if (end_ptr < ptr + 1) {
    return (nullptr);
  }

  flags = mach_read_from_1(ptr);
  ptr++;

  ptr = row_upd_parse_sys_vals(ptr, end_ptr, &pos, &trx_id, &roll_ptr);

  if (ptr == nullptr) {
    return (nullptr);
  }

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  rec_offset = mach_read_from_2(ptr);
  ptr += 2;

  ut_a(rec_offset <= UNIV_PAGE_SIZE);

  heap = mem_heap_create(256, UT_LOCATION_HERE);

  ptr = row_upd_index_parse(ptr, end_ptr, heap, &update);
  if (!ptr || !page) {
    goto func_exit;
  }

  if (index->table->is_system_schema()) goto func_exit;

  index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
  tb_index = find_index(index_id, db_name, tb_name);
  if (!tb_index) goto func_exit;

  ut_a(page_is_comp(page) == dict_table_is_comp(index->table));
  rec = page + rec_offset;

  if (!(flags & BTR_KEEP_SYS_FLAG)) {
    /*trx_id here is a new trxid , here, we have diff approach. do nothing for
      trxid and roll_ptr. row_upd_rec_sys_fields_in_recovery
    */
  }

  context->m_extra_info.m_trxid = trx_id;
  context->m_schema_name = db_name;
  context->m_table_name = tb_name;

  // get field length from rapid
  offsets = rec_get_offsets(rec, index, nullptr, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
  parse_and_apply_upd_rec_in_place(context, rec, index, tb_index, offsets, update, page_zip, trx_id);
func_exit:
  mem_heap_free(heap);

  return (ptr);
}

/** Parses a log record of copying a record list end to a new created page.
 @return end of log record or NULL */
byte *LogParser::parse_copy_rec_list_to_created_page(Rapid_load_context *context, byte *ptr, /*!< in: buffer */
                                                     byte *end_ptr,                          /*!< in: buffer end */
                                                     buf_block_t *block,                     /*!< in: block or NULL */
                                                     page_t *page,                           /*!< in: page or NULL */
                                                     page_zip_des_t *page_zip,               /*!< in: page or NULL */
                                                     dict_index_t *index, /*!< in: record descriptor */
                                                     mtr_t *mtr) {        /*!< in: mtr or NULL */

  byte *rec_end;
  ulint log_data_len;
  ut_a(ptr);

  if (ptr + 4 > end_ptr) {
    return (nullptr);
  }

  log_data_len = mach_read_from_4(ptr);
  ptr += 4;

  rec_end = ptr + log_data_len;

  if (rec_end > end_ptr) {
    return (nullptr);
  }

  if (!block) {
    return (rec_end);
  }

  while (ptr < rec_end) {
    ptr = parse_cur_and_apply_insert_rec(context, true, ptr, end_ptr, block, page, page_zip, index, mtr);
  }

  ut_a(ptr == rec_end);
  return (rec_end);
}

/** Parses a redo log record of reorganizing a page.
 @return end of log record or NULL */
byte *LogParser::parse_btr_page_reorganize(byte *ptr,           /*!< in: buffer */
                                           byte *end_ptr,       /*!< in: buffer end */
                                           dict_index_t *index, /*!< in: record descriptor */
                                           bool compressed,     /*!< in: true if compressed page */
                                           buf_block_t *block,  /*!< in: page to be reorganized, or NULL */
                                           mtr_t *mtr) {        /*!< in: mtr or NULL */
  ulint level;

  ut_ad(index != nullptr);

  /* If dealing with a compressed page the record has the
  compression level used during original compression written in
  one byte. Otherwise record is empty. */
  if (compressed) {
    if (ptr == end_ptr) {
      return (nullptr);
    }

    level = mach_read_from_1(ptr);

    ut_a(level <= 9);
    ++ptr;
  } else {
    level = page_zip_level;
  }

  if (block != nullptr) {
    // do nothing in pop thread, just advance the pointer.
  }

  return (ptr);
}

byte *LogParser::parse_btr_cur_del_mark_set_sec_rec(byte *ptr,     /*!< in: buffer */
                                                    byte *end_ptr, /*!< in: buffer end */
                                                    page_t *page,  /*!< in/out: page or NULL */
                                                    page_zip_des_t *page_zip) {
  if (end_ptr < ptr + 3) {
    return (nullptr);
  }

  auto val [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;

  auto offset = mach_read_from_2(ptr);
  ptr += 2;

  // ut_a(offset <= UNIV_PAGE_SIZE);

  if (page) {
    auto rec [[maybe_unused]] = page + offset;

    /* We do not need to reserve search latch, as the page
    is only being recovered, and there cannot be a hash index to
    it. Besides, the delete-mark flag is being updated in place
    and the adaptive hash index does not depend on it. */

    // btr_rec_set_deleted_flag(rec, page_zip, val);
  }

  return (ptr);
}

/** Parses a redo log record of adding an undo log record.
 @return end of log record or NULL */
byte *LogParser::parse_trx_undo_add_undo_rec(byte *ptr,      /*!< in: buffer */
                                             byte *end_ptr,  /*!< in: buffer end */
                                             page_t *page) { /*!< in: page or NULL */
  ulint len;

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  len = mach_read_from_2(ptr);
  ptr += 2;

  if (end_ptr < ptr + len) {
    return (nullptr);
  }

  if (page == nullptr) {
    return (ptr + len);
  }

  return (ptr + len);
}

byte *LogParser::parse_page_header(mlog_id_t type, const byte *ptr, const byte *end_ptr, page_t *page, mtr_t *mtr) {
  trx_id_t trx_id [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);

  if (ptr != nullptr && page != nullptr) {
    switch (type) {
      case MLOG_UNDO_HDR_CREATE:
        return (const_cast<byte *>(ptr));
      case MLOG_UNDO_HDR_REUSE:
        return (const_cast<byte *>(ptr));
      default:
        break;
    }
    ut_d(ut_error);
  }

  return (const_cast<byte *>(ptr));
}

byte *LogParser::advance_mlog_parse_nbytes(mlog_id_t type,      /*!< in: log record type: MLOG_1BYTE, ... */
                                           const byte *ptr,     /*!< in: buffer */
                                           const byte *end_ptr, /*!< in: buffer end */
                                           byte *page,          /*!< in: page where to apply the log
                                                                record, or NULL */
                                           void *page_zip)      /*!< in/out: compressed page, or NULL */
{
  ulint offset;

  ut_a(type <= MLOG_8BYTES);
  ut_a(!page || !page_zip || !fil_page_index_page_check(page));

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  offset = mach_read_from_2(ptr);
  ptr += 2;

  if (offset >= UNIV_PAGE_SIZE) {
    return (nullptr);
  }

  if (type == MLOG_8BYTES) {
    mach_u64_parse_compressed(&ptr, end_ptr);  // just advance the pointer.
    return (ptr == nullptr) ? (nullptr) : const_cast<byte *>(ptr);
  }

  mach_parse_compressed(&ptr, end_ptr);

  if (ptr == nullptr) {
    return (nullptr);
  }
  return const_cast<byte *>(ptr);
}

byte *LogParser::parse_or_apply_log_rec_body(Rapid_load_context *context, mlog_id_t type, byte *ptr, byte *end_ptr,
                                             space_id_t space_id, page_no_t page_no, buf_block_t *block, mtr_t *mtr,
                                             lsn_t start_lsn) {
  /*same as the recv_add_to_hash_table does. dont not add the the system opers
    mlogs when pop thread has been started. such as mlogs of `LOG_DUMMY` table.
  */
  switch (type) {
#ifndef UNIV_HOTBACKUP
    case MLOG_FILE_DELETE:

      return parse_tablespace_redo_delete(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_CREATE:

      return parse_tablespace_redo_create(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_RENAME:

      return parse_tablespace_redo_rename(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_EXTEND:

      return parse_tablespace_redo_extend(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);
#else  /* !UNIV_HOTBACKUP */
      // Mysqlbackup does not execute file operations. It cares for all
      // files to be at their final places when it applies the redo log.
      // The exception is the restore of an incremental_with_redo_log_only
      // backup.
    case MLOG_FILE_DELETE:

      return parse_tablespace_redo_delete(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_CREATE:

      return parse_tablespace_redo_create(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_RENAME:

      return parse_tablespace_redo_rename(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);

    case MLOG_FILE_EXTEND:

      return parse_tablespace_redo_extend(ptr, end_ptr, page_id_t(space_id, page_no), 0, true);
#endif /* !UNIV_HOTBACKUP */

    case MLOG_INDEX_LOAD:
#ifdef UNIV_HOTBACKUP
      // While scanning redo logs during a backup operation a
      // MLOG_INDEX_LOAD type redo log record indicates, that a DDL
      // (create index, alter table...) is performed with
      // 'algorithm=inplace'. The affected tablespace must be re-copied
      // in the backup lock phase. Record it in the index_load_list.
      if (!recv_recovery_on) {
        index_load_list.emplace_back(std::pair<space_id_t, lsn_t>(space_id, recv_sys->recovered_lsn));
      }
#endif /* UNIV_HOTBACKUP */
      if (end_ptr < ptr + 8) {
        return nullptr;
      }

      return ptr + 8;

    default:
      break;
  }

  page_t *page{nullptr};
  page_zip_des_t *page_zip{nullptr};
  dict_index_t *index{nullptr};
  [[maybe_unused]] page_type_t page_type{FIL_PAGE_TYPE_ALLOCATED};

  /**
   * Here, the page perhaps reomved when delete all records opers delivered.
   * a blank page got. we also can get the page from mtr's memo because if we in async mode,
   * the trx may be committed or rollback, does not existed anymore.
   */
  class BlockGuard {
   public:
    BlockGuard(buf_block_t *block) : m_block(block) {}
    ~BlockGuard() {
      if (m_block) buf_block_unfix(m_block);
    }
    buf_block_t *m_block;
  };

  block = get_block(space_id, page_no);
  BlockGuard block_gaurd(block);
  if (block) {  // page_no != 0;
    ut_ad(buf_page_in_file(&block->page));

    buf_block_fix(block);
    page = block ? buf_block_get_frame(block) : nullptr;
    page_type = page ? fil_page_get_type(page) : FIL_PAGE_TYPE_ALLOCATED;

    page_zip = buf_block_get_page_zip(block);
    // TODO: make a copy of page and page_zip. after the page and page_zip parsed, free them.
  } else {
    DBUG_PRINT("ib_log the bloc is nullptr", ("type:%s", get_mlog_string(type)));
  }

  switch (type) {
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
      /* The LSN is checked in recv_parse_log_rec(). */
      break;
#endif /* UNIV_LOG_LSN_DEBUG */
    case MLOG_4BYTES:

      ut_ad(page == nullptr || end_ptr > ptr + 2);

      /* Most FSP flags can only be changed by CREATE or ALTER with
      ALGORITHM=COPY, so they do not change once the file
      is created. The SDI flag is the only one that can be
      changed by a recoverable transaction. So if there is
      change in FSP flags, update the in-memory space structure
      (fil_space_t) */

      if (page != nullptr && page_no == 0 && mach_read_from_2(ptr) == FSP_HEADER_OFFSET + FSP_SPACE_FLAGS) {
        ptr = const_cast<byte *>(advance_mlog_parse_nbytes(MLOG_4BYTES, ptr, end_ptr, page, page_zip));

        /* When applying log, we have complete records.
        They can be incomplete (ptr=nullptr) only during
        scanning (page==nullptr) */
        ut_ad(ptr != nullptr);
        break;
      }

      [[fallthrough]];

    case MLOG_1BYTE:
      /* If 'ALTER TABLESPACE ... ENCRYPTION' was in progress and page 0 has
      REDO entry for this, now while applying this entry, set
      encryption_op_in_progress flag now so that any other page of this
      tablespace in redo log is written accordingly. */
      if (page_no == 0 && page != nullptr && end_ptr >= ptr + 2) {
        ulint offs = mach_read_from_2(ptr);

        fil_space_t *space = fil_space_acquire(space_id);
        ut_ad(space != nullptr);
        ulint offset = fsp_header_get_encryption_progress_offset(page_size_t(space->flags));

        if (offs == offset) {
          ptr = const_cast<byte *>(advance_mlog_parse_nbytes(MLOG_1BYTE, ptr, end_ptr, page, page_zip));
          // ignore the opers, just advance the pointer.
        }
        fil_space_release(space);
      }

      [[fallthrough]];

    case MLOG_2BYTES:
    case MLOG_8BYTES:

      ptr = const_cast<byte *>(advance_mlog_parse_nbytes(type, ptr, end_ptr, nullptr, page_zip));
      break;

    case MLOG_REC_INSERT:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));
        ptr = parse_cur_and_apply_insert_rec(context, false, ptr, end_ptr, block, page, page_zip, index, mtr);
      }
      break;

    case MLOG_REC_INSERT_8027:
    case MLOG_COMP_REC_INSERT_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = const_cast<byte *>(mlog_parse_index_8027(ptr, end_ptr, type == MLOG_COMP_REC_INSERT_8027, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_insert_rec(context, false, ptr, end_ptr, block, page, page_zip, index, mtr);
      }
      break;

    case MLOG_REC_CLUST_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_delete_mark_rec(context, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_REC_CLUST_DELETE_MARK_8027:
    case MLOG_COMP_REC_CLUST_DELETE_MARK_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(
                          mlog_parse_index_8027(ptr, end_ptr, type == MLOG_COMP_REC_CLUST_DELETE_MARK_8027, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_delete_mark_rec(context, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_COMP_REC_SEC_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type));

      /* This log record type is obsolete, but we process it for
      backward compatibility with MySQL 5.0.3 and 5.0.4. */

      ut_a(!page || page_is_comp(page));
      ut_a(!page_zip);

      ptr = const_cast<byte *>(mlog_parse_index_8027(ptr, end_ptr, true, &index));

      if (ptr == nullptr) {
        break;
      }

      [[fallthrough]];

    case MLOG_REC_SEC_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = parse_btr_cur_del_mark_set_sec_rec(ptr, end_ptr, page, page_zip);
      break;

    case MLOG_REC_UPDATE_IN_PLACE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_update_in_place_and_apply(context, ptr, end_ptr, block, page, page_zip, index);
      }

      break;

    case MLOG_REC_UPDATE_IN_PLACE_8027:
    case MLOG_COMP_REC_UPDATE_IN_PLACE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(
                          mlog_parse_index_8027(ptr, end_ptr, type == MLOG_COMP_REC_UPDATE_IN_PLACE_8027, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_update_in_place_and_apply(context, ptr, end_ptr, block, page, page_zip, index);
      }

      break;

    case MLOG_LIST_END_DELETE:
    case MLOG_LIST_START_DELETE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        /* Read the record offset as a 2-byte ulint */
        if (end_ptr < ptr + 2) {
          return nullptr;
        }
        ptr += 2;
      }

      break;

    case MLOG_LIST_END_DELETE_8027:
    case MLOG_COMP_LIST_END_DELETE_8027:
    case MLOG_LIST_START_DELETE_8027:
    case MLOG_COMP_LIST_START_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = const_cast<byte *>(mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_LIST_END_DELETE_8027 || type == MLOG_COMP_LIST_START_DELETE_8027,
               &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));
        /* Read the record offset as a 2-byte ulint */
        if (end_ptr < ptr + 2) {
          return nullptr;
        }

        ptr += 2;
      }

      break;

    case MLOG_LIST_END_COPY_CREATED:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_copy_rec_list_to_created_page(context, ptr, end_ptr, block, page, page_zip, index, mtr);
      }

      break;

    case MLOG_LIST_END_COPY_CREATED_8027:
    case MLOG_COMP_LIST_END_COPY_CREATED_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(
                          mlog_parse_index_8027(ptr, end_ptr, type == MLOG_COMP_LIST_END_COPY_CREATED_8027, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_copy_rec_list_to_created_page(context, ptr, end_ptr, block, page, page_zip, index, mtr);
      }

      break;

    case MLOG_PAGE_REORGANIZE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_btr_page_reorganize(ptr, end_ptr, index, type == MLOG_ZIP_PAGE_REORGANIZE_8027, block, mtr);
      }

      break;

    case MLOG_PAGE_REORGANIZE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));
      /* Uncompressed pages don't have any payload in the
      MTR so ptr and end_ptr can be, and are nullptr */
      mlog_parse_index_8027(ptr, end_ptr, false, &index);
      ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

      ptr = parse_btr_page_reorganize(ptr, end_ptr, index, false, block, mtr);

      break;

    case MLOG_ZIP_PAGE_REORGANIZE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_btr_page_reorganize(ptr, end_ptr, index, true, block, mtr);
      }

      break;

    case MLOG_COMP_PAGE_REORGANIZE_8027:
    case MLOG_ZIP_PAGE_REORGANIZE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index_8027(ptr, end_ptr, true, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_btr_page_reorganize(ptr, end_ptr, index, type == MLOG_ZIP_PAGE_REORGANIZE_8027, block, mtr);
      }
      break;

    case MLOG_PAGE_CREATE:
    case MLOG_COMP_PAGE_CREATE:
    case MLOG_PAGE_CREATE_RTREE:
    case MLOG_COMP_PAGE_CREATE_RTREE:
    case MLOG_PAGE_CREATE_SDI:
    case MLOG_COMP_PAGE_CREATE_SDI:
      break;

    case MLOG_UNDO_INSERT:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = parse_trx_undo_add_undo_rec(ptr, end_ptr, page);
      break;

    case MLOG_UNDO_ERASE_END:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      break;

    case MLOG_UNDO_INIT:
      /* Allow anything in page_type when creating a page. */
      mach_parse_compressed((const byte **)&ptr, end_ptr);
      if (ptr == nullptr) {
        return nullptr;
      }

      break;

    case MLOG_UNDO_HDR_CREATE:
    case MLOG_UNDO_HDR_REUSE:
      // just only advance the pointer.
      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = parse_page_header(type, ptr, end_ptr, page, mtr);

      break;

    case MLOG_REC_MIN_MARK:
    case MLOG_COMP_REC_MIN_MARK:

      /* On a compressed page, MLOG_COMP_REC_MIN_MARK
      will be followed by MLOG_COMP_REC_DELETE
      or MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_nullptr)
      in the same mini-transaction. */
      if (end_ptr < ptr + 2) {
        return nullptr;
      }
      ptr += 2;

      break;

    case MLOG_REC_DELETE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_delete_rec(context, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_REC_DELETE_8027:
    case MLOG_COMP_REC_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = const_cast<byte *>(mlog_parse_index_8027(ptr, end_ptr, type == MLOG_COMP_REC_DELETE_8027, &index)))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_delete_rec(context, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_IBUF_BITMAP_INIT:
      /* Allow anything in page_type when creating a page. */
      // do nothing.
      break;

    case MLOG_INIT_FILE_PAGE:
    case MLOG_INIT_FILE_PAGE2:

      /* For clone, avoid initializing page-0. Page-0 should already have been
      initialized. This is to avoid erasing encryption information. We cannot
      update encryption information later with redo logged information for
      clone. Please check comments in MLOG_WRITE_STRING. */
      // here, do nothing.
      break;

    case MLOG_WRITE_STRING:

      ut_ad(!page || page_type != FIL_PAGE_TYPE_ALLOCATED || page_no == 0);
      ptr = const_cast<byte *>(mlog_parse_string(ptr, end_ptr, nullptr, page_zip));
      break;

    case MLOG_ZIP_WRITE_NODE_PTR: {
      ut_ad(!page || fil_page_type_is_index(page_type));

      /*all come from page_zip_parse_write_node_ptr with creating the page,
        just advance the ptr.
      */
      if (UNIV_UNLIKELY(end_ptr < ptr + (2 + 2 + REC_NODE_PTR_SIZE))) {
        return nullptr;
      }

      auto offset = mach_read_from_2(ptr);
      auto z_offset = mach_read_from_2(ptr + 2);

      if (offset < PAGE_ZIP_START || offset >= UNIV_PAGE_SIZE || z_offset >= UNIV_PAGE_SIZE) {
        // corrupt log
        return nullptr;
      }
      ptr = (ptr + (2 + 2 + REC_NODE_PTR_SIZE));
      break;
    }

    case MLOG_ZIP_WRITE_BLOB_PTR:

      ut_ad(!page || fil_page_type_is_index(page_type));

      // just advance the ptr, do nothing. no need to pop to rapid.
      ptr = (ptr + (2 + 2 + BTR_EXTERN_FIELD_REF_SIZE));

      break;

    case MLOG_ZIP_WRITE_HEADER: {
      ut_ad(!page || fil_page_type_is_index(page_type));
      ut_ad(!page == !page_zip);

      if (UNIV_UNLIKELY(end_ptr < ptr + (1 + 1))) {
        return nullptr;
      }

      auto offset = (ulint)*ptr++;
      auto len = (ulint)*ptr++;

      if (len == 0 || offset + len >= PAGE_DATA) {
        // corrupt log
        return nullptr;
      }

      if (end_ptr < ptr + len) {
        return nullptr;
      }

      ptr = (ptr + len);
    } break;

    case MLOG_ZIP_PAGE_COMPRESS: {
      /* Allow anything in page_type when creating a page. */
      // ptr = page_zip_parse_compress(ptr, end_ptr, page, page_zip);
      if (UNIV_UNLIKELY(ptr + (2 + 2) > end_ptr)) {
        return nullptr;
      }

      auto size = mach_read_from_2(ptr);
      ptr += 2;
      auto trailer_size = mach_read_from_2(ptr);
      ptr += 2;

      if (UNIV_UNLIKELY(ptr + 8 + size + trailer_size > end_ptr)) {
        return nullptr;
      }
      ptr = (ptr + 8 + size + trailer_size);
      break;
    }

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index(ptr, end_ptr, &index)))) {
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table)));
        if (end_ptr == ptr) {
          return nullptr;
        }

        ptr = (ptr + 1);
      }
      break;

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027:

      if (nullptr != (ptr = const_cast<byte *>(mlog_parse_index_8027(ptr, end_ptr, true, &index)))) {
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table)));

        if (end_ptr == ptr) {
          return nullptr;
        }

        ptr = (ptr + 1);
      }
      break;

    case MLOG_TEST:
      ut_a(false);
#ifndef UNIV_HOTBACKUP
      if (log_test != nullptr) {
        ptr = const_cast<byte *>(log_test->parse_mlog_rec(ptr, end_ptr));
      } else {
        /* Just parse and ignore record to pass it and go forward. Note that
        this record is also used in the innodb.log_first_rec_group mtr test.
        The record is written in the buf0flu.cc when flushing page in that
        case. */
        Log_test::Key key;
        Log_test::Value value;
        lsn_t start_lsn, end_lsn;

        ptr = const_cast<byte *>(Log_test::parse_mlog_rec(ptr, end_ptr, key, value, start_lsn, end_lsn));
      }
      break;
#endif /* !UNIV_HOTBACKUP */
      /* Fall through. */

    default:
      ptr = nullptr;
  }

  if (index != nullptr) {
    dict_table_t *table = index->table;

    dict_mem_index_free(index);
    dict_mem_table_free(table);
  }

  return ptr;
}

ulint LogParser::parse_log_rec(Rapid_load_context *context, mlog_id_t *type, byte *ptr, byte *end_ptr,
                               space_id_t *space_id, page_no_t *page_no, byte **body) {
  byte *new_ptr;

  *body = nullptr;

  UNIV_MEM_INVALID(type, sizeof *type);
  UNIV_MEM_INVALID(space_id, sizeof *space_id);
  UNIV_MEM_INVALID(page_no, sizeof *page_no);
  UNIV_MEM_INVALID(body, sizeof *body);

  if (ptr == end_ptr) {
    return 0;
  }
  switch (*ptr) {
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN | MLOG_SINGLE_REC_FLAG:
    case MLOG_LSN:

      new_ptr = mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no);

      if (new_ptr != nullptr) {
        const lsn_t lsn = static_cast<lsn_t>(*space_id) << 32 | *page_no;
      }

      *type = MLOG_LSN;
      return new_ptr == nullptr ? 0 : new_ptr - ptr;
#endif /* UNIV_LOG_LSN_DEBUG */

    case MLOG_MULTI_REC_END:
    case MLOG_DUMMY_RECORD:
      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;
      *type = static_cast<mlog_id_t>(*ptr);
      return 1;

    case MLOG_MULTI_REC_END | MLOG_SINGLE_REC_FLAG:
    case MLOG_DUMMY_RECORD | MLOG_SINGLE_REC_FLAG:
      // found_corrupt_log; then, now, we skip this mlogs.
      ut_a(false);
      return 0;

    case MLOG_TABLE_DYNAMIC_META:
    case MLOG_TABLE_DYNAMIC_META | MLOG_SINGLE_REC_FLAG:
      table_id_t id;
      uint64_t version;

      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;

      new_ptr = const_cast<byte *>(mlog_parse_initial_dict_log_record(ptr, end_ptr, type, &id, &version));

      if (new_ptr != nullptr) {
        new_ptr = advance_parseMetadataLog(id, version, new_ptr, end_ptr);
      }

      return new_ptr == nullptr ? 0 : new_ptr - ptr;
  }

  new_ptr = const_cast<byte *>(mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no));

  *body = new_ptr;
  if (new_ptr == nullptr) return 0;

  /**
   * different with recovery, in recovery, delete[all]/drop opreation, can found the corresponding page
   * with page_no., but in rapid population stage, delete[all]/drop operations is proceeded sucessfully,
   * thereby, her we may get a blank page via specific page_no. So we treat the blank page as all flag of
   * delete/drop operation.
   */
  new_ptr = parse_or_apply_log_rec_body(context, *type, new_ptr, end_ptr, *space_id, *page_no, nullptr, nullptr, 0);

  if (new_ptr == nullptr) {  // skip the current mlog, go to next. means this corruption.
    return (end_ptr - ptr);
  }

  return new_ptr - ptr;
}

ulint LogParser::parse_single_rec(Rapid_load_context *context, byte *ptr, byte *end_ptr) {
  /* Try to parse a log record, fetching its type, space id,
  page no, and a pointer to the body of the log record */

  mlog_id_t type{MLOG_BIGGEST_TYPE};
  byte *body{nullptr};
  page_no_t page_no{0};
  space_id_t space_id{0};

  auto ret = parse_log_rec(context, &type, ptr, end_ptr, &space_id, &page_no, &body);
  return ret;
}

ulint LogParser::parse_multi_rec(Rapid_load_context *context, byte *ptr, byte *end_ptr) {
  ut_a(end_ptr >= ptr);
  ulint parsed_bytes{0}, n_recs{0};

  space_id_t space_id = 0;
  for (;;) {
    mlog_id_t type = MLOG_BIGGEST_TYPE;
    byte *body;
    page_no_t page_no = 0;

    ulint len = parse_log_rec(context, &type, ptr, end_ptr, &space_id, &page_no, &body);
    if (len == 0) {
      return parsed_bytes;
    } else if ((*ptr & MLOG_SINGLE_REC_FLAG)) {
      ut_a(false);
      // report_corrupt_log(ptr, type, space_id, page_no);
      return parsed_bytes;
    }

    parsed_bytes += len;
    ptr += len;
    ++n_recs;

    switch (type) {
      case MLOG_MULTI_REC_END:
        /* Found the end mark for the records */
        return parsed_bytes;

#ifdef UNIV_LOG_LSN_DEBUG
      case MLOG_LSN:
        /* Do not add these records to the hash table.
        The page number and space id fields are misused
        for something else. */
        break;
#endif /* UNIV_LOG_LSN_DEBUG */

      case MLOG_FILE_DELETE:
      case MLOG_FILE_CREATE:
      case MLOG_FILE_RENAME:
      case MLOG_FILE_EXTEND:
      case MLOG_TABLE_DYNAMIC_META:
        /* case MLOG_TRUNCATE: Disabled for WL6378 */
        /* These were already handled by
        recv_parse_or_apply_log_rec_body(). */
        break;

      default:
        break;
    }
  }

  return parsed_bytes;
}

// handle single mtr
uint LogParser::parse_redo(Rapid_load_context *context, byte *ptr, byte *end_ptr) {
  /**
   * after secondary_load command excuted, all the data read from data file. the
   * last checkpoint lsn makes all the data lsn is samller than it were written
   * to data file.
   */
  if (ptr == end_ptr) {
    return 0;
  }

  bool single_rec;
  switch (*ptr) {
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
#endif /* UNIV_LOG_LSN_DEBUG */
    case MLOG_DUMMY_RECORD:
      single_rec = true;
      break;
    default:
      single_rec = !!(*ptr & MLOG_SINGLE_REC_FLAG);
  }

  uint32 space_id = mach_read_from_4(ptr + 1);  // lay out[type + sapce_id + page_id.]
  if (is_system_tablespace(space_id))           // skip it, go to next. dont parse the body.
    return (std::ptrdiff_t(end_ptr - ptr));

  return (single_rec) ? parse_single_rec(context, ptr, end_ptr) : parse_multi_rec(context, ptr, end_ptr);
}
}  // namespace Populate
}  // namespace ShannonBase