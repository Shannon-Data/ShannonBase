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
ART::ArtNodePtr ART::null_ptr = nullptr;
/*
 * ART_insert
 *
 * Concurrency model: tree_mutex is acquired exclusively for the whole call.
 * This serialises all concurrent writers and blocks readers for the duration.
 *
 * Upgrade path (future work): Replace with Optimistic Lock Coupling (OLC).
 * Each Art_node carries a version counter; readers optimistically read then
 * validate; writers lock only the target node.  The node_mutex fields kept in
 * Art_node are the scaffold for that migration.
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

  m_tree->size.fetch_sub(1, std::memory_order_acq_rel);
  return result;
}

void *ART::ART_search(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root;
  if (!n) return nullptr;

  int depth = 0;

  while (n) {
    if (is_leaf(n.get())) {
      auto leaf = to_leaf(n.get());
      // leaf_mutex shared: guard against a concurrent add_value expansion
      std::shared_lock lk(leaf->leaf_mutex);
      if (!Leaf_matches(leaf, key, key_len, depth)) {
        if (!leaf->values.empty()) return static_cast<void *>(const_cast<uint8_t *>(leaf->values[0].data()));
      }
      return nullptr;
    }

    if (n->partial_len) {
      int prefix_len = Check_prefix(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_len) != std::min(MAX_PREFIX_LEN, n->partial_len)) return nullptr;
      depth += n->partial_len;
    }

    if (depth >= key_len) return nullptr;

    ArtNodePtr child = Find_child(n, key[depth]);
    if (!child) return nullptr;

    n = child;
    depth++;
  }
  return nullptr;
}

std::vector<std::vector<uint8_t>> ART::ART_search_all(const unsigned char *key, int key_len) {
  std::vector<std::vector<uint8_t>> results;
  if (!key || key_len <= 0 || !m_inited) return results;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root;
  if (!n) return results;

  int depth = 0;

  while (n) {
    if (is_leaf(n.get())) {
      auto l = to_leaf(n.get());
      std::shared_lock lk(l->leaf_mutex);
      if (!Leaf_matches(l, key, key_len, depth)) results = l->values;
      return results;
    }

    if (n->partial_len) {
      int prefix_len = Check_prefix(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_len) != std::min(MAX_PREFIX_LEN, n->partial_len)) return results;
      depth += n->partial_len;
    }

    if (depth >= key_len) return results;

    ArtNodePtr child = Find_child(n, key[depth]);
    if (!child) return results;

    n = child;
    depth++;
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
  auto root_ptr = m_tree->root;
  return Minimum(root_ptr);
}

ART::Art_leaf *ART::ART_maximum() {
  if (!m_inited) return nullptr;
  std::shared_lock lk(m_tree->tree_mutex);
  auto root_ptr = m_tree->root;
  return Maximum(root_ptr);
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
    auto leaf = static_cast<Art_leaf *>(node.get());

    // leaf_mutex unique: we are about to mutate values.
    std::unique_lock<std::shared_mutex> leaf_lock(leaf->leaf_mutex);

    if (!Leaf_matches(leaf, key, key_len, depth)) {
      // Same key — append or replace value.
      if (replace && !leaf->values.empty()) {
        std::vector<uint8_t> new_val(static_cast<uint8_t *>(value), static_cast<uint8_t *>(value) + value_len);
        leaf->values[0] = std::move(new_val);
        *old = 1;
        return leaf->values[0].data();
      }
      // Multi-value append (no lock call — we already hold leaf_lock).
      leaf->values.emplace_back(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + value_len);
      *old = 0;
      return nullptr;
    }

    // Different key — split into a Node4.
    leaf_lock.unlock();  // finished touching the old leaf

    ArtNodePtr new_node = make_art_node<Art_node4>();
    if (!new_node) return nullptr;

    auto l2 = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!l2) return nullptr;

    int longest_prefix = Longest_common_prefix(leaf, l2.get(), depth);
    new_node->partial_len = longest_prefix;
    std::memcpy(new_node->partial, key + depth, std::min(static_cast<int>(MAX_PREFIX_LEN), longest_prefix));

    new_node = Add_child4(new_node, leaf->key[depth + longest_prefix], node);
    new_node = Add_child4(new_node, l2->key[depth + longest_prefix], l2);
    node = new_node;
    *old = 0;
    return nullptr;
  }

  // 3. Internal node
  if (node->partial_len) {
    int prefix_diff = Prefix_mismatch(node, key, key_len, depth);

    if (static_cast<uint32_t>(prefix_diff) < node->partial_len) {
      // Prefix mismatch — split this node.
      ArtNodePtr new_node = make_art_node<Art_node4>();
      if (!new_node) return nullptr;

      new_node->partial_len = prefix_diff;
      std::memcpy(new_node->partial, node->partial, prefix_diff);

      if (node->partial_len <= MAX_PREFIX_LEN) {
        new_node = Add_child4(new_node, node->partial[prefix_diff], node);
        node->partial_len -= (prefix_diff + 1);
        int copy_len = std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(node->partial_len));
        std::memmove(node->partial, node->partial + prefix_diff + 1, copy_len);
      } else {
        node->partial_len -= (prefix_diff + 1);
        Art_leaf *leaf = Minimum(node);
        new_node = Add_child4(new_node, leaf->key[depth + prefix_diff], node);
        int copy_len = std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(node->partial_len));
        std::memcpy(node->partial, leaf->key.data() + depth + prefix_diff + 1, copy_len);
      }

      auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
      if (!new_leaf) return nullptr;

      new_node = Add_child4(new_node, key[depth + prefix_diff], new_leaf);
      node = new_node;
      *old = 0;
      return nullptr;
    }

    depth += node->partial_len;
  }

  if (depth >= key_len) return nullptr;

  // Find_child returns by value: no dangling reference after tree
  // modifications.  We retrieve the slot reference for in-place update.
  ArtNodePtr child = Find_child(node, key[depth]);
  if (!child) {
    auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!new_leaf) return nullptr;
    Add_child(node, node, key[depth], new_leaf);
    *old = 0;
    return nullptr;
  }

  // Recurse: we need to pass the actual slot reference so the caller can
  // replace it (e.g. leaf → Node4).  Re-obtain it via a helper that returns
  // a reference — safe because tree_mutex exclusive is still held.
  ArtNodePtr &child_ref = [&]() -> ArtNodePtr & {
    switch (node->type()) {
      case NODE4: {
        auto *n4 = static_cast<Art_node4 *>(node.get());
        for (int i = 0; i < n4->num_children; ++i)
          if (n4->keys[i] == key[depth]) return n4->children[i];
        break;
      }
      case NODE16: {
        auto *n16 = static_cast<Art_node16 *>(node.get());
        for (int i = 0; i < n16->num_children; ++i)
          if (n16->keys[i] == key[depth]) return n16->children[i];
        break;
      }
      case NODE48: {
        auto *n48 = static_cast<Art_node48 *>(node.get());
        int idx = n48->keys[key[depth]];
        if (idx) return n48->children[idx - 1];
        break;
      }
      case NODE256: {
        auto *n256 = static_cast<Art_node256 *>(node.get());
        return n256->children[key[depth]];
      }
      default:
        break;
    }
    return null_ptr;
  }();

  return Recursive_insert(child_ref, key, key_len, value, value_len, depth + 1, old, replace);
}

ART::ArtNodePtr ART::Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth,
                                      void *&result) {
  if (!node) return nullptr;

  if (is_leaf(node.get())) {
    auto leaf = const_cast<Art_leaf *>(to_leaf(node.get()));
    bool match = false;
    {
      std::shared_lock lk(leaf->leaf_mutex);
      match = !Leaf_matches(leaf, key, key_len, depth);
    }
    if (match) {
      std::unique_lock lk(leaf->leaf_mutex);
      if (!leaf->values.empty()) {
        result = static_cast<void *>(leaf->values[0].data());
        leaf->values.clear();
        leaf->key.clear();
      }
      return node;
    }
    return nullptr;
  }

  // No per-node lock — tree_mutex exclusive covers us.
  if (node->partial_len) {
    int prefix_len = Check_prefix(node, key, key_len, depth);
    if (prefix_len != std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(node->partial_len))) return nullptr;
    depth += node->partial_len;
  }

  if (depth >= key_len) return nullptr;

  // Find_child returns by value.
  ArtNodePtr child = Find_child(node, key[depth]);
  if (!child) return nullptr;

  ArtNodePtr res = Recursive_delete(child, key, key_len, depth + 1, result);
  if (res) {
    // Pass the same shared_ptr copy to Remove_child; pointer equality holds.
    Remove_child(node, key[depth], child);
  }
  return res;
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
    std::shared_lock lk(leaf->leaf_mutex);
    for (size_t vi = 0; vi < leaf->values.size(); ++vi) {
      int r = cb(data, static_cast<const void *>(leaf->key.data()), static_cast<uint32_t>(leaf->key.size()),
                 static_cast<const void *>(leaf->values[vi].data()), static_cast<uint32_t>(leaf->values[vi].size()));
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
ART::Art_leaf *ART::Minimum(const ArtNodePtr &n) {
  if (!n) return nullptr;
  if (is_leaf(n.get())) return const_cast<Art_leaf *>(to_leaf(n.get()));

  ArtNodePtr child;
  switch (n->type()) {
    case NODE4:
      child = static_cast<Art_node4 *>(n.get())->children[0];
      break;
    case NODE16:
      child = static_cast<Art_node16 *>(n.get())->children[0];
      break;
    case NODE48: {
      auto *p = static_cast<Art_node48 *>(n.get());
      for (int i = 0; i < 256; ++i)
        if (p->keys[i]) {
          child = p->children[p->keys[i] - 1];
          break;
        }
      break;
    }
    case NODE256: {
      auto *p = static_cast<Art_node256 *>(n.get());
      for (int i = 0; i < 256; ++i)
        if (p->children[i]) {
          child = p->children[i];
          break;
        }
      break;
    }
    default:
      return nullptr;
  }
  return child ? Minimum(child) : nullptr;
}

ART::Art_leaf *ART::Maximum(const ArtNodePtr &n) {
  if (!n) return nullptr;
  if (is_leaf(n.get())) return const_cast<Art_leaf *>(to_leaf(n.get()));

  ArtNodePtr child;
  switch (n->type()) {
    case NODE4: {
      auto *p = static_cast<Art_node4 *>(n.get());
      child = p->children[n->num_children - 1];
      break;
    }
    case NODE16: {
      auto *p = static_cast<Art_node16 *>(n.get());
      child = p->children[n->num_children - 1];
      break;
    }
    case NODE48: {
      auto *p = static_cast<Art_node48 *>(n.get());
      for (int i = 255; i >= 0; --i)
        if (p->keys[i]) {
          child = p->children[p->keys[i] - 1];
          break;
        }
      break;
    }
    case NODE256: {
      auto *p = static_cast<Art_node256 *>(n.get());
      for (int i = 255; i >= 0; --i)
        if (p->children[i]) {
          child = p->children[i];
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
    auto *n48 = static_cast<Art_node48 *>(new_node.get());
    int pos = 0;
    for (int i = 0; i < 256; ++i) {
      if (n->children[i]) {
        n48->children[pos] = n->children[i];
        n48->keys[i] = static_cast<unsigned char>(pos + 1);
        pos++;
      }
    }
    n48->num_children = pos;
    Copy_header(new_node.get(), node.get());
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
    Copy_header(new_node.get(), node.get());
    auto *n16 = static_cast<Art_node16 *>(new_node.get());
    int slot = 0;
    for (int i = 0; i < 256 && slot < 16; ++i) {
      if (n->keys[i]) {
        n16->keys[slot] = static_cast<unsigned char>(i);
        n16->children[slot] = n->children[n->keys[i] - 1];
        slot++;
      }
    }
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
    Copy_header(new_node.get(), node.get());
    auto *n4 = static_cast<Art_node4 *>(new_node.get());
    for (int i = 0; i < n->num_children; ++i) {
      n4->keys[i] = n->keys[i];
      n4->children[i] = n->children[i];
    }
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
    if (!is_leaf(remaining.get())) {
      // Build combined prefix: parent_partial + edge_byte + child_partial
      uint32_t prefix = node->partial_len;
      unsigned char buf[MAX_PREFIX_LEN];

      uint32_t copy_len = std::min(prefix, MAX_PREFIX_LEN);
      std::memcpy(buf, node->partial, copy_len);

      if (prefix < MAX_PREFIX_LEN) {
        buf[prefix++] = n->keys[0];  // the single remaining edge byte
      }

      if (prefix < MAX_PREFIX_LEN) {
        uint32_t sub = std::min(remaining->partial_len, MAX_PREFIX_LEN - prefix);
        std::memcpy(buf + prefix, remaining->partial, sub);
        prefix += sub;
      }

      uint32_t final_len = std::min(prefix, MAX_PREFIX_LEN);
      std::memcpy(remaining->partial, buf, final_len);
      remaining->partial_len = final_len;

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

int ART::Check_prefix(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = static_cast<int>(std::min(n->partial_len, MAX_PREFIX_LEN));
  int max_cmp = std::min(min_tmp, key_len - depth);
  for (int i = 0; i < max_cmp; ++i)
    if (n->partial[i] != key[depth + i]) return i;
  return max_cmp;
}

int ART::Prefix_mismatch(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = static_cast<int>(std::min(n->partial_len, MAX_PREFIX_LEN));
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx = 0;

  for (; idx < max_cmp; ++idx)
    if (n->partial[idx] != key[depth + idx]) return idx;

  if (n->partial_len > MAX_PREFIX_LEN) {
    Art_leaf *l = Minimum(n);
    int mkcmp = std::min(static_cast<int>(l->key.size()), key_len) - depth;
    for (; idx < mkcmp; ++idx)
      if (l->key[idx + depth] != key[depth + idx]) return idx;
  }
  return idx;
}

int ART::Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth) {
  int max_cmp = static_cast<int>(std::min(l1->key.size(), l2->key.size())) - depth;
  for (int i = 0; i < max_cmp; ++i)
    if (l1->key[depth + i] != l2->key[depth + i]) return i;
  return max_cmp;
}

int ART::Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int /*depth*/) {
  if (n->key.size() != static_cast<uint32_t>(key_len)) return 1;
  return std::memcmp(n->key.data(), key, key_len);
}

int ART::Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth) {
  int max_cmp =
      static_cast<int>(std::min(static_cast<uint32_t>(n->key.size()), static_cast<uint32_t>(key_len))) - depth;
  if (max_cmp < 0) return 1;
  return std::memcmp(n->key.data() + depth, key + depth, max_cmp);
}

void ART::Copy_header(Art_node *dest, const Art_node *src) {
  dest->partial_len = src->partial_len;
  std::memcpy(dest->partial, src->partial, std::min(MAX_PREFIX_LEN, src->partial_len));
}
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase