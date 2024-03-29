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

   used for a tools to read the log file.
*/

#ifndef __SHANNONBASE_LOG_READER_H__
#define __SHANNONBASE_LOG_READER_H__

#include "storage/innobase/include/log0types.h"
#include "storage/innobase/include/mtr0types.h"
#include "storage/innobase/include/trx0types.h"
#include "storage/innobase/include/univ.i"

#include "storage/rapid_engine/populate/log_commons.h"
namespace ShannonBase {
namespace Populate {

/** Size of a MLOG_CHECKPOINT record in bytes.
The record consists of a MLOG_CHECKPOINT byte followed by
mach_write_to_8(checkpoint_lsn). */
#define SIZE_OF_MLOG_CHECKPOINT 9

class RecordScanner {
 public:
  RecordScanner(const uint64_t &_size) : parse_buffer(nullptr), size(_size) {}
  // do initialization
  bool init();
  // gets the end of record buffers.
  unsigned char *end_ptr() { return (parse_buffer + size); }
  /**
   * Scans a block and add necessary parts to the parse_buffer
   * @param block
   * @return
   */
  bool scan(const unsigned char *block,
            const uint32_t &offset = LOG_BLOCK_HDR_SIZE);
  uint64_t get_length() { return length; }
  // buffer for parsing.
  unsigned char *parse_buffer{nullptr};

 private:
  const uint64_t size{0};
  uint64_t length{0};
};

// To handler the records.
class RecordHandler {
 public:
  RecordHandler() : m_continue(true) {}
  int64_t handle_system_records(const mlog_id_t &type,
                                const unsigned char *buffer, const lsn_t &lsn,
                                const unsigned char *end_ptr);

  void suspend_processing() { m_continue = false; }

  void resume_processing() { m_continue = true; }

  bool is_continue_processing() { return m_continue; }

