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

   The fundmental code for imcs. Column Unit.
*/
#ifndef __SHANNONBASE_CU_H__
#define __SHANNONBASE_CU_H__

#include <atomic>
#include <memory>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase {
class ShannonBaseContext;
class Rapid_load_context;

namespace Imcs {
class Index;
class Dictionary;
class Chunk;
class Cu : public MemoryObject {
 public:
  using cu_fd_t = uint64;
  using Cu_header = struct alignas(CACHE_LINE_SIZE) Cu_header_t {
   public:
    // physical row count. If you want to get logical rows, you should consider
    // MVCC to decide that whether this phyical row is visiable or not to this
    // transaction.
    std::atomic<row_id_t> m_prows{0};
    // a copy of source field info, only use its meta info. do NOT use it
    // directly.
    Field *m_source_fld{nullptr};

    // field type of this cu.
    enum_field_types m_type{MYSQL_TYPE_NULL};

    // whether the is not null or not.
    bool m_nullable{false};

    // encoding type. pls ref to:
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-varlen-encoding.html
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-dictionary-encoding.html
    Compress::Encoding_type m_encoding_type{Compress::Encoding_type::NONE};

    // local dictionary.
    std::unique_ptr<Compress::Dictionary> m_local_dict;

    // charset info of this  CU.
    const CHARSET_INFO *m_charset;

    // statistics info.
    std::atomic<double> m_max{0}, m_min{0}, m_middle{0}, m_median{0}, m_avg{0}, m_sum{0};
  };

  explicit Cu(const Field *field);
  virtual ~Cu();
  Cu(Cu &&) = delete;
  Cu &operator=(Cu &&) = delete;

  /** write the data to a cu. `data` the data will be written. returns the
  address where the data wrote. the data append to tail of Cu. returns nullptr
  failed.*/
  uchar *write_row(const Rapid_load_context *context, uchar *data, size_t len);

  /** write the data to a cu. `data` the data will be written. returns the
   address where the data wrote. the data append to tail of Cu. returns nullptr
   if failed. otherwise, return addr of where the data written.*/
  uchar *write_row_from_log(const Rapid_load_context *context, uchar *data, size_t len);

  /** read the data from this Cu, traverse all chunks in cu to get the data from
  where m_r_ptr locates. */
  uchar *read_row(const Rapid_load_context *context, uchar *data, size_t len);

  /** delete the data from this Cu, traverse all chunks in cu to delete the data
  from where m_r_ptr locates. */
  uchar *delete_row(const Rapid_load_context *context, uchar *data, size_t len);

  // delete the row by rowid.
  uchar *delete_row(const Rapid_load_context *context, row_id_t rowid);

  // delete the data from this cu. all the records equal to data will be
  // removed.
  uchar *delete_row_from_log(const Rapid_load_context *context, uchar *data, size_t len);

  // update the data located at rowid with new value-'data'.
  uchar *update_row(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len);

  // gets the base address of CU.
  uchar *base();

  // gets to the last address of CU.
  uchar *last();

  // get the chunk header.
  Cu_header *header() {
    std::scoped_lock lk(m_header_mutex);
    return m_header.get();
  }

  // get the pointer of chunk with its id.
  Chunk *chunk(uint id);

  // get how many chunks in this CU.
  inline uint32 chunks() const {
    assert((m_header->m_prows.load() / SHANNON_ROWS_IN_CHUNK + 1) == m_chunks.size());
    return m_chunks.size();
  }

  // get how many rows in this cu. Here, we dont care about MVCC. just physical
  // rows.
  row_id_t prows();

  // returns the normalized length, the text type encoded with uint32.
  size_t normalized_pack_length();

  // return the real pack length: field->pack_length.
  size_t pack_length();

 private:
  // get the field value. if field is string/text then return its stringid.
  // or, do nothing.
  uchar *get_field_value(uchar *&data, size_t &len, bool need_pack = false);

  // upda the header info, such as row count, sum, avg, etc.
  void update_meta_info(OPER_TYPE type, uchar *data);

 private:
  // proctect header.
  std::mutex m_header_mutex;
  // header info of this Cu.
  std::unique_ptr<Cu_header> m_header{nullptr};

  // chunks in this cu.
  std::vector<std::unique_ptr<Chunk>> m_chunks;

  // reader pointer.
  std::atomic<uint32> m_current_chunk{0};

  // name of this cu.
  std::string m_name;

  // magic number for CU.
  const char *m_magic = "SHANNON_CU";
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CU_H__