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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

/**
 * Bidirectional ART cursor.
 *
 * The path stores, for every internal node on the current leaf path, the edge
 * label used to reach the child below it.  That is the ART equivalent of a
 * B+Tree cursor's parent/slot stack: successor/predecessor walks up until a
 * right/left sibling exists, then descends to the leftmost/rightmost leaf.
 *
 * `seek_ge()` and `seek_le()` use Rapid's boundary-prefix semantics.  A shorter
 * multipart boundary is treated as the whole equivalence class of full ART
 * keys having that canonical byte prefix:
 *
 *   seek_ge(prefix, inclusive=true)  -> first key with that prefix
 *   seek_ge(prefix, inclusive=false) -> first key after that prefix group
 *   seek_le(prefix, inclusive=true)  -> last key with that prefix
 *   seek_le(prefix, inclusive=false) -> last key before that prefix group
 *
 * This matches the range semantics previously implemented by scanning from the
 * left and applying prefix_compare(), but does so in O(tree height).
 */
template <typename key_t, typename value_t>
class ARTIterator {
  struct PathEntry {
    std::shared_ptr<ART::Art_node> node;
    int edge_label{-1};
  };

  enum class RangeCheckResult { BELOW_START, IN_RANGE, ABOVE_END };

  struct ChildResult {
    std::shared_ptr<ART::Art_node> child;
    int label{-1};
  };

 public:
  explicit ARTIterator(ART *art) : m_art(art) {}

  /** Configure a forward range and position on its first physical candidate. */
  void init_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    set_range(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
    clear_position();

    if (!tree_root()) return;
    if (startkey && startkey_len > 0)
      (void)seek_ge(reinterpret_cast<const unsigned char *>(startkey), static_cast<uint32_t>(startkey_len),
                    start_inclusive);
    else
      (void)first();
  }

  /** Configure a reverse range and position on its last physical candidate. */
  void init_reverse_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey,
                         int endkey_len, bool end_inclusive) {
    set_range(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
    clear_position();

    if (!tree_root()) return;
    if (endkey && endkey_len > 0)
      (void)seek_le(reinterpret_cast<const unsigned char *>(endkey), static_cast<uint32_t>(endkey_len), end_inclusive);
    else
      (void)last();
  }

  /** B+Tree-style lower_bound: position at the first key >= target. */
  bool lower_bound(const unsigned char *target, uint32_t target_len) { return seek_ge(target, target_len, true); }

  /** B+Tree-style upper_bound: position after target's complete prefix group. */
  bool upper_bound(const unsigned char *target, uint32_t target_len) { return seek_ge(target, target_len, false); }

  /** Position at the first key >= target (or strictly after target's prefix group). */
  bool seek_ge(const unsigned char *target, uint32_t target_len, bool inclusive = true) {
    clear_position();
    if (!target || target_len == 0) return first();

    std::shared_ptr<ART::Art_node> current = tree_root();
    if (!current) return false;
    uint32_t depth = 0;

    while (current && !ART::is_leaf(current.get())) {
      const PrefixDecision prefix_decision = compare_target_with_node_prefix(current, target, target_len, depth);
      if (prefix_decision == PrefixDecision::TARGET_SMALLER) {
        return descend_leftmost(current, true);
      }
      if (prefix_decision == PrefixDecision::TARGET_GREATER) {
        return move_to_successor_subtree(true);
      }
      if (prefix_decision == PrefixDecision::TARGET_EXHAUSTED) {
        return inclusive ? descend_leftmost(current, true) : move_to_successor_subtree(true);
      }

      depth += current->partial_len;
      if (depth >= target_len) {
        return inclusive ? descend_leftmost(current, true) : move_to_successor_subtree(true);
      }

      const unsigned char target_byte = target[depth];
      ChildResult cr = find_child_ge(current, target_byte);
      if (!cr.child) return move_to_successor_subtree(true);

      m_path.push_back({current, cr.label});
      if (cr.label > static_cast<int>(target_byte)) return descend_leftmost(cr.child, true);

      current = cr.child;
      ++depth;
    }

    if (!current || !ART::is_leaf(current.get())) return false;
    auto leaf = std::static_pointer_cast<ART::Art_leaf>(current);
    const int cmp = boundary_compare(leaf->key.data(), static_cast<uint32_t>(leaf->key.size()), target, target_len);
    if (cmp > 0 || (cmp == 0 && inclusive)) return set_leaf(std::move(leaf), false, true);
    return move_to_successor_subtree(true);
  }

  /** Position at the last key <= target (or strictly before target's prefix group). */
  bool seek_le(const unsigned char *target, uint32_t target_len, bool inclusive = true) {
    clear_position();
    if (!target || target_len == 0) return last();

    std::shared_ptr<ART::Art_node> current = tree_root();
    if (!current) return false;
    uint32_t depth = 0;

    while (current && !ART::is_leaf(current.get())) {
      const PrefixDecision prefix_decision = compare_target_with_node_prefix(current, target, target_len, depth);
      if (prefix_decision == PrefixDecision::TARGET_SMALLER) {
        return move_to_predecessor_subtree(true);
      }
      if (prefix_decision == PrefixDecision::TARGET_GREATER) {
        return descend_rightmost(current, true);
      }
      if (prefix_decision == PrefixDecision::TARGET_EXHAUSTED) {
        return inclusive ? descend_rightmost(current, true) : move_to_predecessor_subtree(true);
      }

      depth += current->partial_len;
      if (depth >= target_len) {
        return inclusive ? descend_rightmost(current, true) : move_to_predecessor_subtree(true);
      }

      const unsigned char target_byte = target[depth];
      ChildResult cr = find_child_le(current, target_byte);
      if (!cr.child) return move_to_predecessor_subtree(true);

      m_path.push_back({current, cr.label});
      if (cr.label < static_cast<int>(target_byte)) return descend_rightmost(cr.child, true);

      current = cr.child;
      ++depth;
    }

    if (!current || !ART::is_leaf(current.get())) return false;
    auto leaf = std::static_pointer_cast<ART::Art_leaf>(current);
    const int cmp = boundary_compare(leaf->key.data(), static_cast<uint32_t>(leaf->key.size()), target, target_len);
    if (cmp < 0 || (cmp == 0 && inclusive)) return set_leaf(std::move(leaf), true, true);
    return move_to_predecessor_subtree(true);
  }

  /** Position at the first physical key/value pair. */
  bool first() {
    clear_position();
    auto root = tree_root();
    return root ? descend_leftmost(root, true) : false;
  }

  /** Position at the last physical key/value pair. */
  bool last() {
    clear_position();
    auto root = tree_root();
    return root ? descend_rightmost(root, true) : false;
  }

  /** Emit current on the first call after positioning, then advance forward. */
  bool next(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out || !m_leaf) return false;

    for (;;) {
      if (m_pending_current) {
        m_pending_current = false;
      } else if (!advance_forward()) {
        return false;
      }

      const RangeCheckResult rc = key_in_range(m_leaf->key.data(), static_cast<uint32_t>(m_leaf->key.size()));
      if (rc == RangeCheckResult::ABOVE_END) return false;
      if (rc == RangeCheckResult::BELOW_START) continue;
      if (emit_current(key_out, key_len_out, value_out)) return true;
      // A concurrent value deletion may leave the saved value slot invalid.
      // Keep walking the ordered tree instead of turning that into EOF.
    }
  }

  /** Emit current on the first call after positioning, then advance backward. */
  bool prev(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out || !m_leaf) return false;

    for (;;) {
      if (m_pending_current) {
        m_pending_current = false;
      } else if (!advance_backward()) {
        return false;
      }

      const RangeCheckResult rc = key_in_range(m_leaf->key.data(), static_cast<uint32_t>(m_leaf->key.size()));
      if (rc == RangeCheckResult::BELOW_START) return false;
      if (rc == RangeCheckResult::ABOVE_END) continue;
      if (emit_current(key_out, key_len_out, value_out)) return true;
    }
  }

 private:
  enum class PrefixDecision { MATCHED, TARGET_SMALLER, TARGET_GREATER, TARGET_EXHAUSTED };

  void set_range(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    if (startkey && startkey_len > 0) {
      m_start_key.assign(reinterpret_cast<const unsigned char *>(startkey),
                         reinterpret_cast<const unsigned char *>(startkey) + startkey_len);
      m_start_incl = start_inclusive;
    } else {
      m_start_key.clear();
      m_start_incl = false;
    }

    if (endkey && endkey_len > 0) {
      m_end_key.assign(reinterpret_cast<const unsigned char *>(endkey),
                       reinterpret_cast<const unsigned char *>(endkey) + endkey_len);
      m_end_incl = end_inclusive;
    } else {
      m_end_key.clear();
      m_end_incl = false;
    }
  }

  void clear_position() {
    m_path.clear();
    m_leaf.reset();
    m_value_idx = -1;
    m_pending_current = false;
  }

  std::shared_ptr<ART::Art_node> tree_root() const {
    if (!m_art) return nullptr;
    ART::Art_tree *tree = m_art->tree();
    return tree ? tree->root : nullptr;
  }

  // Compare a full ART key against a possibly-shorter range boundary. If the
  // boundary is a prefix of the ART key, treat them as equal so inclusive/
  // exclusive controls the entire multipart-prefix equivalence class.
  static int boundary_compare(const unsigned char *key, uint32_t key_len, const unsigned char *boundary,
                              uint32_t boundary_len) {
    const uint32_t common = std::min(key_len, boundary_len);
    const int cmp = common == 0 ? 0 : std::memcmp(key, boundary, common);
    if (cmp != 0) return cmp;
    if (key_len < boundary_len) return -1;
    return 0;
  }

  RangeCheckResult key_in_range(const unsigned char *key, uint32_t key_len) const {
    if (!key || key_len == 0) return RangeCheckResult::BELOW_START;

    if (!m_start_key.empty()) {
      const int cmp = boundary_compare(key, key_len, m_start_key.data(), static_cast<uint32_t>(m_start_key.size()));
      if (cmp < 0 || (cmp == 0 && !m_start_incl)) return RangeCheckResult::BELOW_START;
    }

    if (!m_end_key.empty()) {
      const int cmp = boundary_compare(key, key_len, m_end_key.data(), static_cast<uint32_t>(m_end_key.size()));
      if (cmp > 0 || (cmp == 0 && !m_end_incl)) return RangeCheckResult::ABOVE_END;
    }

    return RangeCheckResult::IN_RANGE;
  }

  std::shared_ptr<ART::Art_leaf> representative_leaf(const std::shared_ptr<ART::Art_node> &node) const {
    std::shared_ptr<ART::Art_node> current = node;
    while (current && !ART::is_leaf(current.get())) {
      ChildResult first_child = find_child_ge(current, 0);
      if (!first_child.child) return nullptr;
      current = first_child.child;
    }
    return current && ART::is_leaf(current.get()) ? std::static_pointer_cast<ART::Art_leaf>(current) : nullptr;
  }

  PrefixDecision compare_target_with_node_prefix(const std::shared_ptr<ART::Art_node> &node,
                                                 const unsigned char *target, uint32_t target_len,
                                                 uint32_t depth) const {
    if (!node || node->partial_len == 0) return PrefixDecision::MATCHED;

    std::shared_ptr<ART::Art_leaf> representative;
    if (node->partial_len > ART::MAX_PREFIX_LEN) representative = representative_leaf(node);

    for (uint32_t i = 0; i < node->partial_len; ++i) {
      if (depth + i >= target_len) return PrefixDecision::TARGET_EXHAUSTED;

      const unsigned char prefix_byte =
          (i < ART::MAX_PREFIX_LEN)
              ? node->partial[i]
              : (representative && depth + i < representative->key.size() ? representative->key[depth + i] : 0);

      if (target[depth + i] < prefix_byte) return PrefixDecision::TARGET_SMALLER;
      if (target[depth + i] > prefix_byte) return PrefixDecision::TARGET_GREATER;
    }
    return PrefixDecision::MATCHED;
  }

  ChildResult find_child_ge(const std::shared_ptr<ART::Art_node> &node, int target_label) const {
    if (!node || ART::is_leaf(node.get()) || target_label > 255) return {};
    const int begin = std::max(0, target_label);

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<ART::Art_node4 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= begin) return {n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<ART::Art_node16 *>(node.get());
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= begin) return {n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(node.get());
        for (int label = begin; label < 256; ++label) {
          const uint8_t idx = n->keys[label];
          if (idx) return {n->children[idx - 1], label};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(node.get());
        for (int label = begin; label < 256; ++label)
          if (n->children[label]) return {n->children[label], label};
        break;
      }
      default:
        break;
    }
    return {};
  }

  ChildResult find_child_le(const std::shared_ptr<ART::Art_node> &node, int target_label) const {
    if (!node || ART::is_leaf(node.get()) || target_label < 0) return {};
    const int begin = std::min(255, target_label);

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<ART::Art_node4 *>(node.get());
        for (int i = static_cast<int>(n->num_children) - 1; i >= 0; --i)
          if (n->keys[i] <= begin) return {n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<ART::Art_node16 *>(node.get());
        for (int i = static_cast<int>(n->num_children) - 1; i >= 0; --i)
          if (n->keys[i] <= begin) return {n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<ART::Art_node48 *>(node.get());
        for (int label = begin; label >= 0; --label) {
          const uint8_t idx = n->keys[label];
          if (idx) return {n->children[idx - 1], label};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<ART::Art_node256 *>(node.get());
        for (int label = begin; label >= 0; --label)
          if (n->children[label]) return {n->children[label], label};
        break;
      }
      default:
        break;
    }
    return {};
  }

  bool set_leaf(std::shared_ptr<ART::Art_leaf> leaf, bool use_last_value, bool pending_current) {
    if (!leaf) return false;
    std::shared_lock lk(leaf->leaf_mutex);
    if (leaf->values.empty()) return false;
    m_leaf = std::move(leaf);
    m_value_idx = use_last_value ? static_cast<int64_t>(m_leaf->values.size()) - 1 : 0;
    m_pending_current = pending_current;
    return true;
  }

  bool descend_leftmost(std::shared_ptr<ART::Art_node> node, bool pending_current) {
    if (!node) return false;
    while (!ART::is_leaf(node.get())) {
      ChildResult child = find_child_ge(node, 0);
      if (!child.child) return false;
      m_path.push_back({node, child.label});
      node = child.child;
    }
    return set_leaf(std::static_pointer_cast<ART::Art_leaf>(node), false, pending_current);
  }

  bool descend_rightmost(std::shared_ptr<ART::Art_node> node, bool pending_current) {
    if (!node) return false;
    while (!ART::is_leaf(node.get())) {
      ChildResult child = find_child_le(node, 255);
      if (!child.child) return false;
      m_path.push_back({node, child.label});
      node = child.child;
    }
    return set_leaf(std::static_pointer_cast<ART::Art_leaf>(node), true, pending_current);
  }

  // `m_path` already describes the ancestors of the subtree/leaf being
  // skipped. Walk up to the first parent having a greater sibling.
  bool move_to_successor_subtree(bool pending_current) {
    // Inspect ancestors without destroying the current position.  If no
    // successor exists, EOF is non-destructive and a subsequent prev() can
    // reverse direction from the current key.
    for (size_t i = m_path.size(); i > 0; --i) {
      PathEntry &parent = m_path[i - 1];
      ChildResult sibling = find_child_ge(parent.node, parent.edge_label + 1);
      if (!sibling.child) continue;

      m_path.resize(i);
      m_path.back().edge_label = sibling.label;
      m_leaf.reset();
      m_value_idx = -1;
      m_pending_current = false;
      return descend_leftmost(sibling.child, pending_current);
    }
    return false;
  }

  // Symmetric predecessor walk: first smaller sibling, then rightmost descent.
  bool move_to_predecessor_subtree(bool pending_current) {
    for (size_t i = m_path.size(); i > 0; --i) {
      PathEntry &parent = m_path[i - 1];
      ChildResult sibling = find_child_le(parent.node, parent.edge_label - 1);
      if (!sibling.child) continue;

      m_path.resize(i);
      m_path.back().edge_label = sibling.label;
      m_leaf.reset();
      m_value_idx = -1;
      m_pending_current = false;
      return descend_rightmost(sibling.child, pending_current);
    }
    return false;
  }

  bool advance_forward() {
    if (!m_leaf) return false;
    {
      std::shared_lock lk(m_leaf->leaf_mutex);
      if (m_value_idx >= 0 && static_cast<size_t>(m_value_idx + 1) < m_leaf->values.size()) {
        ++m_value_idx;
        return true;
      }
    }
    return move_to_successor_subtree(false);
  }

  bool advance_backward() {
    if (!m_leaf) return false;
    {
      std::shared_lock lk(m_leaf->leaf_mutex);
      if (!m_leaf->values.empty() && m_value_idx > 0) {
        m_value_idx = std::min<int64_t>(m_value_idx - 1, static_cast<int64_t>(m_leaf->values.size()) - 1);
        return true;
      }
    }
    return move_to_predecessor_subtree(false);
  }

  bool emit_current(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) const {
    if (!m_leaf || m_value_idx < 0) return false;
    std::shared_lock lk(m_leaf->leaf_mutex);
    if (static_cast<size_t>(m_value_idx) >= m_leaf->values.size()) return false;
    const auto &value = m_leaf->values[static_cast<size_t>(m_value_idx)];
    if (value.size() < sizeof(value_t)) return false;

    *key_out = reinterpret_cast<const key_t *>(m_leaf->key.data());
    *key_len_out = static_cast<uint32_t>(m_leaf->key.size());
    std::memcpy(value_out, value.data(), sizeof(value_t));
    return true;
  }

  ART *m_art{nullptr};
  std::vector<PathEntry> m_path;
  std::shared_ptr<ART::Art_leaf> m_leaf;
  int64_t m_value_idx{-1};
  bool m_pending_current{false};

  std::vector<unsigned char> m_start_key;
  bool m_start_incl{false};
  std::vector<unsigned char> m_end_key;
  bool m_end_incl{false};
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ART_ITERATOR_H__
