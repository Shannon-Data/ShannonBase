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

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/

#include "storage/rapid_engine/populate/log_parser.h"

#include "current_thd.h"

#include "storage/innobase/include/btr0pcur.h"  //for btr_pcur_t
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/dict0mem.h"  //for dict_index_t, etc.
#include "storage/innobase/include/ibuf0ibuf.h"
#include "storage/innobase/include/log0log.h"
#include "storage/innobase/include/log0test.h"
#include "storage/innobase/include/log0write.h"
#include "storage/innobase/include/row0ins.h"
#include "storage/innobase/include/row0sel.h"
#include "storage/innobase/include/row0mysql.h"
#include "storage/innobase/include/row0upd.h"
#include "storage/innobase/include/trx0rec.h"
#include "storage/innobase/include/trx0undo.h"
#include "storage/innobase/include/mtr0types.h"
#include "storage/innobase/include/mtr0mtr.h"
#include "storage/innobase/rem/rec.h"

#include "sql/table.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
namespace ShannonBase {
extern ShannonLoadedTables* shannon_loaded_tables;

namespace Populate {

int LogParser::parse_cur_rec_change_apply_low(const rec_t *rec, const dict_index_t *index,
                                              const ulint *offsets, mlog_id_t type, bool all,
                                              page_zip_des_t *page_zip, const upd_t * upd,
                                              trx_id_t trxid) {
  ut_a(index);
  assert(index->n_def == offsets[1]);
  ut_ad(rec_offs_validate(rec, nullptr, offsets));
  std::string db_name, table_name;
  index->table->get_table_name(db_name, table_name);

  std::unique_ptr<ShannonBase::RapidContext> context = std::make_unique<ShannonBase::RapidContext>();
  context->m_current_db = db_name;
  context->m_current_table = table_name;

  trx_id_t trx_id;
  uint pk_len = get_trxid(rec, index, offsets, (uchar*)&trx_id);
  ut_a(pk_len);
  context->m_extra_info.m_trxid = trx_id;

  context->m_extra_info.m_key_buff = std::make_unique<uchar[]>(pk_len+1);
  context->m_extra_info.m_key_len = pk_len;
  auto len = get_PK(rec, index, offsets, context->m_extra_info.m_key_buff.get());
  ut_a(len == pk_len);

  auto imcs_instance = ShannonBase::Imcs::Imcs::get_instance();
  ut_a(imcs_instance);

  int ret;
  switch (type) {
    case MLOG_REC_INSERT:
    case MLOG_REC_DELETE:{
      for (auto idx = 0u; idx < index->n_fields; idx++) {
        const char* field_name = index->get_field(idx)->name;
        auto idx_col  = index->get_field(idx);
        if (idx_col->col->mtype == DATA_SYS || idx_col->col->mtype == DATA_SYS_CHILD) continue;

        if (type == MLOG_REC_INSERT) {
          ulint len {0};
          byte *data = rec_get_nth_field(index, rec, offsets, idx_col->col->get_phy_pos(), &len);
          ut_a(idx_col->col->len >= len);
          auto field_data = std::make_unique<uchar[]>(idx_col->col->len + 1);

          if (len == UNIV_SQL_NULL) { // is null.
          } else {
            store_field_in_mysql_format(index, idx_col, field_data.get(), data, len);
          }
          ret = imcs_instance->write_direct(context.get(), db_name.c_str(), table_name.c_str(), field_name,
                                            field_data.get(),
                                            (len == UNIV_SQL_NULL)? UNIV_SQL_NULL: idx_col->col->len);
        } else{
          ret = imcs_instance->delete_direct(context.get(), db_name.c_str(), table_name.c_str(),
                                             field_name,
                                             all ? nullptr : context->m_extra_info.m_key_buff.get(),
                                             all ? 0 : context->m_extra_info.m_key_len);
        }
        if (ret) return ret;
      } //for
    } break;
    case MLOG_REC_UPDATE_IN_PLACE:{
      ut_a(upd);
      context->m_extra_info.m_trxid = trxid; //set to new trx id.
      auto n_fields = upd_get_n_fields(upd);
      for (auto i = 0u; i < n_fields; i++) {
        auto upd_field = upd_get_nth_field(upd, i);
        /* No need to update virtual columns for non-virtual index */
        if (upd_fld_is_virtual_col(upd_field) && !dict_index_has_virtual(index)) {
          continue;
        }

        uint32_t field_no = upd_field->field_no;
        auto new_val = &(upd_field->new_val);
        ut_ad(!dfield_is_ext(new_val) ==
              !rec_offs_nth_extern(index, offsets, field_no));
        /* Updating default value for instantly added columns must not be done in-place.
           See also row_upd_changes_field_size_or_external() */
        ut_ad(!rec_offs_nth_default(index, offsets, field_no));

        auto field_data = std::make_unique<uchar[]>(index->get_field(field_no)->col->len + 1);
        store_field_in_mysql_format(index, index->get_field(field_no), field_data.get(),
                                   (const byte*)dfield_get_data(new_val), dfield_get_len(new_val));

        const char* field_name = index->get_field(field_no)->name;
        ret = imcs_instance->update_direct(context.get(), db_name.c_str(), table_name.c_str(), field_name,
                                           field_data.get(), dfield_get_len(new_val), true);
        if (ret) return ret;
      }
    } break;
    default:
      break;
  }

  return 0;
}

uint LogParser::get_PK(const rec_t *rec, const dict_index_t *index, const ulint *offsets, uchar* pk) {
  ut_a(pk);
  uint off_pos{0};

  for (auto idx = 0; idx < index->n_uniq; idx++) {
    auto idx_col  = index->get_field(idx); //the PK field. rapid table must be has at leat one PK.
    ut_a (idx_col->col->mtype != DATA_SYS && idx_col->col->mtype != DATA_SYS_CHILD);

    ulint len{0};
    byte *data = rec_get_nth_field(index, rec, offsets, idx_col->col->get_phy_pos(), &len);
    ut_a(idx_col->col->len >= len);

    store_field_in_mysql_format(index, idx_col, pk + off_pos, data, len);
    off_pos += idx_col->col->len;
  } //for

  return off_pos;
}

uint LogParser::get_trxid(const rec_t *rec, const dict_index_t *index, const ulint *offsets, uchar* trx_id) {
  uint pk_len {0};
  for (auto idx = 0; idx < index->n_fields; idx++) {
    auto idx_col  = index->get_field(idx);
    if (idx < index->n_uniq) { //this is part of PK.
      pk_len += idx_col->col->len;
    }

    if (strncmp(idx_col->name, "DB_TRX_ID", 9)) continue;

    auto col_mask = idx_col->col->prtype & DATA_SYS_PRTYPE_MASK;
    if (idx_col->col->mtype == DATA_SYS && col_mask == DATA_TRX_ID) {
        mysql_row_templ_t templ;
        templ.type = DATA_SYS;
        templ.is_virtual = false;
        templ.is_multi_val = false;
        templ.mysql_col_len = DATA_TRX_ID_LEN;
        ulint len{0};
        byte *data = rec_get_nth_field(index, rec, offsets, idx_col->col->get_phy_pos(), &len);
        ut_a(len ==DATA_TRX_ID_LEN);
        row_sel_field_store_in_mysql_format(trx_id, &templ, index, idx_col->col->get_phy_pos(),
                                            data, len, ULINT_UNDEFINED);
    }
    break;
  }

  return pk_len;
}

int LogParser::store_field_in_mysql_format(const dict_index_t*index, const dict_field_t* col,
                                           const byte *dest, const byte* src, ulint len) {
  mysql_row_templ_t templ;
  templ.type = col->col->mtype;
  templ.is_virtual = templ.is_multi_val = false;
  templ.mysql_col_len = col->col->len;
  templ.mysql_type = col->col->mtype;

  if (templ.mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
    templ.mysql_length_bytes = (col->col->len > 255) ? 2 : 1; //field->get_length_bytes();
  } else {
    templ.mysql_length_bytes = 0;
  }
  templ.charset = dtype_get_charset_coll(col->col->prtype);
  templ.mbminlen = col->col->get_mbminlen();
  templ.mbmaxlen = col->col->get_mbmaxlen();
  templ.is_unsigned = col->col->prtype & DATA_UNSIGNED;

  row_sel_field_store_in_mysql_format(const_cast<byte*>(dest), &templ, index,
                                      col->col->get_phy_pos(), src, len, ULINT_UNDEFINED);
  return 0;
}

const dict_index_t* LogParser::find_index(uint64 idx_id) {
  btr_pcur_t pcur;
  const rec_t *rec;
  mem_heap_t *heap;
  mtr_t mtr;
  MDL_ticket *mdl = nullptr;
  dict_table_t *dd_indexes;
  THD* thd = current_thd;
  bool ret;

  DBUG_TRACE;

  heap = mem_heap_create(100, UT_LOCATION_HERE);
  dict_sys_mutex_enter();
  mtr_start(&mtr);

  /* Start scan the mysql.indexes */
  rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, dd_indexes_name.c_str(),
                            &dd_indexes);

