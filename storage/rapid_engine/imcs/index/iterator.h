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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_INDEX_ITERATOR_H__
#define __SHANNONBASE_INDEX_ITERATOR_H__

#include <memory>
#include <vector>

#include "my_inttypes.h"
#include "storage/rapid_engine/imcs/index/art/art.h"
#include "storage/rapid_engine/imcs/index/art/iterator.h"  // ARTIterator template

namespace ShannonBase {
namespace Imcs {
namespace Index {
class Iterator {
 public:
  explicit Iterator(ART * /*art*/) {}
  virtual ~Iterator() = default;

  /**
   * init_scan — set up a range scan.
   *
   * @param startkey      first key of the range (nullptr = open start)
   * @param startkey_len  byte length of startkey
   * @param start_inclusive  include the start key?
   * @param endkey        last key of the range (nullptr = open end)
   * @param endkey_len    byte length of endkey
   * @param end_inclusive include the end key?
   */
  virtual void init_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey,
                         int endkey_len, bool end_inclusive) = 0;

  /** Initialize the same logical range at its reverse (right-hand) boundary. */
  virtual void init_reverse_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey,
                                 int endkey_len, bool end_inclusive) = 0;

  /**
   * next — advance to the next key/value pair.
   *
   * @param key_out     set to point to the current key bytes (owned by tree)
   * @param key_len_out number of bytes in *key_out
   * @param value_out   caller-allocated storage; the value is written here
   * @return true if a row was produced, false when the scan is exhausted
   */
  virtual bool next(const uchar **key_out, uint32_t *key_len_out, void *value_out) = 0;

  /** prev — advance to the previous key/value pair. */
  virtual bool prev(const uchar **key_out, uint32_t *key_len_out, void *value_out) = 0;

  virtual bool initialized() const = 0;
};

class Art_Iterator : public Iterator {
 public:
  explicit Art_Iterator(ART *art) : Iterator(art), m_art(art), m_art_tree(art ? art->tree() : nullptr) {
    if (art) {
      m_art_iter = std::make_unique<ARTIterator<uchar, row_id_t>>(art);
    }
  }

  ~Art_Iterator() override = default;

  // Non-copyable, non-movable (owns the iterator state).
  Art_Iterator(const Art_Iterator &) = delete;
  Art_Iterator &operator=(const Art_Iterator &) = delete;

  void init_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey, int endkey_len,
                 bool end_inclusive) override {
    if (!m_art_iter || !m_art_tree) return;
    // Positioning walks compressed ART prefixes and parent/edge state. Keep a
    // shared tree lock for the complete seek so concurrent writers cannot
    // restructure the path while it is being built.
    std::shared_lock lk(m_art_tree->tree_mutex);
    m_art_iter->init_scan(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
    m_initialized = true;
  }

  void init_reverse_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey,
                         int endkey_len, bool end_inclusive) override {
    if (!m_art_iter || !m_art_tree) return;
    std::shared_lock lk(m_art_tree->tree_mutex);
    m_art_iter->init_reverse_scan(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
    m_initialized = true;
  }

  /**
   * next
   *
   * Acquires tree_mutex shared for the duration of each step so that the
   * ARTIterator's node references remain valid.  The underlying ARTIterator
   * already holds shared_ptr refs to each node on its parent/edge path; the lock here
   * provides a consistent view for the single call.
   *
   * NOTE: For long-running scans, callers may want to release and re-acquire
   *       tree_mutex between rows to allow writers to proceed (i.e. call
   *       next() in a loop with other work in between).  The current design
   *       acquires/releases per row, which is appropriate for most use-cases.
   */
  bool next(const uchar **key_out, uint32_t *key_len_out, void *value_out) override {
    return step(/*forward=*/true, key_out, key_len_out, value_out);
  }

  bool prev(const uchar **key_out, uint32_t *key_len_out, void *value_out) override {
    return step(/*forward=*/false, key_out, key_len_out, value_out);
  }

  bool initialized() const override { return m_initialized; }

 private:
  bool step(bool forward, const uchar **key_out, uint32_t *key_len_out, void *value_out) {
    if (!m_art_iter || !m_art_tree || !key_out || !key_len_out || !value_out) return false;

    const uchar *temp_key = nullptr;
    row_id_t temp_val = 0;

    std::shared_lock lk(m_art_tree->tree_mutex);
    const bool result = forward ? m_art_iter->next(&temp_key, key_len_out, &temp_val)
                                : m_art_iter->prev(&temp_key, key_len_out, &temp_val);
    if (result) {
      const uint32_t len = *key_len_out;
      m_key_copy.assign(temp_key, temp_key + len);
      *key_out = m_key_copy.data();
      *static_cast<row_id_t *>(value_out) = temp_val;
    } else {
      m_key_copy.clear();
    }
    return result;
  }

  ART *m_art{nullptr};
  ART::Art_tree *m_art_tree{nullptr};
  bool m_initialized{false};
  std::unique_ptr<ARTIterator<uchar, row_id_t>> m_art_iter;
  std::vector<uchar> m_key_copy;
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_INDEX_ITERATOR_H__