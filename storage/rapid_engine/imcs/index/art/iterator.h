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
#include <unordered_set>
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {
template <typename key_t, typename value_t>
class ARTIterator {
  struct TraversalState {
    std::shared_ptr<ART::Art_node> node;
    int child_pos{0};       // next child index / label to visit
    uint32_t value_idx{0};  // next value index within a leaf
    uint32_t depth{0};
  };

  enum class RangeCheckResult { IN_RANGE, OUT_OF_RANGE, BEYOND_END };

  struct ChildResult {
    std::shared_ptr<ART::Art_node> child;
    int next_sibling_pos{0};  // child_pos to set on parent after consuming this child
  };

 public:
  explicit ARTIterator(ART *art) : m_art(art) {}

  void init_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    if (!m_art) return;

    m_stack.clear();
    m_seen_leaves.clear();
    m_finished = false;

    if (startkey && startkey_len > 0) {
      m_start_key.assign(reinterpret_cast<const unsigned char *>(startkey),
                         reinterpret_cast<const unsigned char *>(startkey) + startkey_len);
      m_start_incl = start_inclusive;
    } else {
      m_start_key.clear();
    }

    if (endkey && endkey_len > 0) {
      m_end_key.assign(reinterpret_cast<const unsigned char *>(endkey),
                       reinterpret_cast<const unsigned char *>(endkey) + endkey_len);
      m_end_incl = end_inclusive;
    } else {
      m_end_key.clear();
    }

    if (m_art->root()) {
      if (startkey)
        find_start_position();
      else
        find_leftmost_leaf();
    }
  }

  bool next(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out) return false;
    if (m_finished) return false;

    while (!m_stack.empty()) {
      TraversalState &st = m_stack.back();

      if (ART::is_leaf(st.node.get())) {
        auto *leaf = static_cast<ART::Art_leaf *>(st.node.get());

        if (m_seen_leaves.count(leaf)) {
          m_stack.pop_back();
          continue;
        }

        std::shared_lock lk(leaf->leaf_mutex);
        if (st.value_idx >= leaf->values.size()) {
          lk.unlock();
          m_seen_leaves.insert(leaf);
          m_stack.pop_back();
          continue;
        }

        // Evaluate range once; all values in a leaf share the same key.
        RangeCheckResult rc = key_in_range(leaf->key.data(), static_cast<uint32_t>(leaf->key.size()));

        if (rc == RangeCheckResult::BEYOND_END) {
          // ART is sorted: everything remaining is also past the end boundary.
          m_finished = true;
          return false;
        }
        if (rc == RangeCheckResult::OUT_OF_RANGE) {
          lk.unlock();
          m_seen_leaves.insert(leaf);
          m_stack.pop_back();
          continue;
        }

        // Emit next value.
        for (; st.value_idx < leaf->values.size(); ++st.value_idx) {
          void *val_ptr = const_cast<uint8_t *>(leaf->values[st.value_idx].data());
          if (!val_ptr) continue;

          *key_out = reinterpret_cast<const key_t *>(leaf->key.data());
          *key_len_out = static_cast<uint32_t>(leaf->key.size());
          *value_out = *reinterpret_cast<value_t *>(val_ptr);
          st.value_idx++;
          return true;
        }

        lk.unlock();
        m_seen_leaves.insert(leaf);
        m_stack.pop_back();
        continue;
      }

      auto child = get_next_child(st);
      if (child) {
        if (ART::is_leaf(child.get()))
          m_stack.push_back({child, 0, 0, st.depth + 1});
        else
          navigate_to_leftmost_leaf_from(child, st.depth + 1);
      } else {
        m_stack.pop_back();
      }
    }

    m_finished = true;
    return false;
  }

 private:
  // prefix_compare  [for RANGE CHECKS]
  //   Compare only min(len1,len2) bytes.  Returns 0 when all compared bytes
  //   match, regardless of length difference.
  //   Rationale: a composite-key leaf has 8 bytes; the id-only boundary key
  //   has 4 bytes.  The leaf IS in range if its 4-byte id prefix matches the
  //   boundary — we must NOT treat the 4-byte boundary as "less than" the
  //   8-byte leaf when their shared prefix is equal.
  inline int prefix_compare(const unsigned char *k1, uint32_t len1, const unsigned char *k2, uint32_t len2) const {
    return std::memcmp(k1, k2, std::min(len1, len2));
  }

  // full_compare  [for TREE NAVIGATION]
  //   After prefix equality, longer key > shorter key.  Needed so that
  //   find_position_ge positions the cursor correctly in the tree.
  inline int full_compare(const unsigned char *k1, uint32_t len1, const unsigned char *k2, uint32_t len2) const {
    int cmp = std::memcmp(k1, k2, std::min(len1, len2));
    if (cmp != 0) return cmp;
    if (len1 < len2) return -1;
    if (len1 > len2) return 1;
    return 0;
  }

  // key_in_range  (uses prefix_compare)
  RangeCheckResult key_in_range(const unsigned char *key, uint32_t key_len) const {
    if (!key || !key_len) return RangeCheckResult::OUT_OF_RANGE;

    if (!m_start_key.empty()) {
      int cmp = prefix_compare(key, key_len, m_start_key.data(), static_cast<uint32_t>(m_start_key.size()));
      if (cmp < 0) return RangeCheckResult::OUT_OF_RANGE;
      if (cmp == 0 && !m_start_incl) return RangeCheckResult::OUT_OF_RANGE;
    }

    if (!m_end_key.empty()) {
      int cmp = prefix_compare(key, key_len, m_end_key.data(), static_cast<uint32_t>(m_end_key.size()));
      if (cmp > 0) return RangeCheckResult::BEYOND_END;
      if (cmp == 0 && !m_end_incl) return RangeCheckResult::OUT_OF_RANGE;
    }

    return RangeCheckResult::IN_RANGE;
  }

  void find_start_position() {
    if (m_start_key.empty() || !m_art->root()) return;
    find_position_ge(m_start_key.data(), static_cast<uint32_t>(m_start_key.size()));
  }

  void find_position_ge(const unsigned char *target, uint32_t target_len) {
    ART::Art_tree *tree = m_art->tree();
    if (!tree) return;

    std::shared_ptr<ART::Art_node> current;
    {
      std::shared_lock lk(tree->tree_mutex);
      current = tree->root;
    }

    uint32_t depth = 0;

    while (current && !ART::is_leaf(current.get())) {
      if (current->partial_len > 0) {
        uint32_t avail = (depth < target_len) ? (target_len - depth) : 0;
        uint32_t cmp_len = std::min(current->partial_len, avail);

        if (cmp_len > 0) {
          int cmp = std::memcmp(target + depth, current->partial, cmp_len);
          if (cmp < 0) {
            navigate_to_leftmost_leaf_from(current, depth);
            return;
          }
          if (cmp > 0) {
            find_next_branch_gt(current, target, target_len, depth);
            return;
          }
        }

        depth += current->partial_len;
        if (depth >= target_len) {
          // target is fully consumed; start from leftmost below here
          navigate_to_leftmost_leaf_from(current, depth);
          return;
        }
      }

      unsigned char target_byte = target[depth];

      // get child >= target_byte and its sibling index
      ChildResult cr = find_child_ge_with_pos(current, target_byte);
      if (!cr.child) {
        navigate_to_leftmost_leaf_from(current, depth);
        return;
      }

      // Push parent with child_pos = next sibling, not 0
      m_stack.push_back({current, cr.next_sibling_pos, 0, depth});
      current = cr.child;
      depth++;
    }

    if (current && ART::is_leaf(current.get())) m_stack.push_back({current, 0, 0, depth});
  }

  void find_next_branch_gt(std::shared_ptr<ART::Art_node> node, const unsigned char *target, uint32_t target_len,
                           uint32_t depth) {
    if (depth >= target_len) {
      navigate_to_leftmost_leaf_from(node, depth);
      return;
    }
    unsigned char target_byte = target[depth];
    ChildResult cr = find_child_gt_with_pos(node, target_byte);
    if (!cr.child) {
      m_finished = true;
      return;
    }
    navigate_to_leftmost_leaf_from(cr.child, depth + 1);
  }

  // find_child_{ge,gt}_with_pos
  //
  // For NODE4/NODE16: child_pos is a direct array index.
  // For NODE48/NODE256: child_pos is the next label (0..255) to scan.
  //
  ChildResult find_child_ge_with_pos(const std::shared_ptr<ART::Art_node> &node, unsigned char target_byte) {
    if (!node) return {};
    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<ART::Art_node4 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= target_byte) return {n->children[i], i + 1};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<ART::Art_node16 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= target_byte) return {n->children[i], i + 1};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(node.get());
        for (int lbl = static_cast<int>(target_byte); lbl < 256; ++lbl) {
          uint8_t idx = n->keys[lbl];
          if (idx) return {n->children[idx - 1], lbl + 1};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(node.get());
        for (int lbl = static_cast<int>(target_byte); lbl < 256; ++lbl)
          if (n->children[lbl]) return {n->children[lbl], lbl + 1};
        break;
      }
      default:
        break;
    }
    return {};
  }

  ChildResult find_child_gt_with_pos(const std::shared_ptr<ART::Art_node> &node, unsigned char target_byte) {
    if (!node) return {};
    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<ART::Art_node4 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] > target_byte) return {n->children[i], i + 1};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<ART::Art_node16 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] > target_byte) return {n->children[i], i + 1};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(node.get());
        for (int lbl = static_cast<int>(target_byte) + 1; lbl < 256; ++lbl) {
          uint8_t idx = n->keys[lbl];
          if (idx) return {n->children[idx - 1], lbl + 1};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(node.get());
        for (int lbl = static_cast<int>(target_byte) + 1; lbl < 256; ++lbl)
          if (n->children[lbl]) return {n->children[lbl], lbl + 1};
        break;
      }
      default:
        break;
    }
    return {};
  }

  // navigate_to_leftmost_leaf_from
  void navigate_to_leftmost_leaf_from(std::shared_ptr<ART::Art_node> node, uint32_t depth) {
    if (!node) return;

    if (ART::is_leaf(node.get())) {
      m_stack.push_back({node, 0, 0, depth});
      return;
    }

    // child_pos = 1: leftmost (child 0 / label 0) is consumed in this recursion.
    m_stack.push_back({node, 1, 0, depth});

    auto leftmost = get_first_child(node);
    if (leftmost) navigate_to_leftmost_leaf_from(leftmost, depth + 1);
  }

  std::shared_ptr<ART::Art_node> get_first_child(const std::shared_ptr<ART::Art_node> &node) {
    if (!node || ART::is_leaf(node.get())) return nullptr;
    switch (node->type()) {
      case ART::NODE4:
        return static_cast<ART::Art_node4 *>(node.get())->children[0];
      case ART::NODE16:
        return static_cast<ART::Art_node16 *>(node.get())->children[0];
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(node.get());
        for (int i = 0; i < 256; ++i)
          if (n->keys[i]) return n->children[n->keys[i] - 1];
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(node.get());
        for (int i = 0; i < 256; ++i)
          if (n->children[i]) return n->children[i];
        break;
      }
      default:
        break;
    }
    return nullptr;
  }

  void find_leftmost_leaf() {
    if (!m_art) return;
    ART::Art_tree *tree = m_art->tree();
    if (!tree) return;
    std::shared_ptr<ART::Art_node> root;
    {
      std::shared_lock lk(tree->tree_mutex);
      root = tree->root;
    }
    if (root) navigate_to_leftmost_leaf_from(root, 0);
  }

  std::shared_ptr<ART::Art_node> get_next_child(TraversalState &st) {
    if (!st.node || ART::is_leaf(st.node.get())) return nullptr;

    switch (st.node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<ART::Art_node4 *>(st.node.get());
        if (st.child_pos >= n->num_children) return nullptr;
        return n->children[st.child_pos++];
      }
      case ART::NODE16: {
        auto *n = static_cast<ART::Art_node16 *>(st.node.get());
        if (st.child_pos >= n->num_children) return nullptr;
        return n->children[st.child_pos++];
      }
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(st.node.get());
        while (st.child_pos < 256) {
          int lbl = st.child_pos++;
          uint8_t idx = n->keys[lbl];
          if (idx) return n->children[idx - 1];
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(st.node.get());
        while (st.child_pos < 256) {
          int lbl = st.child_pos++;
          if (n->children[lbl]) return n->children[lbl];
        }
        break;
      }
      default:
        break;
    }
    return nullptr;
  }

  ART *m_art;
  std::unordered_set<ART::Art_leaf *> m_seen_leaves;
  std::vector<TraversalState> m_stack;
  bool m_finished{false};

  std::vector<unsigned char> m_start_key;
  bool m_start_incl{false};
  std::vector<unsigned char> m_end_key;
  bool m_end_incl{false};
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ART_ITERATOR_H__