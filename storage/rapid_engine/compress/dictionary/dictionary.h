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
#include "include/sql_string.h"             //String

namespace ShannonBase {
namespace Compress {

enum class Encoding_type : uint8 { NONE, SORTED, VARLEN };
// Dictionary, which store all the dictionary data.
class Dictionary {
 public:
  Dictionary(Encoding_type type) : m_encoding_type(type) {}
  Dictionary() = default;
  virtual ~Dictionary() = default;
  virtual uint32 store(String &, Encoding_type type = Encoding_type::NONE);
  virtual uint32 get(uint64 strid, String &val,
                     CHARSET_INFO &charset = my_charset_bin);
  virtual void set_algo(Encoding_type type) { m_encoding_type = type; }
  virtual inline Encoding_type get_algo() const { return m_encoding_type; }
  virtual inline uint32 content_size() const { return m_content.size(); }
  virtual int lookup(String &);
  virtual int lookup(uchar *&);

 private:
  std::shared_mutex m_content_mtx;
  std::atomic<uint64> m_content_id{0};
  // cotent string<--->id map.
  std::map<std::string, uint64> m_content;
  // id<--> content string map. for access accleration.
  std::map<uint64, std::string> m_id2content;
  Encoding_type m_encoding_type{Encoding_type::NONE};
};

}  // namespace Compress
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COMPRESS_DICTIONARY_H__