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

#include "field_types.h" //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "sql/sql_class.h"
#include "sql/current_thd.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase{
class RapidContext;
namespace Imcs{
/**
 * The chunk is an memory pool area, where the data writes here.
 * The format of rows is following: (ref to: https://github.com/Shannon-Data/ShannonBase/issues/8)
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
  void operator() (T* ptr) {
    if (ptr) my_free(ptr);
  }
};

class Chunk : public MemoryObject{
  public:
    class Chunk_header{
      public:
        //ctor and dctor.
        Chunk_header() {}
        virtual ~Chunk_header() {}
        //field no.
        uint16 m_field_no{0};
        //statistics data.
        std::atomic<double> m_max{0}, m_min{0}, m_median{0}, m_middle{0}, m_avg{0}, m_sum{0};
        std::atomic<uint64> m_rows{0};
        //pointer to the next or prev.
        Chunk* m_next_chunk{nullptr}, *m_prev_chunk {nullptr};
        //data type in mysql.
        enum_field_types m_chunk_type {MYSQL_TYPE_TINY};
        //is null or not.
        bool m_null {false};
        //whether it is var type or not
        bool m_varlen {false};
    };
    explicit Chunk(Field* field);
    virtual ~Chunk();
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;

    Chunk_header* get_header() {
      std::scoped_lock lk(m_header_mutex);
      return m_header;
    }
    //initial the read opers.
    uint rnd_init(bool scan);
    //End of Rnd scan.
    uint rnd_end();
    //writes the data into this chunk. length unspecify means calc by chunk.
    uchar* write_data_direct(ShannonBase::RapidContext* context, uchar* data, uint length = 0);
    //reads the data by from address .
    uchar* read_data_direct(ShannonBase::RapidContext* context, uchar*buffer);
    //reads the data by rowid.
    uchar* read_data_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* buffer);
    //deletes the data by rowid.
    uchar* delete_data_direct(ShannonBase::RapidContext* context, uchar* rowid);
    //deletes all.
    uchar* delete_all_direct();
    //updates the data.
    uchar* update_date_direct(ShannonBase::RapidContext* context, uchar* rowid, uchar* data, uint length =0);
    //flush the data to disk. by now, we cannot impl this part.
    uint flush_direct(RapidContext* context, uchar* from = nullptr, uchar* to = nullptr);
    //the start loc of chunk. where the data wrtes from.
    inline uchar* get_base() const { return m_data_base; }
    //the end loc of chunk. is base + chunk_size
    inline uchar* get_end() const { return m_data_end; }
    //gets the max valid loc of current the data has written to.
    inline uchar* get_data() const {return m_data;}
    bool is_full () {return (m_data == m_data_end)? true : false;}
    ha_rows records_in_range(ShannonBase::RapidContext* context, double& min_key, double& max_key);

    uchar* where(uint offset);
    uchar* seek(uint offset);
  private:
    std::mutex m_header_mutex;
    Chunk_header* m_header{nullptr};
    //started or not
    std::atomic<uint8> m_inited;
    std::mutex m_data_mutex;
    /** the base pointer of chunk, and the current pos of data. whether data should be in order or not */
    uchar* m_data_base {nullptr};
    //current pointer, where the data is. use write.
    std::atomic<uchar*> m_data{nullptr};
    //pointer of cursor, which used for reading.
    std::atomic<uchar*> m_data_cursor {nullptr};
    //end address of memory, to determine whether the memory is full or not.
    uchar* m_data_end {nullptr};
    //the check sum of this chunk. it used to do check when the data flush to disk.
    uint m_check_sum {0};
    //maigic num of chunk.
    uint m_magic = SHANNON_MAGIC_CHUNK;
};

} //ns:imcs
} //ns:shannonbase
#endif //__SHANNONBASE_CHUNK_H__