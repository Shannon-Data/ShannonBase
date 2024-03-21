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
#include "storage/innobase/include/log0test.h"
#include "storage/innobase/include/log0write.h"
#include "storage/innobase/include/row0ins.h"
#include "storage/innobase/include/row0mysql.h"
#include "storage/innobase/include/row0upd.h"
#include "storage/innobase/include/trx0rec.h"
#include "storage/innobase/include/trx0undo.h"
#include "storage/innobase/rem/rec.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
namespace ShannonBase {
extern ShannonLoadedTables* shannon_loaded_tables;

namespace Populate {
bool LogParser::parse_redo(log_t *log_ptr, lsn_t rapid_from_lsn, lsn_t rapid_to_lsn) {
  assert(log_ptr != nullptr);
  // adapt for redo log parse
  recv_sys_init();

  // initialize rapid lsn.
   log_ptr->rapid_lsn = log_ptr->recent_written.tail();

  log_allocate_rapid_events(*log_ptr);

  rapid_from_lsn = log_ptr->rapid_lsn.load();

  while (rapid_from_lsn >= rapid_to_lsn) {
    log_write_up_to(*log_ptr, rapid_from_lsn, true);
    rapid_to_lsn = log_ptr->flushed_to_disk_lsn.load(std::memory_order_relaxed);
  }

  lsn_t next_lsn = parse_redo_and_apply(log_ptr, rapid_from_lsn, rapid_to_lsn);

  if (next_lsn > rapid_from_lsn) {
    lsn_t lsn = rapid_from_lsn;
    const lsn_t notified_up_to_lsn =
        ut_uint64_align_up(next_lsn, OS_FILE_LOG_BLOCK_SIZE);
    while (lsn <= notified_up_to_lsn) {
      const size_t slot = compute_rapid_event_slot(lsn);
      lsn += OS_FILE_LOG_BLOCK_SIZE;
      os_event_set(log_ptr->rapid_events[slot]);
    }
  }

  if (!log_ptr->rapid_lsn.compare_exchange_strong(rapid_from_lsn, next_lsn)) {
    // only one thread can update the lsn
    assert(0);
  }

  return false;
}

int LogParser::handle_multi_rec(byte *ptr, byte *end_ptr, size_t *len,
                                lsn_t start_lsn) {
  ulint n_recs = 0;
  ulint total_len = 0;

  byte *orig_ptr = ptr;

  // just check whether exist a complete mtr
  for (;;) {
    mlog_id_t type = MLOG_BIGGEST_TYPE;
    byte *body;
    page_no_t page_no = 0;
    space_id_t space_id = 0;

    ulint len = parse_single_redo(&type, ptr, end_ptr, &space_id, &page_no,
                                  &body, start_lsn, false);

    if (len == 0) {
      return 1;
    }

    total_len += len;
    ++n_recs;

    ptr += len;

    if (type == MLOG_MULTI_REC_END) {
      break;
    }
  }

  ptr = orig_ptr;

  for (uint32 i = 0; i < n_recs; ++i) {
    mlog_id_t type = MLOG_BIGGEST_TYPE;
    byte *body;
    page_no_t page_no = 0;
    space_id_t space_id = 0;
    ulint len = parse_single_redo(&type, ptr, end_ptr, &space_id, &page_no,
                                  &body, start_lsn, true);
    ptr += len;
  }

  *len = total_len;
  return 0;
}

ulint LogParser::parse_single_redo(mlog_id_t *type, byte *ptr, byte *end_ptr,
                                   space_id_t *space_id, page_no_t *page_no,
                                   byte **body, lsn_t start_lsn,
                                   bool apply_to_rapid) {
  byte *new_ptr;

  *body = nullptr;

  if (ptr == end_ptr) {
    return 0;
  }
  switch (*ptr) {
    case MLOG_MULTI_REC_END:
    case MLOG_DUMMY_RECORD:
      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;
      *type = static_cast<mlog_id_t>(*ptr);
      return 1;
    case MLOG_MULTI_REC_END | MLOG_SINGLE_REC_FLAG:
    case MLOG_DUMMY_RECORD | MLOG_SINGLE_REC_FLAG:
      // corrupt log
      assert(0);
    case MLOG_TABLE_DYNAMIC_META:
    case MLOG_TABLE_DYNAMIC_META | MLOG_SINGLE_REC_FLAG:
      table_id_t id;
      uint64_t version;

      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;

      new_ptr =
          mlog_parse_initial_dict_log_record(ptr, end_ptr, type, &id, &version);

      if (new_ptr != nullptr) {
        new_ptr = recv_sys->metadata_recover->parseMetadataLog(
            id, version, new_ptr, end_ptr);
      }

      return new_ptr == nullptr ? 0 : new_ptr - ptr;
  }

  new_ptr =
      mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no);
  *body = new_ptr;
  if (new_ptr == nullptr) {
    return 0;
  }

  assert(*type != 0);

  new_ptr = parse_log_body(*type, new_ptr, end_ptr, *space_id, *page_no,
                           new_ptr - ptr, start_lsn, apply_to_rapid);

  if (new_ptr == nullptr) {
    return 0;
  }

  return new_ptr - ptr;
}

int LogParser::handle_single_rec(byte *ptr, byte *end_ptr, size_t *len,
                                 lsn_t start_lsn) {
  mlog_id_t type;
  byte *body;
  page_no_t page_no;
  space_id_t space_id;
  *len = parse_single_redo(&type, ptr, end_ptr, &space_id, &page_no, &body,
                           +start_lsn, true);
  if (*len == 0) {
    return 1;
  }
  return 0;
}

// handle single mtr
lsn_t LogParser::parse_redo_and_apply(log_t *log_ptr, lsn_t start_lsn,
                                      lsn_t target_lsn) {
  size_t start_offset = start_lsn % log_ptr->buf_size;
  size_t end_offset = target_lsn % log_ptr->buf_size;

  size_t size;
  if (start_offset < end_offset) {
    size = end_offset - start_offset;
  } else {
    size = log_ptr->buf_size - start_offset + end_offset;
  }

  byte *buf = new byte[size];

  size_t end_buf_offset = 0;
  size_t cur_offset;
  lsn_t cur_lsn = start_lsn;

  std::vector<std::pair<size_t, lsn_t>> seg;

  if (start_offset >= end_offset) {
    cur_offset = start_offset;
    size_t sz = log_ptr->buf_size;

    while (cur_offset < sz) {
      size_t cur_off_in_block = cur_offset % OS_FILE_LOG_BLOCK_SIZE;
      size_t bytes_in_block = OS_FILE_LOG_BLOCK_SIZE - cur_off_in_block;
      if (cur_offset + bytes_in_block > sz) {
        bytes_in_block += sz - cur_offset;
      }
      size_t to_off_in_block = cur_off_in_block + bytes_in_block;
      if (to_off_in_block < LOG_BLOCK_HDR_SIZE) {
        cur_offset += bytes_in_block;
        continue;
      }
      if (cur_off_in_block >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
        cur_offset += bytes_in_block;
        continue;
      }

      size_t fix_start_offset = cur_offset;
      size_t fix_lsn = cur_lsn;
      size_t fix_len = bytes_in_block;
      if (cur_off_in_block < LOG_BLOCK_HDR_SIZE) {
        fix_start_offset += LOG_BLOCK_HDR_SIZE - cur_off_in_block;
        fix_lsn += LOG_BLOCK_HDR_SIZE - cur_off_in_block;
        fix_len -= LOG_BLOCK_HDR_SIZE - cur_off_in_block;
      }
      if (to_off_in_block >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
        fix_len -=
            to_off_in_block - (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) + 1;
      }

      memcpy(buf + end_buf_offset, log_ptr->buf + fix_start_offset, fix_len);

      seg.emplace_back(end_buf_offset, fix_lsn);

      cur_offset += bytes_in_block;
      cur_lsn += bytes_in_block;
      end_buf_offset += fix_len;
    }
    cur_offset = 0;
  } else {
    cur_offset = start_offset;
  }
  while (cur_offset < end_offset) {
    size_t cur_off_in_block = cur_offset % OS_FILE_LOG_BLOCK_SIZE;
    size_t bytes_in_block = OS_FILE_LOG_BLOCK_SIZE - cur_off_in_block;
    if (cur_offset + bytes_in_block >= end_offset) {
      bytes_in_block = end_offset - cur_offset;
    }
    size_t to_off_in_block = cur_off_in_block + bytes_in_block;
    if (to_off_in_block < LOG_BLOCK_HDR_SIZE) {
      cur_offset += bytes_in_block;
      continue;
    }
    if (cur_off_in_block >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
      cur_offset += bytes_in_block;
      continue;
    }

    size_t fix_start_offset = cur_offset;
    size_t fix_lsn = cur_lsn;
    size_t fix_len = bytes_in_block;
    if (cur_off_in_block < LOG_BLOCK_HDR_SIZE) {
      fix_start_offset += LOG_BLOCK_HDR_SIZE - cur_off_in_block;
      fix_lsn += LOG_BLOCK_HDR_SIZE - cur_off_in_block;
      fix_len -= LOG_BLOCK_HDR_SIZE - cur_off_in_block;
    }
    if (to_off_in_block >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
      fix_len -=
          to_off_in_block - (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE);
    }

    memcpy(buf + end_buf_offset, log_ptr->buf + fix_start_offset, fix_len);

    seg.emplace_back(end_buf_offset, fix_lsn);

    cur_offset += bytes_in_block;
    cur_lsn += bytes_in_block;
    end_buf_offset += fix_len;
  }

  byte *ptr = buf;
  assert(end_buf_offset <= size);
  byte *end_ptr = buf + end_buf_offset;

  size_t offset = 0;

  while (ptr < end_ptr) {
    bool single_rec;
    switch (*ptr) {
      case MLOG_DUMMY_RECORD:
        single_rec = true;
        break;
      default:
        single_rec = !!(*ptr & MLOG_SINGLE_REC_FLAG);
    }
    size_t len = 0;
    if (single_rec) {
      if (handle_single_rec(ptr, end_ptr, &len, start_lsn)) {
        break;
      }
    } else {
      if (handle_multi_rec(ptr, end_ptr, &len, start_lsn)) {
        break;
      }
    }

    // ============== debug ===============
    auto it = upper_bound(seg.begin(), seg.end(), offset,
                          [](size_t lhs, const std::pair<size_t, lsn_t> &rhs) {
                            return lhs < rhs.first;
                          });
    it--;
    lsn_t left [[maybe_unused]] = it->second + (offset - it->first);
    // ============== debug ===============
    offset += len;
    ptr += len;
    // ============== debug ===============
    it = upper_bound(seg.begin(), seg.end(), offset,
                     [](size_t lhs, const std::pair<size_t, lsn_t> &rhs) {
                       return lhs < rhs.first;
                     });
    it--;
    lsn_t right [[maybe_unused]] = it->second + (offset - it->first);
    // ============== debug ===============
  }

  auto it = upper_bound(seg.begin(), seg.end(), offset,
                        [](size_t lhs, const std::pair<size_t, lsn_t> &rhs) {
                          return lhs < rhs.first;
                        });
  assert(it != seg.begin());
  it--;
  lsn_t next_lsn = it->second + (offset - it->first);

  delete[] buf;
  return next_lsn;
}

byte *LogParser::parse_log_body(mlog_id_t type, byte *ptr, byte *end_ptr,
                                space_id_t space_id, page_no_t page_no,
                                ulint parsed_bytes, lsn_t start_lsn,
                                bool apply_to_rapid) {
  bool applying_redo = false;
  buf_block_t *block = nullptr;
  mtr_t *mtr = nullptr;

  switch (type) {
#ifndef UNIV_HOTBACKUP
    case MLOG_FILE_DELETE:

      return fil_tablespace_redo_delete(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_CREATE:

      return fil_tablespace_redo_create(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_RENAME:

      return fil_tablespace_redo_rename(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_EXTEND:

      return fil_tablespace_redo_extend(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);
#else  /* !UNIV_HOTBACKUP */
      // Mysqlbackup does not execute file operations. It cares for all
      // files to be at their final places when it applies the redo log.
      // The exception is the restore of an incremental_with_redo_log_only
      // backup.
    case MLOG_FILE_DELETE:

      return fil_tablespace_redo_delete(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_CREATE:

      return fil_tablespace_redo_create(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_RENAME:

      return fil_tablespace_redo_rename(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_EXTEND:

      return fil_tablespace_redo_extend(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);
#endif /* !UNIV_HOTBACKUP */

    case MLOG_INDEX_LOAD:
#ifdef UNIV_HOTBACKUP
      // While scaning redo logs during a backup operation a
      // MLOG_INDEX_LOAD type redo log record indicates, that a DDL
      // (create index, alter table...) is performed with
      // 'algorithm=inplace'. The affected tablespace must be re-copied
      // in the backup lock phase. Record it in the index_load_list.
      if (!recv_recovery_on) {
        index_load_list.emplace_back(
            std::pair<space_id_t, lsn_t>(space_id, recv_sys->recovered_lsn));
      }
#endif /* UNIV_HOTBACKUP */
      if (end_ptr < ptr + 8) {
        return nullptr;
      }

      return ptr + 8;

    case MLOG_WRITE_STRING:

#ifdef UNIV_HOTBACKUP
      if (recv_recovery_on && meb_is_space_loaded(space_id)) {
#endif /* UNIV_HOTBACKUP */
        /* For encrypted tablespace, we need to get the encryption key
        information before the page 0 is recovered. Otherwise, redo will not
        find the key to decrypt the data pages. */
        if (page_no == 0 && !applying_redo &&
            !fsp_is_system_or_temp_tablespace(space_id) &&
            /* For cloned db header page has the encryption information. */
            !recv_sys->is_cloned_db) {
          ut_ad(LSN_MAX != start_lsn);
          return fil_tablespace_redo_encryption(ptr, end_ptr, space_id,
                                                start_lsn);
        }
#ifdef UNIV_HOTBACKUP
      }
#endif /* UNIV_HOTBACKUP */

      break;

    default:
      break;
  }

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

        ulint offs [[maybe_unused]] = mach_read_from_2(ptr);

        switch (type) {
          default:
            ut_error;
          case MLOG_2BYTES:
            break;
          case MLOG_4BYTES:
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
        // ptr = page_cur_parse_insert_rec(false, ptr, end_ptr, block, index,
        // mtr);

        mtr_t mtr;
        if (apply_to_rapid) {
          // find block
          const page_id_t page_id(space_id, page_no);
          bool found;
          const page_size_t page_size =
              fil_space_get_page_size(space_id, &found);
          if (!found) {
            // TODO: handle error
            assert(0);
          }
          mtr_start(&mtr);
          block = buf_page_get(page_id, page_size, RW_X_LATCH, UT_LOCATION_HERE,
                               &mtr);
        }

        // parse and apply
        ptr = parse_insert_rec(false, ptr, end_ptr, block, index,
                               apply_to_rapid, mtr);

        if (apply_to_rapid && !ptr) {
          mtr_commit(&mtr);
        }
      }

      break;

    case MLOG_REC_INSERT_8027:
    case MLOG_COMP_REC_INSERT_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_INSERT_8027, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_cur_parse_insert_rec(false, ptr, end_ptr, block, index, mtr);
      }
      break;

    case MLOG_REC_CLUST_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        mtr_t mtr;
        if (apply_to_rapid) {
          // find block
          const page_id_t page_id(space_id, page_no);
          bool found;
          const page_size_t page_size =
              fil_space_get_page_size(space_id, &found);
          if (!found) {
            // TODO: handle error
            assert(0);
          }
          mtr_start(&mtr);
          block = buf_page_get(page_id, page_size, RW_X_LATCH, UT_LOCATION_HERE,
                               &mtr);
        }

        ptr = parse_delete_rec(ptr, end_ptr, page, nullptr, block, index,
                               apply_to_rapid, mtr);

        // ptr = btr_cur_parse_del_mark_set_clust_rec(ptr, end_ptr, page,
        // page_zip,
        //                                            index);

        if (apply_to_rapid && !ptr) {
          mtr_commit(&mtr);
        }
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

        ptr = btr_cur_parse_del_mark_set_clust_rec(ptr, end_ptr, page, page_zip,
                                                   index);
      }

      break;

    case MLOG_COMP_REC_SEC_DELETE_MARK:

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

      ut_ad(!page || fil_page_type_is_index(page_type));

      ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr, page, page_zip);
      break;

    case MLOG_REC_UPDATE_IN_PLACE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr =
            btr_cur_parse_update_in_place(ptr, end_ptr, page, page_zip, index);
      }

      break;

    case MLOG_REC_UPDATE_IN_PLACE_8027:
    case MLOG_COMP_REC_UPDATE_IN_PLACE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_UPDATE_IN_PLACE_8027,
               &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr =
            btr_cur_parse_update_in_place(ptr, end_ptr, page, page_zip, index);
      }

      break;

    case MLOG_LIST_END_DELETE:
    case MLOG_LIST_START_DELETE:

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

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_parse_copy_rec_list_to_created_page(ptr, end_ptr, block,
                                                       index, mtr);
      }

      break;

    case MLOG_LIST_END_COPY_CREATED_8027:
    case MLOG_COMP_LIST_END_COPY_CREATED_8027:

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

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index,
                                        type == MLOG_ZIP_PAGE_REORGANIZE_8027,
                                        block, mtr);
      }

      break;

    case MLOG_PAGE_REORGANIZE_8027:
      ut_ad(!page || fil_page_type_is_index(page_type));
      /* Uncompressed pages don't have any payload in the
      MTR so ptr and end_ptr can be, and are nullptr */
      mlog_parse_index_8027(ptr, end_ptr, false, &index);
      ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

      ptr = btr_parse_page_reorganize(ptr, end_ptr, index, false, block, mtr);

      break;

    case MLOG_ZIP_PAGE_REORGANIZE:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index, true, block, mtr);
      }

      break;

    case MLOG_COMP_PAGE_REORGANIZE_8027:
    case MLOG_ZIP_PAGE_REORGANIZE_8027:

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

      /* Allow anything in page_type when creating a page. */
      ut_a(!page_zip);

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE, FIL_PAGE_INDEX);

      break;

    case MLOG_PAGE_CREATE_RTREE:
    case MLOG_COMP_PAGE_CREATE_RTREE:

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_RTREE,
                        FIL_PAGE_RTREE);

      break;

    case MLOG_PAGE_CREATE_SDI:
    case MLOG_COMP_PAGE_CREATE_SDI:

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_SDI, FIL_PAGE_SDI);

      break;

    case MLOG_UNDO_INSERT:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);

      break;

    case MLOG_UNDO_ERASE_END:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_erase_page_end(ptr, end_ptr, page, mtr);

      break;

    case MLOG_UNDO_INIT:

      /* Allow anything in page_type when creating a page. */

      ptr = trx_undo_parse_page_init(ptr, end_ptr, page, mtr);

      break;
    case MLOG_UNDO_HDR_CREATE:
    case MLOG_UNDO_HDR_REUSE:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);

      ptr = trx_undo_parse_page_header(type, ptr, end_ptr, page, mtr);

      break;

    case MLOG_REC_MIN_MARK:
    case MLOG_COMP_REC_MIN_MARK:

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

        ptr = page_cur_parse_delete_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_REC_DELETE_8027:
    case MLOG_COMP_REC_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type));

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_DELETE_8027, &index))) {
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table));

        ptr = page_cur_parse_delete_rec(ptr, end_ptr, block, index, mtr);
      }

      break;

    case MLOG_IBUF_BITMAP_INIT:

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
      bool is_encryption = check_encryption(page_no, space_id, ptr, end_ptr);

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
      assert(0);
  }

  if (index != nullptr) {
    dict_table_t *table = index->table;

    dict_mem_index_free(index);
    dict_mem_table_free(table);
  }

  return ptr;
}

void LogParser::log_allocate_rapid_events(log_t &log) {
  const size_t n = INNODB_LOG_EVENTS_DEFAULT;

  log.rapid_events_size = n;
  // TODO: use arena allocator
  log.rapid_events =
      ut::new_arr_withkey<os_event_t>(UT_NEW_THIS_FILE_PSI_KEY, ut::Count{n});
  for (size_t i = 0; i < log.rapid_events_size; ++i) {
    log.rapid_events[i] = os_event_create();
  }
}
bool LogParser::check_encryption(page_no_t page_no, space_id_t space_id,
                                 const byte *start, const byte *end) {
  /* Only page zero contains encryption metadata. */
  if (page_no != 0 || fsp_is_system_or_temp_tablespace(space_id) ||
      end < start + 4) {
    return false;
  }

  bool found = false;

  const page_size_t &page_size = fil_space_get_page_size(space_id, &found);

  if (!found) {
    return false;
  }

  auto encryption_offset = fsp_header_get_encryption_offset(page_size);
  auto offset = mach_read_from_2(start);

  /* Encryption offset at page 0 is the only way we can identify encryption
  information as of today. Ideally we should have a separate redo type. */
  if (offset == encryption_offset) {
    auto len = mach_read_from_2(start + 2);
    ut_ad(len == Encryption::INFO_SIZE);

    if (len != Encryption::INFO_SIZE) {
      /* purecov: begin inspected */
      ib::warn(ER_IB_WRN_ENCRYPTION_INFO_SIZE_MISMATCH, size_t{len},
               Encryption::INFO_SIZE);
      return false;
      /* purecov: end */
    }
    return true;
  }

  return false;
}

