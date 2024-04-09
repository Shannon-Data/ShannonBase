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
#ifndef __SHANNONBASE_CHUNK_H__
#define __SHANNONBASE_CHUNK_H__

#include <atomic>
#include <type_traits>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase {
class RapidContext;
namespace Imcs {
/**
 * The chunk is an memory pool area, where the data writes here.
 * The format of rows is following: (ref to:
 * https://github.com/Shannon-Data/ShannonBase/issues/8)
 *     +-----------------------------------------+
 *     | Info bits | TrxID | PK | SMU_ptr | Data |
 *     +-----------------------------------------+
 *   Info bits: highest bit: var bit flag: 1 two bytes, 0 one byte.
 *              (N-1)th: null flag, 1 null, 0 not null;
 *              (N-2)th: delete flag: 1 deleted, 0 not deleted.
 *              [(N-3) - 0] : data lenght;
 */
template <typename T>
struct chunk_deleter_helper {
  void operator()(T *ptr) {
    if (ptr) my_free(ptr);
  }
};

class Index;
class Chunk : public MemoryObject {
 public:
  using Chunk_header = struct alignas(CACHE_LINE_SIZE) Chunk_header_t {
   public:
    // is null or not.
    bool m_null{false};
    // whether it is var type or not
    bool m_varlen{false};
    // data type in mysql.
    enum_field_types m_chunk_type{MYSQL_TYPE_TINY};
    // field no.
    uint16 m_field_no{0};
    // pointer to the next or prev.
    Chunk *m_next_chunk{nullptr}, *m_prev_chunk{nullptr};
    // statistics data.
    std::atomic<double> m_max{0}, m_min{0}, m_median{0}, m_middle{0}, m_avg{0},
        m_sum{0};
    std::atomic<uint64> m_rows{0}, m_delete_marked{0};
  };

  explicit Chunk(Field *field);
  virtual ~Chunk();
  Chunk(Chunk &&) = delete;
  Chunk &operator=(Chunk &&) = delete;

  Chunk_header *get_header() {
    std::scoped_lock lk(m_header_mutex);
    return m_header.get();
  }
  // initial the read opers.
  uint rnd_init(bool scan);
  // End of Rnd scan.
  uint rnd_end();
  // writes the data into this chunk. length unspecify means calc by chunk.
  uchar *write_data_direct(ShannonBase::RapidContext *context, const uchar *data,
                           uint length = 0);
  // writes the data into this chunk at pos. length unspecify means calc by chunk.
  uchar *write_data_direct(ShannonBase::RapidContext *context, const uchar* pos,
                           const uchar *data, uint length = 0);
  // reads the data by from address .
  uchar *read_data_direct(ShannonBase::RapidContext *context, uchar *buffer);
  // reads the data by rowid.
  uchar *read_data_direct(ShannonBase::RapidContext *context, const uchar *rowid,
                          uchar *buffer);
  // deletes the data by rowid.
  uchar *delete_data_direct(ShannonBase::RapidContext *context, const uchar *rowid);
  // deletes all.
  uchar *delete_all_direct();
  // updates the data. rowid is pos of where the data will be updated.
  uchar *update_data_direct(ShannonBase::RapidContext *context, const uchar *rowid,
                            const uchar *data, uint length = 0);
  // flush the data to disk. by now, we cannot impl this part.
  uint flush_direct(RapidContext *context, const uchar *from = nullptr,
                    const uchar *to = nullptr);
  // the start loc of chunk. where the data wrtes from.
  inline uchar *get_base() const { return m_data_base; }
  // the end loc of chunk. is base + chunk_size
  inline uchar *get_end() const { return m_data_end; }
  // gets the max valid loc of current the data has written to.
  inline uchar *get_data() const { return m_data; }
  //the free size of this chunk.
  inline uint free_size() { return m_data_end - m_data; }
  //whether is full or not.
  inline bool is_full() { return (m_data == m_data_end) ? true : false; }
  inline bool is_empty() {return (m_data == m_data_base)? true : false; }
  inline uint data_size() { return (m_data - m_data_base); }

  //how many rows are in this range of min and max?
  ha_rows records_in_range(ShannonBase::RapidContext *context, double &min_key,
                           double &max_key);
  //get the phy address of offset.
  uchar *where(ShannonBase::RapidContext *context, uint offset);
  //set to the offset in physical address format, return the address.
  uchar *seek(ShannonBase::RapidContext *context, uint offset);
  //return the next available address in new chunk. you SHOULD set the GC trxid in context.
  uchar* GC(ShannonBase::RapidContext *context);
  //relocate the chunk to specific address[address should be in this chunk].
  uchar* reshift(ShannonBase::RapidContext *context, const uchar* from,
                  const uchar* to);
  //to set this chunk to a empty chunk.
  uchar* set_empty();
  uchar* set_full();
 private:
  inline bool init_header_info(const Field* field);
  inline bool update_statistics(double old_v, double new_v, OPER_TYPE type);
  inline bool reset_statistics();

  //value is marked as deleted return true, or false.
  inline bool deleted (const uchar* data);
  //value is null return true, or false
  inline bool is_null(const uchar* data);
 private:
  std::mutex m_header_mutex;
  std::unique_ptr<Chunk_header> m_header{nullptr};
  // started or not
  std::atomic<uint8> m_inited;
  std::mutex m_data_mutex;
  /** the base pointer of chunk, and the current pos of data. whether data
   * should be in order or not */
  uchar *m_data_base{nullptr};
  // current pointer, where the data is. use write.
  std::atomic<uchar *> m_data{nullptr};
  // pointer of cursor, which used for reading.
  std::atomic<uchar *> m_data_cursor{nullptr};
  // end address of memory, to determine whether the memory is full or not.
  uchar *m_data_end{nullptr};
  // the check sum of this chunk. it used to do check when the data flush to
  // disk.
  uint m_check_sum{0};
  // maigic num of chunk.
  uint m_magic = SHANNON_MAGIC_CHUNK;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CHUNK_H__