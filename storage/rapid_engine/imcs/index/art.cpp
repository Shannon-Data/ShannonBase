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

#include "storage/rapid_engine/imcs/index/art.h"

namespace ShannonBase {
namespace Imcs {

#define IS_LEAF(x) (((uintptr_t)x & 1))
#define SET_LEAF(x) ((void *)((uintptr_t)x | 1))
#define LEAF_RAW(x) ((Art_leaf *)((void *)((uintptr_t)x & ~1)))

Art_index::Art_node *Art_index::Alloc_node(NodeType type) {
  Art_node *n;
  switch (type) {
    case NODE4:
      n = (Art_node *)calloc(1, sizeof(Art_node4));
      break;
    case NODE16:
      n = (Art_node *)calloc(1, sizeof(Art_node16));
      break;
    case NODE48:
      n = (Art_node *)calloc(1, sizeof(Art_node48));
      break;
    case NODE256:
      n = (Art_node *)calloc(1, sizeof(Art_node256));
      break;
    default:
      abort();
  }
  n->type = type;
  return n;
}
void Art_index::Destroy_node(Art_node *n) {
  // Break if null
  if (!n) return;

  // Special case leafs
  if (IS_LEAF(n)) {
    free(LEAF_RAW(n));
    return;
  }

  // Handle each node type
  int i, idx;
  union {
    Art_node4 *p1;
    Art_node16 *p2;
    Art_node48 *p3;
    Art_node256 *p4;
  } p;
  switch (n->type) {
    case NodeType::NODE4:
      p.p1 = (Art_node4 *)n;
      for (i = 0; i < n->num_children; i++) {
        Destroy_node(p.p1->children[i]);
      }
      break;
    case NodeType::NODE16:
      p.p2 = (Art_node16 *)n;
      for (i = 0; i < n->num_children; i++) {
        Destroy_node(p.p2->children[i]);
      }
      break;
    case NodeType::NODE48:
      p.p3 = (Art_node48 *)n;
      for (i = 0; i < 256; i++) {
        idx = ((Art_node48 *)n)->keys[i];
        if (!idx) continue;
        Destroy_node(p.p3->children[idx - 1]);
      }
      break;
    case NodeType::NODE256:
      p.p4 = (Art_node256 *)n;
      for (i = 0; i < 256; i++) {
        if (p.p4->children[i]) Destroy_node(p.p4->children[i]);
      }
      break;
    default:
      abort();
  }
  // Free ourself on the way up
  free(n);
}

int Art_index::ART_tree_init() {
  if (!m_tree) {
    m_tree = new Art_tree();
  }
  m_tree->root = nullptr;
  m_tree->size = 0;
  m_inited = true;
  ART_reset_cursor();
  return 0;
}
int Art_index::ART_tree_destroy() {
  Destroy_node(m_tree->root);
  if (m_tree) {
    delete m_tree;
    m_tree = nullptr;
    m_inited = false;
  }
  return 0;
}
void Art_index::ART_reset_cursor() {
  while (!m_current_nodes.empty()) {
    m_current_nodes.pop();
  }
  m_current_nodes.emplace(m_tree->root);
}
Art_index::Art_node **Art_index::Find_child(Art_node *n, unsigned char c) {
  int i, mask, bitfield;
  union {
    Art_node4 *p1;
    Art_node16 *p2;
    Art_node48 *p3;
    Art_node256 *p4;
  } p;
  switch (n->type) {
    case NodeType::NODE4:
      p.p1 = (Art_node4 *)n;
      for (i = 0; i < n->num_children; i++) {
        /* this cast works around a bug in gcc 5.1 when unrolling loops
         * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=59124
         */
        if (((unsigned char *)p.p1->keys)[i] == c) return &p.p1->children[i];
      }
      break;
      {
        case NodeType::NODE16:
          p.p2 = (Art_node16 *)n;

// support non-86 architectures
#ifdef __i386__
          // Compare the key to all 16 stored keys
          __m128i cmp;
          cmp = _mm_cmpeq_epi8(_mm_set1_epi8(c),
                               _mm_loadu_si128((__m128i *)p.p2->keys));

          // Use a mask to ignore children that don't exist
          mask = (1 << n->num_children) - 1;
          bitfield = _mm_movemask_epi8(cmp) & mask;
#else
#ifdef __amd64__
          // Compare the key to all 16 stored keys
          __m128i cmp;
          cmp = _mm_cmpeq_epi8(_mm_set1_epi8(c),
                               _mm_loadu_si128((__m128i *)p.p2->keys));

          // Use a mask to ignore children that don't exist
          mask = (1 << n->num_children) - 1;
          bitfield = _mm_movemask_epi8(cmp) & mask;
#else
          // Compare the key to all 16 stored keys
          bitfield = 0;
          for (i = 0; i < 16; ++i) {
            if (p.p2->keys[i] == c) bitfield |= (1 << i);
          }

          // Use a mask to ignore children that don't exist
          mask = (1 << n->num_children) - 1;
          bitfield &= mask;
#endif
#endif

          /*
           * If we have a match (any bit set) then we can
           * return the pointer match using ctz to get
           * the index.
           */
          if (bitfield) return &p.p2->children[__builtin_ctz(bitfield)];
          break;
      }
    case NodeType::NODE48:
      p.p3 = (Art_node48 *)n;
      i = p.p3->keys[c];
      if (i) return &p.p3->children[i - 1];
      break;
    case NodeType::NODE256:
      p.p4 = (Art_node256 *)n;
      if (p.p4->children[c]) return &p.p4->children[c];
      break;
    default:
      abort();
  }
  return nullptr;
}
int Art_index::Check_prefix(const Art_node *n, const unsigned char *key,
                            int key_len, int depth) {
  int min_tmp = std::min(n->partial_len, Art_index::MAX_PREFIX_LEN);
  int max_cmp = std::min(min_tmp, key_len - depth);
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (n->partial[idx] != key[depth + idx]) return idx;
  }
  return idx;
}

int Art_index::Leaf_matches(const Art_leaf *n, const unsigned char *key,
                            int key_len, int depth) {
  (void)depth;
  // Fail if the key lengths are different
  if (n->key_len != (uint32)key_len) return 1;

  // Compare the keys starting at the depth
  return std::memcmp(n->key, key, key_len);
}

int Art_index::Leaf_partial_matches(const Art_leaf *n, const unsigned char *key,
                                    uint key_offset, int key_len, int depth) {
  (void)depth;
  // Fail if the key lengths are different
  // if it's composite index, such as, (col1, colN). query xxx from where colN.
  // if (n->key_len != (key_offset + (uint32)key_len)) return 1;

  // Compare the keys starting at the depth
  return std::memcmp((n->key + key_offset), key, key_len);
}

void *Art_index::ART_search(const unsigned char *key, int key_len) {
  Art_node **child;
  Art_node *n = m_tree->root;
  int prefix_len, depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      n = (Art_node *)LEAF_RAW(n);
      // Check if the expanded path matches
      if (!Leaf_matches((Art_leaf *)n, key, key_len, depth)) {
        return ((Art_leaf *)n)->value;
      }
      return nullptr;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      int min_v = std::min(MAX_PREFIX_LEN, n->partial_len);
      if (prefix_len != min_v) return nullptr;
      depth = depth + n->partial_len;
    }

