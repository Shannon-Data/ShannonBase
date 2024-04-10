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

#include "storage/rapid_engine/populate/log_reader.h"

#include "current_thd.h"

#include "storage/innobase/include/btr0pcur.h"  //for btr_pcur_t
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/dict0mem.h"  //for dict_index_t, etc.
#include "storage/innobase/include/ibuf0ibuf.h"
#include "storage/innobase/include/log0test.h"
#include "storage/innobase/include/row0ins.h"
#include "storage/innobase/include/row0upd.h"
#include "storage/innobase/include/trx0rec.h"
#include "storage/innobase/include/trx0undo.h"

#include "storage/innobase/include/row0mysql.h"
#include "storage/innobase/rem/rec.h"
namespace ShannonBase {
namespace Populate {

/*** how to use LogReader to read the red log files.
  int main () {
    LogReader logReader("../mysql-replication/primary/data/",
  "../mysql-replication/primary/data/ib_logfile0");
   if (!logReader.init()) {
        std::cout << "[ERROR] Error initializing the log reader!" << std::endl;
        return -1;
    }

    if (logReader.read())
        return 0;
    else {
        std::cout << "[ERROR] Log reader failed!" << std::endl;
        return -1;
    }
  }
*/
bool RecordScanner::init() {
  if (parse_buffer == NULL) parse_buffer = new unsigned char[size];

  std::fill(parse_buffer, parse_buffer + size, 0);
  length = 0;
  return true;
}

bool RecordScanner::scan(const unsigned char *block, const uint32_t &offset) {
  assert(offset >= LOG_BLOCK_HDR_SIZE);
  assert(offset < (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE));

  ulint data_length = mach_read_from_2(block + LOG_BLOCK_HDR_DATA_LEN);
  assert(data_length >= offset);

  uint32_t cp_sz{0};
  if (data_length == OS_FILE_LOG_BLOCK_SIZE)
    cp_sz = data_length - (offset + LOG_BLOCK_TRL_SIZE);
  else {
    assert(data_length < OS_FILE_LOG_BLOCK_SIZE);
    cp_sz = data_length - offset;
  }
  // not enough space in the buffer to copy the data
  if ((size - length) < cp_sz) return false;

  memcpy(parse_buffer + length, (block + offset), cp_sz);
  length += cp_sz;
  return true;
}

// The record handler.
int64_t RecordHandler::handle_system_records(const mlog_id_t &type,
                                             const unsigned char *buffer,
                                             const lsn_t &lsn,
                                             const unsigned char *end_ptr) {
  // to check the 'type'.
  return handle_mlog_checkpoint(buffer, end_ptr);
}

int64_t RecordHandler::handle_mlog_8bytes(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          ulint page_offset,
                                          const unsigned char *end_ptr) {
  // could take variable number of bytes ...
  const unsigned char *ptr = buffer;
  uint64_t dval [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);

  int64_t buffer_offset = (ptr - buffer);
  return buffer_offset;
}

int64_t RecordHandler::handle_mlog_file_rename2(const unsigned char *buffer,
                                                uint32_t space_id,
                                                uint32_t page_id, ulint len,
                                                const unsigned char *end_ptr) {
  ulint new_name_len = mach_read_from_2(buffer + len);
  return len + 2 + new_name_len;
}

int64_t RecordHandler::handle_mlog_2bytes(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          ulint page_offset,
                                          const unsigned char *end_ptr) {
  ulint val;
  return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
}

int64_t RecordHandler::handle_mlog_4bytes(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          ulint page_offset,
                                          const unsigned char *end_ptr) {
  ulint val;
  return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
}

int64_t RecordHandler::handle_mlog_1byte(const mlog_id_t &type,
                                         const unsigned char *buffer,
                                         uint32_t space_id, uint32_t page_id,
                                         ulint page_offset,
                                         const unsigned char *end_ptr) {
  ulint val;
  return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
}

int64_t RecordHandler::calculate_bytes_consumed_4bytes(
    const unsigned char *buffer, ulint *val, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  (*val) = mach_parse_compressed(&ptr, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_write_string(const mlog_id_t &type,
                                                const unsigned char *buffer,
                                                uint32_t space_id,
                                                uint32_t page_id,
                                                const unsigned char *end_ptr) {
  // a string includes the page offset; i.e., to where the string should be
  // written in a page and the string's length.
  int64_t page_offset [[maybe_unused]] = mach_read_from_2(buffer);
  int64_t len = mach_read_from_2(buffer + 2);
  std::string s((const char *)(buffer + 4), len);
  return (2 + 2 + len);
}

int64_t RecordHandler::handle_mlog_undo_hdr_reuse(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  trx_id_t trx_id [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_undo_hdr_create(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  trx_id_t trx_id [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_rec_insert_comp(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  ptr += handle_index_info("insert_comp", type, buffer, space_id, page_id,
                           end_ptr);
  ptr += handle_mlog_rec_insert(type, ptr, space_id, page_id, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_rec_delete_mark_comp(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  ptr += handle_index_info("delete_comp", type, buffer, space_id, page_id,
                           end_ptr);
  ptr += handle_mlog_rec_delete_mark(type, ptr, space_id, page_id, end_ptr);

  return (ptr - buffer);
}

int64_t RecordHandler::handle_secondary_index_delete(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  ulint val [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  return (ptr - buffer);
}

int64_t RecordHandler::handle_rec_update_inplace_comp(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  ptr += handle_index_info("update_inplace_comp", type, buffer, space_id,
                           page_id, end_ptr);
  ptr += handle_rec_update_inplace(type, ptr, space_id, page_id, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_delete_record_list(const mlog_id_t &type,
                                                 const unsigned char *buffer,
                                                 uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;
  return (ptr - buffer);
}

int64_t RecordHandler::handle_delete_record_list_comp(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  ptr += handle_index_info("delete_record_list", type, buffer, space_id,
                           page_id, end_ptr);
  ptr += handle_delete_record_list(type, ptr, space_id, page_id, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_copy_rec_list_to_created_page(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  ulint log_data_len = mach_read_from_4(ptr);
  ptr += 4;
  ptr += log_data_len;
  return (ptr - buffer);
}

int64_t RecordHandler::handle_copy_rec_list_to_created_page_comp(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  ptr += handle_index_info("copy_rec_list_to_created_page", type, buffer,
                           space_id, page_id, end_ptr);
  ptr += handle_copy_rec_list_to_created_page(type, ptr, space_id, page_id,
                                              end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_file_x(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          const unsigned char *end_ptr) {
  uint64_t offset = 0;
  ulint len = mach_read_from_2(buffer);
  offset += 2;  // length 2 bytes ...

  switch (type) {
    case MLOG_FILE_DELETE:
      return (2 +
              handle_mlog_file_delete(buffer, space_id, page_id, len, end_ptr));
    case MLOG_FILE_CREATE:
      return (2 + handle_mlog_file_create2(buffer, space_id, page_id, len,
                                           end_ptr));
    case MLOG_FILE_RENAME:
      return (2 + handle_mlog_file_rename2(buffer, space_id, page_id, len,
                                           end_ptr));
    default: {
      return 0;
    }
  }
}

int64_t RecordHandler::handle_mlog_nbytes(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          const unsigned char *end_ptr) {
  // mlog_nbytes contain an offset and then the value ...
  // offset is 2 bytes -- this is the offset within the page ...
  ulint page_offset = mach_read_from_2(buffer);
  int64_t buffer_offset = 2;  // cos 2 bytes is for the page offset ...

  switch (type) {
    case MLOG_1BYTE:
      return (buffer_offset + handle_mlog_1byte(type, (buffer + buffer_offset),
                                                space_id, page_id, page_offset,
                                                end_ptr));
    case MLOG_2BYTES:
      return (buffer_offset + handle_mlog_2bytes(type, (buffer + buffer_offset),
                                                 space_id, page_id, page_offset,
                                                 end_ptr));
    case MLOG_4BYTES:
      return (buffer_offset + handle_mlog_4bytes(type, (buffer + buffer_offset),
                                                 space_id, page_id, page_offset,
                                                 end_ptr));
    case MLOG_8BYTES:
      return (buffer_offset + handle_mlog_8bytes(type, (buffer + buffer_offset),
                                                 space_id, page_id, page_offset,
                                                 end_ptr));
    default:
      return 0;
  }
}

int64_t RecordHandler::handle_index_info(const char *operation,
                                         const mlog_id_t &type,
                                         const unsigned char *buffer,
                                         uint32_t space_id, uint32_t page_id,
                                         const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...

  // number of fields in the index ..
  ulint n_fields = mach_read_from_2(ptr);
  ptr += 2;
  ulint n_uniq_fields [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  // if n_fields == n_uniq_fields, then this is a normal index; otherwise
  // we are dealing with a clustered index ...
  for (ulint i = 0; i < n_fields; i++) {
    ulint len [[maybe_unused]] = mach_read_from_2(ptr);
    ptr += 2;
    /* The high-order bit of len is the NOT NULL flag;
       the rest is 0 or 0x7fff for variable-length fields,
       and 1..0x7ffe for fixed-length fields. */
  }

  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_rec_insert(const mlog_id_t &type,
                                              const unsigned char *buffer,
                                              uint32_t space_id,
                                              uint32_t page_id,
                                              const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  // 1. first the index ...
  // no index stuff for non-compact records ....

  // 2. now the record itself ...
  // page offset ...
  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  ulint end_seg_len = mach_parse_compressed(&ptr, end_ptr);

  if (end_seg_len & 0x1UL) {
    /* Read the info bits */
    ulint info_and_status_bits [[maybe_unused]] = mach_read_from_1(ptr);
    ptr++;

    ulint origin_offset [[maybe_unused]] = mach_parse_compressed(&ptr, end_ptr);
    ulint mismatch_index [[maybe_unused]] =
        mach_parse_compressed(&ptr, end_ptr);
  }

  ptr += (end_seg_len >> 1);

  return (ptr - buffer);
}
int64_t RecordHandler::handle_mlog_rec_delete_mark(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  // 2. now the record itself ...
  // page offset ...
  ulint flags [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;
  ulint val [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;

  ulint pos [[maybe_unused]] = mach_parse_compressed(&ptr, end_ptr);

  ulint roll_ptr [[maybe_unused]] = trx_read_roll_ptr(ptr);
  ptr += DATA_ROLL_PTR_LEN;

  ulint trx_id [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  offset = mach_read_from_2(ptr);
  ptr += 2;

  return (ptr - buffer);
}

int64_t RecordHandler::handle_rec_update_inplace(const mlog_id_t &type,
                                                 const unsigned char *buffer,
                                                 uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  ulint flags [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;

  ulint pos [[maybe_unused]] = mach_parse_compressed(&ptr, end_ptr);

  roll_ptr_t roll_ptr [[maybe_unused]] = trx_read_roll_ptr(ptr);
  ptr += DATA_ROLL_PTR_LEN;

  trx_id_t trx_id [[maybe_unused]] = mach_u64_parse_compressed(&ptr, end_ptr);

  ulint rec_offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  // index info ....
  ulint info_bits [[maybe_unused]] = mach_read_from_1(ptr);
  ptr++;
  ulint n_fields = mach_parse_compressed(&ptr, end_ptr);

  for (ulint i = 0; i < n_fields; i++) {
    ulint field_no [[maybe_unused]];
    field_no = mach_parse_compressed(&ptr, end_ptr);
    ulint len = mach_parse_compressed(&ptr, end_ptr);

    if (len != UNIV_SQL_NULL) {
      ptr += len;
    }
  }

  return (ptr - buffer);
}

int64_t RecordHandler::handle_page_reorganize(const mlog_id_t &type,
                                              const unsigned char *buffer,
                                              uint32_t space_id,
                                              uint32_t page_id,
                                              const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  if (type != MLOG_PAGE_REORGANIZE) {
    ptr += handle_index_info("page_reorganize", type, buffer, space_id, page_id,
                             end_ptr);
  }

  if (type == MLOG_ZIP_PAGE_REORGANIZE) {
    ulint level [[maybe_unused]] = mach_read_from_1(ptr);
    ++ptr;
  }

  return (ptr - buffer);
}

int64_t RecordHandler::handle_add_undo_rec(const mlog_id_t &type,
                                           const unsigned char *buffer,
                                           uint32_t space_id, uint32_t page_id,
                                           const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint len = mach_read_from_2(ptr);
  ptr += 2;

  ptr += len;
  return (ptr - buffer);
}

int64_t RecordHandler::handle_undo_init(const mlog_id_t &type,
                                        const unsigned char *buffer,
                                        uint32_t space_id, uint32_t page_id,
                                        const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint undo_type [[maybe_unused]] = mach_parse_compressed(&ptr, end_ptr);
  return (ptr - buffer);
}

int64_t RecordHandler::handle_rec_min_mark(const mlog_id_t &type,
                                           const unsigned char *buffer,
                                           uint32_t space_id, uint32_t page_id,
                                           const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;
  return (ptr - buffer);
}
int64_t RecordHandler::handle_mlog_rec_delete(const mlog_id_t &type,
                                              const unsigned char *buffer,
                                              uint32_t space_id,
                                              uint32_t page_id,
                                              const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  if (type == MLOG_REC_DELETE)
    ptr += handle_index_info("delete_rec", type, buffer, space_id, page_id,
                             end_ptr);

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  return (ptr - buffer);
}
int64_t RecordHandler::handle_bitmap_init(const mlog_id_t &type,
                                          const unsigned char *buffer,
                                          uint32_t space_id, uint32_t page_id,
                                          const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  return (ptr - buffer);
}

int64_t RecordHandler::handle_zip_write_node_ptr(const mlog_id_t &type,
                                                 const unsigned char *buffer,
                                                 uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;
  ulint z_offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  ptr += REC_NODE_PTR_SIZE;

  return (ptr - buffer);
}

int64_t RecordHandler::handle_zip_write_blob_ptr(const mlog_id_t &type,
                                                 const unsigned char *buffer,
                                                 uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;
  ulint z_offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;

  ptr += BTR_EXTERN_FIELD_REF_SIZE;

  return (ptr - buffer);
}

int64_t RecordHandler::handle_zip_write_header(const mlog_id_t &type,
                                               const unsigned char *buffer,
                                               uint32_t space_id,
                                               uint32_t page_id,
                                               const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint offset [[maybe_unused]] = (ulint)*ptr++;
  ulint len = (ulint)*ptr++;
  ptr += len;

  return (ptr - buffer);
}

int64_t RecordHandler::handle_zip_page_compress(const mlog_id_t &type,
                                                const unsigned char *buffer,
                                                uint32_t space_id,
                                                uint32_t page_id,
                                                const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;

  ulint size = mach_read_from_2(ptr);
  ptr += 2;
  ulint trailer_size = mach_read_from_2(ptr);
  ptr += 2;

  ptr += (8 + size + trailer_size);

  return (ptr - buffer);
}

int64_t RecordHandler::handle_zip_page_compress_no_data(
    const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
    uint32_t page_id, const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  ptr += handle_index_info("zip_page_compress_no_data", type, buffer, space_id,
                           page_id, end_ptr);
  ulint level [[maybe_unused]] = mach_read_from_1(ptr);
  ptr += 1;
  return (ptr - buffer);
}

int64_t RecordHandler::handle_file_crypt_data(const mlog_id_t &type,
                                              const unsigned char *buffer,
                                              uint32_t space_id,
                                              uint32_t page_id,
                                              const unsigned char *end_ptr) {
  const unsigned char *ptr = buffer;
  uint entry_size [[maybe_unused]] = 4 +  // size of space_id
                                     2 +  // size of offset
                                     1 +  // size of type
                                     1 +  // size of iv-len
                                     4 +  // size of min_key_version
                                     4 +  // size of key_id
                                     1;   // fil_encryption_t

  ulint en_space_id [[maybe_unused]] = mach_read_from_4(ptr);
  ptr += 4;
  uint offset [[maybe_unused]] = mach_read_from_2(ptr);
  ptr += 2;
  uint en_type [[maybe_unused]] = mach_read_from_1(ptr);
  ptr += 1;
  uint len [[maybe_unused]] = mach_read_from_1(ptr);
  ptr += 1;

  uint min_key_version [[maybe_unused]] = mach_read_from_4(ptr);
  ptr += 4;

  uint key_id [[maybe_unused]] = mach_read_from_4(ptr);
  ptr += 4;

  // fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(ptr);
  uint encryption [[maybe_unused]] = mach_read_from_1(ptr);
  ptr += 1;

  ptr += len;
  // TODO

  return (ptr - buffer);
}

int64_t RecordHandler::handle_mlog_checkpoint(const unsigned char *buffer,
                                              const unsigned char *end_ptr) {
  // mlog checkpoint contains the checkpoint LSN which is 8 bytes ...
  lsn_t lsn [[maybe_unused]] = mach_read_from_8(buffer);
  return sizeof(lsn_t);
  // see the comments, removed from ver 8.x.
  return (SIZE_OF_MLOG_CHECKPOINT - 1);
}

int64_t MLogRecordHandler::handle_mlog_checkpoint(
    const unsigned char *buffer, const unsigned char *end_ptr) {
  // mlog checkpoint contains the checkpoint LSN which is 8 bytes ...
  lsn_t lsn = mach_read_from_8(buffer);

  if (given_cp_lsn == 0) {
    given_cp_lsn = lsn;
    mlog_checkpoint_found = true;
  } else {
    if (given_cp_lsn == lsn)
      mlog_checkpoint_found = true;
    else {
      return -1;
    }
  }

  // once we find the m_log_checkpoint, we do not want to continue the
  // processing ...
  suspend_processing();

  return (SIZE_OF_MLOG_CHECKPOINT - 1);
}

template <typename RecordHandler>
bool RecordParser<RecordHandler>::get_record_type(const unsigned char *buffer,
                                                  mlog_id_t *type) {
  *type = mlog_id_t(buffer[record_offset] & ~MLOG_SINGLE_REC_FLAG);
  if (UNIV_UNLIKELY(*type > MLOG_BIGGEST_TYPE)) {
    // PRINT_ERR << "Found an invalid redo log record type at offset: " <<
    // record_offset << "." << std::endl;
    return false;
  }

  // increase the offset
  ++record_offset;
  return true;
}

template <typename RecordHandler>
lsn_t RecordParser<RecordHandler>::offset_to_lsn(const lsn_t &chunk_start_lsn) {
  const lsn_t block_sz_wo_hdr_trl =
      (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_TRL_SIZE);
  // how many blocks are there in the offset ?
  lsn_t blocks = record_offset / block_sz_wo_hdr_trl;
  lsn_t remainder = record_offset % block_sz_wo_hdr_trl;

  lsn_t adjustment = 0;
  if (remainder > 0) {
    // is the start lsn is in the middle of the block ?
    lsn_t lsn_mod = chunk_start_lsn % OS_FILE_LOG_BLOCK_SIZE;

    if (lsn_mod == 0)
      adjustment = LOG_BLOCK_HDR_SIZE;
    else {
      assert(lsn_mod > LOG_BLOCK_HDR_SIZE);

      lsn_t lsn_remain = OS_FILE_LOG_BLOCK_SIZE - lsn_mod;
      assert(lsn_remain > LOG_BLOCK_TRL_SIZE);
      lsn_remain -= LOG_BLOCK_TRL_SIZE;

      if (lsn_remain == 0)
        adjustment = LOG_BLOCK_HDR_SIZE;
      else if (remainder > lsn_remain)
        adjustment = (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);
      else if (remainder == lsn_remain)
        adjustment = LOG_BLOCK_TRL_SIZE;
      else
        adjustment = 0;
    }
  }

  lsn_t lsn_at_offset = chunk_start_lsn + record_offset +
                        blocks * (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE) +
                        adjustment;
  return lsn_at_offset;
}

template <typename RecordHandler>
bool RecordParser<RecordHandler>::parse_record(const unsigned char *buffer,
                                               const lsn_t &chunk_start_lsn) {
  // get the type of the record -- type is 1 byte
  mlog_id_t type;
  if (!get_record_type(buffer, &type)) return false;

  // PRINT_INFO << "[Handler] [LSN=" << offset_to_lsn(chunk_start_lsn) - 1 << "]
  // Record Type: " << get_mlog_string(type) << std::endl;

  // handle record types that does not involve a page/space id
  switch (type) {
    case MLOG_MULTI_REC_END:
    case MLOG_DUMMY_RECORD:
      break;
      // case MLOG_CHECKPOINT: {
      //     record_offset += p_Handler->handle_system_records(type, buffer +
      //     record_offset, offset_to_lsn(chunk_start_lsn),
      //     p_Scanner->end_ptr()); return true;
      // }
  }

  // read the space id and page id
  // space id and page id is stored in 2 bytes, 2 bytes fields in some
  // compressed format ...
  byte *ptr = const_cast<byte *>(buffer + record_offset);
  uint32_t space_id = mach_parse_compressed(const_cast<const byte **>(&ptr),
                                            p_Scanner->end_ptr());

  uint32_t page_id = 0;
  if (ptr != NULL) {
    page_id = mach_parse_compressed(const_cast<const byte **>(&ptr),
                                    p_Scanner->end_ptr());
  } else {
    // PRINT_ERR << "LSN: " << (record_offset) << ", unable to retrieve page id
    // for space id: " << space_id << std::endl;
    return false;
  }

  record_offset += (ptr - (buffer + record_offset));

  int64_t length =
      (*p_Handler)(type, buffer + record_offset, space_id, page_id,
                   offset_to_lsn(chunk_start_lsn), p_Scanner->end_ptr());
  if (length < 0) {
    // PRINT_ERR << "Error parsing record " << get_mlog_string(type) <<
    // std::endl;
    return false;
  }

  record_offset += length;

  return true;
}

// returns false if a parse record failed
template <typename RecordHandler>
bool RecordParser<RecordHandler>::parse_records(const lsn_t &chunk_start_lsn) {
  while ((record_offset < p_Scanner->get_length()) &&
         p_Handler->is_continue_processing())
    if (!parse_record(p_Scanner->parse_buffer, chunk_start_lsn)) {
      return false;
    }

  return true;
}

}  // namespace Populate
}  // namespace ShannonBase