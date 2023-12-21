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
#include <memory>
#include <type_traits>

#include "field_types.h" //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_sys.h"      //for page size
#include "sql/sql_class.h"
#include "sql/field.h"   //Field
#include "sql/current_thd.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase{
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
class Chunk : public MemoryObject{
  public:
    class Chunk_header{
      public:
        //ctor and dctor.
        Chunk_header() = default;
        ~Chunk_header() = default;
        //field of this chunk.
        Field* m_field;
        //statistics data.
        std::atomic<long long> m_max, m_min, m_median, m_middle, m_avg, m_rows, m_sum;
        //pointer to the next or prev.
        Chunk* m_next_chunk{nullptr}, *m_prev_chunk {nullptr};
        //data type in mysql.
        enum_field_types m_chunk_type {MYSQL_TYPE_TINY};
        //is null or not.
        bool m_null {false};
        //whether it is var type or not
        bool m_varlen {false};
        //line number of this chunk.
        uint32 m_lines;
    };

    explicit Chunk(Field* field);
    virtual ~Chunk();
    
    Chunk_header& Get_header() { 
      std::scoped_lock lk(m_header_mutex);
      return *m_header;
    }
    //writes the data into this chunk. length unspecify means calc by chunk. 
    uchar* Write_data(RapidContext* context, uchar* data, uint length = 0);
    //reads the data by from address .
    uchar* Read_data(RapidContext* context, uchar* from, uchar* to, uint length = 0);
    //reads the data by rowid.
    uchar* Read_data(RapidContext* context, uchar* rowid, uint length = 0);
    //deletes the data by rowid.
    uchar* Delete_data(RapidContext* context, uchar* rowid);
    //deletes all.
    uchar* Delete_all();
    //updates the data.
    uchar* Update_date(RapidContext* context, uchar* rowid, uchar* data, uint length =0);
    //flush the data to disk. by now, we cannot impl this part.
    uint flush(RapidContext* context, uchar* from = nullptr, uchar* to = nullptr);

    void Set_next(Chunk* next) { m_header->m_next_chunk = next; }
    void Set_prev(Chunk* prev) { m_header->m_prev_chunk = prev; }
    inline Chunk* Get_next() const { return m_header->m_next_chunk; }
    inline Chunk* Get_prev() const { return m_header->m_prev_chunk; }
    inline uchar* Get_base() const { return m_data_base; }
    inline uchar* Get_end() const { return m_data_end; }
  private:

    static constexpr uint64 SHANNON_CHUNK_SIZE = 8 * SHANNON_MB;
    static constexpr uint SHANNON_CHUNK_VAR_MASK = 0x8;
    static constexpr uint SHANNON_CHUNK_NULL_MASK = 0x40;
    static constexpr uint SHANNON_CHUNK_DELETE_MASK = 0x20;
    static constexpr uint SHANNON_CHUNK_DATA_MASK = 0x1F;
    std::mutex m_header_mutex;
    std::unique_ptr<Chunk_header> m_header{nullptr};

    std::mutex m_data_mutex;
    
    /** the base pointer of chunk, and the current pos of data. whether data should be in order or not */
    uchar* m_data_base {nullptr};
    //current pointer, where the data is.
    uchar* m_data {nullptr};
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