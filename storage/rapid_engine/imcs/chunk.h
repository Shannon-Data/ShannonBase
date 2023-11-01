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

class Chunk : public MemoryObject{
  public:
    class Chunk_header{
      public:
        std::atomic<long long> m_max, m_min, m_median, m_middle, m_avg, m_rows;
    };

    Chunk(enum_field_types type);
    virtual ~Chunk();
    
    Chunk_header& Get_header() { 
      std::scoped_lock lk(m_header_mutex);
      return m_headers;
    }
    inline bool Is_full () { return (m_headers.m_rows == Chunk::MAX_CHUNK_NUMS_LIMIT); }
    uchar* Write_data(uchar* data, uint length);
    template<typename data_T> uchar*
    Write_fixed_value(data_T value);
    
    template<typename data_T> uchar*
    Write_var_value(data_T value, uint length, CHARSET_INFO* charset);
    uchar* Read_data(uchar* data, uint length);

    uint Capacity() {return 0;}
    double Ratio() { return 100; }
    Chunk* Get_next() { return m_next; }
    Chunk* Get_prev() { return m_prev; }
    void Set_next(Chunk* next) { m_next = next; }
    void Set_prev(Chunk* prev) { m_prev = prev; }
  private:
    inline int8 Get_unit(enum_field_types type) {
      switch (type) {
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_LONG :
          return 8;
        case MYSQL_TYPE_STRING:
          return -1;
        case MYSQL_TYPE_GEOMETRY:
          return -1;
        default:
          return 4;
      }
      return 0;
    }

    std::mutex m_header_mutex;
    Chunk_header m_headers;

    std::mutex m_data_mutex;
    /** the base pointer of chunk, and the current pos of data. 
      whether data should be in order or not */
    uchar *m_data_base {nullptr}, *m_data {nullptr};
    uchar* m_data_end = m_data_base;
    static constexpr uint MAX_CHUNK_NUMS_LIMIT = MAX_NUM_CHUNK_ROWS;
    Chunk* m_next{nullptr}, *m_prev{nullptr}, *m_curr{nullptr};
    //the check sum of this chunk.
    uint m_check_sum;
};

} //ns:imcs
} //ns:shannonbase
#endif //__SHANNONBASE_CHUNK_H__