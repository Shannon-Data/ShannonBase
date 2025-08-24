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

#include <cstring>
#include <memory>
#include <stack>
#include <unordered_set>
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

template <typename key_t, typename value_t>
class ARTIterator {
  struct TraversalState {
    ART::Art_node *node;
    int child_pos;
    uint32_t value_idx;
    uint32_t depth{0};
  };

 public:
  explicit ARTIterator(ART *art) : m_art(art) {}

  void init_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    if (!m_art) return;

    m_stack.clear();
    m_seen_nodes.clear();
    m_finished = false;

    m_start_key = reinterpret_cast<const unsigned char *>(startkey);
    m_start_keylen = startkey_len;
    m_start_incl = start_inclusive;

    m_end_key = reinterpret_cast<const unsigned char *>(endkey);
    m_end_keylen = endkey_len;
    m_end_incl = end_inclusive;

    if (m_art->root()) {
      if (startkey) {
        find_start_position();
      } else {
        find_leftmost_leaf();
      }
    }
  }

  bool next(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out) return false;
    if (m_finished) return false;

    while (!m_stack.empty()) {
      TraversalState &current_state = m_stack.back();

      if (ART::is_leaf(const_cast<ART::Art_node *>(current_state.node))) {
        ART::Art_leaf *leaf = static_cast<ART::Art_leaf *>(current_state.node);
        if (!leaf || m_seen_nodes.count(leaf)) {
          m_stack.pop_back();
          continue;
        }

        if (current_state.value_idx >= leaf->vcount) {  // all the values of this leaf have been processed.
          m_seen_nodes.insert(leaf);
          m_stack.pop_back();
          continue;
        }

        // Iterate through all values in the leaf
        for (; current_state.value_idx < leaf->vcount; ++current_state.value_idx) {
          void *val_ptr = leaf->values[current_state.value_idx].data();
          if (!val_ptr) continue;

          // Strict range check for each value
          if (!key_in_range_value(leaf->key.data(), leaf->key.size())) {
            continue;  // Skip if out of range
          }

          *key_out = reinterpret_cast<const key_t *>(leaf->key.data());
          *key_len_out = leaf->key.size();
          *value_out = *reinterpret_cast<value_t *>(val_ptr);
          current_state.value_idx++;  // Continue with next value in leaf
          return true;
        }

        // All values processed, pop the leaf
        m_seen_nodes.insert(leaf);
        m_stack.pop_back();
        continue;
      }

      // Non-leaf node: get next child
      ART::Art_node *child = get_next_child(current_state);
      if (child) {
        if (ART::is_leaf(child)) {
          m_stack.push_back({child, 0, 0, current_state.depth + 1});
        } else {
          navigate_to_leftmost_leaf_from(child, current_state.depth + 1);
        }
      } else {
        m_stack.pop_back();
      }
    }

    m_finished = true;
    return false;
  }

 private:
  inline int compare_keys(const unsigned char *k1, uint32_t len1, const unsigned char *k2, uint32_t n) {
    uint32_t min_len = std::min(len1, n);
    return memcmp(k1, k2, min_len);
  }

  bool key_in_range_value(const unsigned char *key, uint32_t key_len) {
    if (!key || !key_len) return false;

    if (m_start_key) {
      int cmp_start = compare_keys(key, key_len, m_start_key, m_start_keylen);
      if (cmp_start < 0) return false;
      if (cmp_start == 0 && !m_start_incl) {
        return false;
      }
    }

    if (m_end_key) {
      int cmp_end = compare_keys(key, key_len, m_end_key, m_end_keylen);
      if (cmp_end > 0) return false;
      if (cmp_end == 0 && !m_end_incl) return false;
    }

    return true;
  }

  void find_start_position() {
    if (!m_start_key || !m_art->root()) return;
    find_position_ge(m_start_key, m_start_keylen);
  }

  void find_position_ge(const unsigned char *target_key, uint32_t target_len) {
    ART::Art_node *current = m_art->root();
    uint32_t depth = 0;

    while (current && !ART::is_leaf(current)) {
      if (current->partial_len > 0) {  // Handle node prefix
        uint32_t compare_len = std::min(current->partial_len, target_len - depth);
        if (compare_len > 0) {
          int prefix_cmp = memcmp(target_key + depth, current->partial, compare_len);
          if (prefix_cmp < 0) {
            navigate_to_leftmost_leaf_from(current, depth);
            return;
          } else if (prefix_cmp > 0) {
            find_next_branch_gt(current, target_key, depth);
            return;
          }
        }
        depth += current->partial_len;
        if (depth >= target_len) {
          navigate_to_leftmost_leaf_from(current, depth);
          return;
        }
      }

      unsigned char target_byte = target_key[depth];
      ART::Art_node *child = find_child_ge(current, target_byte);

      if (!child) {
        navigate_to_leftmost_leaf_from(current, depth);
        return;
      }

      // Push current node state to enable sibling traversal in next()
      m_stack.push_back({current, 0, 0, depth});

      current = child;
      depth++;
    }

    if (current && ART::is_leaf(current)) {
      m_stack.push_back({current, 0, 0, depth});
    }
  }

  void find_next_branch_gt(ART::Art_node *node, const unsigned char *target_key, uint32_t depth) {
    unsigned char target_byte = target_key[depth];
    ART::Art_node *next_child = find_child_gt(node, target_byte);
    if (!next_child) {
      m_finished = true;
      return;
    }
    navigate_to_leftmost_leaf_from(next_child, depth + 1);
  }

  ART::Art_node *find_child_ge(ART::Art_node *node, unsigned char target_byte) {
    if (!node) return nullptr;

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = reinterpret_cast<ART::Art_node4 *>(node);
        for (int i = 0; i < n->num_children; ++i) {
          if (n->keys[i] >= target_byte) {
            return n->children[i].get();
          }
        }
        break;
      }
      case ART::NODE16: {
        auto *n = reinterpret_cast<ART::Art_node16 *>(node);
        for (int i = 0; i < n->num_children; ++i) {
          if (n->keys[i] >= target_byte) {
            return n->children[i].get();
          }
        }
        break;
      }
      case ART::NODE48: {
        auto *n = reinterpret_cast<ART::Art_node48 *>(node);
        for (int lbl = target_byte; lbl < 256; ++lbl) {
          uint8_t idx = n->keys[lbl];
          if (idx != 0) {
            return n->children[idx - 1].get();
          }
        }
        break;
      }
      case ART::NODE256: {
        auto *n = reinterpret_cast<ART::Art_node256 *>(node);
        for (int lbl = target_byte; lbl < 256; ++lbl) {
          if (n->children[lbl]) {
            return n->children[lbl].get();
          }
        }
        break;
      }
      default:
        return nullptr;
    }
    return nullptr;
  }

  ART::Art_node *find_child_gt(ART::Art_node *node, unsigned char target_byte) {
    if (!node) return nullptr;

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = reinterpret_cast<ART::Art_node4 *>(node);
        for (int i = 0; i < n->num_children; ++i) {
          if (n->keys[i] > target_byte) {
            return n->children[i].get();
          }
        }
        break;
      }
      case ART::NODE16: {
        auto *n = reinterpret_cast<ART::Art_node16 *>(node);
        for (int i = 0; i < n->num_children; ++i) {
          if (n->keys[i] > target_byte) {
            return n->children[i].get();
          }
        }
        break;
      }
      case ART::NODE48: {
        auto *n = reinterpret_cast<ART::Art_node48 *>(node);
        for (int lbl = target_byte + 1; lbl < 256; ++lbl) {
          uint8_t idx = n->keys[lbl];
          if (idx != 0) {
            return n->children[idx - 1].get();
          }
        }
        break;
      }
      case ART::NODE256: {
        auto *n = reinterpret_cast<ART::Art_node256 *>(node);
        for (int lbl = target_byte + 1; lbl < 256; ++lbl) {
          if (n->children[lbl]) {
            return n->children[lbl].get();
          }
        }
        break;
      }
      default:
        return nullptr;
    }
    return nullptr;
  }

  void navigate_to_leftmost_leaf_from(ART::Art_node *node, uint32_t depth) {
    if (!node) return;

    if (ART::is_leaf(node)) {
      m_stack.push_back({node, 0, 0, depth});
      return;
    }

    m_stack.push_back({node, 0, 0, depth});

    ART::Art_node *leftmost_child = get_leftmost_child(node);
    if (leftmost_child) {
      navigate_to_leftmost_leaf_from(leftmost_child, depth + 1);
    }
  }

  ART::Art_node *get_leftmost_child(ART::Art_node *node) {
    if (!node || ART::is_leaf(node)) return nullptr;

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = reinterpret_cast<ART::Art_node4 *>(node);
        return n->children[0].get();
      }
      case ART::NODE16: {
        auto *n = reinterpret_cast<ART::Art_node16 *>(node);
        return n->children[0].get();
      }
      case ART::NODE48: {
        auto *n = reinterpret_cast<ART::Art_node48 *>(node);
        for (int i = 0; i < 256; ++i) {
          if (n->keys[i] != 0) {
            return n->children[n->keys[i] - 1].get();
          }
        }
        break;
      }
      case ART::NODE256: {
        auto *n = reinterpret_cast<ART::Art_node256 *>(node);
        for (int i = 0; i < 256; ++i) {
          if (n->children[i]) {
            return n->children[i].get();
          }
        }
        break;
      }
      default:
        return nullptr;
    }
    return nullptr;
  }

  void find_leftmost_leaf() {
    if (!m_art || !m_art->root()) return;
    navigate_to_leftmost_leaf_from(m_art->root(), 0);
  }

  ART::Art_node *get_next_child(TraversalState &state) {
    if (!state.node || ART::is_leaf(state.node)) return nullptr;

    switch (state.node->type()) {
      case ART::NODE4: {
        auto *n = reinterpret_cast<ART::Art_node4 *>(state.node);
        if (state.child_pos >= n->num_children) return nullptr;
        return n->children[state.child_pos++].get();
      }
      case ART::NODE16: {
        auto *n = reinterpret_cast<ART::Art_node16 *>(state.node);
        if (state.child_pos >= n->num_children) return nullptr;
        return n->children[state.child_pos++].get();
      }
      case ART::NODE48: {
        auto *n = reinterpret_cast<ART::Art_node48 *>(state.node);
        while (state.child_pos < 256) {
          int lbl = state.child_pos++;
          uint8_t idx = n->keys[lbl];
          if (idx != 0) {
            return n->children[idx - 1].get();
          }
        }
        break;
      }
      case ART::NODE256: {
        auto *n = reinterpret_cast<ART::Art_node256 *>(state.node);
        while (state.child_pos < 256) {
          int lbl = state.child_pos++;
          if (n->children[lbl]) {
            return n->children[lbl].get();
          }
        }
        break;
      }
      default:
        return nullptr;
    }
    return nullptr;
  }

 private:
  ART *m_art;
  std::unordered_set<ART::Art_leaf *> m_seen_nodes;
  std::vector<TraversalState> m_stack;
  bool m_finished{false};

  const unsigned char *m_start_key{nullptr};
  int m_start_keylen{0};
  bool m_start_incl{false};

  const unsigned char *m_end_key{nullptr};
  int m_end_keylen{0};
  bool m_end_incl{false};
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_ITERATOR_H__