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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_COMPRESS_DICTIONARY_H__
#define __SHANNONBASE_COMPRESS_DICTIONARY_H__

#include <map>
#include <mutex>
#include <shared_mutex>
#include "include/my_inttypes.h"
#include "include/mysql/strings/m_ctype.h"  //CHARSET_INFO
#include "include/sql_string.h" //String

namespace ShannonBase{
namespace Compress{

//Dictionary, which store all the dictionary data.
class Dictionary {
  public:
    enum class Dictionary_algo_t : uint8 {NONE, SORTED, VARLEN};
    Dictionary(Dictionary_algo_t algo) : m_algo(algo) {}
    Dictionary() = default;
    virtual ~Dictionary()  = default;
    virtual uint32 Store(String&, Dictionary_algo_t algo = Dictionary_algo_t::SORTED);
    virtual uint32 Get(uint64 strid, String& val, CHARSET_INFO& charset = my_charset_bin);
    virtual void Set_algo (Dictionary_algo_t algo) { m_algo = algo; }
    inline Dictionary_algo_t Get_algo () const { return m_algo; }
  private:
    std::shared_mutex m_content_mtx;
    std::map<std::string, uint64> m_content;
    Dictionary_algo_t m_algo{Dictionary_algo_t::SORTED};
};


} //ns:compress
} //ns:shannonbase
#endif //__SHANNONBASE_COMPRESS_DICTIONARY_H__