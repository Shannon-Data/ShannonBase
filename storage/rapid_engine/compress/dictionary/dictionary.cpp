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
#include <limits>
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
  // "unknown" takes ID 0. It is deliberately left out of the reverse index:
  // it is a sentinel, not a value any row stores, and indexing it would let
  // store("unknown") hand back DEFAULT_STRID.
  static constexpr std::string_view kUnknown{"unknown"};
  publish_entry(0, Entry{arena_append(kUnknown.data(), kUnknown.size()), static_cast<uint32>(kUnknown.size()),
                         static_cast<uint32>(kUnknown.size())});
  // Entry 0 is deliberately not counted: DICT_SIZE_BYTES reports what the
  // column's values cost, and the sentinel is not one of them. The layout
  // this replaced did not count it either.
}

Dictionary::~Dictionary() {
  for (auto &segment : m_segments) delete[] segment.load(std::memory_order_relaxed);
}

void Dictionary::publish_entry(uint64 strid, Entry entry) {
  uint32 segment = 0, index = 0;
  locate(strid, segment, index);
  if (segment >= kNumSegments) return;  // beyond the uint32 id space; store() cannot reach it

  EntrySlot *slots = m_segments[segment].load(std::memory_order_relaxed);
  if (slots == nullptr) {
    slots = new EntrySlot[segment_capacity(segment)]();
    m_segments[segment].store(slots, std::memory_order_release);
  }

  // The Entry is finished before its address becomes reachable, so a reader
  // that loads the slot sees a complete entry without holding anything.
  // restore_entry() may replace an id's entry; the superseded one stays in the
  // pool rather than being mutated, so a reader already holding it stays
  // consistent.
  m_entry_pool.push_back(entry);
  slots[index].store(&m_entry_pool.back(), std::memory_order_release);
}

const char *Dictionary::arena_append(const char *data, size_t len) {
  // An empty value still needs a distinct, non-null address: get_view() uses a
  // null data() to mean "no view available", so the empty string has to come
  // back as a non-null, zero-length view rather than as a miss.
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

bool Dictionary::entry_matches(const Entry *entry, std::string_view probe) const {
  if (entry == nullptr) return false;
  if (!entry->compressed()) {
    return entry->length == probe.size() && std::memcmp(entry->data, probe.data(), probe.size()) == 0;
  }
  // Rare: the hash is taken over the decoded value, so a compressed candidate
  // has to be decoded to be compared. Only entries whose hash already matched
  // reach here.
  const std::string decoded = get_compressor(m_encoding_type)->decompress(std::string_view(entry->data, entry->length));
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
      if (entry_matches(lookup(it->second), value)) return it->second;
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
    if (entry_matches(lookup(it->second), value)) return it->second;
  }

  // Take the id under the lock that publishes the slot, so an id can never
  // name a slot that has not been written yet.
  const uint64 id = m_next_id.fetch_add(1, std::memory_order_relaxed);

  const std::string_view payload = is_compressed ? std::string_view(compressed) : value;
  publish_entry(id, Entry{arena_append(payload.data(), payload.size()), static_cast<uint32>(payload.size()),
                          static_cast<uint32>(value.size())});
  // Keeps DICT_SIZE_BYTES comparable with the flag-byte layout this replaced.
  m_content_bytes.fetch_add(payload.size() + 1, std::memory_order_relaxed);

  m_reverse_index.emplace(hash, static_cast<uint32>(id));

  return static_cast<uint32>(id);
}

void Dictionary::restore_entry(uint64 strid, const uchar *data, size_t len) {
  // Ids are uint32 by construction -- store() allocates them and returns uint32
  // -- so a wider one comes from a corrupt snapshot and has no slot to go in.
  if (strid > std::numeric_limits<uint32>::max()) return;

  // The serialized value is the decoded value, so it is stored uncompressed.
  // An empty string is a valid entry.
  const std::string_view value(data ? reinterpret_cast<const char *>(data) : "", data ? len : 0);

  std::unique_lock lock(m_dict_mutex);

  // restore_entry() may overwrite an occupied slot. Drop the old value's index
  // entry first, or a lookup for it would keep resolving to this id and hand
  // back the new value.
  const Entry *previous = lookup(strid);
  if (previous != nullptr) {
    m_content_bytes.fetch_sub(previous->length + 1, std::memory_order_relaxed);
    std::string decoded;
    std::string_view old_value;
    if (previous->compressed()) {
      decoded = get_compressor(m_encoding_type)->decompress(std::string_view(previous->data, previous->length));
      old_value = decoded;
    } else {
      old_value = std::string_view(previous->data, previous->length);
    }
    const auto old_range = m_reverse_index.equal_range(value_hash(old_value));
    for (auto it = old_range.first; it != old_range.second; ++it) {
      if (it->second == strid) {
        m_reverse_index.erase(it);
        break;
      }
    }
  }

  publish_entry(strid, Entry{arena_append(value.data(), value.size()), static_cast<uint32>(value.size()),
                             static_cast<uint32>(value.size())});
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

// The four accessors below take no lock: lookup() reaches a published Entry
// through an atomic slot, and an Entry never changes once published. Scans call
// them once per cell per projected column, so the rwlock they used to take was
// the single hottest frame in a TPC-H profile.

std::string Dictionary::get(uint64 strid) const {
  const Entry *entry = lookup(strid);
  if (entry == nullptr || entry->length == 0) return {};

  const std::string_view payload(entry->data, entry->length);
  if (entry->compressed()) return get_compressor(m_encoding_type)->decompress(payload);
  return std::string(payload);
}

std::optional<size_t> Dictionary::get(uint64 strid, char *buf, size_t buf_len) const {
  const Entry *entry = lookup(strid);
  if (entry == nullptr) return std::nullopt;
  if (entry->length == 0) return static_cast<size_t>(0);  // the empty string

  const std::string_view payload(entry->data, entry->length);
  if (!entry->compressed()) {
    if (payload.size() > buf_len) return std::nullopt;
    std::memcpy(buf, payload.data(), payload.size());
    return payload.size();
  }

  const size_t decoded = get_compressor(m_encoding_type)->decompress(payload, buf, buf_len);
  if (decoded == 0) return std::nullopt;  // a stored entry never decodes to nothing
  return decoded;
}

std::string_view Dictionary::get_view(uint64 strid) const {
  const Entry *entry = lookup(strid);
  // A compressed entry's stored bytes are not the value, so there is nothing to
  // borrow. An empty value has nothing to copy either, but it does exist, and
  // arena_append() gave it a distinct non-null address -- returning it keeps
  // data() == nullptr meaning "no view", not "empty".
  if (entry == nullptr || entry->compressed()) return {};
  return std::string_view(entry->data, entry->length);
}

size_t Dictionary::length_of(uint64 strid) const {
  const Entry *entry = lookup(strid);
  return entry == nullptr ? 0 : entry->decoded_length;
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
  // Still locked: the reverse index is a plain unordered_multimap that store()
  // mutates. This is a per-query constant lookup, not a per-cell one.
  std::shared_lock lock(m_dict_mutex);
  const auto range = m_reverse_index.equal_range(value_hash(str));
  for (auto it = range.first; it != range.second; ++it) {
    if (entry_matches(lookup(it->second), str)) return static_cast<int64>(it->second);
  }
  return INVALID_STRID;
}
}  // namespace Compress
}  // namespace ShannonBase