/** Parses a log record of a record insert on a page.
@return end of log record or NULL */
byte *LogParser::parse_insert_rec(
    bool is_short,       /*!< in: true if short inserts */
    const byte *ptr,     /*!< in: buffer */
    const byte *end_ptr, /*!< in: buffer end */
    buf_block_t *block,  /*!< in: page or NULL */
    dict_index_t *index, /*!< in: record descriptor */
    bool apply_to_rapid, mtr_t &mtr) {
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

  page = block ? buf_block_get_frame(block) : nullptr;

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

  page_cur_position(cursor_rec, block, &cursor);

  offsets = rec_get_offsets(buf + origin_offset, index, offsets,
                            ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
  // if (UNIV_UNLIKELY(!page_cur_rec_insert(&cursor, buf + origin_offset, index,
  //                                        offsets, mtr))) {
  //   /* The redo log record should only have been written
  //   after the write was successful. */
  //   ut_error;
  // }

  // for rapid
  if (apply_to_rapid) {
    uint64_t index_id =
        mach_read_from_8(block->frame + PAGE_HEADER + PAGE_INDEX_ID);

    mtr_commit(&mtr);

    const dict_index_t *real_index = nullptr;

    find_index(index_id, &real_index);

    if (real_index != nullptr) {
      if (apply_insert(buf + origin_offset, index, offsets, real_index)) {
        // TODO: handle erro
        return nullptr;
      }
    }
  }

  if (buf != buf1) {
    ut::free(buf);
  }

  if (UNIV_LIKELY_NULL(heap)) {
    mem_heap_free(heap);
  }

  return (const_cast<byte *>(ptr + end_seg_len));
}

byte *LogParser::parse_delete_rec(
    byte *ptr,                /*!< in: buffer */
    byte *end_ptr,            /*!< in: buffer end */
    page_t *page,             /*!< in/out: page or NULL */
    page_zip_des_t *page_zip, /*!< in/out: compressed page, or NULL */
    buf_block_t *block,
    dict_index_t *index, /*!< in: index corresponding to page */
    bool apply_to_rapid, mtr_t &mtr) {
  ulint pos;
  trx_id_t trx_id;
  roll_ptr_t roll_ptr;
  ulint offset;
  rec_t *rec;

  ut_ad(!page || page_is_comp(page) == dict_table_is_comp(index->table));

  if (end_ptr < ptr + 2) {
    return (nullptr);
  }

  auto flags [[maybe_unused]] = mach_read_from_1(ptr);
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

  if (apply_to_rapid) {
    uint64_t index_id =
        mach_read_from_8(block->frame + PAGE_HEADER + PAGE_INDEX_ID);

    mtr_commit(&mtr);

    const dict_index_t *real_index = nullptr;

    find_index(index_id, &real_index);

    if (real_index != nullptr) {
      ulint offsets_[REC_OFFS_NORMAL_SIZE];
      ulint *offsets = offsets_;
      rec_offs_init(offsets_);
      mem_heap_t *heap = nullptr;

      rec = block->frame + offset;

      offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                                UT_LOCATION_HERE, &heap);

      if (apply_delete(rec, real_index, offsets)) {
        // TODO: handle erro
        return nullptr;
      }
      if (UNIV_LIKELY_NULL(heap)) {
        mem_heap_free(heap);
      }
    }
  }
  return ptr;
}

void LogParser::find_index(uint64 idx_id, const dict_index_t **index) {
  btr_pcur_t pcur;
  mtr_t mtr;
  MDL_ticket *mdl = nullptr;
  THD *thd = current_thd;
  dict_table_t *dd_indexes;

  *index = nullptr;

  mem_heap_t *heap = mem_heap_create(100, UT_LOCATION_HERE);
  dict_sys_mutex_enter();
  mtr_start(&mtr);

  const rec_t *rec = dd_startscan_system(thd, &mdl, &pcur, &mtr,
                                         dd_indexes_name.c_str(), &dd_indexes);
  while (rec) {
    const dict_index_t *index_rec;
    MDL_ticket *mdl_on_tab = nullptr;
    dict_table_t *parent = nullptr;
    MDL_ticket *mdl_on_parent = nullptr;

    bool ret =
        dd_process_dd_indexes_rec(heap, rec, &index_rec, nullptr, &parent,
                                  &mdl_on_parent, dd_indexes, &mtr);

    dict_sys_mutex_exit();

    if (ret) {
      if (index_rec->id == idx_id) {
        *index = index_rec;
        mem_heap_empty(heap);
        return;
      }
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
  }

  mtr_commit(&mtr);
  dd_table_close(dd_indexes, thd, &mdl, true);
  dict_sys_mutex_exit();
  mem_heap_free(heap);
}

int LogParser::apply_insert(const byte *rec, dict_index_t *index,
                            ulint *offsets, const dict_index_t *real_index) {
  std::string db_name, table_name;
  real_index->table->get_table_name(db_name, table_name);
  // get field length from rapid
  ShannonBase::RapidShare *share = shannon_loaded_tables->get(db_name, table_name);
  if (share == nullptr) {  // skip
    return 0;
  }

  // TABLE *table = &share->table_;
  TABLE *table = nullptr;
  uint32 n_fields = table->s->fields;

  // convert
  innodb_rec_convert_to_mysql_rec(rec, real_index, offsets, table, n_fields);
  // insert into rapid ...

  return 0;
}

int LogParser::apply_delete(const byte *rec, const dict_index_t *index,
                            ulint *offsets) {
  std::string db_name, table_name;
  index->table->get_table_name(db_name, table_name);
  // get field length from rapid
  ShannonBase::RapidShare *share = shannon_loaded_tables->get(db_name, table_name);
  if (share == nullptr) {  // skip
    return 0;
  }

  TABLE *table = nullptr;

  uint32 n_fields = table->s->fields;

  // convert
  innodb_rec_convert_to_mysql_rec(rec, index, offsets, table, n_fields);

  // apply to rapdi
  return 0;
}

uint64 LogParser::get_trx_id(const rec_t *rec, const dict_index_t *rec_index,
                             const ulint *offsets) {
  ulint len;
  const byte *data = rec_get_nth_field(rec, offsets, rec_index->n_uniq, &len);
  assert(len == 6);
  trx_id_t trx_id = mach_read_from_6(data);
  return trx_id;
}

const byte *LogParser::rec_get_nth_field(const rec_t *rec, const ulint *offsets,
                                         ulint n, ulint *len) {
  ulint off = rec_get_nth_field_offs(offsets, n, len);
  return (rec + off);
}

void LogParser::innodb_rec_convert_to_mysql_rec(const rec_t *rec,
                                                const dict_index_t *rec_index,
                                                const ulint *offsets,
                                                TABLE *table, uint32 n_fields) {
  dict_table_t *dict_table = rec_index->table;
  uchar *buf = table->record[0];

  assert(rec_index->n_def == offsets[1]);

  for (uint32 i = 0; i < n_fields; ++i) {
    dict_col_t *col = dict_table->cols + i;
    uint32 index_col = find_col_in_index(rec_index, col);
    ulint len;
    const byte *data = rec_get_nth_field(rec, offsets, index_col, &len);
    Field *field = *(table->field + i);
    if (len == UNIV_SQL_NULL) {
      field->set_null();
    }
    uint32 offset = field->offset(buf);
    innodb_field_convert_to_mysql_field(buf + offset, data, len, col, field);
  }
}

ulint LogParser::rec_get_nth_field_offs(const ulint *offsets, ulint n,
                                        ulint *len) {
  ulint offs;
  ulint length;
  ut_ad(n < rec_offs_n_fields(offsets));
  ut_ad(len);

  if (n == 0) {
    offs = 0;
  } else {
    offs = rec_offs_base(offsets)[n] & REC_OFFS_MASK;
  }

  length = rec_offs_base(offsets)[1 + n];

  if (length & REC_OFFS_SQL_NULL) {
    length = UNIV_SQL_NULL;
  } else if (length & REC_OFFS_DEFAULT) {
    length = UNIV_SQL_ADD_COL_DEFAULT;
  } else if (length & REC_OFFS_DROP) {
    length = UNIV_SQL_INSTANT_DROP_COL;
  } else {
    length &= REC_OFFS_MASK;
    length -= offs;
  }

  *len = length;
  return (offs);
}

uint32 LogParser::find_col_in_index(const dict_index_t *index,
                                    const dict_col_t *col) {
  for (uint32 i = 0; i < index->n_def; ++i) {
    const dict_field_t *field = index->fields + i;
    if (field->col == col) {
      return i;
    }
  }
  // TODO: handle error
  assert(0);
}

// here we will use the innodb.
void LogParser::innodb_field_convert_to_mysql_field(byte *dest,
                                                    const byte *data, ulint len,
                                                    dict_col_t *col,
                                                    Field *field) {
  byte *ptr;
  switch (col->mtype) {
    case DATA_INT:
      ptr = dest + len;

      for (;;) {
        ptr--;
        *ptr = *data;
        if (ptr == dest) {
          break;
        }
        data++;
      }

      if (!(col->prtype & DATA_UNSIGNED)) {
        dest[len - 1] = (byte)(dest[len - 1] ^ 128);
      }

      break;

    case DATA_VARCHAR:
    case DATA_VARMYSQL:
    case DATA_BINARY: {
      ulint lenlen = len > 255 ? 2 : 1;
      dest = row_mysql_store_true_var_len(dest, len, lenlen);
      memcpy(dest, data, len);
      break;
    }
    case DATA_BLOB:
    case DATA_POINT:
    case DATA_VAR_POINT:
    case DATA_GEOMETRY:
      // TODO: handle more type
      assert(0);

    case DATA_MYSQL:
      memcpy(dest, data, len);
      if (col->get_mbminlen() == 1 && col->get_mbmaxlen() != 1) {
        // add mysql col len
        memset(dest + len, 0x20, field->pack_length() - len);
      }
      break;
    case DATA_SYS:
      if (field->type() == MYSQL_TYPE_DB_TRX_ID) {
        trx_id_t id = mach_read_from_6(data);
        memcpy(dest, &id, sizeof(trx_id_t));
      }
      break;
    default:
      memcpy(dest, data, len);
  }
}

}  // namespace Populate
}  // namespace ShannonBase