  /* Process each record in the table */
  while (rec) {
    const dict_index_t *index_rec;
    MDL_ticket *mdl_on_tab = nullptr;
    dict_table_t *parent = nullptr;
    MDL_ticket *mdl_on_parent = nullptr;

    /* Populate a dict_index_t structure with information from
    a INNODB_INDEXES row */
    ret = dd_process_dd_indexes_rec(heap, rec, &index_rec, &mdl_on_tab, &parent,
                                    &mdl_on_parent, dd_indexes, &mtr);

    dict_sys_mutex_exit();

    if (ret && (index_rec->id  == idx_id)) {
      mem_heap_empty(heap);
      dict_sys_mutex_enter();
      dd_table_close(index_rec->table, thd, &mdl_on_tab, true);

      /* Close parent table if it's a fts aux table. */
      if (index_rec->table->is_fts_aux() && parent) {
        dd_table_close(parent, thd, &mdl_on_parent, true);
      }
      dict_sys_mutex_exit();
      return  index_rec;
    }

    mem_heap_empty(heap);

    /* Get the next record */
    dict_sys_mutex_enter();

    if (index_rec != nullptr) {
      dd_table_close(index_rec->table, thd, &mdl_on_tab, true);

      /* Close parent table if it's a fts aux table. */
      if (index_rec->table->is_fts_aux() && parent) {
        dd_table_close(parent, thd, &mdl_on_parent, true);
      }
    }

    mtr_start(&mtr);
    rec = dd_getnext_system_rec(&pcur, &mtr);
  } //while(rec)

  mtr_commit(&mtr);
  dd_table_close(dd_indexes, thd, &mdl, true);
  dict_sys_mutex_exit();
  mem_heap_free(heap);
  return nullptr;
}

buf_block_t* LogParser::get_block(space_id_t space_id, page_no_t page_no) {
  buf_block_t*block {nullptr};
  const page_id_t page_id(space_id, page_no);
  bool found;
  const page_size_t page_size = fil_space_get_page_size(space_id, &found);
  if (found && buf_page_peek(page_id)) {
    mtr_t mtr_p;
    mtr_start(&mtr_p);
    block =
       buf_page_get(page_id, page_size, RW_X_LATCH, UT_LOCATION_HERE, &mtr_p);
    mtr_commit(&mtr_p);
  }

  return block;
}

byte *LogParser::parse_cur_and_apply_delete_mark_rec(
    byte *ptr,           /*!< in: buffer */
    byte *end_ptr,       /*!< in: buffer end */
    buf_block_t *block,  /*!< in: page or NULL */
    dict_index_t *index, /*!< in: record descriptor */
    mtr_t *mtr)  {         /*!< in: mtr or NULL */
  ulint pos;
  trx_id_t trx_id;
  roll_ptr_t roll_ptr;
  ulint offset;
  rec_t *rec;
  //may the page in this block not used by any one, it could be evicted.???
  page_t *page = block ? ((buf_frame_t *)block->frame) : nullptr;
  ut_ad(!page || page_is_comp(page) == dict_table_is_comp(index->table));

  auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
  const dict_index_t* tb_index = find_index(index_id);

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  auto flags = mach_read_from_1(ptr);
  ptr++;
  auto val[[maybe_unused]] = mach_read_from_1(ptr);
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

  if (page) {
    rec = page + offset;

    /* We do not need to reserve search latch, as the page
    is only being recovered, and there cannot be a hash index to
    it. Besides, these fields are being updated in place
    and the adaptive hash index does not depend on them. */
    //btr_rec_set_deleted_flag(rec, page_zip, val);

    if (!(flags & BTR_KEEP_SYS_FLAG)) {
      mem_heap_t *heap = nullptr;
      ulint offsets_[REC_OFFS_NORMAL_SIZE];
      rec_offs_init(offsets_);

      std::string db_name, table_name;
      tb_index->table->get_table_name(db_name, table_name);
      // get field length from rapid
      auto share = ShannonBase::shannon_loaded_tables->get(db_name, table_name);
      if (share) { //was not loaded table, return
        auto all = (page[PAGE_HEADER + PAGE_N_HEAP + 1] == PAGE_HEAP_NO_USER_LOW) ? true : false;
        parse_cur_rec_change_apply_low(rec, tb_index,
                                       rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED,
                                       UT_LOCATION_HERE, &heap),
                                       MLOG_REC_DELETE, all,
                                       nullptr, nullptr, trx_id);
      }
      if (UNIV_LIKELY_NULL(heap)) {
        mem_heap_free(heap);
      }
    }
  }

  return (ptr);
}

byte *LogParser::parse_cur_and_apply_delete_rec(
    byte *ptr,           /*!< in: buffer */
    byte *end_ptr,       /*!< in: buffer end */
    buf_block_t *block,  /*!< in: page or NULL */
    dict_index_t *index, /*!< in: record descriptor */
    mtr_t *mtr)  {         /*!< in: mtr or NULL */
  ulint offset;
  page_cur_t cursor;

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  /* Read the cursor rec offset as a 2-byte ulint */
  offset = mach_read_from_2(ptr);
  ptr += 2;

  ut_a(offset <= UNIV_PAGE_SIZE);

  //may the page in this block not used by any one, it could be evicted.???
  page_t *page = block ? ((buf_frame_t *)block->frame) : nullptr;
  if (page) {
    mem_heap_t *heap = nullptr;
    ulint offsets_[REC_OFFS_NORMAL_SIZE];
    rec_t *rec = page + offset;
    rec_offs_init(offsets_);

    auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
    const dict_index_t* tb_index = find_index(index_id);

#ifdef UNIV_HOTBACKUP
    ib::trace_1() << "page_cur_parse_delete_rec: offset " << offset;
#endif /* UNIV_HOTBACKUP */
    ut_ad(!buf_block_get_page_zip(block) || page_is_comp(page));

    std::string db_name, table_name;
    tb_index->table->get_table_name(db_name, table_name);
    // get field length from rapid
    auto share = ShannonBase::shannon_loaded_tables->get(db_name, table_name);
    if (!share) //was not loaded table, return
      goto finish;

    parse_cur_rec_change_apply_low(rec, tb_index,
                                  rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED,
                                  UT_LOCATION_HERE, &heap), MLOG_REC_DELETE, false);
finish:
    if (UNIV_LIKELY_NULL(heap)) {
      mem_heap_free(heap);
    }
  }

  return (ptr);
}

byte *LogParser::parse_cur_and_apply_insert_rec(
    bool is_short,       /*!< in: true if short inserts */
    const byte *ptr,     /*!< in: buffer */
    const byte *end_ptr, /*!< in: buffer end */
    buf_block_t *block,  /*!< in: page or NULL */
    dict_index_t *index, /*!< in: record descriptor */
    mtr_t *mtr) {         /*!< in: mtr or NULL */

  ulint origin_offset = 0; /* remove warning */
  ulint end_seg_len;
  ulint mismatch_index = 0; /* remove warning */
  page_t *page;
  rec_t *cursor_rec{nullptr};
  byte buf1[1024];
  byte *buf;
  const byte *ptr2 = ptr;
  ulint info_and_status_bits = 0; /* remove warning */
  page_cur_t cursor;
  mem_heap_t *heap = nullptr;
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint *offsets = offsets_;
  rec_offs_init(offsets_);

  page = block ? ((buf_frame_t *)block->frame) : nullptr;

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
      recv_sys->found_corrupt_log = true;

      return (nullptr);
    }
  }

  end_seg_len = mach_parse_compressed(&ptr, end_ptr);

  if (ptr == nullptr) {
    return (nullptr);
  }

  if (end_seg_len >= UNIV_PAGE_SIZE << 1) {
    recv_sys->found_corrupt_log = true;

    return (nullptr);
  }

  if (end_seg_len & 0x1UL) {
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

  if (!block) {
    return (const_cast<byte *>(ptr + (end_seg_len >> 1)));
  }

  ut_ad(page_is_comp(page) == dict_table_is_comp(index->table));
  ut_ad(!buf_block_get_page_zip(block) || page_is_comp(page));

  /* Read from the log the inserted index record end segment which
  differs from the cursor record */

  if ((end_seg_len & 0x1UL) && mismatch_index == 0) {
    /* This is a record has nothing common to cursor record. */
  } else {
    offsets = rec_get_offsets(cursor_rec, index, offsets, ULINT_UNDEFINED,
                              UT_LOCATION_HERE, &heap);

    if (!(end_seg_len & 0x1UL)) {
      info_and_status_bits =
          rec_get_info_and_status_bits(cursor_rec, page_is_comp(page));
      origin_offset = rec_offs_extra_size(offsets);
      mismatch_index = rec_offs_size(offsets) - (end_seg_len >> 1);
    }
  }

  end_seg_len >>= 1;

  if (mismatch_index + end_seg_len < sizeof buf1) {
    buf = buf1;
  } else {
    buf = static_cast<byte *>(ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY,
                                                 mismatch_index + end_seg_len));
  }

  /* Build the inserted record to buf */

  if (UNIV_UNLIKELY(mismatch_index >= UNIV_PAGE_SIZE)) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_859)
        << "is_short " << is_short << ", "
        << "info_and_status_bits " << info_and_status_bits << ", offset "
        << page_offset(cursor_rec)
        << ","
           " o_offset "
        << origin_offset << ", mismatch index " << mismatch_index
        << ", end_seg_len " << end_seg_len << " parsed len " << (ptr - ptr2);
  }

  if (mismatch_index) {
    ut_memcpy(buf, rec_get_start(cursor_rec, offsets), mismatch_index);
  }
  ut_memcpy(buf + mismatch_index, ptr, end_seg_len);

  if (page_is_comp(page)) {
    rec_set_info_and_status_bits(buf + origin_offset, info_and_status_bits);
  } else {
    rec_set_info_bits_old(buf + origin_offset, info_and_status_bits);
  }

  page = page_align(cursor_rec);
  auto index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
  const dict_index_t* tb_index = find_index(index_id);
  if (tb_index){
    offsets = rec_get_offsets(buf + origin_offset, index, offsets,
                              ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

    std::string db_name, table_name;
    tb_index->table->get_table_name(db_name, table_name);
    // get field length from rapid
    auto share = ShannonBase::shannon_loaded_tables->get(db_name, table_name);
    if (!share) //was not loaded table, return
      goto finish;
    
    parse_cur_rec_change_apply_low(buf + origin_offset, tb_index, offsets,
                                  MLOG_REC_INSERT, false);
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

byte* LogParser::parse_row_and_apply_upd_rec_in_place(
    rec_t *rec,                /*!< in/out: record where replaced */
    const dict_index_t *index, /*!< in: the index the record belongs to */
    const ulint *offsets,      /*!< in: array returned by rec_get_offsets() */
    const upd_t *update,       /*!< in: update vector */
    page_zip_des_t *page_zip,  /*!< in: compressed page with enough space available, or NULL */
    trx_id_t  trx_id) {

  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(!index->table->skip_alter_undo);

  parse_cur_rec_change_apply_low(rec, index, offsets, MLOG_REC_UPDATE_IN_PLACE, false,
                                 page_zip, update, trx_id);

  /*now, we dont want to support zipped page now. how to deal with pls ref to:
    page_zip_write_rec(page_zip, rec, index, offsets, 0); */

  return rec;
}
byte *LogParser::parse_cur_update_in_place_and_apply(
    byte *ptr,                /*!< in: buffer */
    byte *end_ptr,            /*!< in: buffer end */
    buf_block_t *block,       /*!< in/out: page or NULL */
    page_zip_des_t *page_zip, /*!< in/out: compressed page, or NULL */
    dict_index_t *index) {     /*!< in: index corresponding to page */
  ulint flags;
  rec_t *rec;
  upd_t *update;
  ulint pos;
  trx_id_t trx_id;
  roll_ptr_t roll_ptr;
  ulint rec_offset;
  mem_heap_t *heap;
  ulint *offsets;
  const dict_index_t* tb_index;

  auto page = block ? ((buf_frame_t *)block->frame) : nullptr;

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

  tb_index = find_index(mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID));

  if (!ptr || !page || !tb_index) {
    goto func_exit;
  }

  ut_a(page_is_comp(page) == dict_table_is_comp(index->table));
  rec = page + rec_offset;

  if (!(flags & BTR_KEEP_SYS_FLAG)) {
    /*trx_id here is a new trxid , here, we have diff approach. do nothing for trxid
      and roll_ptr. row_upd_rec_sys_fields_in_recovery
    */
  }

  if (tb_index){
    std::string db_name, table_name;
    tb_index->table->get_table_name(db_name, table_name);
    // get field length from rapid
    auto share = ShannonBase::shannon_loaded_tables->get(db_name, table_name);
    if (share) {//was not loaded table, return
      offsets = rec_get_offsets(rec, tb_index, nullptr, ULINT_UNDEFINED,
                            UT_LOCATION_HERE, &heap);
      parse_row_and_apply_upd_rec_in_place(rec, tb_index, offsets, update, page_zip, trx_id);
    }
  }
func_exit:
  mem_heap_free(heap);

  return (ptr);
}

byte *LogParser::parse_parse_or_apply_log_rec_body(
      mlog_id_t type, byte *ptr, byte *end_ptr, space_id_t space_id,
      page_no_t page_no, buf_block_t *block, mtr_t *mtr, lsn_t start_lsn) {
  bool applying_redo = (block != nullptr);

  page_t *page;
  page_zip_des_t *page_zip;
  dict_index_t *index = nullptr;

#ifdef UNIV_DEBUG
  ulint page_type;
#endif /* UNIV_DEBUG */

#if defined(UNIV_HOTBACKUP) && defined(UNIV_DEBUG)
  ib::trace_3() << "recv_parse_or_apply_log_rec_body: type "
                << get_mlog_string(type) << " space_id " << space_id
                << " page_nr " << page_no << " ptr "
                << static_cast<const void *>(ptr) << " end_ptr "
                << static_cast<const void *>(end_ptr) << " block "
                << static_cast<const void *>(block) << " mtr "
                << static_cast<const void *>(mtr);
#endif /* UNIV_HOTBACKUP && UNIV_DEBUG */

  if (applying_redo) {
    /* Applying a page log record. */
    ut_ad(mtr != nullptr);

    page = block->frame;
    page_zip = buf_block_get_page_zip(block);

    ut_d(page_type = fil_page_get_type(page));
#if defined(UNIV_HOTBACKUP) && defined(UNIV_DEBUG)
    if (page_type == 0) {
      meb_print_page_header(page);
    }
#endif /* UNIV_HOTBACKUP && UNIV_DEBUG */

  } else {
    /* Parsing a page log record. */
    ut_ad(mtr == nullptr);
    page = nullptr;
    page_zip = nullptr;

    ut_d(page_type = FIL_PAGE_TYPE_ALLOCATED);
  }

  const byte *old_ptr = ptr;

  switch (type) {
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
      /* The LSN is checked in recv_parse_log_rec(). */
      break;
#endif /* UNIV_LOG_LSN_DEBUG */
    case MLOG_4BYTES:

      ut_a(false);
      ut_ad(page == nullptr || end_ptr > ptr + 2);

      /* Most FSP flags can only be changed by CREATE or ALTER with
      ALGORITHM=COPY, so they do not change once the file
      is created. The SDI flag is the only one that can be
      changed by a recoverable transaction. So if there is
      change in FSP flags, update the in-memory space structure
      (fil_space_t) */

      if (page != nullptr && page_no == 0 &&
          mach_read_from_2(ptr) == FSP_HEADER_OFFSET + FSP_SPACE_FLAGS) {
        ptr = mlog_parse_nbytes(MLOG_4BYTES, ptr, end_ptr, page, page_zip);

        /* When applying log, we have complete records.
        They can be incomplete (ptr=nullptr) only during
        scanning (page==nullptr) */

        ut_ad(ptr != nullptr);

        fil_space_t *space = fil_space_acquire(space_id);

        ut_ad(space != nullptr);

        fil_space_set_flags(space, mach_read_from_4(FSP_HEADER_OFFSET +
                                                    FSP_SPACE_FLAGS + page));
        fil_space_release(space);

        break;
      }

      [[fallthrough]];

    case MLOG_1BYTE:
      ut_a(false);
      /* If 'ALTER TABLESPACE ... ENCRYPTION' was in progress and page 0 has
      REDO entry for this, now while applying this entry, set
      encryption_op_in_progress flag now so that any other page of this
      tablespace in redo log is written accordingly. */
      if (page_no == 0 && page != nullptr && end_ptr >= ptr + 2) {
        ulint offs = mach_read_from_2(ptr);

        fil_space_t *space = fil_space_acquire(space_id);
        ut_ad(space != nullptr);
        ulint offset = fsp_header_get_encryption_progress_offset(
            page_size_t(space->flags));

        if (offs == offset) {
          ptr = mlog_parse_nbytes(MLOG_1BYTE, ptr, end_ptr, page, page_zip);
          byte op = mach_read_from_1(page + offset);
          switch (op) {
            case Encryption::ENCRYPT_IN_PROGRESS:
              space->encryption_op_in_progress =
                  Encryption::Progress::ENCRYPTION;
              break;
            case Encryption::DECRYPT_IN_PROGRESS:
              space->encryption_op_in_progress =
                  Encryption::Progress::DECRYPTION;
              break;
            default:
              space->encryption_op_in_progress = Encryption::Progress::NONE;
              break;
          }
        }
        fil_space_release(space);
      }

      [[fallthrough]];

    case MLOG_2BYTES:
    case MLOG_8BYTES:
      ut_a(false);
#ifdef UNIV_DEBUG
      if (page && page_type == FIL_PAGE_TYPE_ALLOCATED && end_ptr >= ptr + 2) {
        /* It is OK to set FIL_PAGE_TYPE and certain
        list node fields on an empty page.  Any other
        write is not OK. */

        /* NOTE: There may be bogus assertion failures for
        dict_hdr_create(), trx_rseg_header_create(),
        trx_sys_create_doublewrite_buf(), and
        trx_sysf_create().
        These are only called during database creation. */

        ulint offs = mach_read_from_2(ptr);

        switch (type) {
          default:
            ut_error;
          case MLOG_2BYTES:
            /* Note that this can fail when the
            redo log been written with something
            older than InnoDB Plugin 1.0.4. */
            ut_ad(
                offs == FIL_PAGE_TYPE ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_BYTE ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_BYTE +
                            FIL_ADDR_SIZE ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                            FIL_ADDR_BYTE + 0 /*FLST_PREV*/
                || offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                               FIL_ADDR_BYTE + FIL_ADDR_SIZE /*FLST_NEXT*/);
            break;
          case MLOG_4BYTES:
            /* Note that this can fail when the
            redo log been written with something
            older than InnoDB Plugin 1.0.4. */
            ut_ad(
                0 ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_SPACE ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER /* flst_init */
                ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_PAGE ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_PAGE +
                            FIL_ADDR_SIZE ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_SPACE ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_SPACE ||
                offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                            FIL_ADDR_PAGE + 0 /*FLST_PREV*/
                || offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                               FIL_ADDR_PAGE + FIL_ADDR_SIZE /*FLST_NEXT*/);
            break;
        }
      }
#endif /* UNIV_DEBUG */

      ptr = mlog_parse_nbytes(type, ptr, end_ptr, page, page_zip);

      if (ptr != nullptr && page != nullptr && page_no == 0 &&
          type == MLOG_4BYTES) {
        ulint offs = mach_read_from_2(old_ptr);

        switch (offs) {
          fil_space_t *space;
          uint32_t val;
          default:
            break;

          case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
          case FSP_HEADER_OFFSET + FSP_SIZE:
          case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
          case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:

            space = fil_space_get(space_id);

            ut_a(space != nullptr);

            val = mach_read_from_4(page + offs);

            switch (offs) {
              case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
                space->flags = val;
                break;

              case FSP_HEADER_OFFSET + FSP_SIZE:

                space->size_in_header = val;

                if (space->size >= val) {
                  break;
                }

                ib::info(ER_IB_MSG_718, ulong{space->id}, space->name,
                         ulong{val});

                if (fil_space_extend(space, val)) {
                  break;
                }

                ib::error(ER_IB_MSG_719, ulong{space->id}, space->name,
                          ulong{val});
                break;

              case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
                space->free_limit = val;
                break;

              case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
                space->free_len = val;
                ut_ad(val == flst_get_len(page + offs));
                break;
            }
        }
      }
      break;

    case MLOG_REC_INSERT:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr = parse_cur_and_apply_insert_rec(false, ptr, end_ptr, block, index, mtr);
      }
      break;

    case MLOG_REC_INSERT_8027:
    case MLOG_COMP_REC_INSERT_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_INSERT_8027, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr = parse_cur_and_apply_insert_rec(false, ptr, end_ptr, block, index, mtr);
      }
      break;

    case MLOG_REC_CLUST_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr = parse_cur_and_apply_delete_mark_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_REC_CLUST_DELETE_MARK_8027:
    case MLOG_COMP_REC_CLUST_DELETE_MARK_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_CLUST_DELETE_MARK_8027,
               &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr = parse_cur_and_apply_delete_mark_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_COMP_REC_SEC_DELETE_MARK:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      /* This log record type is obsolete, but we process it for
      backward compatibility with MySQL 5.0.3 and 5.0.4. */

      ut_a(!page || page_is_comp(page));
      ut_a(!page_zip);

      ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index);

      if (ptr == nullptr) {
        break;
      }

      [[fallthrough]];

    case MLOG_REC_SEC_DELETE_MARK:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr, page, page_zip);
      break;

    case MLOG_REC_UPDATE_IN_PLACE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr =
            parse_cur_update_in_place_and_apply(ptr, end_ptr, block, page_zip, index);
      }

      break;

    case MLOG_REC_UPDATE_IN_PLACE_8027:
    case MLOG_COMP_REC_UPDATE_IN_PLACE_8027:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_UPDATE_IN_PLACE_8027,
               &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr =
            parse_cur_update_in_place_and_apply(ptr, end_ptr, block, page_zip, index);
      }

      break;

    case MLOG_LIST_END_DELETE:
    case MLOG_LIST_START_DELETE:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_parse_delete_rec_list(type, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_LIST_END_DELETE_8027:
    case MLOG_COMP_LIST_END_DELETE_8027:
    case MLOG_LIST_START_DELETE_8027:
    case MLOG_COMP_LIST_START_DELETE_8027:

     ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index_8027(
                          ptr, end_ptr,
                          type == MLOG_COMP_LIST_END_DELETE_8027 ||
                              type == MLOG_COMP_LIST_START_DELETE_8027,
                          &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_parse_delete_rec_list(type, ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_LIST_END_COPY_CREATED:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_parse_copy_rec_list_to_created_page(ptr, end_ptr, block,
                                                       index, mtr);
      }

      break;

    case MLOG_LIST_END_COPY_CREATED_8027:
    case MLOG_COMP_LIST_END_COPY_CREATED_8027:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_LIST_END_COPY_CREATED_8027,
               &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_parse_copy_rec_list_to_created_page(ptr, end_ptr, block,
                                                       index, mtr);
      }

      break;

    case MLOG_PAGE_REORGANIZE:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index,
                                        type == MLOG_ZIP_PAGE_REORGANIZE_8027,
                                        block, mtr);
      }

      break;

    case MLOG_PAGE_REORGANIZE_8027:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));
      /* Uncompressed pages don't have any payload in the
      MTR so ptr and end_ptr can be, and are nullptr */
      mlog_parse_index_8027(ptr, end_ptr, false, &index);
      ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

      ptr = btr_parse_page_reorganize(ptr, end_ptr, index, false, block, mtr);

      break;

    case MLOG_ZIP_PAGE_REORGANIZE:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index, true, block, mtr);
      }

      break;

    case MLOG_COMP_PAGE_REORGANIZE_8027:
    case MLOG_ZIP_PAGE_REORGANIZE_8027:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index,
                                        type == MLOG_ZIP_PAGE_REORGANIZE_8027,
                                        block, mtr);
      }

      break;

    case MLOG_PAGE_CREATE:
    case MLOG_COMP_PAGE_CREATE:

      ut_a(false);
      /* Allow anything in page_type when creating a page. */
      ut_a(!page_zip);

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE, FIL_PAGE_INDEX);

      break;

    case MLOG_PAGE_CREATE_RTREE:
    case MLOG_COMP_PAGE_CREATE_RTREE:

      ut_a(false);
      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_RTREE,
                        FIL_PAGE_RTREE);

      break;

    case MLOG_PAGE_CREATE_SDI:
    case MLOG_COMP_PAGE_CREATE_SDI:

      ut_a(false);
      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_SDI, FIL_PAGE_SDI);

      break;

    case MLOG_UNDO_INSERT:

      ut_a(false);
      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);

      break;

    case MLOG_UNDO_ERASE_END:

      ut_a(false);
      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_erase_page_end(ptr, end_ptr, page, mtr);

      break;

    case MLOG_UNDO_INIT:

      ut_a(false);
      /* Allow anything in page_type when creating a page. */

      ptr = trx_undo_parse_page_init(ptr, end_ptr, page, mtr);

      break;
    case MLOG_UNDO_HDR_CREATE:
    case MLOG_UNDO_HDR_REUSE:

      ut_a(false);
      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_page_header(type, ptr, end_ptr, page, mtr);

      break;

    case MLOG_REC_MIN_MARK:
    case MLOG_COMP_REC_MIN_MARK:

      ut_a(false);
      ut_ad(!page || fil_page_type_is_index(page_type));

      /* On a compressed page, MLOG_COMP_REC_MIN_MARK
      will be followed by MLOG_COMP_REC_DELETE
      or MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_nullptr)
      in the same mini-transaction. */

      ut_a(type == MLOG_COMP_REC_MIN_MARK || !page_zip);

      ptr = btr_parse_set_min_rec_mark(
          ptr, end_ptr, type == MLOG_COMP_REC_MIN_MARK, page, mtr);

      break;

    case MLOG_REC_DELETE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        buf_block_t*block = get_block(space_id, page_no);
        ptr = parse_cur_and_apply_delete_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_REC_DELETE_8027:
    case MLOG_COMP_REC_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(

               ptr, end_ptr, type == MLOG_COMP_REC_DELETE_8027, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = parse_cur_and_apply_delete_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_IBUF_BITMAP_INIT:

      ut_a(false);
      /* Allow anything in page_type when creating a page. */

      ptr = ibuf_parse_bitmap_init(ptr, end_ptr, block, mtr);

      break;

    case MLOG_INIT_FILE_PAGE:
    case MLOG_INIT_FILE_PAGE2: {
      /* For clone, avoid initializing page-0. Page-0 should already have been
      initialized. This is to avoid erasing encryption information. We cannot
      update encryption information later with redo logged information for
      clone. Please check comments in MLOG_WRITE_STRING. */
      bool skip_init = (recv_sys->is_cloned_db && page_no == 0);

      if (!skip_init) {
        /* Allow anything in page_type when creating a page. */
        ptr = fsp_parse_init_file_page(ptr, end_ptr, block);
      }
      break;
    }

    case MLOG_WRITE_STRING: {
      ut_ad(!page || page_type != FIL_PAGE_TYPE_ALLOCATED || page_no == 0);
      bool is_encryption = false;//check_encryption(page_no, space_id, ptr, end_ptr);

#ifndef UNIV_HOTBACKUP
      /* Reset in-mem encryption information for the tablespace here if this
      is "resetting encryprion info" log. */
      if (is_encryption && !recv_sys->is_cloned_db) {
        byte buf[Encryption::INFO_SIZE] = {0};

        if (memcmp(ptr + 4, buf, Encryption::INFO_SIZE - 4) == 0) {
          ut_a(DB_SUCCESS == fil_reset_encryption(space_id));
        }
      }

#endif
      auto apply_page = page;

      /* For clone recovery, skip applying encryption information from
      redo log. It is already updated in page 0. Redo log encryption
      information is encrypted with donor master key and must be ignored. */
      if (recv_sys->is_cloned_db && is_encryption) {
        apply_page = nullptr;
      }

      ptr = mlog_parse_string(ptr, end_ptr, apply_page, page_zip);
      break;
    }

    case MLOG_ZIP_WRITE_NODE_PTR:

      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = page_zip_parse_write_node_ptr(ptr, end_ptr, page, page_zip);

      break;

    case MLOG_ZIP_WRITE_BLOB_PTR:

      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = page_zip_parse_write_blob_ptr(ptr, end_ptr, page, page_zip);

      break;

    case MLOG_ZIP_WRITE_HEADER:

      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = page_zip_parse_write_header(ptr, end_ptr, page, page_zip);

      break;

    case MLOG_ZIP_PAGE_COMPRESS:

      /* Allow anything in page_type when creating a page. */
      ptr = page_zip_parse_compress(ptr, end_ptr, page, page_zip);
      break;

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table)));

        ptr = page_zip_parse_compress_no_data(ptr, end_ptr, page, page_zip,
                                              index);
      }

      break;

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027:

      if (nullptr !=
          (ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index))) {
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table)));

        ptr = page_zip_parse_compress_no_data(ptr, end_ptr, page, page_zip,
                                              index);
      }

      break;

    case MLOG_TEST:
#ifndef UNIV_HOTBACKUP
      if (log_test != nullptr) {
        ptr = log_test->parse_mlog_rec(ptr, end_ptr);
      } else {
        /* Just parse and ignore record to pass it and go forward. Note that
        this record is also used in the innodb.log_first_rec_group mtr test.
        The record is written in the buf0flu.cc when flushing page in that
        case. */
        Log_test::Key key;
        Log_test::Value value;
        lsn_t start_lsn, end_lsn;

        ptr = Log_test::parse_mlog_rec(ptr, end_ptr, key, value, start_lsn,
                                       end_lsn);
      }
      break;
#endif /* !UNIV_HOTBACKUP */
      /* Fall through. */

    default:
      ptr = nullptr;
      recv_sys->found_corrupt_log = true;
  }

  if (index != nullptr) {
    dict_table_t *table = index->table;

    dict_mem_index_free(index);
    dict_mem_table_free(table);
  }

  return ptr;        
}

ulint LogParser::parse_parse_log_rec(mlog_id_t *type, byte *ptr, byte *end_ptr,
                                space_id_t *space_id, page_no_t *page_no,
                                byte **body) {

  byte *new_ptr;

  *body = nullptr;

  UNIV_MEM_INVALID(type, sizeof *type);
  UNIV_MEM_INVALID(space_id, sizeof *space_id);
  UNIV_MEM_INVALID(page_no, sizeof *page_no);
  UNIV_MEM_INVALID(body, sizeof *body);

  if (ptr == end_ptr) {
    return 0;
  }

  new_ptr =
      mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no);
  *body = new_ptr;

  if (new_ptr == nullptr) {
    return 0;
  }

  new_ptr = parse_parse_or_apply_log_rec_body(
      *type, new_ptr, end_ptr, *space_id, *page_no, nullptr, nullptr,
      recv_sys->recovered_lsn);

  if (new_ptr == nullptr) {
    return 0;
  }

  return new_ptr - ptr;
}

uint LogParser::parse_single_rec(byte *ptr, byte *end_ptr) {
  /* Try to parse a log record, fetching its type, space id,
  page no, and a pointer to the body of the log record */

  byte *body;
  mlog_id_t type;
  page_no_t page_no;
  space_id_t space_id;
  bool single_flag;
  if (get_record_type(ptr, &type, &single_flag))
    return 0;

  uint parsed_bytes = parse_parse_log_rec(&type, ptr, end_ptr, &space_id, &page_no, &body);
  return parsed_bytes;  
}

uint LogParser::parse_multi_rec(byte *ptr, byte *end_ptr) {
  ut_a(end_ptr >= ptr);
  return (end_ptr - ptr);
}
// handle single mtr
uint LogParser::parse_redo(byte* ptr, byte* end_ptr) {
/**
 * after secondary_load command excuted, all the data read from data file. the last
 * checkpoint lsn makes all the data lsn is samller than it were written to data file.
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

    return (single_rec) ?  parse_single_rec(ptr, end_ptr) :
                           parse_multi_rec(ptr, end_ptr);
}

}  // namespace Populate
}  // namespace ShannonBase