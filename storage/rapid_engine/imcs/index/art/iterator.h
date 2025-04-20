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
    bool is_leaf;
    uint32_t value_idx;
  };

 public:
  explicit ARTIterator(ART *art) : m_art(art) {}

  void init_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    // Convert keys to unsigned char format
    m_start_key = const_cast<key_t *>(startkey);
    m_start_keylen = startkey_len;
    m_start_incl = start_inclusive;
    m_end_key = const_cast<key_t *>(endkey);
    m_end_keylen = endkey_len;
    m_end_incl = end_inclusive;
    m_current_val_idx = 0;

    // Initialize traversal
    m_stack.clear();
    current_leaf_ = nullptr;
    m_started = false;
    m_finished = false;

    if (m_art->root()) {
      m_stack.push_back({m_art->root(), 0, false, 0});
    }
  }

  bool next(const key_t *key_out, uint32_t *key_len_out, value_t *value_out) {
    while (!m_stack.empty()) {
      auto &state = m_stack.back();

      if (state.is_leaf) {
        ART::Art_leaf *leaf = LEAF_RAW(state.node);
        if (state.value_idx == 0 && !key_in_range(leaf)) {
          m_stack.pop_back();
          continue;
        }

        //*key_len_out = leaf->key_len;
        // memcpy(key_out, leaf->key, *key_len_out);
        *value_out = *reinterpret_cast<value_t *>(leaf->values[state.value_idx]);

        if (++state.value_idx < leaf->vcount) {
          m_stack.back().value_idx = state.value_idx;
        } else {
          m_stack.pop_back();
        }
        return true;
      }

      if (auto child = get_next_child(state)) {
        m_stack.pop_back();
        m_stack.push_back(state);

        if (IS_LEAF(child)) {
          m_stack.push_back({child, 0, true, 0});
        } else {
          expand_node(child);
        }
      } else {
        m_stack.pop_back();
      }
    }
    return false;
  }

 private:
  // Helper functions
  int compare_keys(const key_t *k1, int len1, const key_t *k2, int len2) {
    int cmp = memcmp(k1, k2, std::min(len1, len2));
    return cmp;
    // if (cmp != 0) return cmp;
    // return len1 - len2;
  }

  bool key_in_range(ART::Art_leaf *leaf) {
    if (m_start_key) {
      int cmp_start = compare_keys(leaf->key, leaf->key_len, m_start_key, m_start_keylen);
      if (cmp_start < 0) return false;
      if (!m_start_incl && cmp_start == 0) return false;
    }

    if (m_end_key) {
      int cmp_end = compare_keys(leaf->key, leaf->key_len, m_end_key, m_end_keylen);
      if (cmp_end > 0) return false;
      if (!m_end_incl && cmp_end == 0) return false;
    }
    return true;
  }

  void push_children(ART::Art_node *n) {
    switch (n->type) {
      case ART::NodeType::NODE4: {
        auto node4 = reinterpret_cast<ART::Art_node4 *>(n);
        for (int i = node4->n.num_children - 1; i >= 0; --i) {
          m_stack.push_back({node4->children[i], i, IS_LEAF(node4->children[i])});
        }
        break;
      }
      case ART::NodeType::NODE16: {
        auto node16 = reinterpret_cast<ART::Art_node16 *>(n);
        for (int i = node16->n.num_children - 1; i >= 0; --i) {
          m_stack.push_back({node16->children[i], i, IS_LEAF(node16->children[i])});
        }
        break;
      }
      case ART::NodeType::NODE48: {
        auto node48 = reinterpret_cast<ART::Art_node48 *>(n);
        for (int i = 255; i >= 0; --i) {
          if (node48->keys[i]) {
            m_stack.push_back(
                {node48->children[node48->keys[i] - 1], i, IS_LEAF(node48->children[node48->keys[i] - 1])});
          }
        }
        break;
      }
      case ART::NodeType::NODE256: {
        auto node256 = reinterpret_cast<ART::Art_node256 *>(n);
        for (int i = 255; i >= 0; --i) {
          if (node256->children[i]) {
            m_stack.push_back({node256->children[i], i, IS_LEAF(node256->children[i])});
          }
        }
        break;
      }
      default:
        abort();
    }
  }

  void expand_node(ART::Art_node *node) {
    TraversalState state{node, 0, false, 0};

    switch (node->type) {
      case ART::NodeType::NODE4: {
        state.child_pos = 0;
        break;
      }
      case ART::NodeType::NODE16: {
        state.child_pos = 0;
        break;
      }
      case ART::NodeType::NODE48: {
        auto node48 = reinterpret_cast<ART::Art_node48 *>(node);
        state.child_pos = 0;
        while (state.child_pos < 256 && !node48->keys[state.child_pos]) {
          ++state.child_pos;
        }
        break;
      }
      case ART::NodeType::NODE256: {
        auto node256 = reinterpret_cast<ART::Art_node256 *>(node);
        state.child_pos = 0;
        while (state.child_pos < 256 && !node256->children[state.child_pos]) {
          ++state.child_pos;
        }
        break;
      }
      default:
        abort();
    }

    m_stack.push_back(state);
  }

  ART::Art_node *get_next_child(TraversalState &state) {
    if (state.is_leaf) return nullptr;

    ART::Art_node *child = nullptr;
    switch (state.node->type) {
      case ART::NodeType::NODE4: {
        auto node4 = reinterpret_cast<ART::Art_node4 *>(state.node);
        if (state.child_pos < node4->n.num_children) {
          child = node4->children[state.child_pos];
          ++state.child_pos;
        }
        break;
      }
      case ART::NodeType::NODE16: {
        auto node16 = reinterpret_cast<ART::Art_node16 *>(state.node);
        if (state.child_pos < node16->n.num_children) {
          child = node16->children[state.child_pos];
          ++state.child_pos;
        }
        break;
      }
      case ART::NodeType::NODE48: {
        auto node48 = reinterpret_cast<ART::Art_node48 *>(state.node);
        while (state.child_pos < 256) {
          if (node48->keys[state.child_pos]) {
            child = node48->children[node48->keys[state.child_pos] - 1];
            ++state.child_pos;
            break;
          }
          ++state.child_pos;
        }
        break;
      }
      case ART::NodeType::NODE256: {
        auto node256 = reinterpret_cast<ART::Art_node256 *>(state.node);
        while (state.child_pos < 256) {
          if (node256->children[state.child_pos]) {
            child = node256->children[state.child_pos];
            ++state.child_pos;
            break;
          }
          ++state.child_pos;
        }
        break;
      }
      default:
        abort();
    }
    return child;
  }

 private:
  ART *m_art;
  std::vector<TraversalState> m_stack;
  ART::Art_leaf *current_leaf_{nullptr};

  bool m_started{false};
  bool m_finished{false};

  // Scan range
  key_t *m_start_key{nullptr};
  int m_start_keylen{0};
  bool m_start_incl{false};
  key_t *m_end_key{nullptr};
  int m_end_keylen{0};
  bool m_end_incl{false};
  uint32_t m_current_val_idx{0};
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_ITERATOR_H__