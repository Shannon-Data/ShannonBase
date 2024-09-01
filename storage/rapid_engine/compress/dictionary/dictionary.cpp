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

#include "storage/rapid_engine/compress/algorithms.h"
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
  if (!str || !len) return 0;

  compress_algos alg{compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED:
      alg = compress_algos::ZSTD;
      break;
    case Encoding_type::VARLEN:
      alg = compress_algos::LZ4;
      break;
    case Encoding_type::NONE:
      alg = compress_algos::NONE;
      break;
    default:
      break;
  }

  std::scoped_lock lk(m_content_mtx);
  std::string orgin_str((char *)str, len);
  Compress_algorithm *algr = CompressFactory::get_instance(alg);
  std::string compressed_str(algr->compressString(orgin_str));
  {
    if (m_content.find(compressed_str) == m_content.end()) {  // insert new one.
      m_content_id.fetch_add(1, std::memory_order::memory_order_acq_rel);
      uint64 id = m_content_id.load();
      // compressed string <---> str id. get a copy of string and store it in map.
      m_content.emplace(compressed_str, id);
      // id<---> orginal string
      m_id2content.emplace(id, std::string((char *)str, len));

      ut_a((m_content.size() == m_id2content.size()));
      ut_a(m_content_id == m_content.size());
      return m_content_id;
    } else
      return m_content[compressed_str];
  }

  return 0;
}

uint32 Dictionary::get(uint64 strid, String &val) {
  compress_algos alg [[maybe_unused]]{compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED:
      alg = compress_algos::ZSTD;
      break;
    case Encoding_type::VARLEN:
      alg = compress_algos::LZ4;
      break;
    case Encoding_type::NONE:
      alg = compress_algos::NONE;
      break;
    default:
      break;
  }

  {
    std::scoped_lock lk(m_content_mtx);
    if (m_id2content.find(strid) != m_id2content.end()) {
      String strs(m_id2content[strid].c_str(), m_id2content[strid].length(), val.charset());
      copy_if_not_alloced(&val, &strs, strs.length());
    } else
      return 1;
  }

  return 0;
}

uchar *Dictionary::get(uint64 strid) {
  compress_algos alg [[maybe_unused]]{compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED:
      alg = compress_algos::ZSTD;
      break;
    case Encoding_type::VARLEN:
      alg = compress_algos::LZ4;
      break;
    case Encoding_type::NONE:
      alg = compress_algos::NONE;
      break;
    default:
      break;
  }

  {
    std::scoped_lock lk(m_content_mtx);
    if (m_id2content.find(strid) != m_id2content.end())
      return (uchar *)(m_id2content[strid].c_str());
    else
      return nullptr;
  }
  return nullptr;
}

int Dictionary::lookup(uchar *&str) {
  DBUG_TRACE;
  // returns dictionary id. //encoding alg pls ref to: heatwave document.
  std::string origin_str;
  origin_str.assign((const char *)str);
  compress_algos alg{compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED:
      alg = compress_algos::ZSTD;
      break;
    case Encoding_type::VARLEN:
      alg = compress_algos::LZ4;
      break;
    case Encoding_type::NONE:
      alg = compress_algos::NONE;
      break;
    default:
      break;
  }

  std::string compressed_str(CompressFactory::get_instance(alg)->compressString(origin_str));
  {
    std::scoped_lock lk(m_content_mtx);
    if (m_content.find(compressed_str) == m_content.end()) {  // not found, return -1.
      return -1;
    } else
      return m_content[compressed_str];
  }

  return -1;
}
int Dictionary::lookup(String &str) {
  DBUG_TRACE;
  // returns dictionary id. //encoding alg pls ref to: heatwave document.
  std::string origin_str(str.c_ptr());
  compress_algos alg{compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED:
      alg = compress_algos::ZSTD;
      break;
    case Encoding_type::VARLEN:
      alg = compress_algos::LZ4;
      break;
    case Encoding_type::NONE:
      alg = compress_algos::NONE;
      break;
    default:
      break;
  }

  std::string compressed_str(CompressFactory::get_instance(alg)->compressString(origin_str));
  {
    std::scoped_lock lk(m_content_mtx);
    if (m_content.find(compressed_str) == m_content.end()) {  // not found, return -1.
      return -1;
    } else
      return m_content[compressed_str];
  }

  return -1;
}

}  // namespace Compress
}  // namespace ShannonBase