    // Recursively search
    child = Find_child(n, key[depth]);
    n = (child) ? *child : nullptr;
    depth++;
  }
  return nullptr;
}

Art_index::Art_leaf *Art_index::Minimum(const Art_node *n) {
  // Handle base cases
  if (!n) return NULL;
  if (IS_LEAF(n)) return LEAF_RAW(n);

  int idx;
  switch (n->type) {
    case NodeType::NODE4:
      return Minimum(((const Art_node4 *)n)->children[0]);
    case NodeType::NODE16:
      return Minimum(((const Art_node16 *)n)->children[0]);
    case NodeType::NODE48:
      idx = 0;
      while (!((const Art_node48 *)n)->keys[idx]) idx++;
      idx = ((const Art_node48 *)n)->keys[idx] - 1;
      return Minimum(((const Art_node48 *)n)->children[idx]);
    case NodeType::NODE256:
      idx = 0;
      while (!((const Art_node256 *)n)->children[idx]) idx++;
      return Minimum(((const Art_node256 *)n)->children[idx]);
    default:
      abort();
  }
}
Art_index::Art_leaf *Art_index::Maximum(const Art_node *n) {
  // Handle base cases
  if (!n) return NULL;
  if (IS_LEAF(n)) return LEAF_RAW(n);

  int idx;
  switch (n->type) {
    case NODE4:
      return Maximum(((const Art_node4 *)n)->children[n->num_children - 1]);
    case NODE16:
      return Maximum(((const Art_node16 *)n)->children[n->num_children - 1]);
    case NODE48:
      idx = 255;
      while (!((const Art_node48 *)n)->keys[idx]) idx--;
      idx = ((const Art_node48 *)n)->keys[idx] - 1;
      return Maximum(((const Art_node48 *)n)->children[idx]);
    case NODE256:
      idx = 255;
      while (!((const Art_node256 *)n)->children[idx]) idx--;
      return Maximum(((const Art_node256 *)n)->children[idx]);
    default:
      abort();
  }
}
Art_index::Art_leaf *Art_index::ART_minimum() {
  return Minimum((Art_node *)m_tree->root);
}

