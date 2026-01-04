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

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "include/my_inttypes.h"
#include "include/mysql/strings/m_ctype.h"  //CHARSET_INFO
#include "include/sql_string.h"             //String
#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase {
namespace Compress {
class Dictionary {
 public:
  static constexpr uint64 DEFAULT_STRID = 0;
  static constexpr uint64 INVALID_STRID = static_cast<uint64>(-1);
  static constexpr size_t kMinCompressThreshold = 64;

  explicit Dictionary(ENCODING_TYPE type = ENCODING_TYPE::NONE);
  ~Dictionary() = default;

  Dictionary(const Dictionary &) = delete;
  Dictionary &operator=(const Dictionary &) = delete;

  uint32 store(const uchar *str, size_t len, ENCODING_TYPE type = ENCODING_TYPE::NONE);
  int32 id(uint64 strid, String &ret_val);
  std::string get(uint64 strid);
  int64 id(const std::string &str);

  inline uint32 store(const uchar *str, size_t len, int) { return store(str, len, m_encoding_type); }
  inline ENCODING_TYPE get_algo() const { return m_encoding_type; }
  inline uint32 content_size() const { return static_cast<uint32>(m_next_id.load()); }

  size_t size() const { return m_next_id.load(); }

 private:
  const ENCODING_TYPE m_encoding_type;

  // index is ID. id ↔ flag + payload
  std::vector<std::string> m_storage;

  std::atomic<uint64> m_next_id;

  std::unordered_map<std::string_view, uint64> m_reverse_index;
  mutable std::shared_mutex m_reverse_mutex;

  static constexpr size_t kInitialCapacity = 1ULL << 20;  // 1M
};
}  // namespace Compress
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COMPRESS_DICTIONARY_H__