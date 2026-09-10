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
#include <algorithm>
#include <cstring>
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

Dictionary::Dictionary(ENCODING_TYPE type) : m_encoding_type(type), m_next_id(1) {  // 0 reserved for unknown
  m_entries.reserve(kInitialCapacity);

  // "unknown" takes ID 0. It is deliberately left out of the reverse index:
  // it is a sentinel, not a value any row stores, and indexing it would let
  // store("unknown") hand back DEFAULT_STRID.
  static constexpr std::string_view kUnknown{"unknown"};
  m_entries.resize(1);
  m_entries[0] =
      Entry{arena_append(kUnknown.data(), kUnknown.size()), static_cast<uint32>(kUnknown.size()), false, true};
  // Entry 0 is deliberately not counted: DICT_SIZE_BYTES reports what the
  // column's values cost, and the sentinel is not one of them. The layout
  // this replaced did not count it either.
}

const char *Dictionary::arena_append(const char *data, size_t len) {
  // An empty value still needs a distinct, non-null address so Entry::valid is
  // not the only thing separating it from an unfilled slot.
  if (m_chunks.empty() || m_chunks.back().used + len > m_chunks.back().capacity) {
    const size_t capacity = std::max(len, kArenaChunkSize);
    Chunk chunk;
    chunk.data = std::make_unique<char[]>(capacity);
    chunk.capacity = capacity;
    m_chunks.push_back(std::move(chunk));
  }

  Chunk &chunk = m_chunks.back();
  char *dest = chunk.data.get() + chunk.used;
  if (len > 0) std::memcpy(dest, data, len);
  chunk.used += len;
  return dest;
}

bool Dictionary::entry_matches(const Entry &entry, std::string_view probe) const {
  if (!entry.valid) return false;
  if (!entry.compressed) {
    return entry.length == probe.size() && std::memcmp(entry.data, probe.data(), probe.size()) == 0;
  }
  // Rare: the hash is taken over the decoded value, so a compressed candidate
  // has to be decoded to be compared. Only entries whose hash already matched
  // reach here.
  const std::string decoded = get_compressor(m_encoding_type)->decompress(std::string_view(entry.data, entry.length));
  return decoded.size() == probe.size() && std::memcmp(decoded.data(), probe.data(), probe.size()) == 0;
}

uint32 Dictionary::store(const uchar *data, size_t len, ENCODING_TYPE type) {
  if (!data) return DEFAULT_STRID;

  const std::string_view value(reinterpret_cast<const char *>(data), len);
  const uint64 hash = value_hash(value);

  {
    std::shared_lock lock(m_dict_mutex);
    const auto range = m_reverse_index.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      if (entry_matches(m_entries[it->second], value)) return it->second;
    }
  }

  // Compress outside the lock; it is the expensive part and it needs no shared
  // state beyond the algorithm.
  std::string compressed;
  bool is_compressed = false;
  if (len >= kMinCompressThreshold && type != ENCODING_TYPE::NONE) {
    auto candidate = get_compressor(type)->compress(value);
    if (!candidate.empty() && candidate.size() + 16 < len) {
      compressed = std::move(candidate);
      is_compressed = true;
    }
  }

  std::unique_lock lock(m_dict_mutex);

  // Another writer may have inserted the same value while this one was
  // compressing outside the lock.
  const auto range = m_reverse_index.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (entry_matches(m_entries[it->second], value)) return it->second;
  }

  // Take the id under the lock that publishes the slot, so an id can never
  // name a slot that has not been written yet.
  const uint64 id = m_next_id.fetch_add(1, std::memory_order_relaxed);

  if (unlikely(id >= m_entries.size())) {
    const size_t new_size = std::max(m_entries.size() * 2, static_cast<size_t>(id + 1));
    m_entries.resize(new_size);
  }

  const std::string_view payload = is_compressed ? std::string_view(compressed) : value;
  m_entries[id] =
      Entry{arena_append(payload.data(), payload.size()), static_cast<uint32>(payload.size()), is_compressed, true};
  // Keeps DICT_SIZE_BYTES comparable with the flag-byte layout this replaced.
  m_content_bytes.fetch_add(payload.size() + 1, std::memory_order_relaxed);

  m_reverse_index.emplace(hash, static_cast<uint32>(id));

  return static_cast<uint32>(id);
}

void Dictionary::restore_entry(uint64 strid, const uchar *data, size_t len) {
  // The serialized value is the decoded value, so it is stored uncompressed.
  // An empty string is a valid entry.
  const std::string_view value(data ? reinterpret_cast<const char *>(data) : "", data ? len : 0);

  std::unique_lock lock(m_dict_mutex);

  if (strid >= m_entries.size()) {
    const size_t new_size = std::max(m_entries.size() * 2, static_cast<size_t>(strid + 1));
    m_entries.resize(new_size);
  }

  // restore_entry() may overwrite an occupied slot. Drop the old value's index
  // entry first, or a lookup for it would keep resolving to this id and hand
  // back the new value.
  Entry &slot = m_entries[strid];
  if (slot.valid) {
    m_content_bytes.fetch_sub(slot.length + 1, std::memory_order_relaxed);
    std::string decoded;
    std::string_view old_value;
    if (slot.compressed) {
      decoded = get_compressor(m_encoding_type)->decompress(std::string_view(slot.data, slot.length));
      old_value = decoded;
    } else {
      old_value = std::string_view(slot.data, slot.length);
    }
    const auto old_range = m_reverse_index.equal_range(value_hash(old_value));
    for (auto it = old_range.first; it != old_range.second; ++it) {
      if (it->second == strid) {
        m_reverse_index.erase(it);
        break;
      }
    }
  }

  slot = Entry{arena_append(value.data(), value.size()), static_cast<uint32>(value.size()), false, true};
  m_content_bytes.fetch_add(value.size() + 1, std::memory_order_relaxed);

  // Keep the reverse index consistent, without duplicating this id under the
  // same hash if the same value is restored twice.
  const uint64 hash = value_hash(value);
  const auto range = m_reverse_index.equal_range(hash);
  bool already_indexed = false;
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == strid) {
      already_indexed = true;
      break;
    }
  }
  if (!already_indexed) m_reverse_index.emplace(hash, static_cast<uint32>(strid));

  // Make sure subsequent store() calls never reuse this restored ID.
  uint64 next = m_next_id.load(std::memory_order_relaxed);
  if (next <= strid) m_next_id.store(strid + 1, std::memory_order_relaxed);
}

std::string Dictionary::get(uint64 strid) {
  std::shared_lock lock(m_dict_mutex);

  if (strid >= m_entries.size()) return {};
  const Entry &entry = m_entries[strid];
  if (!entry.valid || entry.length == 0) return {};

  const std::string_view payload(entry.data, entry.length);
  if (entry.compressed) return get_compressor(m_encoding_type)->decompress(payload);
  return std::string(payload);
}

size_t Dictionary::get(uint64 strid, char *buf, size_t buf_len) {
  std::shared_lock lock(m_dict_mutex);

  if (strid >= m_entries.size()) return 0;
  const Entry &entry = m_entries[strid];
  if (!entry.valid || entry.length == 0) return 0;

  const std::string_view payload(entry.data, entry.length);
  if (!entry.compressed) {
    const size_t n = std::min(payload.size(), buf_len);
    std::memcpy(buf, payload.data(), n);
    return n;
  }

  return get_compressor(m_encoding_type)->decompress(payload, buf, buf_len);
}

std::string_view Dictionary::get_view(uint64 strid) const {
  std::shared_lock lock(m_dict_mutex);

  if (strid >= m_entries.size()) return {};
  const Entry &entry = m_entries[strid];
  if (!entry.valid || entry.length == 0 || entry.compressed) return {};
  return std::string_view(entry.data, entry.length);
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
  std::shared_lock lock(m_dict_mutex);
  const auto range = m_reverse_index.equal_range(value_hash(str));
  for (auto it = range.first; it != range.second; ++it) {
    if (entry_matches(m_entries[it->second], str)) return static_cast<int64>(it->second);
  }
  return INVALID_STRID;
}
}  // namespace Compress
}  // namespace ShannonBase