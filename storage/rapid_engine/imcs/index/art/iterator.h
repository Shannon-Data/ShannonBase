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
 *
 * Stability across lock releases: Art_Iterator drops the shared lock between
 * rows, so writers can run mid-scan.  Nodes are kept alive by shared_ptr, but
 * a writer may orphan a node on our path (Node4 -> Node16 growth, collapse)
 * and clear() leaves reachable only through that orphan.  Walking such a path
 * used to end the scan early.  Every mutation bumps Art_tree::version; if it
 * moved since the last step, the cursor re-seeks by the last key it emitted
 * (restoring the value slot within the leaf) instead of trusting its path.
 *
 * Locking: this class takes no locks of its own.  It reads internal nodes and
 * leaf payloads directly and holds shared_ptr references to them across calls,
 * so every call must be made with ART::tree()->tree_mutex held at least
 * shared -- see Art_Iterator in imcs/index/iterator.h, which owns that lock.
 * The leaves used to carry a mutex of their own that covered the value array
 * only; nothing covered the node path, and the leaf mutex cost 56 bytes on
 * every indexed row.
 */
template <typename key_t, typename value_t>
class ARTIterator {
  struct PathEntry {
    std::shared_ptr<ART::Art_node> node;
    int edge_label{-1};
  };

  enum class RangeCheckResult { BELOW_START = 0, IN_RANGE, ABOVE_END };

  // Refers to the child slot inside the parent's children[] instead of copying
  // the shared_ptr out of it. Every walk runs under ART::tree_mutex, held by
  // Art_Iterator, so the slot cannot move underneath us; a reference is taken
  // only for the nodes the iterator actually retains across calls.
  struct ChildResult {
    const ART::ArtNodePtr *slot{nullptr};
    int label{-1};

    ART::Art_node *node() const { return slot ? slot->get() : nullptr; }
    const ART::ArtNodePtr &ref() const { return *slot; }
    explicit operator bool() const { return slot != nullptr && *slot != nullptr; }
  };

 public:
  // art->tree() takes ART::m_node_mutex, so it is read here, once, while the
  // caller holds no tree_mutex.  Taking it later, under tree_mutex, inverts the
  // lock order ART_tree_destroy() uses and can deadlock.
  explicit ARTIterator(ART *art) : m_art(art), m_tree(art ? art->tree() : nullptr) {}

  /** Configure a forward range and position on its first physical candidate. */
  void init_scan(const key_t *startkey, int startkey_len, bool start_inclusive, const key_t *endkey, int endkey_len,
                 bool end_inclusive) {
    set_range(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
    m_reverse = false;
    m_has_last = false;
    clear_position();

    if (!tree_root_ref().get()) return;
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
    m_reverse = true;
    m_has_last = false;
    clear_position();

    if (!tree_root_ref().get()) return;
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

    ART::ArtNodePtr current = tree_root();
    if (!current) return false;
    uint32_t depth = 0;

    while (current && !ART::is_leaf(current.get())) {
      const PrefixDecision prefix_decision = compare_target_with_node_prefix(current.get(), target, target_len, depth);
      if (prefix_decision == PrefixDecision::TARGET_SMALLER) {
        return descend_leftmost(std::move(current), true);
      }
      if (prefix_decision == PrefixDecision::TARGET_GREATER) {
        return move_to_successor_subtree(true);
      }
      if (prefix_decision == PrefixDecision::TARGET_EXHAUSTED) {
        return inclusive ? descend_leftmost(std::move(current), true) : move_to_successor_subtree(true);
      }

      depth += ART::to_inner(current.get())->partial_len;
      if (depth >= target_len) {
        return inclusive ? descend_leftmost(std::move(current), true) : move_to_successor_subtree(true);
      }

      const unsigned char target_byte = target[depth];
      ChildResult cr = find_child_ge(current.get(), target_byte);
      if (!cr) return move_to_successor_subtree(true);

      // One reference for the child, then hand the parent to m_path by move:
      // the descent costs a single refcount update per level.
      ART::ArtNodePtr child = cr.ref();
      m_path.push_back({std::move(current), cr.label});
      if (cr.label > static_cast<int>(target_byte)) return descend_leftmost(std::move(child), true);

      current = std::move(child);
      ++depth;
    }

    if (!current || !ART::is_leaf(current.get())) return false;
    auto leaf = std::static_pointer_cast<ART::Art_leaf>(std::move(current));
    const int cmp = boundary_compare(leaf->key(), leaf->key_length(), target, target_len);
    if (cmp > 0 || (cmp == 0 && inclusive)) return set_leaf(std::move(leaf), false, true);
    return move_to_successor_subtree(true);
  }

  /** Position at the last key <= target (or strictly before target's prefix group). */
  bool seek_le(const unsigned char *target, uint32_t target_len, bool inclusive = true) {
    clear_position();
    if (!target || target_len == 0) return last();

    ART::ArtNodePtr current = tree_root();
    if (!current) return false;
    uint32_t depth = 0;

    while (current && !ART::is_leaf(current.get())) {
      const PrefixDecision prefix_decision = compare_target_with_node_prefix(current.get(), target, target_len, depth);
      if (prefix_decision == PrefixDecision::TARGET_SMALLER) {
        return move_to_predecessor_subtree(true);
      }
      if (prefix_decision == PrefixDecision::TARGET_GREATER) {
        return descend_rightmost(std::move(current), true);
      }
      if (prefix_decision == PrefixDecision::TARGET_EXHAUSTED) {
        return inclusive ? descend_rightmost(std::move(current), true) : move_to_predecessor_subtree(true);
      }

      depth += ART::to_inner(current.get())->partial_len;
      if (depth >= target_len) {
        return inclusive ? descend_rightmost(std::move(current), true) : move_to_predecessor_subtree(true);
      }

      const unsigned char target_byte = target[depth];
      ChildResult cr = find_child_le(current.get(), target_byte);
      if (!cr) return move_to_predecessor_subtree(true);

      ART::ArtNodePtr child = cr.ref();
      m_path.push_back({std::move(current), cr.label});
      if (cr.label < static_cast<int>(target_byte)) return descend_rightmost(std::move(child), true);

      current = std::move(child);
      ++depth;
    }

    if (!current || !ART::is_leaf(current.get())) return false;
    auto leaf = std::static_pointer_cast<ART::Art_leaf>(std::move(current));
    const int cmp = boundary_compare(leaf->key(), leaf->key_length(), target, target_len);
    if (cmp < 0 || (cmp == 0 && inclusive)) return set_leaf(std::move(leaf), true, true);
    return move_to_predecessor_subtree(true);
  }

  /** Position at the first physical key/value pair. */
  bool first() {
    clear_position();
    auto root = tree_root();
    return root ? descend_leftmost(std::move(root), true) : false;
  }

  /** Position at the last physical key/value pair. */
  bool last() {
    clear_position();
    auto root = tree_root();
    return root ? descend_rightmost(std::move(root), true) : false;
  }

  /** Emit current on the first call after positioning, then advance forward. */
  bool next(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out || !m_leaf) return false;
    if (!resync_if_changed(/*forward=*/true)) return false;

    for (;;) {
      if (m_pending_current) {
        m_pending_current = false;
      } else if (!advance_forward()) {
        return false;
      }
      if (!m_leaf) return false;
      // An emptied leaf (deleted while we held it) has no key: skip it in BOTH
      // directions rather than letting key_in_range() misread it as BELOW_START.
      if (m_leaf->key_length() == 0) continue;

      const RangeCheckResult rc = key_in_range(m_leaf->key(), m_leaf->key_length());
      if (rc == RangeCheckResult::ABOVE_END) return false;
      if (rc == RangeCheckResult::BELOW_START) continue;
      if (emit_current(key_out, key_len_out, value_out)) return true;
    }
  }

  /** Emit current on the first call after positioning, then advance backward. */
  bool prev(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!key_out || !key_len_out || !value_out || !m_leaf) return false;
    if (!resync_if_changed(/*forward=*/false)) return false;

    for (;;) {
      if (m_pending_current) {
        m_pending_current = false;
      } else if (!advance_backward()) {
        return false;
      }
      if (!m_leaf) return false;
      if (m_leaf->key_length() == 0) continue;

      const RangeCheckResult rc = key_in_range(m_leaf->key(), m_leaf->key_length());
      if (rc == RangeCheckResult::BELOW_START) return false;
      if (rc == RangeCheckResult::ABOVE_END) continue;
      if (emit_current(key_out, key_len_out, value_out)) return true;
    }
  }

 private:
  enum class PrefixDecision { MATCHED = 0, TARGET_SMALLER, TARGET_GREATER, TARGET_EXHAUSTED };

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
    m_seen_version = m_tree ? m_tree->version.load(std::memory_order_acquire) : 0;
    m_path.clear();
    m_leaf.reset();
    m_value_idx = -1;
    m_pending_current = false;
  }

  const ART::ArtNodePtr &tree_root_ref() const {
    static const ART::ArtNodePtr kNull;
    return m_tree ? m_tree->root : kNull;  // m_tree cached at construction: see ctor
  }

  // Re-run the positioning the scan started with (no key has been emitted yet).
  void reseek_initial() {
    if (m_reverse) {
      if (!m_end_key.empty())
        (void)seek_le(m_end_key.data(), static_cast<uint32_t>(m_end_key.size()), m_end_incl);
      else
        (void)last();
    } else {
      if (!m_start_key.empty())
        (void)seek_ge(m_start_key.data(), static_cast<uint32_t>(m_start_key.size()), m_start_incl);
      else
        (void)first();
    }
  }

  /**
   * If a writer ran since the last step, rebuild the position from the last
   * emitted (key, value) rather than from the retained path.  Returns false
   * when that leaves the cursor at EOF.  Caller holds tree_mutex shared.
   */
  bool resync_if_changed(bool forward) {
    if (!m_tree) return m_leaf != nullptr;
    const uint64_t v = m_tree->version.load(std::memory_order_acquire);
    if (v == m_seen_version) return true;

    if (!m_has_last) {
      reseek_initial();  // positioned but nothing emitted yet
      m_seen_version = v;
      return m_leaf != nullptr;
    }

    const std::vector<unsigned char> key = m_last_key;  // seeks clear our state
    const value_t val = m_last_val;
    const int64_t idx = m_last_idx;

    if (forward)
      (void)seek_ge(key.data(), static_cast<uint32_t>(key.size()), true);
    else
      (void)seek_le(key.data(), static_cast<uint32_t>(key.size()), true);
    m_seen_version = v;
    if (!m_leaf) return false;

    // Landed on a neighbouring key (ours was deleted): the seek left it pending,
    // so it is emitted next.  Landed on our own key: continue after our value.
    const bool same = m_leaf->key_length() == key.size() && std::memcmp(m_leaf->key(), key.data(), key.size()) == 0;
    if (!same) return true;

    const int64_t cnt = static_cast<int64_t>(m_leaf->value_count());
    int64_t pos = -1;
    if (m_leaf->value_length() == sizeof(value_t)) {
      for (int64_t i = 0; i < cnt; ++i)
        if (std::memcmp(m_leaf->value_at(static_cast<uint32_t>(i)), &val, sizeof(value_t)) == 0) {
          pos = i;
          break;
        }
    }
    if (pos >= 0) {
      m_value_idx = pos;
      m_pending_current = false;
    } else if (forward) {  // our value was removed: its successor slid into idx
      if (idx < cnt) {
        m_value_idx = idx;
        m_pending_current = true;
      } else {
        m_value_idx = cnt - 1;
        m_pending_current = false;
      }
    } else {  // unemitted values are the ones below idx, which did not move
      const int64_t i = std::min(idx, cnt) - 1;
      if (i >= 0) {
        m_value_idx = i;
        m_pending_current = true;
      } else {
        m_value_idx = 0;
        m_pending_current = false;
      }
    }
    return true;
  }

  ART::ArtNodePtr tree_root() const { return tree_root_ref(); }

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

  // Borrows the subtree instead of pinning every node on the way down. The
  // caller holds a reference to `node` and runs under tree_mutex, so the leaf
  // this reaches stays alive for as long as the returned pointer is read.
  const ART::Art_leaf *representative_leaf(const ART::Art_node *node) const {
    const ART::Art_node *current = node;
    while (current && !ART::is_leaf(current)) {
      ChildResult first_child = find_child_ge(current, 0);
      if (!first_child) return nullptr;
      current = first_child.node();
    }
    return ART::to_leaf(current);
  }

  PrefixDecision compare_target_with_node_prefix(const ART::Art_node *node, const unsigned char *target,
                                                 uint32_t target_len, uint32_t depth) const {
    const ART::Art_inner_node *inner = ART::to_inner(node);
    if (!inner || inner->partial_len == 0) return PrefixDecision::MATCHED;

    const ART::Art_leaf *representative = nullptr;
    if (inner->partial_len > ART::MAX_PREFIX_LEN) representative = representative_leaf(node);

    for (uint32_t i = 0; i < inner->partial_len; ++i) {
      if (depth + i >= target_len) return PrefixDecision::TARGET_EXHAUSTED;

      const unsigned char prefix_byte =
          (i < ART::MAX_PREFIX_LEN)
              ? inner->partial[i]
              : (representative && depth + i < representative->key_length() ? representative->key()[depth + i] : 0);

      if (target[depth + i] < prefix_byte) return PrefixDecision::TARGET_SMALLER;
      if (target[depth + i] > prefix_byte) return PrefixDecision::TARGET_GREATER;
    }
    return PrefixDecision::MATCHED;
  }

  ChildResult find_child_ge(const ART::Art_node *node, int target_label) const {
    if (!node || ART::is_leaf(node) || target_label > 255) return {};
    const int begin = std::max(0, target_label);

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<const ART::Art_node4 *>(node);
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= begin) return {&n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<const ART::Art_node16 *>(node);
        for (int i = 0; i < n->num_children; ++i)
          if (n->keys[i] >= begin) return {&n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<const ART::Art_node48 *>(node);
        for (int label = begin; label < 256; ++label) {
          const uint8_t idx = n->keys[label];
          if (idx) return {&n->children[idx - 1], label};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<const ART::Art_node256 *>(node);
        for (int label = begin; label < 256; ++label)
          if (n->children[label]) return {&n->children[label], label};
        break;
      }
      default:
        break;
    }
    return {};
  }

  ChildResult find_child_le(const ART::Art_node *node, int target_label) const {
    if (!node || ART::is_leaf(node) || target_label < 0) return {};
    const int begin = std::min(255, target_label);

    switch (node->type()) {
      case ART::NODE4: {
        auto *n = static_cast<const ART::Art_node4 *>(node);
        for (int i = static_cast<int>(n->num_children) - 1; i >= 0; --i)
          if (n->keys[i] <= begin) return {&n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE16: {
        auto *n = static_cast<const ART::Art_node16 *>(node);
        for (int i = static_cast<int>(n->num_children) - 1; i >= 0; --i)
          if (n->keys[i] <= begin) return {&n->children[i], static_cast<int>(n->keys[i])};
        break;
      }
      case ART::NODE48: {
        auto *n = static_cast<const ART::Art_node48 *>(node);
        for (int label = begin; label >= 0; --label) {
          const uint8_t idx = n->keys[label];
          if (idx) return {&n->children[idx - 1], label};
        }
        break;
      }
      case ART::NODE256: {
        auto *n = static_cast<const ART::Art_node256 *>(node);
        for (int label = begin; label >= 0; --label)
          if (n->children[label]) return {&n->children[label], label};
        break;
      }
      default:
        break;
    }
    return {};
  }

  bool set_leaf(std::shared_ptr<ART::Art_leaf> leaf, bool use_last_value, bool pending_current) {
    if (!leaf || leaf->value_count() == 0) return false;
    m_leaf = std::move(leaf);
    m_value_idx = use_last_value ? static_cast<int64_t>(m_leaf->value_count()) - 1 : 0;
    m_pending_current = pending_current;
    return true;
  }

  bool descend_leftmost(ART::ArtNodePtr node, bool pending_current) {
    if (!node) return false;
    while (!ART::is_leaf(node.get())) {
      ChildResult child = find_child_ge(node.get(), 0);
      if (!child) return false;
      ART::ArtNodePtr next = child.ref();
      m_path.push_back({std::move(node), child.label});
      node = std::move(next);
    }
    return set_leaf(std::static_pointer_cast<ART::Art_leaf>(std::move(node)), false, pending_current);
  }

  bool descend_rightmost(ART::ArtNodePtr node, bool pending_current) {
    if (!node) return false;
    while (!ART::is_leaf(node.get())) {
      ChildResult child = find_child_le(node.get(), 255);
      if (!child) return false;
      ART::ArtNodePtr next = child.ref();
      m_path.push_back({std::move(node), child.label});
      node = std::move(next);
    }
    return set_leaf(std::static_pointer_cast<ART::Art_leaf>(std::move(node)), true, pending_current);
  }

  // `m_path` already describes the ancestors of the subtree/leaf being
  // skipped. Walk up to the first parent having a greater sibling.
  bool move_to_successor_subtree(bool pending_current) {
    // Inspect ancestors without destroying the current position.  If no
    // successor exists, EOF is non-destructive and a subsequent prev() can
    // reverse direction from the current key.
    for (size_t i = m_path.size(); i > 0; --i) {
      PathEntry &parent = m_path[i - 1];
      ChildResult sibling = find_child_ge(parent.node.get(), parent.edge_label + 1);
      if (!sibling) continue;

      ART::ArtNodePtr subtree = sibling.ref();
      m_path.resize(i);
      m_path.back().edge_label = sibling.label;
      m_leaf.reset();
      m_value_idx = -1;
      m_pending_current = false;
      return descend_leftmost(std::move(subtree), pending_current);
    }
    return false;
  }

  // Symmetric predecessor walk: first smaller sibling, then rightmost descent.
  bool move_to_predecessor_subtree(bool pending_current) {
    for (size_t i = m_path.size(); i > 0; --i) {
      PathEntry &parent = m_path[i - 1];
      ChildResult sibling = find_child_le(parent.node.get(), parent.edge_label - 1);
      if (!sibling) continue;

      ART::ArtNodePtr subtree = sibling.ref();
      m_path.resize(i);
      m_path.back().edge_label = sibling.label;
      m_leaf.reset();
      m_value_idx = -1;
      m_pending_current = false;
      return descend_rightmost(std::move(subtree), pending_current);
    }
    return false;
  }

  bool advance_forward() {
    if (!m_leaf) return false;
    if (m_value_idx >= 0 && static_cast<uint32_t>(m_value_idx + 1) < m_leaf->value_count()) {
      ++m_value_idx;
      return true;
    }
    return move_to_successor_subtree(false);
  }

  bool advance_backward() {
    if (!m_leaf) return false;
    if (m_leaf->value_count() && m_value_idx > 0) {
      m_value_idx = std::min<int64_t>(m_value_idx - 1, static_cast<int64_t>(m_leaf->value_count()) - 1);
      return true;
    }
    return move_to_predecessor_subtree(false);
  }

  bool emit_current(const key_t **key_out, uint32_t *key_len_out, value_t *value_out) {
    if (!m_leaf || m_value_idx < 0) return false;
    if (static_cast<uint32_t>(m_value_idx) >= m_leaf->value_count()) return false;
    if (m_leaf->value_length() < sizeof(value_t)) return false;

    *key_out = reinterpret_cast<const key_t *>(m_leaf->key());
    *key_len_out = m_leaf->key_length();
    std::memcpy(value_out, m_leaf->value_at(static_cast<uint32_t>(m_value_idx)), sizeof(value_t));

    // Remember where we are so a later step can recover if the path goes stale.
    m_last_key.assign(m_leaf->key(), m_leaf->key() + m_leaf->key_length());
    std::memcpy(&m_last_val, value_out, sizeof(value_t));
    m_last_idx = m_value_idx;
    m_has_last = true;
    return true;
  }

  ART *m_art{nullptr};
  ART::Art_tree *m_tree{nullptr};
  std::vector<PathEntry> m_path;
  std::shared_ptr<ART::Art_leaf> m_leaf;
  int64_t m_value_idx{-1};
  bool m_pending_current{false};

  // Resynchronisation state (see resync_if_changed).
  uint64_t m_seen_version{0};
  bool m_reverse{false};
  bool m_has_last{false};
  std::vector<unsigned char> m_last_key;
  value_t m_last_val{};
  int64_t m_last_idx{-1};

  std::vector<unsigned char> m_start_key;
  bool m_start_incl{false};
  std::vector<unsigned char> m_end_key;
  bool m_end_incl{false};
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ART_ITERATOR_H__