  template <typename mlog_id_t>
  int64_t operator()(const mlog_id_t &type, const unsigned char *buffer,
                     uint32_t space_id, uint32_t page_id, const lsn_t &lsn,
                     const unsigned char *end_ptr) {
    switch (type) {
      case MLOG_FILE_DELETE:
      case MLOG_FILE_CREATE:
      case MLOG_FILE_RENAME:
        return handle_mlog_file_x(type, buffer, space_id, page_id, end_ptr);
      case MLOG_INDEX_LOAD:
        return handle_mlog_index_load(type, buffer, space_id, page_id, end_ptr);
      // case MLOG_TRUNCATE:
      //     return handle_mlog_truncate(type, buffer, space_id, page_id,
      //     end_ptr);
      case MLOG_1BYTE:
      case MLOG_2BYTES:
      case MLOG_4BYTES:
      case MLOG_8BYTES:
        return handle_mlog_nbytes(type, buffer, space_id, page_id, end_ptr);
#if 0    
            case MLOG_COMP_REC_INSERT:
                return handle_mlog_rec_insert_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_INSERT:
                return handle_mlog_rec_insert(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_CLUST_DELETE_MARK:
                return handle_mlog_rec_delete_mark(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_REC_CLUST_DELETE_MARK:
                return handle_mlog_rec_delete_mark_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_SEC_DELETE_MARK:
                return handle_secondary_index_delete(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_UPDATE_IN_PLACE:
                return handle_rec_update_inplace(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_REC_UPDATE_IN_PLACE:
                return handle_rec_update_inplace_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_LIST_END_DELETE:
            case MLOG_LIST_START_DELETE:
                return handle_delete_record_list(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_LIST_END_DELETE:
            case MLOG_COMP_LIST_START_DELETE:
                return handle_delete_record_list_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_LIST_END_COPY_CREATED:
                return handle_copy_rec_list_to_created_page(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_LIST_END_COPY_CREATED:
                return handle_copy_rec_list_to_created_page_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_PAGE_REORGANIZE:
            case MLOG_COMP_PAGE_REORGANIZE:
            case MLOG_ZIP_PAGE_REORGANIZE:
                return handle_page_reorganize(type, buffer, space_id, page_id, end_ptr);
#endif
      case MLOG_PAGE_CREATE:
      case MLOG_COMP_PAGE_CREATE:
      case MLOG_PAGE_CREATE_RTREE:
      case MLOG_COMP_PAGE_CREATE_RTREE:
        return handle_page_create(type, buffer, space_id, page_id, end_ptr);
      case MLOG_UNDO_INSERT:
        return handle_add_undo_rec(type, buffer, space_id, page_id, end_ptr);
      case MLOG_UNDO_ERASE_END:
        return handle_undo_erase_page_end(type, buffer, space_id, page_id,
                                          end_ptr);
      case MLOG_UNDO_INIT:
        return handle_undo_init(type, buffer, space_id, page_id, end_ptr);
      case MLOG_UNDO_HDR_REUSE:
        return handle_mlog_undo_hdr_reuse(type, buffer, space_id, page_id,
                                          end_ptr);
      case MLOG_UNDO_HDR_CREATE:
        return handle_mlog_undo_hdr_create(type, buffer, space_id, page_id,
                                           end_ptr);
      case MLOG_REC_MIN_MARK:
      case MLOG_COMP_REC_MIN_MARK:
        return handle_rec_min_mark(type, buffer, space_id, page_id, end_ptr);
      case MLOG_REC_DELETE_8027:
      case MLOG_COMP_REC_DELETE_8027:
        return handle_mlog_rec_delete(type, buffer, space_id, page_id, end_ptr);
      case MLOG_IBUF_BITMAP_INIT:
        return handle_bitmap_init(type, buffer, space_id, page_id, end_ptr);
      case MLOG_INIT_FILE_PAGE2:
        return handle_mlog_init_file_page2(type, buffer, space_id, page_id,
                                           end_ptr);
      case MLOG_WRITE_STRING:
        return handle_mlog_write_string(type, buffer, space_id, page_id,
                                        end_ptr);
      case MLOG_ZIP_WRITE_NODE_PTR:
        return handle_zip_write_node_ptr(type, buffer, space_id, page_id,
                                         end_ptr);
      case MLOG_ZIP_WRITE_BLOB_PTR:
        return handle_zip_write_blob_ptr(type, buffer, space_id, page_id,
                                         end_ptr);
      case MLOG_ZIP_WRITE_HEADER:
        return handle_zip_write_header(type, buffer, space_id, page_id,
                                       end_ptr);
      case MLOG_ZIP_PAGE_COMPRESS:
        return handle_zip_page_compress(type, buffer, space_id, page_id,
                                        end_ptr);
      case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
        return handle_zip_page_compress_no_data(type, buffer, space_id, page_id,
                                                end_ptr);
      // case MLOG_FILE_WRITE_CRYPT_DATA:
      //     return handle_file_crypt_data(type, buffer, space_id, page_id,
      //     end_ptr);
      default:
        ib::error() << "Unidentified redo log record type "
                    << ib::hex(unsigned(type));
        return -1;
    }
  }

 protected:
  int64_t handle_mlog_file_name(const unsigned char *buffer, uint32_t space_id,
                                uint32_t page_id, ulint len,
                                const unsigned char *end_ptr) {
    // nothing to do ..
    return len;
  }
  int64_t handle_mlog_file_delete(const unsigned char *buffer,
                                  uint32_t space_id, uint32_t page_id,
                                  ulint len, const unsigned char *end_ptr) {
    return len;
  }
  int64_t handle_mlog_file_create2(const unsigned char *buffer,
                                   uint32_t space_id, uint32_t page_id,
                                   ulint len, const unsigned char *end_ptr) {
    return len;
  }
  int64_t handle_mlog_file_rename2(const unsigned char *buffer,
                                   uint32_t space_id, uint32_t page_id,
                                   ulint len, const unsigned char *end_ptr);
  int64_t handle_mlog_file_x(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             const unsigned char *end_ptr);
  int64_t handle_mlog_index_load(const mlog_id_t &type,
                                 const unsigned char *buffer, uint32_t space_id,
                                 uint32_t page_id,
                                 const unsigned char *end_ptr) {
    return 0;
  }
  int64_t handle_mlog_truncate(const mlog_id_t &type,
                               const unsigned char *buffer, uint32_t space_id,
                               uint32_t page_id, const unsigned char *end_ptr) {
    // truncate contains the truncated LSN
    return sizeof(lsn_t);
  }
  int64_t handle_mlog_nbytes(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             const unsigned char *end_ptr);
  int64_t handle_mlog_1byte(const mlog_id_t &type, const unsigned char *buffer,
                            uint32_t space_id, uint32_t page_id,
                            ulint page_offset, const unsigned char *end_ptr);
  int64_t handle_mlog_2bytes(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             ulint page_offset, const unsigned char *end_ptr);
  int64_t handle_mlog_4bytes(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             ulint page_offset, const unsigned char *end_ptr);
  int64_t handle_mlog_8bytes(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             ulint page_offset, const unsigned char *end_ptr);
  int64_t handle_mlog_init_file_page2(const mlog_id_t &type,
                                      const unsigned char *buffer,
                                      uint32_t space_id, uint32_t page_id,
                                      const unsigned char *end_ptr) {
    return 0;
  }

  int64_t handle_mlog_write_string(const mlog_id_t &type,
                                   const unsigned char *buffer,
                                   uint32_t space_id, uint32_t page_id,
                                   const unsigned char *end_ptr);

  int64_t handle_mlog_undo_hdr_reuse(const mlog_id_t &type,
                                     const unsigned char *buffer,
                                     uint32_t space_id, uint32_t page_id,
                                     const unsigned char *end_ptr);

  int64_t handle_mlog_undo_hdr_create(const mlog_id_t &type,
                                      const unsigned char *buffer,
                                      uint32_t space_id, uint32_t page_id,
                                      const unsigned char *end_ptr);

  int64_t handle_index_info(const char *operation, const mlog_id_t &type,
                            const unsigned char *buffer, uint32_t space_id,
                            uint32_t page_id, const unsigned char *end_ptr);

  int64_t handle_mlog_rec_insert(const mlog_id_t &type,
                                 const unsigned char *buffer, uint32_t space_id,
                                 uint32_t page_id,
                                 const unsigned char *end_ptr);

  int64_t handle_mlog_rec_insert_comp(const mlog_id_t &type,
                                      const unsigned char *buffer,
                                      uint32_t space_id, uint32_t page_id,
                                      const unsigned char *end_ptr);

  int64_t handle_mlog_rec_delete_mark(const mlog_id_t &type,
                                      const unsigned char *buffer,
                                      uint32_t space_id, uint32_t page_id,
                                      const unsigned char *end_ptr);
  int64_t handle_mlog_rec_delete_mark_comp(const mlog_id_t &type,
                                           const unsigned char *buffer,
                                           uint32_t space_id, uint32_t page_id,
                                           const unsigned char *end_ptr);

  int64_t handle_secondary_index_delete(const mlog_id_t &type,
                                        const unsigned char *buffer,
                                        uint32_t space_id, uint32_t page_id,
                                        const unsigned char *end_ptr);

  int64_t handle_rec_update_inplace(const mlog_id_t &type,
                                    const unsigned char *buffer,
                                    uint32_t space_id, uint32_t page_id,
                                    const unsigned char *end_ptr);
  int64_t handle_rec_update_inplace_comp(const mlog_id_t &type,
                                         const unsigned char *buffer,
                                         uint32_t space_id, uint32_t page_id,
                                         const unsigned char *end_ptr);

  int64_t handle_delete_record_list(const mlog_id_t &type,
                                    const unsigned char *buffer,
                                    uint32_t space_id, uint32_t page_id,
                                    const unsigned char *end_ptr);

  int64_t handle_delete_record_list_comp(const mlog_id_t &type,
                                         const unsigned char *buffer,
                                         uint32_t space_id, uint32_t page_id,
                                         const unsigned char *end_ptr);

  int64_t handle_copy_rec_list_to_created_page(const mlog_id_t &type,
                                               const unsigned char *buffer,
                                               uint32_t space_id,
                                               uint32_t page_id,
                                               const unsigned char *end_ptr);

  int64_t handle_copy_rec_list_to_created_page_comp(
      const mlog_id_t &type, const unsigned char *buffer, uint32_t space_id,
      uint32_t page_id, const unsigned char *end_ptr);

  int64_t handle_page_reorganize(const mlog_id_t &type,
                                 const unsigned char *buffer, uint32_t space_id,
                                 uint32_t page_id,
                                 const unsigned char *end_ptr);
  int64_t handle_page_create(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             const unsigned char *end_ptr) {
    const unsigned char *ptr = buffer;
    return (ptr - buffer);
  }

  int64_t handle_add_undo_rec(const mlog_id_t &type,
                              const unsigned char *buffer, uint32_t space_id,
                              uint32_t page_id, const unsigned char *end_ptr);

  int64_t handle_undo_erase_page_end(const mlog_id_t &type,
                                     const unsigned char *buffer,
                                     uint32_t space_id, uint32_t page_id,
                                     const unsigned char *end_ptr) {
    const unsigned char *ptr = buffer;
    return (ptr - buffer);
  }

  int64_t handle_undo_init(const mlog_id_t &type, const unsigned char *buffer,
                           uint32_t space_id, uint32_t page_id,
                           const unsigned char *end_ptr);

  int64_t handle_rec_min_mark(const mlog_id_t &type,
                              const unsigned char *buffer, uint32_t space_id,
                              uint32_t page_id, const unsigned char *end_ptr);

  int64_t handle_mlog_rec_delete(const mlog_id_t &type,
                                 const unsigned char *buffer, uint32_t space_id,
                                 uint32_t page_id,
                                 const unsigned char *end_ptr);

  int64_t handle_bitmap_init(const mlog_id_t &type, const unsigned char *buffer,
                             uint32_t space_id, uint32_t page_id,
                             const unsigned char *end_ptr);

  int64_t handle_zip_write_node_ptr(const mlog_id_t &type,
                                    const unsigned char *buffer,
                                    uint32_t space_id, uint32_t page_id,
                                    const unsigned char *end_ptr);

  int64_t handle_zip_write_blob_ptr(const mlog_id_t &type,
                                    const unsigned char *buffer,
                                    uint32_t space_id, uint32_t page_id,
                                    const unsigned char *end_ptr);

  int64_t handle_zip_write_header(const mlog_id_t &type,
                                  const unsigned char *buffer,
                                  uint32_t space_id, uint32_t page_id,
                                  const unsigned char *end_ptr);

  int64_t handle_zip_page_compress(const mlog_id_t &type,
                                   const unsigned char *buffer,
                                   uint32_t space_id, uint32_t page_id,
                                   const unsigned char *end_ptr);

  int64_t handle_zip_page_compress_no_data(const mlog_id_t &type,
                                           const unsigned char *buffer,
                                           uint32_t space_id, uint32_t page_id,
                                           const unsigned char *end_ptr);

  int64_t handle_file_crypt_data(const mlog_id_t &type,
                                 const unsigned char *buffer, uint32_t space_id,
                                 uint32_t page_id,
                                 const unsigned char *end_ptr);
  virtual int64_t handle_mlog_checkpoint(const unsigned char *buffer,
                                         const unsigned char *end_ptr);

 private:
  int64_t calculate_bytes_consumed_4bytes(const unsigned char *buffer,
                                          ulint *val,
                                          const unsigned char *end_ptr);
  bool m_continue;
};

class MLogRecordHandler : public RecordHandler {
 private:
  bool mlog_checkpoint_found;
  lsn_t given_cp_lsn;

 public:
  MLogRecordHandler(const lsn_t &checkpoint_lsn)
      : mlog_checkpoint_found(false), given_cp_lsn(checkpoint_lsn) {}

  bool is_mlog_cp_found() { return mlog_checkpoint_found; }

  lsn_t checkpoint_lsn() { return given_cp_lsn; }
  int64_t handle_mlog_checkpoint(const unsigned char *buffer,
                                 const unsigned char *end_ptr) override;
};

template <typename RecordHandler>
class RecordParser {
 public:
  RecordParser(RecordScanner *_pScanner, RecordHandler *_pHandler)
      : p_Scanner(_pScanner), p_Handler(_pHandler), record_offset(0) {}

  // returns false if a parse record failed
  bool parse_records(const lsn_t &chunk_start_lsn);
  bool scanner_full() { return (record_offset >= p_Scanner->get_length()); }

 private:
  bool get_record_type(const unsigned char *buffer, mlog_id_t *type);

  lsn_t offset_to_lsn(const lsn_t &chunk_start_lsn);

  bool parse_record(const unsigned char *buffer, const lsn_t &chunk_start_lsn);

  RecordScanner *p_Scanner;
  RecordHandler *p_Handler;
  uint32_t record_offset;
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_READER_H__