Art_index::Art_leaf *Art_index::ART_maximum() {
  return Maximum((Art_node *)m_tree->root);
}

Art_index::Art_leaf *Art_index::Make_leaf(const unsigned char *key, int key_len,
                                          void *value) {
  Art_leaf *l = (Art_leaf *)calloc(1, sizeof(Art_leaf) + key_len);
  l->value = value;
  l->key_len = key_len;
  memcpy(l->key, key, key_len);
  return l;
}

int Art_index::Longest_common_prefix(Art_leaf *l1, Art_leaf *l2, int depth) {
  int max_cmp = std::min(l1->key_len, l2->key_len) - depth;
  int idx;
  for (idx = 0; idx < max_cmp; idx++) {
    if (l1->key[depth + idx] != l2->key[depth + idx]) return idx;
  }
  return idx;
}

void Art_index::Copy_header(Art_node *dest, Art_node *src) {
  dest->num_children = src->num_children;
  dest->partial_len = src->partial_len;
  std::memcpy(dest->partial, src->partial,
              std::min(MAX_PREFIX_LEN, src->partial_len));
}

void Art_index::Add_child256(Art_node256 *n, Art_node **ref, unsigned char c,
                             void *child) {
  (void)ref;
  n->n.num_children++;
  n->children[c] = (Art_node *)child;
}

void Art_index::Add_child48(Art_node48 *n, Art_node **ref, unsigned char c,
                            void *child) {
  if (n->n.num_children < 48) {
    int pos = 0;
    while (n->children[pos]) pos++;
    n->children[pos] = (Art_node *)child;
    n->keys[c] = pos + 1;
    n->n.num_children++;
  } else {
    Art_node256 *new_node = (Art_node256 *)Alloc_node(NodeType::NODE256);
    for (int i = 0; i < 256; i++) {
      if (n->keys[i]) {
        new_node->children[i] = n->children[n->keys[i] - 1];
      }
    }
    Copy_header((Art_node *)new_node, (Art_node *)n);
    *ref = (Art_node *)new_node;
    free(n);
    Add_child256(new_node, ref, c, child);
  }
}

void Art_index::Add_child16(Art_node16 *n, Art_node **ref, unsigned char c,
                            void *child) {
  if (n->n.num_children < 16) {
    unsigned mask = (1 << n->n.num_children) - 1;

// support non-x86 architectures
#ifdef __i386__
    __m128i cmp;

    // Compare the key to all 16 stored keys
    cmp = _mm_cmplt_epi8(_mm_set1_epi8(c), _mm_loadu_si128((__m128i *)n->keys));

    // Use a mask to ignore children that don't exist
    unsigned bitfield = _mm_movemask_epi8(cmp) & mask;
#else
#ifdef __amd64__
    __m128i cmp;

    // Compare the key to all 16 stored keys
    cmp = _mm_cmplt_epi8(_mm_set1_epi8(c), _mm_loadu_si128((__m128i *)n->keys));

    // Use a mask to ignore children that don't exist
    unsigned bitfield = _mm_movemask_epi8(cmp) & mask;
#else
    // Compare the key to all 16 stored keys
    unsigned bitfield = 0;
    for (short i = 0; i < 16; ++i) {
      if (c < n->keys[i]) bitfield |= (1 << i);
    }

    // Use a mask to ignore children that don't exist
    bitfield &= mask;
#endif
#endif

    // Check if less than any
    unsigned idx;
    if (bitfield) {
      idx = __builtin_ctz(bitfield);
      memmove(n->keys + idx + 1, n->keys + idx, n->n.num_children - idx);
      memmove(n->children + idx + 1, n->children + idx,
              (n->n.num_children - idx) * sizeof(void *));
    } else
      idx = n->n.num_children;

    // Set the child
    n->keys[idx] = c;
    n->children[idx] = (Art_node *)child;
    n->n.num_children++;
  } else {
    Art_node48 *new_node = (Art_node48 *)Alloc_node(NodeType::NODE48);
    // Copy the child pointers and populate the key map
    memcpy(new_node->children, n->children, sizeof(void *) * n->n.num_children);
    for (int i = 0; i < n->n.num_children; i++) {
      new_node->keys[n->keys[i]] = i + 1;
    }
    Copy_header((Art_node *)new_node, (Art_node *)n);
    *ref = (Art_node *)new_node;
    free(n);
    Add_child48(new_node, ref, c, child);
  }
}
void Art_index::Add_child4(Art_node4 *n, Art_node **ref, unsigned char c,
                           void *child) {
  if (n->n.num_children < 4) {
    int idx;
    for (idx = 0; idx < n->n.num_children; idx++) {
      if (c < n->keys[idx]) break;
    }

    // Shift to make room
    memmove(n->keys + idx + 1, n->keys + idx, n->n.num_children - idx);
    memmove(n->children + idx + 1, n->children + idx,
            (n->n.num_children - idx) * sizeof(void *));

    // Insert element
    n->keys[idx] = c;
    n->children[idx] = (Art_node *)child;
    n->n.num_children++;

  } else {
    Art_node16 *new_node = (Art_node16 *)Alloc_node(NodeType::NODE16);
    // Copy the child pointers and the key map
    memcpy(new_node->children, n->children, sizeof(void *) * n->n.num_children);
    memcpy(new_node->keys, n->keys, sizeof(unsigned char) * n->n.num_children);
    Copy_header((Art_node *)new_node, (Art_node *)n);
    *ref = (Art_node *)new_node;
    free(n);
    Add_child16(new_node, ref, c, child);
  }
}
void Art_index::Add_child(Art_node *n, Art_node **ref, unsigned char c,
                          void *child) {
  switch (n->type) {
    case NodeType::NODE4:
      return Add_child4((Art_node4 *)n, ref, c, child);
    case NodeType::NODE16:
      return Add_child16((Art_node16 *)n, ref, c, child);
    case NodeType::NODE48:
      return Add_child48((Art_node48 *)n, ref, c, child);
    case NodeType::NODE256:
      return Add_child256((Art_node256 *)n, ref, c, child);
    default:
      abort();
  }
}
int Art_index::Prefix_mismatch(const Art_node *n, const unsigned char *key,
                               int key_len, int depth) {
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
    int min_key_len = std::min((int)l->key_len, key_len);
    max_cmp = min_key_len - depth;
    for (; idx < max_cmp; idx++) {
      if (l->key[idx + depth] != key[depth + idx]) return idx;
    }
  }
  return idx;
}
void *Art_index::Recursive_insert(Art_node *n, Art_node **ref,
                                  const unsigned char *key, int key_len,
                                  void *value, int depth, int *old,
                                  int replace) {
  // If we are at a NULL node, inject a leaf
  if (!n) {
    *ref = (Art_node *)SET_LEAF(Make_leaf(key, key_len, value));
    return nullptr;
  }

  // If we are at a leaf, we need to replace it with a node
  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    // Check if we are updating an existing value
    if (!Leaf_matches(l, key, key_len, depth)) {
      *old = 1;
      void *old_val = l->value;
      if (replace) l->value = value;
      return old_val;
    }

    // New value, we must split the leaf into a node4
    Art_node4 *new_node = (Art_node4 *)Alloc_node(NodeType::NODE4);

    // Create a new leaf
    Art_leaf *l2 = Make_leaf(key, key_len, value);

    // Determine longest prefix
    int longest_prefix = Longest_common_prefix(l, l2, depth);
    new_node->n.partial_len = longest_prefix;
    int min_len = std::min((int)Art_index::MAX_PREFIX_LEN, longest_prefix);
    memcpy(new_node->n.partial, key + depth, min_len);
    // Add the leafs to the new node4
    *ref = (Art_node *)new_node;
    Add_child4(new_node, ref, l->key[depth + longest_prefix], SET_LEAF(l));
    Add_child4(new_node, ref, l2->key[depth + longest_prefix], SET_LEAF(l2));
    return nullptr;
  }

  // Check if given node has a prefix
  if (n->partial_len) {
    // Determine if the prefixes differ, since we need to split
    int prefix_diff = Prefix_mismatch(n, key, key_len, depth);
    if ((uint32)prefix_diff >= n->partial_len) {
      depth += n->partial_len;
      goto RECURSE_SEARCH;
    }

    // Create a new node
    Art_node4 *new_node = (Art_node4 *)Alloc_node(NodeType::NODE4);
    *ref = (Art_node *)new_node;
    new_node->n.partial_len = prefix_diff;
    int min_len = std::min((int)Art_index::MAX_PREFIX_LEN, prefix_diff);
    memcpy(new_node->n.partial, n->partial, min_len);

    // Adjust the prefix of the old node
    if (n->partial_len <= Art_index::MAX_PREFIX_LEN) {
      Add_child4(new_node, ref, n->partial[prefix_diff], n);
      n->partial_len -= (prefix_diff + 1);
      int min_len =
          std::min((int)Art_index::MAX_PREFIX_LEN, (int)n->partial_len);
      memmove(n->partial, n->partial + prefix_diff + 1, min_len);
    } else {
      n->partial_len -= (prefix_diff + 1);
      Art_leaf *l = Minimum(n);
      Add_child4(new_node, ref, l->key[depth + prefix_diff], n);
      int min_len =
          std::min((int)Art_index::MAX_PREFIX_LEN, (int)n->partial_len);
      memcpy(n->partial, l->key + depth + prefix_diff + 1, min_len);
    }

    // Insert the new leaf
    Art_leaf *l = Make_leaf(key, key_len, value);
    Add_child4(new_node, ref, key[depth + prefix_diff], SET_LEAF(l));
    return nullptr;
  }
RECURSE_SEARCH:;
  // Find a child to recurse to
  Art_node **child = Find_child(n, key[depth]);
  if (child) {
    return Recursive_insert(*child, child, key, key_len, value, depth + 1, old,
                            replace);
  }

  // No child, node goes within us
  Art_leaf *l = Make_leaf(key, key_len, value);
  Add_child(n, ref, key[depth], SET_LEAF(l));
  return nullptr;
}
void *Art_index::ART_insert(const unsigned char *key, int key_len,
                            void *value) {
  int old_val = 0;
  void *old = Recursive_insert(m_tree->root, &m_tree->root, key, key_len, value,
                               0, &old_val, 0);
  if (!old_val) m_tree->size++;
  return old;
}
void *Art_index::ART_insert_with_replace(const unsigned char *key, int key_len,
                                         void *value) {
  int old_val = 0;
  void *old = Recursive_insert(m_tree->root, &m_tree->root, key, key_len, value,
                               0, &old_val, 1);
  if (!old_val) m_tree->size++;
  return old;
}
void Art_index::Remove_child256(Art_node256 *n, Art_node **ref,
                                unsigned char c) {
  n->children[c] = NULL;
  n->n.num_children--;

  // Resize to a node48 on underflow, not immediately to prevent
  // trashing if we sit on the 48/49 boundary
  if (n->n.num_children == 37) {
    Art_node48 *new_node = (Art_node48 *)Alloc_node(NodeType::NODE48);
    *ref = (Art_node *)new_node;
    Copy_header((Art_node *)new_node, (Art_node *)n);

    int pos = 0;
    for (int i = 0; i < 256; i++) {
      if (n->children[i]) {
        new_node->children[pos] = n->children[i];
        new_node->keys[i] = pos + 1;
        pos++;
      }
    }
    free(n);
  }
}
void Art_index::Remove_child48(Art_node48 *n, Art_node **ref, unsigned char c) {
  int pos = n->keys[c];
  n->keys[c] = 0;
  n->children[pos - 1] = NULL;
  n->n.num_children--;

  if (n->n.num_children == 12) {
    Art_node16 *new_node = (Art_node16 *)Alloc_node(NodeType::NODE16);
    *ref = (Art_node *)new_node;
    Copy_header((Art_node *)new_node, (Art_node *)n);

    int child = 0;
    for (int i = 0; i < 256; i++) {
      pos = n->keys[i];
      if (pos) {
        new_node->keys[child] = i;
        new_node->children[child] = n->children[pos - 1];
        child++;
      }
    }
    free(n);
  }
}
void Art_index::Remove_child16(Art_node16 *n, Art_node **ref, Art_node **l) {
  int pos = l - n->children;
  memmove(n->keys + pos, n->keys + pos + 1, n->n.num_children - 1 - pos);
  memmove(n->children + pos, n->children + pos + 1,
          (n->n.num_children - 1 - pos) * sizeof(void *));
  n->n.num_children--;

  if (n->n.num_children == 3) {
    Art_node4 *new_node = (Art_node4 *)Alloc_node(NodeType::NODE4);
    *ref = (Art_node *)new_node;
    Copy_header((Art_node *)new_node, (Art_node *)n);
    memcpy(new_node->keys, n->keys, 4);
    memcpy(new_node->children, n->children, 4 * sizeof(void *));
    free(n);
  }
}
void Art_index::Remove_child4(Art_node4 *n, Art_node **ref, Art_node **l) {
  int pos = l - n->children;
  memmove(n->keys + pos, n->keys + pos + 1, n->n.num_children - 1 - pos);
  memmove(n->children + pos, n->children + pos + 1,
          (n->n.num_children - 1 - pos) * sizeof(void *));
  n->n.num_children--;

  // Remove nodes with only a single child
  if (n->n.num_children == 1) {
    Art_node *child = n->children[0];
    if (!IS_LEAF(child)) {
      // Concatenate the prefixes
      uint prefix = n->n.partial_len;
      if (prefix < Art_index::MAX_PREFIX_LEN) {
        n->n.partial[prefix] = n->keys[0];
        prefix++;
      }
      if (prefix < Art_index::MAX_PREFIX_LEN) {
        int sub_prefix = std::min((int)child->partial_len,
                                  (int)(Art_index::MAX_PREFIX_LEN - prefix));
        memcpy(n->n.partial + prefix, child->partial, sub_prefix);
        prefix += sub_prefix;
      }

      // Store the prefix in the child
      memcpy(child->partial, n->n.partial,
             std::min((int)prefix, (int)Art_index::MAX_PREFIX_LEN));
      child->partial_len += n->n.partial_len + 1;
    }
    *ref = child;
    free(n);
  }
}

void Art_index::Remove_child(Art_node *n, Art_node **ref, unsigned char c,
                             Art_node **l) {
  switch (n->type) {
    case NodeType::NODE4:
      return Remove_child4((Art_node4 *)n, ref, l);
    case NodeType::NODE16:
      return Remove_child16((Art_node16 *)n, ref, l);
    case NodeType::NODE48:
      return Remove_child48((Art_node48 *)n, ref, c);
    case NodeType::NODE256:
      return Remove_child256((Art_node256 *)n, ref, c);
    default:
      abort();
  }
}
Art_index::Art_leaf *Art_index::Recursive_delete(Art_node *n, Art_node **ref,
                                                 const unsigned char *key,
                                                 int key_len, int depth) {
  // Search terminated
  if (!n) return nullptr;

  // Handle hitting a leaf node
  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    if (!Leaf_matches(l, key, key_len, depth)) {
      *ref = nullptr;
      return l;
    }
    return nullptr;
  }

  // Bail if the prefix does not match
  if (n->partial_len) {
    int prefix_len = Check_prefix(n, key, key_len, depth);
    if (prefix_len !=
        std::min((int)Art_index::MAX_PREFIX_LEN, (int)n->partial_len)) {
      return nullptr;
    }
    depth = depth + n->partial_len;
  }

  // Find child node
  Art_node **child = Find_child(n, key[depth]);
  if (!child) return nullptr;

  // If the child is leaf, delete from this node
  if (IS_LEAF(*child)) {
    Art_leaf *l = LEAF_RAW(*child);
    if (!Leaf_matches(l, key, key_len, depth)) {
      Remove_child(n, ref, key[depth], child);
      return l;
    }
    return nullptr;

    // Recurse
  } else {
    return Recursive_delete(*child, child, key, key_len, depth + 1);
  }
}
void *Art_index::ART_delete(const unsigned char *key, int key_len) {
  Art_leaf *l = Recursive_delete(m_tree->root, &m_tree->root, key, key_len, 0);
  if (l) {
    m_tree->size--;
    void *old = l->value;
    free(l);
    return old;
  }
  return nullptr;
}

// return the value of this key.
void *Art_index::Cruise_fast(uint key_offset, unsigned char *key, int key_len) {
  Art_node **child;
  // Art_node *n = m_tree->root;
  int prefix_len, depth = 0;
  while (!m_current_nodes.empty()) {
    Art_node *n = m_current_nodes.top();
    m_current_nodes.pop();
    // Might be a leaf
    if (IS_LEAF(n)) {
      n = (Art_node *)LEAF_RAW(n);
      // Check if the expanded path matches
      if (!Leaf_partial_matches((Art_leaf *)n, key, key_offset, key_len,
                                depth)) {
        return ((Art_leaf *)n)->value;
      }
      // return nullptr;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      prefix_len = Check_prefix(n, key, key_len, depth);
      int min_v = std::min(MAX_PREFIX_LEN, n->partial_len);
      if (prefix_len != min_v) return nullptr;
      depth = depth + n->partial_len;
    }

    while (depth < key_len) {  // Recursively search
      child = Find_child(n, key[depth]);
      n = (child) ? *child : nullptr;
      if (!n || IS_LEAF(n)) {
        if (!n) break;
        m_current_nodes.emplace(n);
        break;
      }
      depth++;
    }
  }
  return nullptr;
}

int Art_index::Cruise(ART_Func &cb, void *data, int data_len) {
  while (!m_current_nodes.empty()) {
    Art_node *n = m_current_nodes.top();
    m_current_nodes.pop();

    if (!n) return 0;
    if (IS_LEAF(n)) {
      Art_leaf *l = LEAF_RAW(n);
      auto ret = cb(data, (const unsigned char *)l->key, l->key_len, l->value,
                    data_len);
      if (ret)
        return ret;
      else
        continue;
    }

    int idx;
    switch (n->type) {
      case NodeType::NODE4:
        for (int i = 0; i < n->num_children; i++) {
          auto child = ((Art_node4 *)n)->children[i];
          m_current_nodes.emplace(child);
        }
        break;
      case NodeType::NODE16:
        for (int i = 0; i < n->num_children; i++) {
          auto child = ((Art_node16 *)n)->children[i];
          if (child) m_current_nodes.emplace(child);
        }
        break;

      case NodeType::NODE48:
        for (int i = 0; i < 256; i++) {
          idx = ((Art_node48 *)n)->keys[i];
          if (!idx) continue;

          auto child = ((Art_node48 *)n)->children[idx - 1];
          if (child) m_current_nodes.emplace(child);
        }
        break;

      case NodeType::NODE256:
        for (int i = 0; i < 256; i++) {
          auto child = ((Art_node256 *)n)->children[i];
          if (!child) continue;
          m_current_nodes.emplace(child);
        }
        break;
      default:
        abort();
    }  // switch
  }    // while

  return 0;
}

int Art_index::Recursive_iter(Art_node *n, ART_Func &cb, void *data,
                              int data_len) {
  // Handle base cases
  if (!n) return 0;
  if (IS_LEAF(n)) {
    Art_leaf *l = LEAF_RAW(n);
    return cb(data, (const unsigned char *)l->key, l->key_len, l->value,
              data_len);
  }

  int idx, res;
  switch (n->type) {
    case NodeType::NODE4:
      for (int i = 0; i < n->num_children; i++) {
        res = Recursive_iter(((Art_node4 *)n)->children[i], cb, data, data_len);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE16:
      for (int i = 0; i < n->num_children; i++) {
        res =
            Recursive_iter(((Art_node16 *)n)->children[i], cb, data, data_len);
        if (res) {
          return res;
        }
      }
      break;

    case NodeType::NODE48:
      for (int i = 0; i < 256; i++) {
        idx = ((Art_node48 *)n)->keys[i];
        if (!idx) continue;

        res = Recursive_iter(((Art_node48 *)n)->children[idx - 1], cb, data,
                             data_len);
        if (res) {
          return res;
        };
      }
      break;

    case NodeType::NODE256:
      for (int i = 0; i < 256; i++) {
        if (!((Art_node256 *)n)->children[i]) continue;

        res =
            Recursive_iter(((Art_node256 *)n)->children[i], cb, data, data_len);
        if (res) {
          return res;
        }
      }
      break;
    default:
      abort();
  }
  return 0;
}

void *Art_index::ART_iter_fast(uint key_offset, unsigned char *key,
                               int key_len) {
  return Cruise_fast(key_offset, key, key_len);
}

int Art_index::ART_iter(ART_Func &cb, void *data, int data_len) {
  return Cruise(cb, data, data_len);
  // return Recursive_iter(m_tree->root, cb, data, data_len);
}

int Art_index::Leaf_prefix_matches(const Art_leaf *n,
                                   const unsigned char *prefix,
                                   int prefix_len) {
  // Fail if the key length is too short
  if (n->key_len < (uint32_t)prefix_len) return 1;

  // Compare the keys
  return memcmp(n->key, prefix, prefix_len);
}
int Art_index::ART_iter_prefix(const unsigned char *key, int key_len,
                               ART_Func &cb, void *data, int data_len) {
  Art_node **child;
  Art_node *n = m_tree->root;
  int prefix_len, depth = 0;
  while (n) {
    // Might be a leaf
    if (IS_LEAF(n)) {
      n = (Art_node *)LEAF_RAW(n);
      // Check if the expanded path matches
      if (!Leaf_prefix_matches((Art_leaf *)n, key, key_len)) {
        Art_leaf *l = (Art_leaf *)n;
        return cb(data, (const unsigned char *)l->key, l->key_len, l->value,
                  data_len);
      }
      return 0;
    }

    // If the depth matches the prefix, we need to handle this node
    if (depth == key_len) {
      Art_leaf *l = Minimum(n);
      if (!Leaf_prefix_matches(l, key, key_len))
        return Recursive_iter(n, cb, data, data_len);
      return 0;
    }

    // Bail if the prefix does not match
    if (n->partial_len) {
      prefix_len = Prefix_mismatch(n, key, key_len, depth);

      // Guard if the mis-match is longer than the MAX_PREFIX_LEN
      if ((uint32_t)prefix_len > n->partial_len) {
        prefix_len = n->partial_len;
      }

      // If there is no match, search is terminated
      if (!prefix_len) {
        return 0;

        // If we've matched the prefix, iterate on this node
      } else if (depth + prefix_len == key_len) {
        return Recursive_iter(n, cb, data, data_len);
      }

      // if there is a full match, go deeper
      depth = depth + n->partial_len;
    }

    // Recursively search
    child = Find_child(n, key[depth]);
    n = (child) ? *child : NULL;
    depth++;
  }
  return 0;
}
}  // namespace Imcs
}  // namespace ShannonBase