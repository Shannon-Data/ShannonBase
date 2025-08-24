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

ART::ArtNodePtr ART::Alloc_node(NodeType type) {
  Art_node *raw_node = nullptr;

  switch (type) {
    case NODE4: {
      auto node4 = std::make_unique<Art_node4>();
      for (int i = 0; i < 4; ++i) {
        node4->children[i].store(nullptr, std::memory_order_release);
      }
      raw_node = reinterpret_cast<Art_node *>(node4.release());
      break;
    }
    case NODE16: {
      auto node16 = std::make_unique<Art_node16>();
      for (int i = 0; i < 16; ++i) {
        node16->children[i].store(nullptr, std::memory_order_release);
      }
      raw_node = reinterpret_cast<Art_node *>(node16.release());
      break;
    }
    case NODE48: {
      auto node48 = std::make_unique<Art_node48>();
      for (int i = 0; i < 48; ++i) {
        node48->children[i].store(nullptr, std::memory_order_release);
      }
      raw_node = reinterpret_cast<Art_node *>(node48.release());
      break;
    }
    case NODE256: {
      auto node256 = std::make_unique<Art_node256>();
      for (int i = 0; i < 256; ++i) {
        node256->children[i].store(nullptr, std::memory_order_release);
      }
      raw_node = reinterpret_cast<Art_node *>(node256.release());
      break;
    }
    default:
      return nullptr;
  }

  if (raw_node) {
    raw_node->type = type;
    raw_node->ref_count.store(1, std::memory_order_release);
  }

  return make_art_node(raw_node);
}

void ART::Destroy_node(Art_node *n) {
  if (!n) return;

  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    Free_leaf(l);
    return;
  }

  std::unique_lock lock(n->node_mutex);

  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        p1->children[i].store(nullptr, std::memory_order_acq_rel);
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        p2->children[i].store(nullptr, std::memory_order_acq_rel);
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
      for (int i = 0; i < 256; i++) {
        int idx = p3->keys[i];
        if (!idx) continue;
        p3->children[idx - 1].store(nullptr, std::memory_order_acq_rel);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
      for (int i = 0; i < 256; i++) {
        p4->children[i].store(nullptr, std::memory_order_acq_rel);
      }
      break;
    }
    default:
      std::abort();
  }

  n->num_children = 0;
  lock.unlock();

  // Memory will be automatically freed by smart pointer deleter
  delete n;
}

std::atomic<ART::ArtNodePtr> *ART::Find_child(ArtNodePtr n, unsigned char c) {
  if (!n || IS_LEAF(n.get())) return nullptr;

  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c) return &p1->children[i];
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
      int bitfield = 0;
      for (int i = 0; i < n->num_children; ++i) {
        if (p2->keys[i] == c) bitfield |= (1 << i);
      }
      if (bitfield) {
        int idx = __builtin_ctz(bitfield);
        return &p2->children[idx];
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
      int idx = p3->keys[c];
      if (idx) return &p3->children[idx - 1];
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
      return &p4->children[c];
    }
    default:
      break;
  }
  return nullptr;
}

void ART::Find_children(ArtNodePtr n, unsigned char c, std::vector<ART::ArtNodePtr> &children) {
  if (!n || IS_LEAF(n.get())) return;

  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c) {
          auto child = p1->children[i].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
      for (int i = 0; i < n->num_children; ++i) {
        if (p2->keys[i] == c) {
          auto child = p2->children[i].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
      int idx = p3->keys[c];
      if (idx) {
        auto child = p3->children[idx - 1].load(std::memory_order_acquire);
        if (child) children.push_back(child);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
      auto child = p4->children[c].load(std::memory_order_acquire);
      if (child) children.push_back(child);
      break;
    }
    default:
      break;
  }
}

int ART::Check_prefix(const ArtNodePtr n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = std::min(n->partial_len, MAX_PREFIX_LEN);
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) return idx;
  }
  return idx;
}

int ART::Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth) {
  if (n->key_len != static_cast<uint32_t>(key_len)) return 1;
  return std::memcmp(n->key, key, key_len);
}

int ART::Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth) {
  int max_cmp = std::min(n->key_len, static_cast<uint32_t>(key_len)) - depth;
  if (max_cmp < 0) return 1;
  return std::memcmp(n->key + depth, key + depth, max_cmp);
}

void *ART::ART_search(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root.load(std::memory_order_acquire);
  if (!n) return nullptr;

  int prefix_len, depth = 0;

  while (n) {
    if (IS_LEAF(n.get())) {
      Art_leaf *leaf = LEAF_RAW(n.get());
      std::shared_lock leaf_lock(leaf->leaf_mutex);

      void *result = nullptr;
      if (!Leaf_matches(leaf, key, key_len, depth)) {
        void **values = leaf->values.load(std::memory_order_acquire);
        if (values && leaf->vcount.load(std::memory_order_acquire) > 0) {
          result = values[0];
        }
      }

      return result;
    }

    std::shared_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if ((uint)prefix_len != std::min(MAX_PREFIX_LEN, static_cast<uint>(n->partial_len))) {
        return nullptr;
      }
      depth += n->partial_len;
    }

    if (depth >= key_len) {
      return nullptr;
    }

    auto child_slot = Find_child(n, key[depth]);
    if (!child_slot) return nullptr;

    n = child_slot->load(std::memory_order_acquire);
    depth++;
  }
  return nullptr;
}

std::vector<void *> ART::ART_search_all(const unsigned char *key, int key_len) {
  std::vector<void *> results;
  if (!key || key_len <= 0 || !m_inited) return results;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root.load(std::memory_order_acquire);
  if (!n) return results;

  uint prefix_len, depth = 0u;

  while (n) {
    if (IS_LEAF(n.get())) {
      Art_leaf *l = LEAF_RAW(n.get());
      std::shared_lock leaf_lock(l->leaf_mutex);

      if (!Leaf_matches(l, key, key_len, depth)) {
        void **values = l->values.load(std::memory_order_acquire);
        uint32_t vcount = l->vcount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < vcount; ++i) {
          if (values && values[i]) {
            results.push_back(values[i]);
          }
        }
      }

      return results;
    }

    std::shared_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if (prefix_len != std::min(MAX_PREFIX_LEN, n->partial_len)) {
        return results;
      }
      depth += n->partial_len;
    }

    if (depth >= static_cast<uint>(key_len)) {
      return results;
    }

    auto child_slot = Find_child(n, key[depth]);
    if (!child_slot) return results;

    n = child_slot->load(std::memory_order_acquire);
    depth++;
  }
  return results;
}

ART::Art_leaf *ART::Minimum(const ArtNodePtr n) {
  if (!n) return nullptr;
  if (IS_LEAF(n.get())) {
    return LEAF_RAW(n.get());
  }

  std::shared_lock lock(n->node_mutex);
  ArtNodePtr child = nullptr;

  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(const_cast<Art_node *>(n.get()));
      child = p->children[0].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(const_cast<Art_node *>(n.get()));
      child = p->children[0].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(const_cast<Art_node *>(n.get()));
      int idx = 0;
      while (idx < 256 && !p->keys[idx]) idx++;
      if (idx < 256) {
        child = p->children[p->keys[idx] - 1].load(std::memory_order_acquire);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(const_cast<Art_node *>(n.get()));
      int idx = 0;
      while (idx < 256 && !p->children[idx].load(std::memory_order_acquire)) idx++;
      if (idx < 256) {
        child = p->children[idx].load(std::memory_order_acquire);
      }
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

ART::Art_leaf *ART::Maximum(const ArtNodePtr n) {
  if (!n) return nullptr;
  if (IS_LEAF(n.get())) {
    return LEAF_RAW(n.get());
  }

  std::shared_lock lock(n->node_mutex);
  ArtNodePtr child = nullptr;

  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(const_cast<Art_node *>(n.get()));
      child = p->children[n->num_children - 1].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(const_cast<Art_node *>(n.get()));
      child = p->children[n->num_children - 1].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(const_cast<Art_node *>(n.get()));
      int idx = 255;
      while (idx >= 0 && !p->keys[idx]) idx--;
      if (idx >= 0) {
        child = p->children[p->keys[idx] - 1].load(std::memory_order_acquire);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(const_cast<Art_node *>(n.get()));
      int idx = 255;
      while (idx >= 0 && !p->children[idx].load(std::memory_order_acquire)) idx--;
      if (idx >= 0) {
        child = p->children[idx].load(std::memory_order_acquire);
      }
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
  auto root_ptr = m_tree->root.load(std::memory_order_acquire);
  return Minimum(root_ptr);
}

ART::Art_leaf *ART::ART_maximum() {
  if (!m_inited) return nullptr;
  std::shared_lock lock(m_tree->tree_mutex);
  auto root_ptr = m_tree->root.load(std::memory_order_acquire);
  return Maximum(root_ptr);
}

ART::ArtLeafPtr ART::Make_leaf(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;

  size_t total_size = sizeof(Art_leaf) + key_len;
  auto leaf_buffer = std::make_unique<uint8_t[]>(total_size);
  Art_leaf *leaf = reinterpret_cast<Art_leaf *>(leaf_buffer.get());

  new (leaf) Art_leaf();
  leaf->key_len = key_len;
  leaf->ref_count.store(1, std::memory_order_release);

  std::memcpy(leaf->key, key, key_len);

  auto values = std::make_unique<void *[]>(initial_capacity);

  auto value_copy = std::make_unique<uint8_t[]>(value_len);
  std::memcpy(value_copy.get(), value, value_len);

  values[0] = value_copy.release();

  leaf->values.store(values.release(), std::memory_order_release);
  leaf->value_len.store(value_len, std::memory_order_release);
  leaf->capacity.store(initial_capacity, std::memory_order_release);
  leaf->vcount.store(1, std::memory_order_release);

  return make_art_leaf(leaf);
}

int ART::Longest_common_prefix(Art_leaf *l1, Art_leaf *l2, int depth) {
  int max_cmp = std::min(l1->key_len, l2->key_len) - depth;
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (l1->key[depth + idx] != l2->key[depth + idx]) return idx;
  }
  return idx;
}

void ART::Copy_header(Art_node *dest, Art_node *src) {
  dest->num_children = src->num_children;
  dest->partial_len = src->partial_len;
  std::memcpy(dest->partial, src->partial, std::min(MAX_PREFIX_LEN, src->partial_len));
}

void ART::Add_child256(Art_node256 *n, std::atomic<ArtNodePtr> *ref, unsigned char c, ArtNodePtr child) {
  std::unique_lock lock(n->n.node_mutex);
  auto old = n->children[c].exchange(child, std::memory_order_acq_rel);

  if (!old) n->n.num_children++;
}

void ART::Add_child48(Art_node48 *n, std::atomic<ArtNodePtr> *ref, unsigned char c, ArtNodePtr child) {
  std::unique_lock lock(n->n.node_mutex);
  if (n->n.num_children < 48) {
    int pos = 0;
    while (pos < 48 && n->children[pos].load(std::memory_order_acquire).get()) pos++;
    if (pos < 48) {
      n->children[pos].store(child, std::memory_order_release);
      n->keys[c] = pos + 1;
      n->n.num_children++;
    }
    return;
  }

  // Upgrade to NODE256
  ArtNodePtr new_node = Alloc_node(NodeType::NODE256);
  if (!new_node) return;

  Art_node256 *new_node256 = reinterpret_cast<Art_node256 *>(new_node.get());

  for (int i = 0; i < 256; i++) {
    if (n->keys[i]) {
      auto ch = n->children[n->keys[i] - 1].load(std::memory_order_acquire);
      new_node256->children[i].store(ch, std::memory_order_release);
    }
  }

  Copy_header(&new_node256->n, &n->n);

  if (ref) ref->store(new_node, std::memory_order_release);

  lock.unlock();
  Add_child256(new_node256, ref, c, child);

  // Keep new_node alive until end of scope
  // new_node.release();  // Transfer ownership to ref
}

void ART::Add_child16(Art_node16 *n, std::atomic<ArtNodePtr> *ref, unsigned char c, ArtNodePtr child) {
  std::unique_lock lock(n->n.node_mutex);
  if (n->n.num_children < 16) {
    int idx = 0;
    for (; idx < n->n.num_children; idx++) {
      if (c < n->keys[idx]) break;
    }

    std::memmove(n->keys + idx + 1, n->keys + idx, n->n.num_children - idx);
    for (int i = n->n.num_children; i > idx; i--) {
      n->children[i].store(n->children[i - 1].load(std::memory_order_acquire), std::memory_order_release);
    }
    n->keys[idx] = c;
    n->children[idx].store(child, std::memory_order_release);
    n->n.num_children++;
    return;
  }

  // Upgrade to NODE48
  ArtNodePtr new_node = Alloc_node(NodeType::NODE48);
  if (!new_node.get()) return;

  Art_node48 *new_node48 = reinterpret_cast<Art_node48 *>(new_node.get());

  for (int i = 0; i < 16; i++) {
    auto ch = n->children[i].load(std::memory_order_acquire);
    new_node48->children[i].store(ch, std::memory_order_release);
    new_node48->keys[n->keys[i]] = i + 1;
  }

  Copy_header(&new_node48->n, &n->n);

  if (ref) ref->store(new_node, std::memory_order_release);

  lock.unlock();
  Add_child48(new_node48, ref, c, child);

  // new_node.release();  // Transfer ownership to ref
}

void ART::Add_child4(Art_node4 *n, std::atomic<ArtNodePtr> *ref, unsigned char c, ArtNodePtr child) {
  std::unique_lock lock(n->n.node_mutex);
  if (n->n.num_children < 4) {
    int idx = 0;
    for (; idx < n->n.num_children; idx++) {
      if (c < n->keys[idx]) break;
    }

    std::memmove(n->keys + idx + 1, n->keys + idx, n->n.num_children - idx);
    for (int i = n->n.num_children; i > idx; i--) {
      n->children[i].store(n->children[i - 1].load(std::memory_order_acquire), std::memory_order_release);
    }
    n->keys[idx] = c;
    n->children[idx].store(child, std::memory_order_release);
    n->n.num_children++;
    return;
  }

  // Upgrade to NODE16
  ArtNodePtr new_node = Alloc_node(NodeType::NODE16);
  if (!new_node) return;

  Art_node16 *new_node16 = reinterpret_cast<Art_node16 *>(new_node.get());

  for (int i = 0; i < 4; i++) {
    auto ch = n->children[i].load(std::memory_order_acquire);
    new_node16->children[i].store(ch, std::memory_order_release);
    new_node16->keys[i] = n->keys[i];
  }

  Copy_header(&new_node16->n, &n->n);

  if (ref) ref->store(new_node, std::memory_order_release);

  lock.unlock();
  Add_child16(new_node16, ref, c, child);

  // new_node.release();  // Transfer ownership to ref
}

void ART::Add_child(ArtNodePtr n, std::atomic<ArtNodePtr> *ref, unsigned char c, ArtNodePtr child) {
  if (!n) return;
  assert(!IS_LEAF(n));

  switch (n->type) {
    case NodeType::NODE4:
      Add_child4(reinterpret_cast<Art_node4 *>(n.get()), ref, c, child);
      break;
    case NodeType::NODE16:
      Add_child16(reinterpret_cast<Art_node16 *>(n.get()), ref, c, child);
      break;
    case NodeType::NODE48:
      Add_child48(reinterpret_cast<Art_node48 *>(n.get()), ref, c, child);
      break;
    case NodeType::NODE256:
      Add_child256(reinterpret_cast<Art_node256 *>(n.get()), ref, c, child);
      break;
    default:
      std::abort();
  }
}

int ART::Prefix_mismatch(const ArtNodePtr n, const unsigned char *key, int key_len, int depth) {
  int min_tmp = std::min(n->partial_len, MAX_PREFIX_LEN);
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx;

  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) return idx;
  }

  if (n->partial_len > MAX_PREFIX_LEN) {
    Art_leaf *leaf = Minimum(n);
    if (leaf) {
      int min_key_len = std::min(static_cast<int>(leaf->key_len), key_len);
      max_cmp = min_key_len - depth;
      for (; idx < max_cmp; idx++) {
        if (leaf->key[idx + depth] != key[depth + idx]) {
          return idx;
        }
      }
    }
  }

  return idx;
}

void *ART::ART_insert(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  int old_val = 0;
  std::atomic<ArtNodePtr> &root_ref = m_tree->root;
  void *old = Recursive_insert(root_ref.load(std::memory_order_acquire), &root_ref, key, key_len, value, value_len, 0,
                               &old_val, 0);

  if (!old_val) {
    m_tree->size.fetch_add(1, std::memory_order_acq_rel);
  }

  return old;
}

void *ART::ART_insert_with_replace(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  int old_val = 0;
  std::atomic<ArtNodePtr> &root_ref = m_tree->root;
  void *old = Recursive_insert(root_ref.load(std::memory_order_acquire), &root_ref, key, key_len, value, value_len, 0,
                               &old_val, 1);

  if (!old_val) {
    m_tree->size.fetch_add(1, std::memory_order_acq_rel);
  }

  return old;
}

void *ART::Recursive_insert(ArtNodePtr n, std::atomic<ArtNodePtr> *ref, const unsigned char *key, int key_len,
                            void *value, int value_len, int depth, int *old, int replace) {
  if (!n) {
    ArtLeafPtr new_leaf = Make_leaf(key, key_len, value, value_len);
    if (new_leaf) {
      auto node_ptr = make_art_node(reinterpret_cast<Art_node *>(SET_LEAF(new_leaf.get())));
      ref->store(node_ptr, std::memory_order_release);
      // new_leaf.release();  // Transfer ownership
      *old = 0;
    }
    return nullptr;
  }

  if (IS_LEAF(n.get())) {
    Art_leaf *l = LEAF_RAW(n.get());
    std::unique_lock leaf_lock(l->leaf_mutex);

    if (!Leaf_matches(l, key, key_len, depth)) {
      if (replace) {
        void **values = l->values.load(std::memory_order_acquire);
        void *old_value = values[0];

        auto value_copy = std::make_unique<uint8_t[]>(value_len);
        std::memcpy(value_copy.get(), value, value_len);

        values[0] = value_copy.get();
        l->value_len.store(value_len, std::memory_order_release);
        *old = 1;

        delete[] static_cast<uint8_t *>(old_value);
        return old_value;
      } else {
        uint32_t current_count = l->vcount.load(std::memory_order_acquire);
        uint32_t current_capacity = l->capacity.load(std::memory_order_acquire);

        void **values = l->values.load(std::memory_order_acquire);
        if (current_count == current_capacity) {
          uint32_t new_capacity = current_capacity * 2;
          auto new_values = std::make_unique<void *[]>(new_capacity);
          std::memcpy(new_values.get(), values, current_capacity * sizeof(void *));

          delete[] values;
          values = new_values.release();
          l->values.store(values, std::memory_order_release);
          l->capacity.store(new_capacity, std::memory_order_release);
        }

        auto value_copy = std::make_unique<uint8_t[]>(value_len);
        std::memcpy(value_copy.get(), value, value_len);

        values[current_count] = value_copy.release();
        l->vcount.store(current_count + 1, std::memory_order_release);
        *old = 0;

        return nullptr;
      }
    }

    // Create new internal node to split the leaf
    ArtNodePtr new_node = Alloc_node(NodeType::NODE4);
    if (!new_node) return nullptr;

    Art_node4 *new_node4 = reinterpret_cast<Art_node4 *>(new_node.get());
    ArtLeafPtr l2 = Make_leaf(key, key_len, value, value_len);
    if (!l2) return nullptr;

    int longest_prefix = Longest_common_prefix(l, l2.get(), depth);
    new_node4->n.partial_len = longest_prefix;
    int min_len = std::min(static_cast<int>(MAX_PREFIX_LEN), longest_prefix);
    std::memcpy(new_node4->n.partial, key + depth, min_len);

    unsigned char c1 = l->key[depth + longest_prefix];
    unsigned char c2 = l2->key[depth + longest_prefix];

    auto node_ptr = make_art_node(reinterpret_cast<Art_node *>(SET_LEAF(l2.get())));
    if (c1 < c2) {
      new_node4->keys[0] = c1;
      new_node4->keys[1] = c2;
      new_node4->children[0].store(n, std::memory_order_release);
      new_node4->children[1].store(node_ptr, std::memory_order_release);
    } else {
      new_node4->keys[0] = c2;
      new_node4->keys[1] = c1;
      new_node4->children[0].store(node_ptr, std::memory_order_release);
      new_node4->children[1].store(n, std::memory_order_release);
    }

    new_node4->n.num_children = 2;
    ref->store(new_node, std::memory_order_release);

    *old = 0;
    // new_node.release();  // Transfer ownership to ref
    // l2.release();        // Transfer ownership to node
    return nullptr;
  }

  {
    std::unique_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      int prefix_diff = Prefix_mismatch(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_diff) < n->partial_len) {
        ArtNodePtr new_node = Alloc_node(NodeType::NODE4);
        if (!new_node) return nullptr;

        Art_node4 *new_node4 = reinterpret_cast<Art_node4 *>(new_node.get());
        new_node4->n.partial_len = prefix_diff;
        std::memcpy(new_node4->n.partial, n->partial, prefix_diff);

        n->partial_len = n->partial_len - prefix_diff - 1;
        std::memmove(n->partial, n->partial + prefix_diff + 1,
                     std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(n->partial_len)));

        new_node4->keys[0] = n->partial[prefix_diff];
        new_node4->children[0].store(n, std::memory_order_release);
        new_node4->n.num_children = 1;

        ArtLeafPtr new_leaf = Make_leaf(key, key_len, value, value_len);
        if (!new_leaf) return nullptr;

        auto node_ptr = make_art_node(reinterpret_cast<Art_node *>(SET_LEAF(new_leaf.get())));
        new_node4->keys[1] = key[depth + prefix_diff];
        new_node4->children[1].store(node_ptr, std::memory_order_release);
        new_node4->n.num_children = 2;
        ref->store(new_node, std::memory_order_release);

        *old = 0;
        // new_node.release();  // Transfer ownership to ref
        // new_leaf.release();  // Transfer ownership to node
        return nullptr;
      }
      depth += n->partial_len;
    }
  }

  if (depth >= key_len) return nullptr;

  auto child_ref = Find_child(n, key[depth]);
  ArtNodePtr child = child_ref ? child_ref->load(std::memory_order_acquire) : nullptr;
  if (!child) {
    ArtLeafPtr new_leaf = Make_leaf(key, key_len, value, value_len);
    if (new_leaf) {
      auto node_ptr = make_art_node(reinterpret_cast<Art_node *>(SET_LEAF(new_leaf.get())));
      Add_child(n, ref, key[depth], node_ptr);
      // new_leaf.release();  // Transfer ownership to node
      *old = 0;
    }
    return nullptr;
  }

  void *result =
      Recursive_insert(child, Find_child(n, key[depth]), key, key_len, value, value_len, depth + 1, old, replace);
  return result;
}

void ART::Remove_child256(Art_node256 *n, std::atomic<ArtNodePtr> *ref, unsigned char c) {
  std::unique_lock lock(n->n.node_mutex);
  ArtNodePtr old = n->children[c].exchange(nullptr, std::memory_order_acq_rel);
  if (old) {
    n->n.num_children--;
  }

  if (n->n.num_children < 37 && ref) {
    ArtNodePtr new_node = Alloc_node(NodeType::NODE48);
    if (!new_node) return;

    Art_node48 *new_node48 = reinterpret_cast<Art_node48 *>(new_node.get());
    int pos = 0;
    for (int i = 0; i < 256 && pos < 48; i++) {
      ArtNodePtr child = n->children[i].load(std::memory_order_acquire);
      if (child) {
        new_node48->children[pos].store(child, std::memory_order_release);
        new_node48->keys[i] = pos + 1;
        pos++;
      }
    }

    Copy_header(&new_node48->n, &n->n);
    ref->store(new_node, std::memory_order_release);
    // new_node.release();  // Transfer ownership to ref
  }
}

void ART::Remove_child48(Art_node48 *n, std::atomic<ArtNodePtr> *ref, unsigned char c) {
  std::unique_lock lock(n->n.node_mutex);
  int pos = n->keys[c];
  if (pos) {
    pos--;
    ArtNodePtr old = n->children[pos].exchange(nullptr, std::memory_order_acq_rel);
    n->keys[c] = 0;
    n->n.num_children--;
  }

  if (n->n.num_children < 13 && ref) {
    ArtNodePtr new_node = Alloc_node(NodeType::NODE16);
    if (!new_node) return;

    Art_node16 *new_node16 = reinterpret_cast<Art_node16 *>(new_node.get());
    int pos = 0;
    for (int i = 0; i < 256 && pos < 16; i++) {
      if (n->keys[i]) {
        ArtNodePtr ch = n->children[n->keys[i] - 1].load(std::memory_order_acquire);
        new_node16->keys[pos] = i;
        new_node16->children[pos].store(ch, std::memory_order_release);
        pos++;
      }
    }

    Copy_header(&new_node16->n, &n->n);
    ref->store(new_node, std::memory_order_release);
    // new_node.release();  // Transfer ownership to ref
  }
}

void ART::Remove_child16(Art_node16 *n, std::atomic<ArtNodePtr> *ref, std::atomic<ArtNodePtr> *child_ref) {
  std::unique_lock lock(n->n.node_mutex);
  ArtNodePtr old = child_ref->exchange(nullptr, std::memory_order_acq_rel);
  int idx = 0;
  for (; idx < n->n.num_children; idx++) {
    if (&n->children[idx] == child_ref) break;
  }

  if (idx < n->n.num_children) {
    std::memmove(n->keys + idx, n->keys + idx + 1, n->n.num_children - idx - 1);
    for (int i = idx; i < n->n.num_children - 1; i++) {
      n->children[i].store(n->children[i + 1].load(std::memory_order_acquire), std::memory_order_release);
    }
    n->children[n->n.num_children - 1].store(nullptr, std::memory_order_release);
    n->n.num_children--;
  }

  if (n->n.num_children < 5 && ref) {
    ArtNodePtr new_node = Alloc_node(NodeType::NODE4);
    if (!new_node) return;

    Art_node4 *new_node4 = reinterpret_cast<Art_node4 *>(new_node.get());
    for (int i = 0; i < n->n.num_children; i++) {
      ArtNodePtr ch = n->children[i].load(std::memory_order_acquire);
      new_node4->keys[i] = n->keys[i];
      new_node4->children[i].store(ch, std::memory_order_release);
    }

    Copy_header(&new_node4->n, &n->n);
    ref->store(new_node, std::memory_order_release);
    // new_node.release();  // Transfer ownership to ref
  }
}

void ART::Remove_child4(Art_node4 *n, std::atomic<ArtNodePtr> *ref, std::atomic<ArtNodePtr> *child_ref) {
  std::unique_lock lock(n->n.node_mutex);
  ArtNodePtr old = child_ref->exchange(nullptr, std::memory_order_acq_rel);
  int idx = 0;
  for (; idx < n->n.num_children; idx++) {
    if (&n->children[idx] == child_ref) break;
  }

  if (idx < n->n.num_children) {
    std::memmove(n->keys + idx, n->keys + idx + 1, n->n.num_children - idx - 1);
    for (int i = idx; i < n->n.num_children - 1; i++) {
      n->children[i].store(n->children[i + 1].load(std::memory_order_acquire), std::memory_order_release);
    }
    n->children[n->n.num_children - 1].store(nullptr, std::memory_order_release);
    n->n.num_children--;
  }

  if (n->n.num_children == 1 && ref) {
    ArtNodePtr child = n->children[0].load(std::memory_order_acquire);
    if (!IS_LEAF(child.get())) {
      Copy_header(child.get(), &n->n);
      ref->store(child, std::memory_order_release);
    }
  }
}

void ART::Remove_child(ArtNodePtr n, std::atomic<ArtNodePtr> *ref, unsigned char c,
                       std::atomic<ArtNodePtr> *child_ref) {
  if (!n) return;
  switch (n->type) {
    case NodeType::NODE4:
      Remove_child4(reinterpret_cast<Art_node4 *>(n.get()), ref, child_ref);
      break;
    case NodeType::NODE16:
      Remove_child16(reinterpret_cast<Art_node16 *>(n.get()), ref, child_ref);
      break;
    case NodeType::NODE48:
      Remove_child48(reinterpret_cast<Art_node48 *>(n.get()), ref, c);
      break;
    case NodeType::NODE256:
      Remove_child256(reinterpret_cast<Art_node256 *>(n.get()), ref, c);
      break;
    default:
      std::abort();
  }
}

void *ART::ART_delete(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  std::atomic<ArtNodePtr> &root_ref = m_tree->root;
  Art_leaf *l = Recursive_delete(root_ref.load(std::memory_order_acquire), &root_ref, key, key_len, 0);
  if (!l) return nullptr;

  m_tree->size.fetch_sub(1, std::memory_order_acq_rel);

  void *result = nullptr;
  {
    std::unique_lock leaf_lock(l->leaf_mutex);

    void **values = l->values.load(std::memory_order_acquire);
    uint32_t vcount = l->vcount.load(std::memory_order_acquire);

    if (values && vcount > 0) {
      result = values[0];
      for (uint32_t i = 1; i < vcount; ++i) {
        if (values[i]) delete[] static_cast<uint8_t *>(values[i]);
      }
      values[0] = nullptr;

      delete[] values;
      l->values.store(nullptr, std::memory_order_release);
      l->vcount.store(0, std::memory_order_release);
      l->capacity.store(0, std::memory_order_release);
    }
  }

  Free_leaf(l);
  return result;
}

ART::Art_leaf *ART::Recursive_delete(ArtNodePtr n, std::atomic<ArtNodePtr> *ref, const unsigned char *key, int key_len,
                                     int depth) {
  if (!n) return nullptr;

  if (IS_LEAF(n.get())) {
    Art_leaf *l = LEAF_RAW(n.get());
    bool match = false;
    {
      std::shared_lock leaf_lock(l->leaf_mutex);
      match = !Leaf_matches(l, key, key_len, depth);
    }

    if (match) {
      ref->store(nullptr, std::memory_order_release);
      return l;
    }

    return nullptr;
  }

  std::atomic<ART::ArtNodePtr> *child_ref = nullptr;
  ArtNodePtr child = nullptr;
  {
    std::shared_lock node_lock(n->node_mutex);

    if (n->partial_len) {
      int prefix_len = Check_prefix(n, key, key_len, depth);
      if (prefix_len != std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(n->partial_len))) {
        return nullptr;
      }
      depth += n->partial_len;
    }

    if (depth >= key_len) {
      return nullptr;
    }

    child_ref = Find_child(n, key[depth]);
    child = child_ref->load() ? child_ref->load(std::memory_order_acquire) : nullptr;
    if (!child) return nullptr;
  }

  Art_leaf *result = Recursive_delete(child, child_ref, key, key_len, depth + 1);
  if (result) {
    std::unique_lock parent_lock(n->node_mutex);
    Remove_child(n, ref, key[depth], child_ref);
  }

  return result;
}

void ART::Free_leaf(Art_leaf *l) {
  if (!l) return;

  uint32_t expected = 0;
  if (!l->ref_count.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
    return;
  }

  std::unique_lock lock(l->leaf_mutex);

  void **values = l->values.exchange(nullptr, std::memory_order_acq_rel);
  uint32_t vcount = l->vcount.exchange(0, std::memory_order_acq_rel);

  if (values) {
    for (uint32_t i = 0; i < vcount; ++i) {
      if (values[i]) {
        delete[] static_cast<uint8_t *>(values[i]);
        values[i] = nullptr;
      }
    }
    delete[] values;
  }

  l->capacity.store(0, std::memory_order_release);
  lock.unlock();

  delete[] reinterpret_cast<uint8_t *>(l);
}

int ART::Leaf_prefix_matches(const Art_leaf *n, const unsigned char *prefix, int prefix_len) {
  if (!n || !prefix || prefix_len <= 0) return 1;
  if (n->key_len < static_cast<uint32_t>(prefix_len)) return 1;

  return std::memcmp(n->key, prefix, prefix_len);
}

int ART::Leaf_prefix_matches2(const Art_leaf *n, const unsigned char *prefix, int prefix_len) {
  if (!n || !prefix || prefix_len <= 0) return 1;
  if (n->key_len < static_cast<uint32_t>(prefix_len)) return 1;

  return std::memcmp(n->key, prefix, prefix_len);
}

int ART::Recursive_iter(ArtNodePtr n, ART_Func &cb, void *data) {
  if (!n) return 0;

  if (IS_LEAF(n.get())) {
    Art_leaf *l = LEAF_RAW(n.get());
    std::shared_lock leaf_lock(l->leaf_mutex);

    void **values = l->values.load(std::memory_order_acquire);
    uint32_t vcount = l->vcount.load(std::memory_order_acquire);

    int ret = 0;
    if (values && vcount > 0 && values[0]) {
      ret = cb(data, l, l->key, l->key_len, values[0], l->value_len.load(std::memory_order_acquire));
    }

    return ret;
  }

  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        ArtNodePtr child = p1->children[i].load(std::memory_order_acquire);
        if (child) {
          int ret = Recursive_iter(child, cb, data);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
      for (int i = 0; i < n->num_children; i++) {
        ArtNodePtr child = p2->children[i].load(std::memory_order_acquire);
        if (child) {
          int ret = Recursive_iter(child, cb, data);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
      for (int i = 0; i < 256; i++) {
        int idx = p3->keys[i];
        if (!idx) continue;
        ArtNodePtr child = p3->children[idx - 1].load(std::memory_order_acquire);
        if (child) {
          int ret = Recursive_iter(child, cb, data);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
      for (int i = 0; i < 256; i++) {
        ArtNodePtr child = p4->children[i].load(std::memory_order_acquire);
        if (child) {
          int ret = Recursive_iter(child, cb, data);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    default:
      break;
  }

  return 0;
}

int ART::Recursive_iter_with_key(ArtNodePtr n, const unsigned char *key, int key_len) {
  if (!n) return 0;

  std::shared_lock lock(n->node_mutex);
  if (IS_LEAF(n.get())) {
    Art_leaf *l = LEAF_RAW(n.get());
    std::shared_lock leaf_lock(l->leaf_mutex);
    if (!Leaf_prefix_matches(l, key, key_len)) {
      m_current_values.push_back(l);
    }
    return 0;
  }

  if (n->partial_len) {
    int prefix_len = Check_prefix(n, key, key_len, 0);
    if (prefix_len != std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(n->partial_len))) {
      return 0;
    }
    key += prefix_len;
    key_len -= prefix_len;
  }

  std::vector<ART::ArtNodePtr> children;
  if (key_len > 0) {
    Find_children(n, key[0], children);
  } else {
    // Collect all children for prefix iteration
    switch (n->type) {
      case NodeType::NODE4: {
        Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n.get());
        for (int i = 0; i < n->num_children; i++) {
          auto child = p1->children[i].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
        break;
      }
      case NodeType::NODE16: {
        Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n.get());
        for (int i = 0; i < n->num_children; i++) {
          auto child = p2->children[i].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
        break;
      }
      case NodeType::NODE48: {
        Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n.get());
        for (int i = 0; i < 256; i++) {
          int idx = p3->keys[i];
          if (!idx) continue;
          auto child = p3->children[idx - 1].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
        break;
      }
      case NodeType::NODE256: {
        Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n.get());
        for (int i = 0; i < 256; i++) {
          auto child = p4->children[i].load(std::memory_order_acquire);
          if (child) children.push_back(child);
        }
        break;
      }
      default:
        std::abort();
    }
  }

  for (auto &child : children) {
    int ret = Recursive_iter_with_key(child, key, key_len);
    if (ret != 0) return ret;
  }
  return 0;
}

int ART::ART_iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len) {
  if (!key || key_len <= 0 || !m_inited) return 1;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  ArtNodePtr n = m_tree->root.load(std::memory_order_acquire);
  int prefix_len, depth = 0;

  while (n) {
    if (IS_LEAF(n.get())) {
      Art_leaf *l = LEAF_RAW(n.get());
      std::shared_lock leaf_lock(l->leaf_mutex);

      int ret = 0;
      if (!Leaf_prefix_matches(l, key, key_len)) {
        void **values = l->values.load(std::memory_order_acquire);
        if (values && l->vcount.load(std::memory_order_acquire) > 0) {
          ret = cb(data, l, l->key, l->key_len, values[0], data_len);
        }
      }

      return ret;
    }

    if (depth == key_len) {
      Art_leaf *l = Minimum(n);
      if (l) {
        int ret = 0;
        if (!Leaf_prefix_matches(l, key, key_len)) {
          ret = Recursive_iter(n, cb, data);
        }
        return ret;
      }
      return 0;
    }

    std::shared_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Prefix_mismatch(n, key, key_len, depth);

      if (static_cast<uint32_t>(prefix_len) > n->partial_len) {
        prefix_len = n->partial_len;
      }

      if (!prefix_len) {
        return 0;
      } else if (depth + prefix_len == key_len) {
        return Recursive_iter(n, cb, data);
      }

      depth += n->partial_len;
    }

    auto child_slot = Find_child(n, key[depth]);
    if (!child_slot->load().get()) return 0;

    n = child_slot->load(std::memory_order_acquire);
    depth++;
  }

  return 0;
}

int ART::ART_iter(ART_Func cb, void *data) {
  if (!m_inited) return 1;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  return Recursive_iter(m_tree->root.load(std::memory_order_acquire), cb, data);
}

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase