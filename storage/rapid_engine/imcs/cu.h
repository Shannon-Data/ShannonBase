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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_CU_H__
#define __SHANNONBASE_CU_H__

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
class RapidContext;

namespace Imcs {
class Index;
class Dictionary;
class Chunk;
class Cu : public MemoryObject {
 public:
  /**A Snapshot Metadata Unit (SMU) contains metadata and transactional
   * information for an associated IMCU.*/
  class Snapshot_meta_unit {
  };

  using Cu_header = struct alignas(CACHE_LINE_SIZE) Cu_header_t {
   public:
    // whether the is not null or not.
    bool m_nullable{false};
    // encoding type. pls ref to:
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-varlen-encoding.html
    // https://dev.mysql.com/doc/heatwave/en/mys-hw-dictionary-encoding.html
    Compress::Encoding_type m_encoding_type{Compress::Encoding_type::NONE};
    // the index of field.
    uint16 m_field_no{0};
    // field type of this cu.
    enum_field_types m_cu_type{MYSQL_TYPE_TINY};
    // local dictionary.
    std::unique_ptr<Compress::Dictionary> m_local_dict;
    // statistics info.
    std::atomic<double> m_max{0}, m_min{0}, m_middle{0}, m_median{0}, m_avg{0},
        m_sum{0};
    std::atomic<uint64> m_rows{0}, m_deleted_mark{0};
    const CHARSET_INFO* m_charset;
    std::unique_ptr<Snapshot_meta_unit> m_smu;
  };

  explicit Cu(Field *field);
  virtual ~Cu();
  Cu(Cu &&) = delete;
  Cu &operator=(Cu &&) = delete;

  // initialization. these're for internal.
  uint rnd_init(bool scan);
  // End of Rnd scan
  uint rnd_end();
  // writes the data into this chunk. length unspecify means calc by chunk.
  uchar *write_data_direct(ShannonBase::RapidContext *context, const uchar *data,
                           uint length = 0);
  // writes the data into this chunk at pos. length unspecify means calc by chunk.
  uchar *write_data_direct(ShannonBase::RapidContext *context, const uchar* pos,
                           const uchar *data, uint length = 0);
  // reads the data by from address.
  uchar *read_data_direct(ShannonBase::RapidContext *context, uchar *buffer);
  // reads the data by rowid to buffer.
  uchar *read_data_direct(ShannonBase::RapidContext *context, const uchar *rowid,
                          uchar *buffer);
  // deletes the data by rowid
  uchar *delete_data_direct(ShannonBase::RapidContext *context, const uchar *rowid);
  // deletes the data by pk
  uchar *delete_data_direct(ShannonBase::RapidContext *context, const uchar *pk, uint pk_len);
  // deletes all
  uchar *delete_all_direct();
  // updates the data with rowid with the new data.
  uchar *update_data_direct(ShannonBase::RapidContext *context, const uchar* rowid,
                            const uchar *data, uint length = 0);
  // flush the data to disk. by now, we cannot impl this part.
  uint flush_direct(ShannonBase::RapidContext *context, const uchar *from = nullptr,
                    const uchar *to = nullptr);
  //does a gc operation of this CU.
  uchar* GC(ShannonBase::RapidContext *context);
  inline Compress::Dictionary *local_dictionary() const {
    return m_header->m_local_dict.get();
  }
  //gets its header.
  Cu_header *get_header() { return m_header.get(); }
  // gets the base address of chunks.
  uchar *get_base();
  // adds a new chunk into this cu.
  void add_chunk(std::unique_ptr<Chunk> &chunk);
  //gets the chunk with chunk id.
  inline Chunk *get_chunk(uint chunkid) {
    return (chunkid < m_chunks.size()) ? m_chunks[chunkid].get() : nullptr;
  }
  //gets the first chunk in this cu.
  inline Chunk *get_first_chunk() { return get_chunk(0); }
  //gets the last chunk in this cu
  inline Chunk *get_last_chunk() { return get_chunk(m_chunks.size() - 1); }
  //gets the how many chunks in this cu.
  inline size_t get_chunk_nums() { return m_chunks.size(); }
  //seek to the real logical physical addr by offset.
  inline uchar *seek(ShannonBase::RapidContext *context, size_t offset);
  //get the address by a PK.
  inline uchar* lookup(ShannonBase::RapidContext *context, const uchar* pk, uint pk_len);
  //the its index of this cu.
  inline Index *get_index() { return m_index.get(); }

 private:
  bool init_header_info(const Field* field);
  bool update_statistics(double old_v, double new_v, OPER_TYPE type);
  bool reset_statistics();
 private:
  uint m_magic{SHANNON_MAGIC_CU};
  // proctect header.
  std::mutex m_header_mutex;
  // header info of this Cu.
  std::unique_ptr<Cu_header> m_header{nullptr};
  // chunks in this cu.
  std::vector<std::unique_ptr<Chunk>> m_chunks;
  // current chunk read.
  std::atomic<uint32> m_current_chunk_id{0};
  // index of Cu
  std::unique_ptr<Index> m_index{nullptr};
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CU_H__