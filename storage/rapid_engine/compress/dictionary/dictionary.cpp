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
Dictionary::Dictionary(Encoding_type type) : m_encoding_type(type), m_next_id(1) {  // 0 reserved for unknown
  m_storage.reserve(kInitialCapacity);

  // "unknown" takes ID 0
  std::string unknown = "\x00unknown";
  if (m_storage.empty()) m_storage.emplace_back();
  m_storage.resize(1);
  m_storage[0] = std::move(unknown);
}

uint32 Dictionary::store(const uchar *data, size_t len, Encoding_type type) {
  if (!data || len == 0) return DEFAULT_STRID;

  std::string payload(reinterpret_cast<const char *>(data), len);
  char flag = '\x00';

  if (len >= kMinCompressThreshold && type != Encoding_type::NONE) {
    auto compressed = get_compressor(type)->compress(payload);
    if (!compressed.empty() && compressed.size() + 16 < len) {
      payload = std::move(compressed);
      flag = '\x01';
    }
  }

  std::string entry;
  entry.reserve(payload.size() + 1);
  entry.push_back(flag);
  entry.append(payload);

  uint64 id = m_next_id.fetch_add(1, std::memory_order_relaxed);

  if (unlikely(id >= m_storage.size())) {
    size_t new_size = std::max(m_storage.size() * 2, id + 1);
    m_storage.resize(new_size);
  }
  m_storage[id] = std::move(entry);

  m_reverse_index.emplace(std::string_view(m_storage[id].data() + 1, m_storage[id].size() - 1), id);
  return static_cast<uint32>(id);
}

std::string Dictionary::get(uint64 strid) {
  if (strid >= m_storage.size()) return {};

  const std::string &stored = m_storage[strid];
  if (stored.size() <= 1) return {};

  char flag = stored[0];
  std::string_view payload(stored.data() + 1, stored.size() - 1);

  if (flag == '\x01') return get_compressor(m_encoding_type)->decompress(payload);
  return std::string(payload);
}

int32 Dictionary::id(uint64 strid, String &ret_val) {
  std::string s = get(strid);
  if (s.empty()) {
    ret_val.length(0);
    return -1;
  }
  if (!ret_val.alloc(s.length() + 1)) return -1;

  memcpy(ret_val.ptr(), s.data(), s.length());
  ret_val.length(s.length());
  ret_val[s.length()] = '\0';
  return 0;
}

int64 Dictionary::id(const std::string &str) {
  std::shared_lock lock(m_reverse_mutex);
  auto it = m_reverse_index.find(str);
  return it != m_reverse_index.end() ? static_cast<int64>(it->second) : INVALID_STRID;
}
}  // namespace Compress
}  // namespace ShannonBase