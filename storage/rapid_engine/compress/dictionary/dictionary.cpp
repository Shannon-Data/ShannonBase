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
  if (!str || !len) return Dictionary::DEFAULT_STRID;

  std::string orig(reinterpret_cast<const char *>(str), len);

  std::string compressed;
  bool use_compression = false;

  if (len >= kMinCompressThreshold) {
    auto algr = CompressFactory::get_instance(type);
    compressed = algr->compressString(orig);

    // do compression or not.
    if (compressed.size() + 1 < orig.size()) {
      use_compression = true;
    }
  }

  std::string final_str;
  if (use_compression) {
    final_str.push_back('\x01');  // Compression flag
    final_str += compressed;
  } else {
    final_str.push_back('\x00');  // No compression flag
    final_str += orig;
  }

  {
    std::unique_lock lk(m_content_mtx);
    auto pos = m_content.find(final_str);
    if (pos != m_content.end()) return pos->second;

    uint64 id = m_content.size();
    m_content.emplace(final_str, id);
    m_id2content.emplace(id, final_str);

    ut_a(m_content.size() == m_id2content.size());
    return id;
  }
}

int32 Dictionary::id(uint64 strid, String &ret_val) {
  std::string decoded = get(strid);
  if (decoded.empty()) return -1;

  std::shared_lock lk(m_content_mtx);
  String strs(decoded.c_str(), decoded.length(), ret_val.charset());
  copy_if_not_alloced(&ret_val, &strs, strs.length());
  return 0;
}

std::string Dictionary::get(uint64 strid) {
  std::shared_lock lk(m_content_mtx);
  auto pos = m_id2content.find(strid);
  if (pos == m_id2content.end()) return {};

  const std::string &stored = pos->second;
  if (stored.empty()) return {};

  char flag = stored[0];
  std::string_view payload(stored.data() + 1, stored.size() - 1);

  if (flag == '\x01') {
    auto algr = CompressFactory::get_instance(m_encoding_type);
    return algr->decompressString(std::string(payload));
  } else {
    return std::string(payload);
  }
}

int64 Dictionary::id(const std::string &str) {
  std::shared_lock lk(m_content_mtx);
  auto pos = m_content.find(str);
  return (pos != m_content.end()) ? pos->second : Dictionary::INVALID_STRID;
}
}  // namespace Compress
}  // namespace ShannonBase