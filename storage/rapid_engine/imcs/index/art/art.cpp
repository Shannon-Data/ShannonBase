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

void *ART::safe_malloc(size_t size) {
  if (size == 0) return nullptr;
  void *ptr = std::malloc(size);
  return ptr;
}

void *ART::safe_calloc(size_t count, size_t size) {
  if (count == 0 || size == 0) return nullptr;
  if (count > SIZE_MAX / size) {
    return nullptr;
  }

  void *ptr = std::calloc(count, size);
  return ptr;
}

void *ART::safe_realloc(void *ptr, size_t size) {
  if (size == 0) {
    std::free(ptr);
    return nullptr;
  }

  void *new_ptr = std::realloc(ptr, size);
  return new_ptr;
}

void ART::AddRef(Art_node *node) {
  if (node && !IS_LEAF(node)) {
    node->ref_count.fetch_add(1, std::memory_order_acq_rel);
  }
}

void ART::Release(Art_node *node) {
  if (!node || IS_LEAF(node)) return;

  uint32_t old_count = node->ref_count.fetch_sub(1, std::memory_order_acq_rel);
  if (old_count == 1) {
    Destroy_node(node);
  }
}

void ART::AddRefLeaf(Art_leaf *leaf) {
  if (leaf) {
    leaf->ref_count.fetch_add(1, std::memory_order_acq_rel);
  }
}

void ART::ReleaseLeaf(Art_leaf *leaf) {
  if (!leaf) return;

  uint32_t old_count = leaf->ref_count.fetch_sub(1, std::memory_order_acq_rel);
  if (old_count == 1) {
    Free_leaf(leaf);
  }
}

ART::Art_node *ART::Alloc_node(NodeType type) {
  Art_node *n;
  switch (type) {
    case NODE4: {
      Art_node4 *node4 = static_cast<Art_node4 *>(safe_calloc(1, sizeof(Art_node4)));
      if (!node4) return nullptr;

      for (int i = 0; i < 4; ++i) {
        node4->children[i].store(nullptr, std::memory_order_release);
      }
      n = reinterpret_cast<Art_node *>(node4);
      break;
    }
    case NODE16: {
      Art_node16 *node16 = static_cast<Art_node16 *>(safe_calloc(1, sizeof(Art_node16)));
      if (!node16) return nullptr;

      for (int i = 0; i < 16; ++i) {
        node16->children[i].store(nullptr, std::memory_order_release);
      }
      n = reinterpret_cast<Art_node *>(node16);
      break;
    }
    case NODE48: {
      Art_node48 *node48 = static_cast<Art_node48 *>(safe_calloc(1, sizeof(Art_node48)));
      if (!node48) return nullptr;

      for (int i = 0; i < 48; ++i) {
        node48->children[i].store(nullptr, std::memory_order_release);
      }
      n = reinterpret_cast<Art_node *>(node48);
      break;
    }
    case NODE256: {
      Art_node256 *node256 = static_cast<Art_node256 *>(safe_calloc(1, sizeof(Art_node256)));
      if (!node256) return nullptr;

      for (int i = 0; i < 256; ++i) {
        node256->children[i].store(nullptr, std::memory_order_release);
      }
      n = reinterpret_cast<Art_node *>(node256);
      break;
    }
    default:
      return nullptr;
  }

  if (n) {
    n->type = type;
    n->ref_count.store(1, std::memory_order_release);
  }
  return n;
}

void ART::Destroy_node(Art_node *n) {
  // Break if null
  if (!n) return;

  // Special case leafs
  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    ReleaseLeaf(l);
    return;
  }

  // Handle each node type
  int i, idx;
  std::unique_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
      for (i = 0; i < n->num_children; i++) {
        Art_node *child = p1->children[i].load(std::memory_order_acquire);
        if (child) {
          Release(child);
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
      for (i = 0; i < n->num_children; i++) {
        Art_node *child = p2->children[i].load(std::memory_order_acquire);
        if (child) {
          Release(child);
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
      for (i = 0; i < 256; i++) {
        idx = p3->keys[i];
        if (!idx) continue;
        Art_node *child = p3->children[idx - 1].load(std::memory_order_acquire);
        if (child) {
          Release(child);
        }
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
      for (i = 0; i < 256; i++) {
        Art_node *child = p4->children[i].load(std::memory_order_acquire);
        if (child) {
          Release(child);
        }
      }
      break;
    }
    default:
      std::abort();
  }

  std::free(n);
}

ART::Art_node *ART::GetChildSafe(Art_node *parent, unsigned char c) {
  if (!parent || IS_LEAF(parent)) return nullptr;

  std::shared_lock lock(parent->node_mutex);
  std::atomic<Art_node *> *slot = Find_child(parent, c);
  if (!slot) return nullptr;

  Art_node *child = slot->load(std::memory_order_acquire);
  if (child) AddRef(child);
  return child;
}

void ART::SetChildSafe(Art_node *parent, unsigned char c, Art_node *child) {
  if (!parent || IS_LEAF(parent)) return;

  std::unique_lock lock(parent->node_mutex);
  std::atomic<Art_node *> *slot = Find_child(parent, c);
  if (!slot) {
    Add_child(parent, nullptr, c, child);
    return;
  }
  Art_node *old = slot->load(std::memory_order_acquire);
  slot->store(child, std::memory_order_release);

  if (child) AddRef(child);
  if (old) Release(old);
}

std::atomic<ART::Art_node *> *ART::Find_child(Art_node *n, unsigned char c) {
  if (!n || IS_LEAF(n)) return nullptr;

  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c) return &p1->children[i];
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
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
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
      int idx = p3->keys[c];
      if (idx) return &p3->children[idx - 1];
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
      return &p4->children[c];
    }
    default:
      break;
  }
  return nullptr;
}

void ART::Find_children(Art_node *n, unsigned char c, std::vector<Art_node *> &children) {
  if (!n || IS_LEAF(n)) return;
  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        if (p1->keys[i] == c) {
          Art_node *child = p1->children[i].load(std::memory_order_acquire);
          if (child) {
            AddRef(child);
            children.emplace_back(child);
          }
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
      int bitfield = 0;
      for (int i = 0; i < n->num_children; ++i) {
        if (p2->keys[i] == c) bitfield |= (1 << i);
      }
      if (bitfield) {
        int idx = __builtin_ctz(bitfield);
        Art_node *child = p2->children[idx].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          children.emplace_back(child);
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
      int idx = p3->keys[c];
      if (idx) {
        Art_node *child = p3->children[idx - 1].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          children.emplace_back(child);
        }
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
      Art_node *child = p4->children[c].load(std::memory_order_acquire);
      if (child) {
        AddRef(child);
        children.emplace_back(child);
      }
      break;
    }
    default:
      break;
  }
}

int ART::Check_prefix(const Art_node *n, const unsigned char *key, int key_len, int depth) {
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
  Art_node *n = m_tree->root.load(std::memory_order_acquire);
  if (!n) return nullptr;

  AddRef(n);
  int prefix_len, depth = 0;

  while (n) {
    if (IS_LEAF(n)) {
      Art_leaf *leaf = LEAF_RAW(n);
      AddRefLeaf(leaf);
      std::shared_lock leaf_lock(leaf->leaf_mutex);

      void *result = nullptr;
      if (!Leaf_matches(leaf, key, key_len, depth)) {
        void **values = leaf->values.load(std::memory_order_acquire);
        if (values && leaf->vcount.load(std::memory_order_acquire) > 0) {
          result = values[0];
        }
      }

      ReleaseLeaf(leaf);
      Release(n);
      return result;
    }

    std::shared_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if ((uint)prefix_len != std::min(MAX_PREFIX_LEN, static_cast<uint>(n->partial_len))) {
        Release(n);
        return nullptr;
      }
      depth += n->partial_len;
    }

    if (depth >= key_len) {
      Release(n);
      return nullptr;
    }

    Art_node *child = GetChildSafe(n, key[depth]);
    Release(n);
    n = child;
    depth++;
  }
  return nullptr;
}

std::vector<void *> ART::ART_search_all(const unsigned char *key, int key_len) {
  std::vector<void *> results;
  if (!key || key_len <= 0 || !m_inited) return results;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  Art_node *n = m_tree->root.load(std::memory_order_acquire);
  if (!n) return results;

  AddRef(n);
  uint prefix_len, depth = 0u;

  while (n) {
    if (IS_LEAF(n)) {
      Art_leaf *l = LEAF_RAW(n);
      AddRefLeaf(l);
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

      ReleaseLeaf(l);
      Release(n);
      return results;
    }

    std::shared_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      if (prefix_len != std::min(MAX_PREFIX_LEN, n->partial_len)) {
        Release(n);
        return results;
      }
      depth += n->partial_len;
    }

    if (depth >= static_cast<uint>(key_len)) {
      Release(n);
      return results;
    }

    Art_node *child = GetChildSafe(n, key[depth]);
    Release(n);
    n = child;
    depth++;
  }
  return results;
}

ART::Art_leaf *ART::Minimum(const Art_node *n) {
  if (!n) return nullptr;
  if (IS_LEAF(n)) {
    Art_leaf *leaf = LEAF_RAW(n);
    AddRefLeaf(leaf);
    return leaf;
  }
  std::shared_lock lock(n->node_mutex);
  Art_node *child = nullptr;
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(const_cast<Art_node *>(n));
      child = p->children[0].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(const_cast<Art_node *>(n));
      child = p->children[0].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(const_cast<Art_node *>(n));
      int idx = 0;
      while (idx < 256 && !p->keys[idx]) idx++;
      if (idx < 256) {
        child = p->children[p->keys[idx] - 1].load(std::memory_order_acquire);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(const_cast<Art_node *>(n));
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

  AddRef(child);
  if (child) {
    Art_leaf *result = Minimum(child);
    Release(child);
    return result;
  }
  return nullptr;
}

ART::Art_leaf *ART::Maximum(const Art_node *n) {
  if (!n) return nullptr;
  if (IS_LEAF(n)) {
    Art_leaf *leaf = LEAF_RAW(n);
    AddRefLeaf(leaf);
    return leaf;
  }
  std::shared_lock lock(n->node_mutex);
  Art_node *child = nullptr;
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p = reinterpret_cast<Art_node4 *>(const_cast<Art_node *>(n));
      child = p->children[n->num_children - 1].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p = reinterpret_cast<Art_node16 *>(const_cast<Art_node *>(n));
      child = p->children[n->num_children - 1].load(std::memory_order_acquire);
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p = reinterpret_cast<Art_node48 *>(const_cast<Art_node *>(n));
      int idx = 255;
      while (idx >= 0 && !p->keys[idx]) idx--;
      if (idx >= 0) {
        child = p->children[p->keys[idx] - 1].load(std::memory_order_acquire);
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p = reinterpret_cast<Art_node256 *>(const_cast<Art_node *>(n));
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

  AddRef(child);
  if (child) {
    Art_leaf *result = Maximum(child);
    Release(child);
    return result;
  }
  return nullptr;
}

ART::Art_leaf *ART::ART_minimum() {
  if (!m_inited) return nullptr;
  std::shared_lock lock(m_tree->tree_mutex);
  return Minimum(m_tree->root.load(std::memory_order_acquire));
}

ART::Art_leaf *ART::ART_maximum() {
  if (!m_inited) return nullptr;
  std::shared_lock lock(m_tree->tree_mutex);
  return Maximum(m_tree->root.load(std::memory_order_acquire));
}

ART::Art_leaf *ART::Make_leaf(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;

  Art_leaf *l = static_cast<Art_leaf *>(safe_calloc(1, sizeof(Art_leaf) + key_len));
  if (!l) return nullptr;

  l->values.store(static_cast<void **>(safe_calloc(initial_capacity, sizeof(void *))), std::memory_order_release);
  void **values = l->values.load(std::memory_order_acquire);
  if (!values) {
    std::free(l);
    return nullptr;
  }

  values[0] = safe_malloc(value_len);
  if (!values[0]) {
    std::free(values);
    std::free(l);
    return nullptr;
  }

  l->value_len.store(value_len, std::memory_order_release);
  l->capacity.store(initial_capacity, std::memory_order_release);
  l->vcount.store(1, std::memory_order_release);
  l->key_len = key_len;
  l->ref_count.store(1, std::memory_order_release);

  std::memcpy(l->key, key, key_len);
  std::memcpy(values[0], value, value_len);

  return l;
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

void ART::Add_child256(Art_node256 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child) {
  std::unique_lock lock(n->n.node_mutex);
  Art_node *old = n->children[c].exchange(child, std::memory_order_acq_rel);

  if (!old) n->n.num_children++;

  if (child) AddRef(child);

  if (old) Release(old);
}

void ART::Add_child48(Art_node48 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child) {
  std::unique_lock lock(n->n.node_mutex);
  if (n->n.num_children < 48) {
    int pos = 0;
    while (pos < 48 && n->children[pos].load(std::memory_order_acquire)) pos++;
    if (pos < 48) {
      n->children[pos].store(child, std::memory_order_release);
      n->keys[c] = pos + 1;
      n->n.num_children++;
      if (child) AddRef(child);
    }
  } else {
    Art_node256 *new_node = reinterpret_cast<Art_node256 *>(Alloc_node(NodeType::NODE256));
    if (!new_node) return;

    for (int i = 0; i < 256; i++) {
      if (n->keys[i]) {
        new_node->children[i].store(n->children[n->keys[i] - 1].load(std::memory_order_acquire),
                                    std::memory_order_release);
      }
    }

    Copy_header(&new_node->n, &n->n);

    if (ref) ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

    AddRef(reinterpret_cast<Art_node *>(new_node));
    std::free(n);

    Add_child256(new_node, ref, c, child);
  }
}

void ART::Add_child16(Art_node16 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child) {
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

    if (child) AddRef(child);
  } else {
    Art_node48 *new_node = reinterpret_cast<Art_node48 *>(Alloc_node(NodeType::NODE48));
    if (!new_node) return;

    for (int i = 0; i < 16; i++) {
      new_node->children[i].store(n->children[i].load(std::memory_order_acquire), std::memory_order_release);
      new_node->keys[n->keys[i]] = i + 1;
    }

    Copy_header(&new_node->n, &n->n);

    if (ref) ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

    AddRef(reinterpret_cast<Art_node *>(new_node));
    lock.unlock();

    Add_child48(new_node, ref, c, child);
  }
}

void ART::Add_child4(Art_node4 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child) {
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

    if (child) AddRef(child);
  } else {
    Art_node16 *new_node = reinterpret_cast<Art_node16 *>(Alloc_node(NodeType::NODE16));
    if (!new_node) return;

    for (int i = 0; i < 4; i++) {
      new_node->children[i].store(n->children[i].load(std::memory_order_acquire), std::memory_order_release);
      new_node->keys[i] = n->keys[i];
    }

    Copy_header(&new_node->n, &n->n);

    if (ref) ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

    AddRef(reinterpret_cast<Art_node *>(new_node));

    Add_child16(new_node, ref, c, child);
  }
}

void ART::Add_child(Art_node *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child) {
  if (!n) return;
  assert(!IS_LEAF(n));

  switch (n->type) {
    case NodeType::NODE4:
      Add_child4(reinterpret_cast<Art_node4 *>(n), ref, c, child);
      break;
    case NodeType::NODE16:
      Add_child16(reinterpret_cast<Art_node16 *>(n), ref, c, child);
      break;
    case NodeType::NODE48:
      Add_child48(reinterpret_cast<Art_node48 *>(n), ref, c, child);
      break;
    case NodeType::NODE256:
      Add_child256(reinterpret_cast<Art_node256 *>(n), ref, c, child);
      break;
    default:
      std::abort();
  }
}

int ART::Prefix_mismatch(const Art_node *n, const unsigned char *key, int key_len, int depth) {
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
          ReleaseLeaf(leaf);
          return idx;
        }
      }

      ReleaseLeaf(leaf);
    }
  }

  return idx;
}

void *ART::ART_insert(const unsigned char *key, int key_len, void *value, uint value_len) {
  if (!key || key_len <= 0 || !value || value_len == 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  int old_val = 0;
  std::atomic<Art_node *> &root_ref = m_tree->root;
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
  std::atomic<Art_node *> &root_ref = m_tree->root;
  void *old = Recursive_insert(root_ref.load(std::memory_order_acquire), &root_ref, key, key_len, value, value_len, 0,
                               &old_val, 1);

  if (!old_val) {
    m_tree->size.fetch_add(1, std::memory_order_acq_rel);
  }

  return old;
}

void *ART::Recursive_insert(Art_node *n, std::atomic<Art_node *> *ref, const unsigned char *key, int key_len,
                            void *value, int value_len, int depth, int *old, int replace) {
  if (!n) {
    Art_leaf *new_leaf = Make_leaf(key, key_len, value, value_len);
    if (new_leaf) {
      ref->store(reinterpret_cast<Art_node *>(SET_LEAF(new_leaf)), std::memory_order_release);
      *old = 0;
    }

    return nullptr;
  }

  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    AddRefLeaf(l);

    std::unique_lock leaf_lock(l->leaf_mutex);
    if (!Leaf_matches(l, key, key_len, depth)) {
      if (replace) {
        void **values = l->values.load(std::memory_order_acquire);
        void *old_value = values[0];

        values[0] = safe_malloc(value_len);
        if (!values[0]) {
          ReleaseLeaf(l);
          return nullptr;
        }

        std::memcpy(values[0], value, value_len);
        l->value_len.store(value_len, std::memory_order_release);
        *old = 1;

        ReleaseLeaf(l);
        return old_value;
      } else {
        uint32_t current_count = l->vcount.load(std::memory_order_acquire);
        uint32_t current_capacity = l->capacity.load(std::memory_order_acquire);

        void **values = l->values.load(std::memory_order_acquire);
        if (current_count == current_capacity) {
          uint32_t new_capacity = current_capacity * 2;
          void **new_values = static_cast<void **>(safe_realloc(values, new_capacity * sizeof(void *)));
          if (!new_values) {
            ReleaseLeaf(l);
            return nullptr;
          }

          l->values.store(new_values, std::memory_order_release);
          l->capacity.store(new_capacity, std::memory_order_release);
          values = new_values;
        }

        values[current_count] = safe_malloc(value_len);
        if (!values[current_count]) {
          ReleaseLeaf(l);
          return nullptr;
        }

        std::memcpy(values[current_count], value, value_len);
        l->vcount.store(current_count + 1, std::memory_order_release);
        *old = 0;

        ReleaseLeaf(l);
        return nullptr;
      }
    }

    Art_node4 *new_node = reinterpret_cast<Art_node4 *>(Alloc_node(NodeType::NODE4));
    if (!new_node) {
      ReleaseLeaf(l);
      return nullptr;
    }

    Art_leaf *l2 = Make_leaf(key, key_len, value, value_len);
    if (!l2) {
      ReleaseLeaf(l);
      std::free(new_node);
      return nullptr;
    }

    int longest_prefix = Longest_common_prefix(l, l2, depth);
    new_node->n.partial_len = longest_prefix;
    int min_len = std::min(static_cast<int>(MAX_PREFIX_LEN), longest_prefix);
    std::memcpy(new_node->n.partial, key + depth, min_len);
    unsigned char c1 = l->key[depth + longest_prefix];
    unsigned char c2 = l2->key[depth + longest_prefix];

    if (c1 < c2) {
      new_node->keys[0] = c1;
      new_node->keys[1] = c2;
      new_node->children[0].store(reinterpret_cast<Art_node *>(SET_LEAF(l)), std::memory_order_release);
      new_node->children[1].store(reinterpret_cast<Art_node *>(SET_LEAF(l2)), std::memory_order_release);
    } else {
      new_node->keys[0] = c2;
      new_node->keys[1] = c1;
      new_node->children[0].store(reinterpret_cast<Art_node *>(SET_LEAF(l2)), std::memory_order_release);
      new_node->children[1].store(reinterpret_cast<Art_node *>(SET_LEAF(l)), std::memory_order_release);
    }

    new_node->n.num_children = 2;
    ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

    AddRef(reinterpret_cast<Art_node *>(new_node));
    *old = 0;
    ReleaseLeaf(l);

    return nullptr;
  }

  {
    std::unique_lock node_lock(n->node_mutex);
    if (n->partial_len) {
      int prefix_diff = Prefix_mismatch(n, key, key_len, depth);
      if (static_cast<uint32_t>(prefix_diff) < n->partial_len) {
        Art_node4 *new_node = reinterpret_cast<Art_node4 *>(Alloc_node(NodeType::NODE4));
        if (!new_node) return nullptr;

        new_node->n.partial_len = prefix_diff;
        std::memcpy(new_node->n.partial, n->partial, prefix_diff);

        n->partial_len = n->partial_len - prefix_diff - 1;
        std::memmove(n->partial, n->partial + prefix_diff + 1,
                     std::min(static_cast<int>(MAX_PREFIX_LEN), static_cast<int>(n->partial_len)));

        new_node->keys[0] = n->partial[prefix_diff];
        new_node->children[0].store(n, std::memory_order_release);
        new_node->n.num_children = 1;
        AddRef(n);

        Art_leaf *new_leaf = Make_leaf(key, key_len, value, value_len);
        if (!new_leaf) {
          std::free(new_node);
          return nullptr;
        }

        new_node->keys[1] = key[depth + prefix_diff];
        new_node->children[1].store(reinterpret_cast<Art_node *>(SET_LEAF(new_leaf)), std::memory_order_release);
        new_node->n.num_children = 2;
        ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

        AddRef(reinterpret_cast<Art_node *>(new_node));
        *old = 0;

        return nullptr;
      }
      depth += n->partial_len;
    }
  }
  if (depth >= key_len) return nullptr;

  std::atomic<Art_node *> *child_ref = Find_child(n, key[depth]);
  Art_node *child = child_ref ? child_ref->load(std::memory_order_acquire) : nullptr;
  if (!child) {
    Art_leaf *new_leaf = Make_leaf(key, key_len, value, value_len);
    if (new_leaf) {
      Add_child(n, ref, key[depth], reinterpret_cast<Art_node *>(SET_LEAF(new_leaf)));
      *old = 0;
    }
    return nullptr;
  }

  AddRef(child);

  void *result =
      Recursive_insert(child, Find_child(n, key[depth]), key, key_len, value, value_len, depth + 1, old, replace);
  Release(child);

  return result;
}

void ART::Remove_child256(Art_node256 *n, std::atomic<Art_node *> *ref, unsigned char c) {
  std::unique_lock lock(n->n.node_mutex);
  Art_node *old = n->children[c].exchange(nullptr, std::memory_order_acq_rel);
  if (old) {
    n->n.num_children--;
    Release(old);
  }

  if (n->n.num_children < 37 && ref) {
    Art_node48 *new_node = reinterpret_cast<Art_node48 *>(Alloc_node(NodeType::NODE48));
    if (!new_node) return;
    int pos = 0;
    for (int i = 0; i < 256 && pos < 48; i++) {
      Art_node *child = n->children[i].load(std::memory_order_acquire);
      if (child) {
        new_node->children[pos].store(child, std::memory_order_release);
        new_node->keys[i] = pos + 1;
        pos++;
      }
    }

    Copy_header(&new_node->n, &n->n);
    ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);

    AddRef(reinterpret_cast<Art_node *>(new_node));
    std::free(n);
  }
}

void ART::Remove_child48(Art_node48 *n, std::atomic<Art_node *> *ref, unsigned char c) {
  std::unique_lock lock(n->n.node_mutex);
  int pos = n->keys[c];
  if (pos) {
    pos--;
    Art_node *old = n->children[pos].exchange(nullptr, std::memory_order_acq_rel);
    n->keys[c] = 0;
    n->n.num_children--;
    if (old) Release(old);
  }

  if (n->n.num_children < 13 && ref) {
    Art_node16 *new_node = reinterpret_cast<Art_node16 *>(Alloc_node(NodeType::NODE16));
    if (!new_node) return;
    int pos = 0;
    for (int i = 0; i < 256 && pos < 16; i++) {
      if (n->keys[i]) {
        new_node->keys[pos] = i;
        new_node->children[pos].store(n->children[n->keys[i] - 1].load(std::memory_order_acquire),
                                      std::memory_order_release);
        pos++;
      }
    }

    Copy_header(&new_node->n, &n->n);
    ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);
    AddRef(reinterpret_cast<Art_node *>(new_node));
    std::free(n);
  }
}

void ART::Remove_child16(Art_node16 *n, std::atomic<Art_node *> *ref, std::atomic<Art_node *> *child_ref) {
  std::unique_lock lock(n->n.node_mutex);
  Art_node *old = child_ref->exchange(nullptr, std::memory_order_acq_rel);
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
    if (old) Release(old);
  }

  if (n->n.num_children < 5 && ref) {
    Art_node4 *new_node = reinterpret_cast<Art_node4 *>(Alloc_node(NodeType::NODE4));
    if (!new_node) return;
    for (int i = 0; i < n->n.num_children; i++) {
      new_node->keys[i] = n->keys[i];
      new_node->children[i].store(n->children[i].load(std::memory_order_acquire), std::memory_order_release);
    }

    Copy_header(&new_node->n, &n->n);
    ref->store(reinterpret_cast<Art_node *>(new_node), std::memory_order_release);
    AddRef(reinterpret_cast<Art_node *>(new_node));
    std::free(n);
  }
}

void ART::Remove_child4(Art_node4 *n, std::atomic<Art_node *> *ref, std::atomic<Art_node *> *child_ref) {
  std::unique_lock lock(n->n.node_mutex);
  Art_node *old = child_ref->exchange(nullptr, std::memory_order_acq_rel);
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
    if (old) Release(old);
  }

  if (n->n.num_children == 1 && ref) {
    Art_node *child = n->children[0].load(std::memory_order_acquire);
    if (!IS_LEAF(child)) {
      Copy_header(child, &n->n);
      ref->store(child, std::memory_order_release);
      AddRef(child);
      std::free(n);
    }
  }
}

void ART::Remove_child(Art_node *n, std::atomic<Art_node *> *ref, unsigned char c, std::atomic<Art_node *> *child_ref) {
  if (!n) return;
  switch (n->type) {
    case NodeType::NODE4:
      Remove_child4(reinterpret_cast<Art_node4 *>(n), ref, child_ref);
      break;
    case NodeType::NODE16:
      Remove_child16(reinterpret_cast<Art_node16 *>(n), ref, child_ref);
      break;
    case NodeType::NODE48:
      Remove_child48(reinterpret_cast<Art_node48 *>(n), ref, c);
      break;
    case NodeType::NODE256:
      Remove_child256(reinterpret_cast<Art_node256 *>(n), ref, c);
      break;
    default:
      std::abort();
  }
}

void ART::RemoveChildSafe(Art_node *parent, unsigned char c) {
  if (!parent || IS_LEAF(parent)) return;

  std::unique_lock lock(parent->node_mutex);
  std::atomic<Art_node *> *child_ref = Find_child(parent, c);

  if (!child_ref) return;
  Remove_child(parent, nullptr, c, child_ref);
}

void *ART::ART_delete(const unsigned char *key, int key_len) {
  if (!key || key_len <= 0 || !m_inited) return nullptr;

  std::unique_lock tree_lock(m_tree->tree_mutex);
  std::atomic<Art_node *> &root_ref = m_tree->root;
  Art_leaf *l = Recursive_delete(root_ref.load(std::memory_order_acquire), &root_ref, key, key_len, 0);

  if (l) {
    m_tree->size.fetch_sub(1, std::memory_order_acq_rel);
    std::unique_lock leaf_lock(l->leaf_mutex);

    void **values = l->values.load(std::memory_order_acquire);
    void *result = nullptr;

    if (values && l->vcount.load(std::memory_order_acquire) > 0) {
      result = values[0];
      uint32_t vcount = l->vcount.load(std::memory_order_acquire);
      for (uint32_t i = 0; i < vcount; ++i) {
        if (values[i]) std::free(values[i]);
      }
      std::free(values);
    }

    std::free(l);
    return result;
  }

  return nullptr;
}

ART::Art_leaf *ART::Recursive_delete(Art_node *n, std::atomic<Art_node *> *ref, const unsigned char *key, int key_len,
                                     int depth) {
  if (!n) return nullptr;

  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    AddRefLeaf(l);

    std::shared_lock leaf_lock(l->leaf_mutex);
    bool match = !Leaf_matches(l, key, key_len, depth);
    if (match) {
      ref->store(nullptr, std::memory_order_release);
      return l;
    }
    ReleaseLeaf(l);
    return nullptr;
  }

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

  std::atomic<Art_node *> *child_ref = Find_child(n, key[depth]);
  Art_node *child = child_ref ? child_ref->load(std::memory_order_acquire) : nullptr;
  if (!child) return nullptr;

  Art_leaf *result = Recursive_delete(child, child_ref, key, key_len, depth + 1);
  if (result) {
    std::unique_lock parent_lock(n->node_mutex);
    Remove_child(n, ref, key[depth], child_ref);
  }

  Release(child);
  return result;
}

void ART::Free_leaf(Art_leaf *l) {
  if (!l) return;

  std::unique_lock lock(l->leaf_mutex);
  void **values = l->values.load(std::memory_order_acquire);
  uint32_t vcount = l->vcount.load(std::memory_order_acquire);
  if (values) {
    for (uint32_t i = 0; i < vcount; ++i) {
      if (values[i]) std::free(values[i]);
    }
    std::free(values);
  }

  std::free(l);
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

int ART::Recursive_iter(Art_node *n, ART_Func &cb, void *data) {
  if (!n) return 0;

  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    AddRefLeaf(l);

    std::shared_lock leaf_lock(l->leaf_mutex);
    void **values = l->values.load(std::memory_order_acquire);
    uint32_t vcount = l->vcount.load(std::memory_order_acquire);

    int ret = 0;
    if (values && vcount > 0 && values[0]) {
      ret = cb(data, l, l->key, l->key_len, values[0], l->value_len.load(std::memory_order_acquire));
    }

    ReleaseLeaf(l);
    return ret;
  }

  std::shared_lock lock(n->node_mutex);
  switch (n->type) {
    case NodeType::NODE4: {
      Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        Art_node *child = p1->children[i].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          int ret = Recursive_iter(child, cb, data);
          Release(child);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE16: {
      Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
      for (int i = 0; i < n->num_children; i++) {
        Art_node *child = p2->children[i].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          int ret = Recursive_iter(child, cb, data);
          Release(child);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE48: {
      Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
      for (int i = 0; i < 256; i++) {
        int idx = p3->keys[i];
        if (!idx) continue;
        Art_node *child = p3->children[idx - 1].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          int ret = Recursive_iter(child, cb, data);
          Release(child);
          if (ret != 0) return ret;
        }
      }
      break;
    }
    case NodeType::NODE256: {
      Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
      for (int i = 0; i < 256; i++) {
        Art_node *child = p4->children[i].load(std::memory_order_acquire);
        if (child) {
          AddRef(child);
          int ret = Recursive_iter(child, cb, data);
          Release(child);
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

int ART::Recursive_iter_with_key(Art_node *n, const unsigned char *key, int key_len) {
  if (!n) return 0;

  std::shared_lock lock(n->node_mutex);
  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    AddRefLeaf(l);
    std::shared_lock leaf_lock(l->leaf_mutex);
    if (!Leaf_prefix_matches(l, key, key_len)) {
      m_current_values.push_back(l);
    } else {
      ReleaseLeaf(l);
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

  std::vector<Art_node *> children;
  if (key_len > 0) {
    Find_children(n, key[0], children);
  } else {
    switch (n->type) {
      case NodeType::NODE4: {
        Art_node4 *p1 = reinterpret_cast<Art_node4 *>(n);
        for (int i = 0; i < n->num_children; i++) {
          Art_node *child = p1->children[i].load(std::memory_order_acquire);
          if (child) {
            AddRef(child);
            children.push_back(child);
          }
        }
        break;
      }
      case NodeType::NODE16: {
        Art_node16 *p2 = reinterpret_cast<Art_node16 *>(n);
        for (int i = 0; i < n->num_children; i++) {
          Art_node *child = p2->children[i].load(std::memory_order_acquire);
          if (child) {
            AddRef(child);
            children.push_back(child);
          }
        }
        break;
      }
      case NodeType::NODE48: {
        Art_node48 *p3 = reinterpret_cast<Art_node48 *>(n);
        for (int i = 0; i < 256; i++) {
          int idx = p3->keys[i];
          if (!idx) continue;
          Art_node *child = p3->children[idx - 1].load(std::memory_order_acquire);
          if (child) {
            AddRef(child);
            children.push_back(child);
          }
        }
        break;
      }
      case NodeType::NODE256: {
        Art_node256 *p4 = reinterpret_cast<Art_node256 *>(n);
        for (int i = 0; i < 256; i++) {
          Art_node *child = p4->children[i].load(std::memory_order_acquire);
          if (child) {
            AddRef(child);
            children.push_back(child);
          }
        }
        break;
      }
      default:
        std::abort();
    }
  }

  for (Art_node *child : children) {
    int ret = Recursive_iter_with_key(child, key, key_len);
    Release(child);
    if (ret != 0) return ret;
  }
  return 0;
}

int ART::ART_iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len) {
  if (!key || key_len <= 0 || !m_inited) return 1;

  std::shared_lock tree_lock(m_tree->tree_mutex);
  Art_node *n = m_tree->root.load(std::memory_order_acquire);
  int prefix_len, depth = 0;

  while (n) {
    if (IS_LEAF(n)) {
      Art_leaf *l = LEAF_RAW(n);
      AddRefLeaf(l);
      std::shared_lock leaf_lock(l->leaf_mutex);

      int ret = 0;
      if (!Leaf_prefix_matches(l, key, key_len)) {
        void **values = l->values.load(std::memory_order_acquire);
        if (values && l->vcount.load(std::memory_order_acquire) > 0) {
          ret = cb(data, l, l->key, l->key_len, values[0], data_len);
        }
      }

      ReleaseLeaf(l);
      return ret;
    }

    if (depth == key_len) {
      Art_leaf *l = Minimum(n);
      if (l) {
        int ret = 0;
        if (!Leaf_prefix_matches(l, key, key_len)) {
          ret = Recursive_iter(n, cb, data);
        }
        ReleaseLeaf(l);
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

    Art_node *child = GetChildSafe(n, key[depth]);
    n = child;
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