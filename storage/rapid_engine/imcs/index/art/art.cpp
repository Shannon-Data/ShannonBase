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

void *ART::ART_insert(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  int old_val = 0;
  ArtNodePtr &root_ref = m_tree->root;
  void *old = Recursive_insert(root_ref, key, key_len, value, value_len, 0, &old_val, 0);

  if (!old_val) {
    m_tree->size.fetch_add(1, std::memory_order_acq_rel);
  }

  return old;
}

void *ART::Recursive_insert(ArtNodePtr &node, const unsigned char *key, int key_len, void *value, uint32_t value_len,
                            int depth, int *old, int replace) {
  if (!node) {
    node = make_art_node<Art_leaf>(key, key_len, value, value_len);
    *old = 0;
    return nullptr;
  }

  if (is_leaf(node.get())) {
    auto leaf = static_cast<Art_leaf *>(node.get());
    std::unique_lock<std::shared_mutex> leaf_lock(leaf->leaf_mutex);

    if (!Leaf_matches(leaf, key, key_len, depth)) {  // same key, multi-value. `Leaf_matches`: returns 0 means key same.
      if (replace && leaf->values.size()) {          // replace the old one.
        // create a  new value vector.
        std::vector<uint8_t> new_value(static_cast<uint8_t *>(value), static_cast<uint8_t *>(value) + value_len);
        leaf->values[0] = std::move(new_value);
        void *old_value = leaf->values[0].data();  // here, we use the new one as the old one.
        *old = 1;
        return old_value;
      }

      leaf->add_value(value, value_len);
      *old = 0;
      return nullptr;
    }

    // there's no key already existed[key not same]. then, New value, we must split the leaf into a node4
    ArtNodePtr new_node = make_art_node<Art_node4>();
    if (!new_node) return nullptr;

    auto l2 = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!l2) return nullptr;

    int longest_prefix = Longest_common_prefix(leaf, l2.get(), depth);
    new_node->partial_len = longest_prefix;
    int min_len = std::min(static_cast<int>(MAX_PREFIX_LEN), longest_prefix);
    std::memcpy(new_node->partial, key + depth, min_len);  // prefix compress.

    new_node = Add_child4(new_node, leaf->key[depth + longest_prefix], node);
    new_node = Add_child4(new_node, l2->key[depth + longest_prefix], l2);
    node = new_node;
    *old = 0;
    return nullptr;
  }

  {
    std::unique_lock<std::shared_mutex> node_lock(node->node_mutex);

    if (node->partial_len) {  // Check if given node has a prefix
      int prefix_diff = Prefix_mismatch(node, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_diff) < node->partial_len) {  // belongs to different branch.
        // Create a new node
        ArtNodePtr new_node = make_art_node<Art_node4>();
        if (!new_node) return nullptr;

        new_node->partial_len = prefix_diff;
        std::memcpy(new_node->partial, node->partial, prefix_diff);

        // Adjust the prefix of the old node
        if (node->partial_len <= ART::MAX_PREFIX_LEN) {
          new_node = Add_child4(new_node, node->partial[prefix_diff], node);

          node->partial_len -= (prefix_diff + 1);
          int min_len = std::min((int)ART::MAX_PREFIX_LEN, (int)node->partial_len);
          memmove(node->partial, node->partial + prefix_diff + 1, min_len);
        } else {
          node->partial_len -= (prefix_diff + 1);
          Art_leaf *leaf = Minimum(node);

          new_node = Add_child4(new_node, leaf->key[depth + prefix_diff], node);

          int min_len = std::min((int)ART::MAX_PREFIX_LEN, (int)node->partial_len);
          memcpy(node->partial, leaf->key.data() + depth + prefix_diff + 1, min_len);
        }

        // Insert the new leaf
        auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
        if (!new_leaf) return nullptr;

        new_node = Add_child4(new_node, key[depth + prefix_diff], new_leaf);
        node = new_node;  // set to new node.

        *old = 0;
        return nullptr;
      }

      depth += node->partial_len;
    }
  }

  if (depth >= key_len) return nullptr;

  // Here, should be reference, because in below `Recursive_insert`, will change it from leave node to `NodeXXX`.
  // the structure of the ART tree will be moidified. Otherwise, it the wrong results will be.
  auto &child = Find_child(node, key[depth]);
  if (!child) {
    auto new_leaf = make_art_node<Art_leaf>(key, key_len, value, value_len);
    if (!new_leaf) return nullptr;
    Add_child(node, node, key[depth], new_leaf);
    *old = 0;
    return nullptr;
  }

  return Recursive_insert(child, key, key_len, value, value_len, depth + 1, old, replace);
}

void *ART::ART_delete(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  void *result = nullptr;
  auto leaf = Recursive_delete(m_tree->root, key, key_len, 0, result);
  if (!leaf) return nullptr;

  m_tree->size.fetch_sub(1, std::memory_order_acq_rel);
  return result;
}

ART::ArtNodePtr ART::Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth,
                                      void *&result) {
  if (!node) return nullptr;

  if (is_leaf(node.get())) {
    auto leaf = const_cast<ART::Art_leaf *>(to_leaf(node.get()));
    bool match = false;
    {
      std::shared_lock<std::shared_mutex> leaf_lock(leaf->leaf_mutex);
      match = !Leaf_matches(leaf, key, key_len, depth);
    }

    if (match) {
      std::unique_lock<std::shared_mutex> leaf_lock(leaf->leaf_mutex);
      if (!leaf->values.empty()) {
        result = (void *)leaf->values[0].data();
        leaf->values.clear();
        leaf->key.clear();
        leaf->vcount.store(0, std::memory_order_release);
      }
      return node;
    }

    return nullptr;
  }

  ArtNodePtr child = nullptr;
  {
    std::shared_lock<std::shared_mutex> node_lock(node->node_mutex);

    if (node->partial_len) {
      int prefix_len = Check_prefix(node, key, key_len, depth);
      if (prefix_len != std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(node->partial_len))) {
        return nullptr;
      }
      depth += node->partial_len;
    }

    if (depth >= key_len) {
      return nullptr;
    }

    child = Find_child(node, key[depth]);
    if (!child) return nullptr;
  }

  auto res = Recursive_delete(child, key, key_len, depth + 1, result);
  if (res) {
    std::unique_lock<std::shared_mutex> parent_lock(node->node_mutex);
    Remove_child(node, key[depth], child);
  }

  return res;
}

void *ART::ART_search(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::shared_lock<std::shared_mutex> tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root;
  if (!n) return nullptr;

  int prefix_len, depth = 0;

  while (n) {
    if (is_leaf(n.get())) {
      auto leaf = to_leaf(n.get());
      std::shared_lock<std::shared_mutex> leaf_lock(leaf->leaf_mutex);

      if (!Leaf_matches(leaf, key, key_len, depth)) {
        if (leaf->vcount.load(std::memory_order_acquire) > 0) {
          return (void *)leaf->values[0].data();
        }
      }
      return nullptr;
    }

    std::shared_lock<std::shared_mutex> node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_len) != std::min(MAX_PREFIX_LEN, n->partial_len)) {
        return nullptr;
      }
      depth += n->partial_len;
    }

    if (depth >= key_len) {
      return nullptr;
    }

    auto child_slot = Find_child(n, key[depth]);
    if (!child_slot) return nullptr;

    n = child_slot;
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

  int prefix_len, depth = 0;

  while (n) {
    if (is_leaf(n.get())) {
      auto l = to_leaf(n.get());
      std::shared_lock<std::shared_mutex> leaf_lock(l->leaf_mutex);

      if (!Leaf_matches(l, key, key_len, depth)) {
        results = l->values;
      }
      return results;
    }

    std::shared_lock<std::shared_mutex> node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_len) != std::min(MAX_PREFIX_LEN, n->partial_len)) {
        return results;
      }
      depth += n->partial_len;
    }

    if (depth >= key_len) {
      return results;
    }

    auto child_slot = Find_child(n, key[depth]);
    if (!child_slot) return results;

    n = child_slot;
    depth++;
  }
  return results;
}

int ART::ART_iter(ART_Func cb, void *data) { return Recursive_iter(m_tree->root.get(), cb, data); }

int ART::Recursive_iter(Art_node *node, ART_Func &cb, void *data) {
  // Handle base cases
  if (!node) return 0;

  if (is_leaf(node)) {
    std::shared_lock lk(node->node_mutex);
    auto leaf = to_leaf(node);
    for (auto vi = 0u; vi < leaf->vcount; vi++) {
      cb(data, (const void *)leaf->key.data(), (uint32_t)leaf->key.size(), (const void *)leaf->values[vi].data(),
         (uint32_t)leaf->values[vi].size());
    }
    return 0;
  }

  int idx, res;
  switch (node->type()) {
    case NodeType::NODE4:
      for (int i = 0; i < node->num_children; i++) {
        auto n4 = static_cast<Art_node4 *>(node)->children[i].get();
        res = Recursive_iter(n4, cb, data);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE16:
      for (int i = 0; i < node->num_children; i++) {
        auto n16 = static_cast<Art_node16 *>(node)->children[i].get();
        res = Recursive_iter(n16, cb, data);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE48:
      for (int i = 0; i < 256; i++) {
        auto n48 = static_cast<Art_node48 *>(node);
        idx = n48->keys[i];
        if (!idx) continue;

        res = Recursive_iter(n48->children[idx - 1].get(), cb, data);
        if (res) {
          return res;
        };
      }
      break;

    case NodeType::NODE256:
      for (int i = 0; i < 256; i++) {
        auto n256 = static_cast<Art_node256 *>(node)->children[i - 1].get();
        if (!n256) continue;

        res = Recursive_iter(n256, cb, data);
        if (res) {
          return res;
        }
      }
      break;
    default:
      assert(false);
  }
  return 0;
}

int ART::Recursive_iter_with_key(const ArtNodePtr &node, const unsigned char *key, int key_len) {
  // Handle base cases
  if (!node) return 0;
  if (is_leaf(node.get())) {
    auto leaf = to_leaf(node.get());
    if (!memcmp((void *)leaf->key.data(), (void *)key, key_len)) {
      // m_current_values.emplace_back(leaf);
    }
    return 0;
  }

  int idx, res;
  switch (node->type()) {
    case NodeType::NODE4:
      for (int i = 0; i < node->num_children; i++) {
        auto n4 = static_cast<Art_node4 *>(node.get())->children[i].get();
        ArtNodePtr n(n4);
        res = Recursive_iter_with_key(n, key, key_len);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE16:
      for (int i = 0; i < node->num_children; i++) {
        auto n16 = static_cast<Art_node16 *>(node.get())->children[i].get();
        ArtNodePtr n(n16);
        res = Recursive_iter_with_key(n, key, key_len);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE48:
      for (int i = 0; i < 256; i++) {
        idx = ((Art_node48 *)node.get())->keys[i];
        if (!idx) continue;

        auto n48 = static_cast<Art_node48 *>(node.get())->children[idx - 1].get();
        ArtNodePtr n(n48);
        res = Recursive_iter_with_key(n, key, key_len);
        if (res) {
          return res;
        };
      }
      break;

    case NodeType::NODE256:
      for (int i = 0; i < 256; i++) {
        auto n256 = static_cast<Art_node256 *>(node.get())->children[i - 1].get();
        if (!n256) continue;

        ArtNodePtr n(n256);
        res = Recursive_iter_with_key(n, key, key_len);
        if (res) {
          return res;
        }
      }
      break;
    default:
      assert(false);
  }

  return 0;
}

int ART::Iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len) {
  return Recursive_iter(m_tree->root.get(), cb, data);
}

ART::ArtNodePtr &ART::Find_child(const ArtNodePtr &n, unsigned char c) {
  if (!n || is_leaf(n.get())) return ART::null_ptr;

  std::shared_lock<std::shared_mutex> lock(n->node_mutex);
  switch (n->type()) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c) return p1->children[i];
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
      for (int i = 0; i < n->num_children; ++i) {
        if (p2->keys[i] == c) return p2->children[i];
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
      int idx = p3->keys[c];
      if (idx) return p3->children[idx - 1];
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
      return p4->children[c];
    }
    default:
      break;
  }
  return ART::null_ptr;
}

void ART::Find_children(const ArtNodePtr &n, unsigned char c, std::vector<ART::ArtNodePtr> &children) {
  if (!n || is_leaf(n.get())) return;

  std::shared_lock<std::shared_mutex> lock(n->node_mutex);
  switch (n->type()) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c && p1->children[i]) {
          children.push_back(p1->children[i]);
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
      for (int i = 0; i < n->num_children; ++i) {
        if (p2->keys[i] == c && p2->children[i]) {
          children.push_back(p2->children[i]);
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
      int idx = p3->keys[c];
      if (idx && p3->children[idx - 1]) {
        children.push_back(p3->children[idx - 1]);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
      if (p4->children[c]) {
        children.push_back(p4->children[c]);
      }
      break;
    }
    default:
      break;
  }
}

int ART::Check_prefix(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = std::min(n->partial_len, MAX_PREFIX_LEN);
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) return idx;
  }
  return idx;
}

int ART::Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int) {
  if (n->key.size() != (uint32)key_len) return 1;

  // Compare the keys starting at the depth
  return std::memcmp(n->key.data(), key, key_len);
}

int ART::Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth) {
  int max_cmp = std::min(static_cast<uint32_t>(n->key.size()), static_cast<uint32_t>(key_len)) - depth;
  if (max_cmp < 0) return 1;
  return std::memcmp(n->key.data() + depth, key + depth, max_cmp);
}

ART::Art_leaf *ART::Minimum(const ArtNodePtr &n) {
  if (!n) return nullptr;
  if (is_leaf(n.get())) {
    return const_cast<ART::Art_leaf *>(to_leaf(n.get()));
  }

  std::shared_lock<std::shared_mutex> lock(n->node_mutex);
  ArtNodePtr child = nullptr;

  switch (n->type()) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(n.get());
      child = p->children[0];
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(n.get());
      child = p->children[0];
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(n.get());
      int idx = 0;
      while (!p->keys[idx]) idx++;
      child = p->children[p->keys[idx] - 1];
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(n.get());
      int idx = 0;
      while (!p->children[idx]) idx++;
      child = p->children[idx];
      break;
    }
    default:
      return nullptr;
  }

  if (child) {
    return Minimum(child);
  }
  return nullptr;
}

ART::Art_leaf *ART::Maximum(const ArtNodePtr &n) {
  if (!n) return nullptr;
  if (is_leaf(n.get())) {
    return const_cast<ART::Art_leaf *>(to_leaf(n.get()));
  }

  std::shared_lock<std::shared_mutex> lock(n->node_mutex);
  ArtNodePtr child = nullptr;

  switch (n->type()) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(n.get());
      child = p->children[n->num_children - 1];
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(n.get());
      child = p->children[n->num_children - 1];
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(n.get());
      int idx = 255;
      while (idx >= 0 && !p->keys[idx]) idx--;
      child = p->children[p->keys[idx] - 1];
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(n.get());
      int idx = 255;
      while (idx >= 0 && !p->children[idx]) idx--;
      child = p->children[idx];
      break;
    }
    default:
      return nullptr;
  }

  if (child) {
    return Maximum(child);
  }
  return nullptr;
}

ART::Art_leaf *ART::ART_minimum() {
  if (!m_inited) return nullptr;
  std::shared_lock lock(m_tree->tree_mutex);
  auto root_ptr = m_tree->root;
  return Minimum(root_ptr);
}

ART::Art_leaf *ART::ART_maximum() {
  if (!m_inited) return nullptr;
  std::shared_lock lock(m_tree->tree_mutex);
  auto root_ptr = m_tree->root;
  return Maximum(root_ptr);
}

int ART::Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth) {
  int max_cmp = std::min(l1->key.size(), l2->key.size()) - depth;
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (l1->key[depth + idx] != l2->key[depth + idx]) return idx;
  }
  return idx;
}

void ART::Copy_header(Art_node *dest, const Art_node *src) {
  dest->partial_len = src->partial_len;
  std::memcpy(dest->partial, src->partial, std::min(MAX_PREFIX_LEN, src->partial_len));
}

ART::ArtNodePtr ART::Add_child256(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NodeType::NODE256);

  std::unique_lock<std::shared_mutex> lock(old_node->node_mutex);
  auto n256_ptr = static_cast<Art_node256 *>(old_node.get());
  if (!n256_ptr) return old_node;

  n256_ptr->num_children++;
  n256_ptr->children[c] = child;

  return old_node;
}

ART::ArtNodePtr ART::Add_child48(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NodeType::NODE48);

  std::unique_lock<std::shared_mutex> lock(old_node->node_mutex);
  auto n48_ptr = static_cast<Art_node48 *>(old_node.get());

  if (n48_ptr->num_children < 48) {
    int pos = 0;
    while (pos < 48 && n48_ptr->children[pos]) pos++;

    n48_ptr->children[pos] = child;
    n48_ptr->keys[c] = pos + 1;
    n48_ptr->num_children++;

    return old_node;
  } else {
    // Upgrade to NODE256
    auto node256 = make_art_node<Art_node256>();
    if (!node256) return old_node;

    Copy_header(node256.get(), n48_ptr);

    for (int i = 0; i < 256; i++) {
      if (n48_ptr->keys[i]) node256->children[i] = n48_ptr->children[n48_ptr->keys[i] - 1];
    }
    node256->num_children = n48_ptr->num_children;
    lock.unlock();

    return Add_child256(node256, c, child);
  }
}

ART::ArtNodePtr ART::Add_child16(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NodeType::NODE16);

  std::unique_lock<std::shared_mutex> lock(old_node->node_mutex);
  auto n16_ptr = static_cast<Art_node16 *>(old_node.get());
  if (n16_ptr->num_children < 16) {
    int idx = 0;
    for (; idx < n16_ptr->num_children; idx++) {
      if (c < n16_ptr->keys[idx]) break;
    }

    if (idx < n16_ptr->num_children) {
      std::memmove(n16_ptr->keys + idx + 1, n16_ptr->keys + idx, n16_ptr->num_children - idx);
      for (int i = n16_ptr->num_children; i > idx; i--) {
        n16_ptr->children[i] = n16_ptr->children[i - 1];
      }
    }

    n16_ptr->keys[idx] = c;
    n16_ptr->children[idx] = child;
    n16_ptr->num_children++;

    return old_node;
  } else {
    // Upgrade to NODE48
    auto node48 = make_art_node<Art_node48>();
    if (!node48) return old_node;

    Copy_header(node48.get(), n16_ptr);

    for (int i = 0; i < n16_ptr->num_children; i++) {
      node48->children[i] = n16_ptr->children[i];
      node48->keys[n16_ptr->keys[i]] = static_cast<unsigned char>(i + 1);
    }
    node48->num_children = n16_ptr->num_children;
    lock.unlock();

    return Add_child48(node48, c, child);
  }
}

ART::ArtNodePtr ART::Add_child4(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child) {
  assert(old_node->type() == NodeType::NODE4);

  std::unique_lock<std::shared_mutex> lock(old_node->node_mutex);
  auto n4_ptr = static_cast<Art_node4 *>(old_node.get());
  if (old_node->num_children < 4) {
    int idx = 0;
    for (; idx < n4_ptr->num_children; idx++) {
      if (c < n4_ptr->keys[idx]) break;
    }

    // make a room for new child.
    std::memmove(n4_ptr->keys + idx + 1, n4_ptr->keys + idx, n4_ptr->num_children - idx);
    n4_ptr->keys[idx] = c;

    for (int i = n4_ptr->num_children; i > idx; i--) {
      n4_ptr->children[i] = n4_ptr->children[i - 1];
    }
    n4_ptr->children[idx] = child;
    n4_ptr->num_children++;

    return old_node;
  } else {
    // Upgrade to NODE16
    auto node16 = make_art_node<Art_node16>();
    if (!node16) return old_node;

    Copy_header(node16.get(), n4_ptr);

    for (int i = 0; i < n4_ptr->num_children; i++) {
      node16->children[i] = n4_ptr->children[i];
      node16->keys[i] = n4_ptr->keys[i];
    }
    node16->num_children = n4_ptr->num_children;
    lock.unlock();

    return Add_child16(node16, c, child);
  }
}

void ART::Add_child(ArtNodePtr &new_node, ArtNodePtr &old_node, unsigned char c, const ArtNodePtr &child) {
  if (!old_node.get()) return;
  assert(!is_leaf(old_node.get()));

  switch (old_node->type()) {
    case NodeType::NODE4:
      new_node = Add_child4(old_node, c, child);
      break;
    case NodeType::NODE16:
      new_node = Add_child16(old_node, c, child);
      break;
    case NodeType::NODE48:
      new_node = Add_child48(old_node, c, child);
      break;
    case NodeType::NODE256:
      new_node = Add_child256(old_node, c, child);
      break;
    default:
      std::abort();
  }
}

void ART::Remove_child256(ArtNodePtr &node, unsigned char c) {
  assert(node->type() == NodeType::NODE256);
  std::unique_lock<std::shared_mutex> lock(node->node_mutex);
  auto n256 = static_cast<Art_node256 *>(node.get());

  if (n256->children[c]) {
    n256->children[c] = nullptr;
    n256->num_children--;
  }

  // Resize to a node48 on underflow, not immediately to prevent
  // trashing if we sit on the 48/49 boundary
  if (n256->num_children == 37) {
    auto new_node = make_art_node<Art_node48>();
    if (!new_node) return;

    auto new_n48 = static_cast<Art_node48 *>(new_node.get());
    int pos = 0;
    for (int i = 0; i < 256; i++) {
      if (n256->children[i]) {
        new_n48->children[pos] = n256->children[i];
        new_n48->keys[i] = static_cast<unsigned char>(pos + 1);
        pos++;
      }
    }
    new_n48->num_children = pos;

    Copy_header(new_node.get(), node.get());
    node = std::move(new_node);
  }
}

void ART::Remove_child48(ArtNodePtr &node, unsigned char c) {
  assert(node->type() == NodeType::NODE48);
  std::unique_lock<std::shared_mutex> lock(node->node_mutex);
  auto n48 = static_cast<Art_node48 *>(node.get());

  unsigned char pos = n48->keys[c];
  if (pos != 0) {
    n48->children[pos - 1] = nullptr;
    n48->keys[c] = 0;
    n48->num_children--;
  }

  if (n48->num_children == 12) {
    auto new_node = make_art_node<Art_node16>();
    if (!new_node) return;

    Copy_header(new_node.get(), node.get());

    auto new_n16 = static_cast<Art_node16 *>(new_node.get());
    int child = 0;
    for (int i = 0; i < 256 && child < 16; i++) {
      if (n48->keys[i]) {
        new_n16->keys[child] = static_cast<unsigned char>(i);
        new_n16->children[child] = n48->children[n48->keys[i] - 1];
        child++;
      }
    }

    node = std::move(new_node);
  }
}

void ART::Remove_child16(ArtNodePtr &node, const ArtNodePtr &child) {
  assert(node->type() == NodeType::NODE16);
  std::unique_lock<std::shared_mutex> lock(node->node_mutex);
  auto n16 = static_cast<Art_node16 *>(node.get());

  int idx = 0;
  for (; idx < n16->num_children; idx++) {
    if (n16->children[idx] == child) break;
  }

  if (idx < n16->num_children) {
    std::memmove(n16->keys + idx, n16->keys + idx + 1, n16->num_children - idx - 1);
    for (int i = idx; i < n16->num_children - 1; i++) {
      n16->children[i] = n16->children[i + 1];
    }
    n16->children[n16->num_children - 1] = nullptr;
    n16->num_children--;
  }

  if (n16->num_children == 3) {
    auto new_node = make_art_node<Art_node4>();
    if (!new_node) return;

    Copy_header(new_node.get(), node.get());

    auto new_n4 = static_cast<Art_node4 *>(new_node.get());
    for (int i = 0; i < 4; i++) {
      if (i < n16->num_children) {
        new_n4->keys[i] = n16->keys[i];
        new_n4->children[i] = n16->children[i];
      } else {
        new_n4->keys[i] = 0;
        new_n4->children[i] = nullptr;
      }
    }

    node = std::move(new_node);
  }
}

void ART::Remove_child4(ArtNodePtr &node, const ArtNodePtr &child) {
  assert(node->type() == NodeType::NODE4);
  std::unique_lock<std::shared_mutex> lock(node->node_mutex);
  auto n4 = static_cast<Art_node4 *>(node.get());

  int idx = 0;
  for (; idx < n4->num_children; idx++) {
    if (n4->children[idx] == child) break;
  }

  if (idx < n4->num_children) {
    std::memmove(n4->keys + idx, n4->keys + idx + 1, n4->num_children - idx - 1);
    for (int i = idx; i < n4->num_children - 1; i++) {
      n4->children[i] = n4->children[i + 1];
    }
    n4->children[n4->num_children - 1] = nullptr;
    n4->num_children--;
  }

  if (n4->num_children == 1) {  // only one child node
    ArtNodePtr remaining_child = n4->children[0];
    if (!is_leaf(remaining_child.get())) {  // merge logic

      uint32_t prefix = node->partial_len;

      if (prefix < MAX_PREFIX_LEN) {  // add the first key of current node to prefix.
        node->partial[prefix] = n4->keys[0];
        prefix++;
      }

      if (prefix < MAX_PREFIX_LEN) {  // sub-child node prefix
        int sub_prefix = std::min(remaining_child->partial_len, MAX_PREFIX_LEN - prefix);
        std::memcpy(node->partial + prefix, remaining_child->partial, sub_prefix);
        prefix += sub_prefix;
      }

      std::memcpy(remaining_child->partial, node->partial, std::min(prefix, MAX_PREFIX_LEN));
      remaining_child->partial_len = std::min(prefix, MAX_PREFIX_LEN);

      Copy_header(remaining_child.get(), node.get());

      node = std::move(remaining_child);
    }
  }
}

void ART::Remove_child(ArtNodePtr &node, unsigned char c, const ArtNodePtr &child) {
  if (!node) return;
  switch (node->type()) {
    case NodeType::NODE4:
      Remove_child4(node, child);
      break;
    case NodeType::NODE16:
      Remove_child16(node, child);
      break;
    case NodeType::NODE48:
      Remove_child48(node, c);
      break;
    case NodeType::NODE256:
      Remove_child256(node, c);
      break;
    default:
      std::abort();
  }
}

int ART::Prefix_mismatch(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = std::min(MAX_PREFIX_LEN, n->partial_len);
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx;

  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) return idx;
  }

  // If the prefix is short we can avoid finding a leaf
  if (n->partial_len > MAX_PREFIX_LEN) {
    // Prefix is longer than what we've checked, find a leaf
    Art_leaf *l = Minimum(n);
    int min_key_len = std::min((int)l->key.size(), key_len);
    max_cmp = min_key_len - depth;
    for (; idx < max_cmp; idx++) {
      if (l->key[idx + depth] != key[depth + idx]) return idx;
    }
  }

  return idx;
}
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase