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

#ifndef __SHANNONBASE_ART_ITERATOR_H__
#define __SHANNONBASE_ART_ITERATOR_H__

#include <memory>
#include <stack>
#include <vector>
#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

template <typename key_t, typename value_t>
class ARTIterator {
 public:
  ARTIterator(ART::Art_tree *tree)
      : m_tree(tree),
        m_startkey(nullptr),
        m_startkey_len(0),
        m_start_inclusive(true),
        m_endkey(nullptr),
        m_endkey_len(0),
        m_end_inclusive(true),
        m_current_leaf(nullptr) {}

  // initialize the scan.
  void init_scan(const void *startkey, int startkey_len, bool start_inclusive, const void *endkey, int endkey_len,
                 bool end_inclusive) {
    m_startkey = startkey;
    m_startkey_len = startkey_len;
    m_start_inclusive = start_inclusive;
    m_endkey = endkey;
    m_endkey_len = endkey_len;
    m_end_inclusive = end_inclusive;

    m_current_leaf = nullptr;

    if (!m_tree) return;

    // Find the first matched node.
    m_tree->ART_iter(
        m_tree,
        [](void *data, const void *key, uint32_t key_len, void *value, uint32_t value_len) {
          auto *iter = static_cast<ARTIterator *>(data);
          if (iter->is_within_range(key, key_len)) {
            iter->m_current_leaf = static_cast<ART::Art_leaf *>(value);
            return 1;  // found.
          }
          return 0;  // not found
        },
        this);
  }

  // get the next key-value pair.
  bool next(const void *&key_out, uint32_t &key_len_out, void *&value_out) {
    if (!m_current_leaf) return false;  // no more node, then end_of_file.

    key_out = m_current_leaf->key;
    key_len_out = m_current_leaf->key_len;
    value_out = m_current_leaf->value;

    // go ahead to the next matched node.
    m_current_leaf = nullptr;
    m_tree->ART_iter(
        m_tree,
        [](void *data, const void *key, uint32_t key_len, void *value, uint32_t value_len) {
          auto *iter = static_cast<ARTIterator *>(data);
          if (iter->is_within_range(key, key_len)) {
            iter->m_current_leaf = static_cast<ART::Art_leaf *>(value);
            return 1;  // found.
          }
          return 0;  // not found.
        },
        this);

    return true;
  }

 private:
  ART::Art_tree *m_tree;
  const key_t *m_startkey;
  int m_startkey_len;
  bool m_start_inclusive;

  const key_t *m_endkey;
  int m_endkey_len;
  bool m_end_inclusive;

  ART::Art_leaf *m_current_leaf;

  bool is_within_range(const key_t *key, int key_len) {
    if (m_startkey) {
      int cmp = std::memcmp(key, m_startkey, std::min(m_startkey_len, key_len));
      if (cmp < 0 || (cmp == 0 && !m_start_inclusive)) return false;
    }
    if (m_endkey) {
      int cmp = std::memcmp(key, m_endkey, std::min(m_endkey_len, key_len));
      if (cmp > 0 || (cmp == 0 && !m_end_inclusive)) return false;
    }
    return true;
  }
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_ITERATOR_H__