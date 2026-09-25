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

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  ~Dictionary();

  Dictionary(const Dictionary &) = delete;
  Dictionary &operator=(const Dictionary &) = delete;

  uint32 store(const uchar *str, size_t len, ENCODING_TYPE type = ENCODING_TYPE::NONE);

  /**
   * Restore a dictionary entry at an explicit ID (snapshot recovery path).
   *
   * Unlike store() this does NOT allocate a new ID; it pins `str` to `strid`
   * so dictionary IDs embedded in column data keep their meaning after a
   * reload.  Empty strings (len == 0) are valid entries and are restored
   * verbatim.
   */
  void restore_entry(uint64 strid, const uchar *str, size_t len);

  int32 id(uint64 strid, String &ret_val);
  int64 id(const std::string &str);

  /**
    Decode entry @a strid into @a buf.

    @return the decoded length, or nullopt when the entry does not exist or
            could not be decoded. A returned 0 means the stored value really is
            the empty string -- callers used to read a plain 0 as "empty" and
            so turned a decode failure into an empty column value.
  */
  std::optional<size_t> get(uint64 strid, char *buf, size_t buf_len) const;
  std::string get(uint64 strid) const;

  /**
    Borrow entry @a strid's bytes without copying them.

    Returns a view whose data() is nullptr when the id is unknown or its entry
    is compressed (the bytes on hand are not the value); an existing empty
    value comes back as a non-null, zero-length view, so data() is what
    separates "no view available" from "the empty string".
  */
  std::string_view get_view(uint64 strid) const;

  /**
    Decoded length of entry @a strid, without materializing it.

    Scans ask for this once per cell per projected column purely to fill in a
    length, so it has to cost a field read: the decoded length is recorded when
    the entry is stored. Returns 0 for an unknown id, which is also the length
    of the empty string -- callers that must tell them apart use get_view().
  */
  size_t length_of(uint64 strid) const;

  inline uint32 store(const uchar *str, size_t len, int) { return store(str, len, m_encoding_type); }
  inline ENCODING_TYPE get_algo() const { return m_encoding_type; }
  inline uint32 content_size() const { return static_cast<uint32>(m_next_id.load()); }

  size_t size() const { return m_next_id.load(); }

  /**
   * Bytes of dictionary payload currently held, for
   * performance_schema.rpd_columns.DICT_SIZE_BYTES.  Maintained incrementally
   * by store()/restore_entry() so reading it stays O(1); it counts the stored
   * entries (flag byte + possibly compressed payload), not the original
   * uncompressed strings.
   */
  size_t content_bytes() const { return m_content_bytes.load(std::memory_order_relaxed); }

 private:
  // A distinct value is held exactly once, in the arena, and everything else refers to it.
  // An Entry is filled in before it is published and never written again, so a
  // reader that has reached one needs no lock to read it.
  struct Entry {
    const char *data{nullptr};  ///< into the arena; stable for the dictionary's life
    uint32 length{0};           ///< bytes of stored payload (compressed, when compressed)
    uint32 decoded_length{0};   ///< bytes the value decodes to; see length_of()

    /// A value is only ever stored compressed when that made it strictly
    /// smaller (see kMinCompressThreshold in store()), so the two lengths
    /// differing is exactly the compressed case -- which keeps the Entry at
    /// two words, one per distinct value in the column.
    bool compressed() const { return length != decoded_length; }
  };
  static_assert(sizeof(Entry) == 16, "one Entry per distinct value; keep it two words");

  /// A chunk of packed value bytes. Chunks are never moved or freed while the
  /// dictionary lives, so an Entry::data pointer stays valid even as the arena
  /// grows -- which is what lets the reverse index hold no copy of its own.
  struct Chunk {
    std::unique_ptr<char[]> data;
    size_t used{0};
    size_t capacity{0};
  };

  /// Copy `len` bytes into the arena and return a stable pointer. Caller holds
  /// the write lock.
  const char *arena_append(const char *data, size_t len);

  /**
    Entry slots, in segments allocated once and never moved.

    This exists so a scan can read an entry without taking m_dict_mutex. The
    lock used to be unavoidable for readers only because the entries lived in a
    std::vector that a concurrent store() could reallocate underneath them --
    a rwlock per cell to read something that, once published, is immutable. A
    segmented table removes the reallocation, and an atomic slot per id removes
    the rest: a reader loads the slot, and either gets nullptr (no such entry
    yet) or a pointer to a finished Entry.

    Segment 0 holds kSegment0 ids; segment k > 0 holds kSegment0 << (k - 1),
    which covers the whole uint32 id space.
  */
  static constexpr uint32 kSegment0Bits = 10;
  static constexpr uint32 kSegment0 = 1u << kSegment0Bits;
  static constexpr uint32 kNumSegments = 32 - kSegment0Bits + 1;

  using EntrySlot = std::atomic<const Entry *>;

  /// Which segment holds @a strid, and where in it.
  static void locate(uint64 strid, uint32 &segment, uint32 &index) {
    if (strid < kSegment0) {
      segment = 0;
      index = static_cast<uint32>(strid);
      return;
    }
    const uint32 high_bit = 63 - static_cast<uint32>(__builtin_clzll(strid));
    segment = high_bit - kSegment0Bits + 1;
    index = static_cast<uint32>(strid - (1ull << high_bit));
  }

  static size_t segment_capacity(uint32 segment) {
    return segment == 0 ? kSegment0 : (static_cast<size_t>(kSegment0) << (segment - 1));
  }

  /// The published entry for @a strid, or nullptr. Lock-free.
  const Entry *lookup(uint64 strid) const {
    uint32 segment = 0, index = 0;
    locate(strid, segment, index);
    if (segment >= kNumSegments) return nullptr;
    EntrySlot *slots = m_segments[segment].load(std::memory_order_acquire);
    if (slots == nullptr) return nullptr;
    return slots[index].load(std::memory_order_acquire);
  }

  /// Publish @a entry as @a strid, allocating its segment if needed. Caller
  /// holds the write lock.
  void publish_entry(uint64 strid, Entry entry);

  /// Is this the value `probe` decodes to? Compressed entries are decompressed
  /// to answer, which is why the hash is taken over the *decoded* value.
  bool entry_matches(const Entry *entry, std::string_view probe) const;

  /// Hash of a decoded value; the reverse index's key.
  static uint64 value_hash(std::string_view value) { return static_cast<uint64>(std::hash<std::string_view>{}(value)); }

  const ENCODING_TYPE m_encoding_type;

  std::vector<Chunk> m_chunks;

  /// Indexed by string id; see the comment on kSegment0Bits. Read without a
  /// lock, written only under m_dict_mutex.
  std::atomic<EntrySlot *> m_segments[kNumSegments]{};

  /// Backing store for the Entry objects the slots point at. A deque because
  /// push_back leaves references to existing elements valid, which is what
  /// lets a published pointer outlive any number of later stores.
  std::deque<Entry> m_entry_pool;

  std::atomic<uint64> m_next_id;

  // Running total of stored entry sizes; see content_bytes().
  std::atomic<size_t> m_content_bytes{0};

  mutable std::shared_mutex m_dict_mutex;

  // hash(decoded value) -> id. Multimap because two values may collide; the
  // candidates are then compared against the arena bytes. Holding the hash
  // instead of the string is what removes the second copy, and it works for
  // compressed entries too, which a payload-keyed index could not look up.
  std::unordered_multimap<uint64, uint32> m_reverse_index;

  // Arena chunk size. Values larger than this get a chunk of their own, so a
  // single oversized value never forces the common chunk to grow.
  static constexpr size_t kArenaChunkSize = 1UL << 20;  // 1MB
};
}  // namespace Compress
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COMPRESS_DICTIONARY_H__