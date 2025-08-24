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
#include <iostream>
#include <sstream>

#include "include/my_inttypes.h"
#include "include/ut0dbg.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"

/**
 * Dictionary used for a local dictionary algorithm, in massive data volumn, maybe, there
 * are huge amount of text, which takes a lot of disk volumn to store these texts. Therefore,
 * we want to use compressed string replace the original one to save disk volumn. But, it
 * takes time to uncompress the compressed string, and send back to users. The tradeoff between
 * performance and space.
 */
namespace ShannonBase {
namespace Compress {

uint32 Dictionary::store(const uchar *str, size_t len, Encoding_type type) {
  DBUG_TRACE;
  // returns dictionary id. //encoding alg pls ref to: heatwave document.
  if (!str || !len) return Dictionary::DEFAULT_STRID;

  std::string strstr(reinterpret_cast<const char *>(str), len);
  auto algr = CompressFactory::get_instance(type);
  std::string compressed_str(algr->compressString(strstr));

  {
    std::scoped_lock lk(m_content_mtx);
    auto content_pos = m_content.find(compressed_str);
    if (content_pos != m_content.end()) return content_pos->second;

    // not found, then insert a new one.
    uint64 id = m_content.size();
    // compressed string <---> str id. get a copy of string and store it in map.
    m_content.emplace(compressed_str, id);

    // id<---> original string
    ut_a(m_id2content.find(id) == m_id2content.end());
    m_id2content.emplace(id, compressed_str);

    ut_a((m_content.size() == m_id2content.size()));
    return id;
  }

  return Dictionary::DEFAULT_STRID;
}

int32 Dictionary::id(uint64 strid, String &ret_val) {
  std::scoped_lock lk(m_content_mtx);
  auto compressed_str = get(strid);
  if (compressed_str.empty()) return -1;

  String strs(compressed_str.c_str(), compressed_str.length(), ret_val.charset());
  copy_if_not_alloced(&ret_val, &strs, strs.length());

  return Dictionary::DEFAULT_STRID;
}

std::string Dictionary::get(uint64 strid) {
  {
    std::scoped_lock lk(m_content_mtx);
    auto id_pos = m_id2content.find(strid);
    if (id_pos != m_id2content.end()) {
      auto compressed_str = id_pos->second;
      return CompressFactory::get_instance(m_encoding_type)->decompressString(compressed_str);
    }
  }

  return std::string();
}

int64 Dictionary::id(const std::string &str) {
  std::scoped_lock lk(m_content_mtx);
  auto content_pos = m_content.find(str);
  return (content_pos != m_content.end()) ? content_pos->second : Dictionary::INVALID_STRID;
}

}  // namespace Compress
}  // namespace ShannonBase