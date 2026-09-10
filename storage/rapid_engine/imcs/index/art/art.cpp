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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <cstring>

#ifdef __i386__
#include <emmintrin.h>
#else
#ifdef __amd64__
#include <emmintrin.h>
#endif
#endif

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {
/*
 * ART_insert
 *
 * Concurrency model: tree_mutex is acquired exclusively for the whole call.
 * This serialises all concurrent writers and blocks readers for the duration.
 *
 * Upgrade path (future work): Replace with Optimistic Lock Coupling (OLC).
 * Each inner node would carry a version counter; readers optimistically read
 * then validate; writers lock only the target node.  That counter is 8 bytes
 * on Art_inner_node, which is why no per-node lock word is reserved today.
 */
void *ART::ART_insert(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  int old_val = 0;
  ArtNodePtr &root_ref = m_tree->root;
  void *old = Recursive_insert(root_ref, key, key_len, value, value_len, 0, &old_val, 0);

  if (!old_val) m_tree->size.fetch_add(1, std::memory_order_acq_rel);

  return old;
}

/*
 * ART_delete
 */
void *ART::ART_delete(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::unique_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);

  void *result = nullptr;
  ArtNodePtr leaf = Recursive_delete(m_tree->root, key, key_len, 0, result);
  if (!leaf) return nullptr;

  // If the deleted leaf was the root itself (tree had exactly one entry),
  // nullify the root so that the next insert does not encounter a zombie
  // leaf with cleared key/values (which would UB on leaf->key[0] access).
  if (leaf.get() == m_tree->root.get() && is_leaf(leaf.get())) {
    m_tree->root = nullptr;
  }

  m_tree->size.fetch_sub(1, std::memory_order_acq_rel);
  return result;
}

void *ART::ART_delete_value(const unsigned char *key, int key_len, const void *value, uint32_t value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);

  Art_leaf *leaf = Find_leaf(key, key_len);
  if (!leaf || !leaf->remove_value(value, value_len)) return nullptr;

  // The leaf keeps its other duplicate values; remove it from the tree only
  // when this was its last value.
  if (leaf->value_count() == 0) {
    void *dummy = nullptr;
    ArtNodePtr res = Recursive_delete(m_tree->root, key, key_len, 0, dummy);
    if (res) {
      if (res.get() == m_tree->root.get() && is_leaf(res.get())) m_tree->root = nullptr;
      m_tree->size.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  // Non-null marker: caller only needs found/not-found.
  return const_cast<void *>(value);
}

/*
 * Find_leaf
 *
 * The descent every point operation shares: follow the compressed prefixes and
 * the per-byte edges, then confirm the leaf really holds this key -- reaching a
 * leaf only means its key agrees on the bytes the descent looked at.
 *
 * Caller holds tree_mutex; the returned leaf is only valid while it does.
 */
ART::Art_leaf *ART::Find_leaf(const unsigned char *key, int key_len) {
  ArtNodePtr n = m_tree->root;
  int depth = 0;

  while (n) {
    if (is_leaf(n.get())) {
      Art_leaf *leaf = to_leaf(n.get());
      return Leaf_matches(leaf, key, key_len, depth) ? nullptr : leaf;
    }

    const Art_inner_node *inner = to_inner(n.get());
    if (inner->partial_len) {
      const int prefix_len = Check_prefix(inner, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_len) != std::min(MAX_PREFIX_LEN, inner->partial_len)) return nullptr;
      depth += inner->partial_len;
    }

    if (depth >= key_len) return nullptr;

    ArtNodePtr child = Find_child(n, key[depth]);
    if (!child) return nullptr;

    n = child;
    depth++;
  }
  return nullptr;
}

void *ART::ART_search(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  Art_leaf *leaf = Find_leaf(key, key_len);
  if (!leaf || leaf->value_count() == 0) return nullptr;
  return leaf->mutable_value_at(leaf->value_count() - 1);
}

bool ART::ART_search_copy(const unsigned char *key, int key_len, void *out, uint32_t out_len) {
  if (!key || key_len <= 0 || !out || out_len == 0 || !m_inited) return false;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  Art_leaf *leaf = Find_leaf(key, key_len);
  if (!leaf || leaf->value_count() == 0 || leaf->value_length() != out_len) return false;
  std::memcpy(out, leaf->value_at(leaf->value_count() - 1), out_len);
  return true;
}

std::vector<std::vector<uint8_t>> ART::ART_search_all(const unsigned char *key, int key_len) {
  std::vector<std::vector<uint8_t>> results;
  if (!key || key_len <= 0 || !m_inited) return results;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  const Art_leaf *leaf = Find_leaf(key, key_len);
  if (!leaf) return results;

  results.reserve(leaf->value_count());
  for (uint32_t vi = 0; vi < leaf->value_count(); ++vi) {
    const unsigned char *v = leaf->value_at(vi);
    results.emplace_back(v, v + leaf->value_length());
  }
  return results;
}

/*
 * ART_iter, acquire shared lock before iterating.
 */
int ART::ART_iter(ART_Func cb, void *data) {
  if (!m_inited || !m_tree) return 0;
  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  return Recursive_iter(m_tree->root.get(), cb, data);
}

ART::Art_leaf *ART::ART_minimum() {
  if (!m_inited) return nullptr;
  std::shared_lock lk(m_tree->tree_mutex);
  return Minimum(m_tree->root.get());
}

ART::Art_leaf *ART::ART_maximum() {
  if (!m_inited) return nullptr;
  std::shared_lock lk(m_tree->tree_mutex);
  return Maximum(m_tree->root.get());
}

void *ART::Recursive_insert(ArtNodePtr &node, const unsigned char *key, int key_len, void *value, uint32_t value_len,
                            int depth, int *old, int replace) {
  // 1. Empty slot: insert a new leaf
  if (!node) {
    node = make_art_node<Art_leaf>(key, key_len, value, value_len);
    *old = 0;
    return nullptr;
  }

  // 2. Leaf node
  if (is_leaf(node.get())) {
    auto leaf = to_leaf(node.get());

    if (!Leaf_matches(leaf, key, key_len, depth)) {
      // Same key — append or replace value.
      if (replace && leaf->value_count()) {
        if (!leaf->replace_first_value(value, value_len)) return nullptr;
        *old = 1;
        return leaf->mutable_value_at(0);
      }
      leaf->add_value(value, value_len);
      *old = 0;
      return nullptr;
    }

    // Different key — split into a Node4.
    auto node4 = make_art_node<Art_node4>();
    if (!node4) return nullptr;

    auto l2 = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!l2) return nullptr;

    int longest_prefix = Longest_common_prefix(leaf, l2.get(), depth);
    node4->partial_len = longest_prefix;
    std::memcpy(node4->partial, key + depth, std::min(static_cast<int>(MAX_PREFIX_LEN), longest_prefix));

    ArtNodePtr new_node = node4;
    new_node = Add_child4(new_node, leaf->key()[depth + longest_prefix], node);
    new_node = Add_child4(new_node, l2->key()[depth + longest_prefix], l2);
    node = new_node;
    *old = 0;
    return nullptr;
  }

  // 3. Internal node
  Art_inner_node *inner = to_inner(node.get());
  if (inner->partial_len) {
    int prefix_diff = Prefix_mismatch(inner, key, key_len, depth);

    if (static_cast<uint32_t>(prefix_diff) < inner->partial_len) {
      // Prefix mismatch — split this node.
      auto node4 = make_art_node<Art_node4>();
      if (!node4) return nullptr;

      // partial_len records the true prefix length, which may exceed
      // MAX_PREFIX_LEN under optimistic path compression; only the first
      // MAX_PREFIX_LEN bytes are actually materialised in partial[].
      // Prefix_mismatch() resumes the comparison from a leaf key past that
      // bound, so prefix_diff can legitimately come back larger than the
      // buffer -- clamp the copy the way every other partial[] write does.
      node4->partial_len = prefix_diff;
      std::memcpy(node4->partial, inner->partial, std::min(static_cast<uint32_t>(prefix_diff), MAX_PREFIX_LEN));

      ArtNodePtr new_node = node4;
      if (inner->partial_len <= MAX_PREFIX_LEN) {
        new_node = Add_child4(new_node, inner->partial[prefix_diff], node);
        inner->partial_len -= (prefix_diff + 1);
        int copy_len = std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(inner->partial_len));
        std::memmove(inner->partial, inner->partial + prefix_diff + 1, copy_len);
      } else {
        inner->partial_len -= (prefix_diff + 1);
        Art_leaf *leaf = Minimum(node.get());
        new_node = Add_child4(new_node, leaf->key()[depth + prefix_diff], node);
        int copy_len = std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(inner->partial_len));
        std::memcpy(inner->partial, leaf->key() + depth + prefix_diff + 1, copy_len);
      }

      auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
      if (!new_leaf) return nullptr;

      new_node = Add_child4(new_node, key[depth + prefix_diff], new_leaf);
      node = new_node;
      *old = 0;
      return nullptr;
    }

    depth += inner->partial_len;
  }

  if (depth >= key_len) return nullptr;

  // Recurse through the parent's actual slot so the callee can replace it
  // (leaf → Node4, say) — safe because tree_mutex exclusive is still held.
  ArtNodePtr *child_slot = Find_child_slot(node.get(), key[depth]);
  if (!child_slot || !*child_slot) {
    auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!new_leaf) return nullptr;
    // Add_child may swap `node` for a wider fan-out, invalidating child_slot;
    // nothing reads it afterwards.
    Add_child(node, node, key[depth], new_leaf);
    *old = 0;
    return nullptr;
  }

  return Recursive_insert(*child_slot, key, key_len, value, value_len, depth + 1, old, replace);
}

ART::ArtNodePtr *ART::Find_child_slot(Art_node *n, unsigned char c) {
  if (!n || is_leaf(n)) return nullptr;

  switch (n->type()) {
    case NODE4: {
      auto *p = static_cast<Art_node4 *>(n);
      for (int i = 0; i < p->num_children; ++i)
        if (p->keys[i] == c) return &p->children[i];
      break;
    }
    case NODE16: {
      auto *p = static_cast<Art_node16 *>(n);
      for (int i = 0; i < p->num_children; ++i)
        if (p->keys[i] == c) return &p->children[i];
      break;
    }
    case NODE48: {
      auto *p = static_cast<Art_node48 *>(n);
      const int idx = p->keys[c];
      if (idx) return &p->children[idx - 1];
      break;
    }
    case NODE256: {
      auto *p = static_cast<Art_node256 *>(n);
      return &p->children[c];  // may hold nullptr; the caller checks
    }
    default:
      break;
  }
  return nullptr;
}

/*
 * Recursive_delete
 *
 * Returns the leaf that was unhooked (a non-null marker), or nullptr when the
 * key is absent.  `result` is set to that same leaf when it held values; the
 * value bytes are freed with it, so callers may only test it for null.
 *
 * The child edge is dropped at the leaf's own parent and nowhere else.  This
 * used to call Remove_child on every frame as the recursion unwound, which
 * tore an entire subtree out of every ancestor on the path: deleting the fifth
 * key of a five-key tree left three of the survivors unreachable.  The descent
 * also has to walk the parent's actual child slot -- not a shared_ptr copy of
 * it -- or a node that collapses into its last remaining child (Remove_child4)
 * updates the copy and leaves the parent pointing at the old node.
 */
ART::ArtNodePtr ART::Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth,
                                      void *&result) {
  if (!node) return nullptr;

  // A leaf sitting at the root: nothing to unhook, the caller nulls the root.
  if (is_leaf(node.get())) {
    auto leaf = to_leaf(node.get());
    if (Leaf_matches(leaf, key, key_len, depth)) return nullptr;
    // The values die with the leaf, so `result` is a found/not-found marker
    // rather than a pointer to them.
    if (leaf->value_count()) result = leaf;
    leaf->clear();
    return node;
  }

  // No per-node lock — tree_mutex exclusive covers us.
  Art_inner_node *inner = to_inner(node.get());
  if (inner->partial_len) {
    int prefix_len = Check_prefix(inner, key, key_len, depth);
    if (prefix_len != std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(inner->partial_len))) return nullptr;
    depth += inner->partial_len;
  }

  if (depth >= key_len) return nullptr;

  ArtNodePtr *child_slot = Find_child_slot(node.get(), key[depth]);
  if (!child_slot || !*child_slot) return nullptr;

  if (is_leaf(child_slot->get())) {
    auto leaf = to_leaf(child_slot->get());
    if (Leaf_matches(leaf, key, key_len, depth)) return nullptr;
    if (leaf->value_count()) result = leaf;

    // Keep the leaf alive past Remove_child, which drops the tree's reference.
    ArtNodePtr removed = *child_slot;
    leaf->clear();
    Remove_child(node, key[depth], removed);
    return removed;
  }

  return Recursive_delete(*child_slot, key, key_len, depth + 1, result);
}

ART::ArtNodePtr ART::Find_child(const ArtNodePtr &n, unsigned char c) {
  if (!n || is_leaf(n.get())) return nullptr;

  // No node_mutex lock: callers always hold tree_mutex (exclusive or shared).
  switch (n->type()) {
    case NODE4: {
      auto *p = static_cast<Art_node4 *>(n.get());
      for (int i = 0; i < p->num_children; ++i)
        if (p->keys[i] == c) return p->children[i];
      break;
    }
    case NODE16: {
      auto *p = static_cast<Art_node16 *>(n.get());
      for (int i = 0; i < p->num_children; ++i)
        if (p->keys[i] == c) return p->children[i];
      break;
    }
    case NODE48: {
      auto *p = static_cast<Art_node48 *>(n.get());
      int idx = p->keys[c];
      if (idx) return p->children[idx - 1];
      break;
    }
    case NODE256: {
      auto *p = static_cast<Art_node256 *>(n.get());
      return p->children[c];  // may be nullptr, that's fine
    }
    default:
      break;
  }
  return nullptr;
}

int ART::Recursive_iter(Art_node *node, ART_Func &cb, void *data) {
  if (!node) return 0;

  if (is_leaf(node)) {
    auto leaf = to_leaf(node);
    for (uint32_t vi = 0; vi < leaf->value_count(); ++vi) {
      int r = cb(data, leaf->key(), leaf->key_length(), leaf->value_at(vi), leaf->value_length());
      if (r) return r;
    }
    return 0;
  }

  int res = 0;
  switch (node->type()) {
    case NODE4: {
      auto *n4 = static_cast<Art_node4 *>(node);
      for (int i = 0; i < n4->num_children; ++i) {
        res = Recursive_iter(n4->children[i].get(), cb, data);
        if (res) return res;
      }
      break;
    }
    case NODE16: {
      auto *n16 = static_cast<Art_node16 *>(node);
      for (int i = 0; i < n16->num_children; ++i) {
        res = Recursive_iter(n16->children[i].get(), cb, data);
        if (res) return res;
      }
      break;
    }
    case NODE48: {
      auto *n48 = static_cast<Art_node48 *>(node);
      for (int i = 0; i < 256; ++i) {
        int idx = n48->keys[i];
        if (!idx) continue;
        res = Recursive_iter(n48->children[idx - 1].get(), cb, data);
        if (res) return res;
      }
      break;
    }
    case NODE256: {
      auto *n256 = static_cast<Art_node256 *>(node);
      for (int i = 0; i < 256; ++i) {
        if (!n256->children[i]) continue;
        res = Recursive_iter(n256->children[i].get(), cb, data);
        if (res) return res;
      }
      break;
    }
    default:
      assert(false);
  }
  return 0;
}

// Minimum / Maximum  (no per-node locks — tree_mutex shared held by caller)
ART::Art_leaf *ART::Minimum(const Art_node *n) {
  if (!n) return nullptr;
  if (is_leaf(n)) return const_cast<Art_leaf *>(to_leaf(n));

  const Art_node *child = nullptr;
  switch (n->type()) {
    case NODE4:
      child = static_cast<const Art_node4 *>(n)->children[0].get();
      break;
    case NODE16:
      child = static_cast<const Art_node16 *>(n)->children[0].get();
      break;
    case NODE48: {
      auto *p = static_cast<const Art_node48 *>(n);
      for (int i = 0; i < 256; ++i)
        if (p->keys[i]) {
          child = p->children[p->keys[i] - 1].get();
          break;
        }
      break;
    }
    case NODE256: {
      auto *p = static_cast<const Art_node256 *>(n);
      for (int i = 0; i < 256; ++i)
        if (p->children[i]) {
          child = p->children[i].get();
          break;
        }
      break;
    }
    default:
      return nullptr;
  }
  return child ? Minimum(child) : nullptr;
}

ART::Art_leaf *ART::Maximum(const Art_node *n) {
  if (!n) return nullptr;
  if (is_leaf(n)) return const_cast<Art_leaf *>(to_leaf(n));

  const Art_node *child = nullptr;
  switch (n->type()) {
    case NODE4: {
      auto *p = static_cast<const Art_node4 *>(n);
      child = p->children[p->num_children - 1].get();
      break;
    }
    case NODE16: {
      auto *p = static_cast<const Art_node16 *>(n);
      child = p->children[p->num_children - 1].get();
      break;
    }
    case NODE48: {
      auto *p = static_cast<const Art_node48 *>(n);
      for (int i = 255; i >= 0; --i)
        if (p->keys[i]) {
          child = p->children[p->keys[i] - 1].get();
          break;
        }
      break;
    }
    case NODE256: {
      auto *p = static_cast<const Art_node256 *>(n);
      for (int i = 255; i >= 0; --i)
        if (p->children[i]) {
          child = p->children[i].get();
          break;
        }
      break;
    }
    default:
      return nullptr;
  }
  return child ? Maximum(child) : nullptr;
}

// Add_child*  — no per-node locks needed (caller holds tree exclusive)
ART::ArtNodePtr ART::Add_child256(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NODE256);
  auto *n = static_cast<Art_node256 *>(old_node.get());
  n->children[c] = child;
  n->num_children++;
  return old_node;
}

ART::ArtNodePtr ART::Add_child48(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NODE48);
  auto *n = static_cast<Art_node48 *>(old_node.get());

  if (n->num_children < 48) {
    int pos = 0;
    while (pos < 48 && n->children[pos]) pos++;
    n->children[pos] = child;
    n->keys[c] = static_cast<unsigned char>(pos + 1);
    n->num_children++;
    return old_node;
  }

  // Upgrade to NODE256
  auto node256 = make_art_node<Art_node256>();
  if (!node256) return old_node;
  Copy_header(node256.get(), n);
  for (int i = 0; i < 256; ++i)
    if (n->keys[i]) node256->children[i] = n->children[n->keys[i] - 1];
  node256->num_children = n->num_children;
  return Add_child256(node256, c, child);
}

ART::ArtNodePtr ART::Add_child16(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NODE16);
  auto *n = static_cast<Art_node16 *>(old_node.get());

  if (n->num_children < 16) {
    int idx = 0;
    for (; idx < n->num_children; ++idx)
      if (c < n->keys[idx]) break;

    std::memmove(n->keys + idx + 1, n->keys + idx, n->num_children - idx);
    for (int i = n->num_children; i > idx; --i) n->children[i] = n->children[i - 1];

    n->keys[idx] = c;
    n->children[idx] = child;
    n->num_children++;
    return old_node;
  }

  // Upgrade to NODE48
  auto node48 = make_art_node<Art_node48>();
  if (!node48) return old_node;
  Copy_header(node48.get(), n);
  for (int i = 0; i < n->num_children; ++i) {
    node48->children[i] = n->children[i];
    node48->keys[n->keys[i]] = static_cast<unsigned char>(i + 1);
  }
  node48->num_children = n->num_children;
  return Add_child48(node48, c, child);
}

ART::ArtNodePtr ART::Add_child4(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NODE4);
  auto *n = static_cast<Art_node4 *>(old_node.get());

  if (n->num_children < 4) {
    int idx = 0;
    for (; idx < n->num_children; ++idx)
      if (c < n->keys[idx]) break;

    std::memmove(n->keys + idx + 1, n->keys + idx, n->num_children - idx);
    for (int i = n->num_children; i > idx; --i) n->children[i] = n->children[i - 1];

    n->keys[idx] = c;
    n->children[idx] = child;
    n->num_children++;
    return old_node;
  }

  // Upgrade to NODE16
  auto node16 = make_art_node<Art_node16>();
  if (!node16) return old_node;
  Copy_header(node16.get(), n);
  for (int i = 0; i < 4; ++i) {
    node16->children[i] = n->children[i];
    node16->keys[i] = n->keys[i];
  }
  node16->num_children = n->num_children;
  return Add_child16(node16, c, child);
}

void ART::Add_child(ArtNodePtr &new_node, ArtNodePtr &old_node, unsigned char c, const ArtNodePtr &child) {
  if (!old_node) return;
  assert(!is_leaf(old_node.get()));
  switch (old_node->type()) {
    case NODE4:
      new_node = Add_child4(old_node, c, child);
      break;
    case NODE16:
      new_node = Add_child16(old_node, c, child);
      break;
    case NODE48:
      new_node = Add_child48(old_node, c, child);
      break;
    case NODE256:
      new_node = Add_child256(old_node, c, child);
      break;
    default:
      std::abort();
  }
}

void ART::Remove_child256(ArtNodePtr &node, unsigned char c) {
  assert(node->type() == NODE256);
  auto *n = static_cast<Art_node256 *>(node.get());

  if (!n->children[c]) return;
  n->children[c] = nullptr;
  n->num_children--;

  if (n->num_children == 37) {
    auto new_node = make_art_node<Art_node48>();
    if (!new_node) return;
    auto *n48 = new_node.get();
    int pos = 0;
    for (int i = 0; i < 256; ++i) {
      if (n->children[i]) {
        n48->children[pos] = n->children[i];
        n48->keys[i] = static_cast<unsigned char>(pos + 1);
        pos++;
      }
    }
    n48->num_children = pos;
    Copy_header(new_node.get(), n);
    node = std::move(new_node);
  }
}

void ART::Remove_child48(ArtNodePtr &node, unsigned char c) {
  assert(node->type() == NODE48);
  auto *n = static_cast<Art_node48 *>(node.get());

  unsigned char pos = n->keys[c];
  if (!pos) return;
  n->children[pos - 1] = nullptr;
  n->keys[c] = 0;
  n->num_children--;

  if (n->num_children == 12) {
    auto new_node = make_art_node<Art_node16>();
    if (!new_node) return;
    Copy_header(new_node.get(), n);
    auto *n16 = new_node.get();
    int slot = 0;
    for (int i = 0; i < 256 && slot < 16; ++i) {
      if (n->keys[i]) {
        n16->keys[slot] = static_cast<unsigned char>(i);
        n16->children[slot] = n->children[n->keys[i] - 1];
        slot++;
      }
    }
    // Without this the replacement reports zero children and every later
    // Find_child on it fails, silently orphaning the whole subtree.
    n16->num_children = static_cast<uint8_t>(slot);
    node = std::move(new_node);
  }
}

void ART::Remove_child16(ArtNodePtr &node, const ArtNodePtr &child) {
  assert(node->type() == NODE16);
  auto *n = static_cast<Art_node16 *>(node.get());

  int idx = 0;
  for (; idx < n->num_children; ++idx)
    if (n->children[idx] == child) break;

  if (idx < n->num_children) {
    std::memmove(n->keys + idx, n->keys + idx + 1, n->num_children - idx - 1);
    for (int i = idx; i < n->num_children - 1; ++i) n->children[i] = n->children[i + 1];
    n->children[n->num_children - 1] = nullptr;
    n->num_children--;
  }

  if (n->num_children == 3) {
    auto new_node = make_art_node<Art_node4>();
    if (!new_node) return;
    Copy_header(new_node.get(), n);
    auto *n4 = new_node.get();
    for (int i = 0; i < n->num_children; ++i) {
      n4->keys[i] = n->keys[i];
      n4->children[i] = n->children[i];
    }
    n4->num_children = n->num_children;
    node = std::move(new_node);
  }
}

void ART::Remove_child4(ArtNodePtr &node, const ArtNodePtr &child) {
  assert(node->type() == NODE4);
  auto *n = static_cast<Art_node4 *>(node.get());

  int idx = 0;
  for (; idx < n->num_children; ++idx)
    if (n->children[idx] == child) break;

  if (idx < n->num_children) {
    std::memmove(n->keys + idx, n->keys + idx + 1, n->num_children - idx - 1);
    for (int i = idx; i < n->num_children - 1; ++i) n->children[i] = n->children[i + 1];
    n->children[n->num_children - 1] = nullptr;
    n->num_children--;
  }

  if (n->num_children == 1) {
    ArtNodePtr remaining = n->children[0];
    if (Art_inner_node *child = to_inner(remaining.get())) {
      // Build combined prefix: parent_partial + edge_byte + child_partial.
      // partial[] only materialises the first MAX_PREFIX_LEN bytes of it, but
      // partial_len must record the *true* combined length -- clamping it to
      // MAX_PREFIX_LEN, which is what this did, shortens the compressed path
      // and makes every key below the collapsed node unreachable once the
      // prefixes involved are long enough to overflow the buffer.
      const uint32_t merged_len = n->partial_len + 1 + child->partial_len;

      unsigned char buf[MAX_PREFIX_LEN];
      uint32_t filled = std::min(n->partial_len, MAX_PREFIX_LEN);
      std::memcpy(buf, n->partial, filled);

      if (n->partial_len < MAX_PREFIX_LEN) {
        buf[filled++] = n->keys[0];  // the single remaining edge byte
        if (filled < MAX_PREFIX_LEN) {
          const uint32_t sub = std::min(child->partial_len, MAX_PREFIX_LEN - filled);
          std::memcpy(buf + filled, child->partial, sub);
          filled += sub;
        }
      }

      std::memcpy(child->partial, buf, filled);
      child->partial_len = merged_len;

      // do NOT call Copy_header here — it would silently overwrite
      // remaining->partial and remaining->partial_len with the shorter parent
      // values, discarding the combined prefix we just computed.
    }
    node = std::move(remaining);
  }
}

void ART::Remove_child(ArtNodePtr &node, unsigned char c, const ArtNodePtr &child) {
  if (!node) return;
  switch (node->type()) {
    case NODE4:
      Remove_child4(node, child);
      break;
    case NODE16:
      Remove_child16(node, child);
      break;
    case NODE48:
      Remove_child48(node, c);
      break;
    case NODE256:
      Remove_child256(node, c);
      break;
    default:
      std::abort();
  }
}

int ART::Check_prefix(const Art_inner_node *n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = static_cast<int>(std::min(n->partial_len, MAX_PREFIX_LEN));
  int max_cmp = std::min(min_tmp, key_len - depth);
  for (int i = 0; i < max_cmp; ++i)
    if (n->partial[i] != key[depth + i]) return i;
  return max_cmp;
}

int ART::Prefix_mismatch(const Art_inner_node *n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = static_cast<int>(std::min(n->partial_len, MAX_PREFIX_LEN));
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx = 0;

  for (; idx < max_cmp; ++idx)
    if (n->partial[idx] != key[depth + idx]) return idx;

  if (n->partial_len > MAX_PREFIX_LEN) {
    // Minimum() walks children, so it needs the shared_ptr-shaped node; the
    // caller always holds one, and Find_child never hands out a raw parent.
    Art_leaf *l = Minimum(n);
    if (!l) return idx;
    int mkcmp = std::min(static_cast<int>(l->key_length()), key_len) - depth;
    for (; idx < mkcmp; ++idx)
      if (l->key()[idx + depth] != key[depth + idx]) return idx;
  }
  return idx;
}

int ART::Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth) {
  int max_cmp = static_cast<int>(std::min(l1->key_length(), l2->key_length())) - depth;
  for (int i = 0; i < max_cmp; ++i)
    if (l1->key()[depth + i] != l2->key()[depth + i]) return i;
  return max_cmp;
}

int ART::Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int /*depth*/) {
  if (n->key_length() != static_cast<uint32_t>(key_len)) return 1;
  return std::memcmp(n->key(), key, key_len);
}

int ART::Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth) {
  int max_cmp = static_cast<int>(std::min(n->key_length(), static_cast<uint32_t>(key_len))) - depth;
  if (max_cmp < 0) return 1;
  return std::memcmp(n->key() + depth, key + depth, max_cmp);
}

void ART::Copy_header(Art_inner_node *dest, const Art_inner_node *src) {
  dest->partial_len = src->partial_len;
  std::memcpy(dest->partial, src->partial, std::min(MAX_PREFIX_LEN, src->partial_len));
}
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase