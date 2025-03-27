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
/** Adaptive Radix Tree from https://github.com/armon/libart, which's in c.
 *  re-impl in c++. But from source code of DuckDB, it does not use mmx
 *  instructions to accelerate the performance.
 */

#ifndef __SHANNONBASE_ART_H__
#define __SHANNONBASE_ART_H__

#include <assert.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

#include "my_inttypes.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

#define IS_LEAF(x) (((uintptr_t)x & 1))
#define SET_LEAF(x) ((void *)((uintptr_t)x | 1))
#define LEAF_RAW(x) ((ART::Art_leaf *)((void *)((uintptr_t)x & ~1)))

enum NodeType { UNKNOWN = 0, NODE4 = 1, NODE16, NODE48, NODE256 };

class ART {
 public:
  static constexpr uint MAX_PREFIX_LEN = 10;
  using ART_Func =
      std::function<int(void *data, const unsigned char *key, uint32 key_len, void *value, uint32 value_len)>;
  typedef struct {
    uint32 partial_len{0};
    uint8 type{NodeType::UNKNOWN};
    uint8 num_children{0};
    unsigned char partial[ART::MAX_PREFIX_LEN];
  } Art_node;

  typedef struct {
    Art_node n;
    unsigned char keys[4];
    Art_node *children[4];
  } Art_node4;

  typedef struct {
    Art_node n;
    unsigned char keys[16];
    Art_node *children[16];
  } Art_node16;

  typedef struct {
    Art_node n;
    unsigned char keys[256];
    Art_node *children[48];
  } Art_node48;

  typedef struct {
    Art_node n;
    Art_node *children[256];
  } Art_node256;

  typedef struct {
    void *value;
    uint32 value_len;
    uint32 key_len;
    unsigned char key[];
  } Art_leaf;

  typedef struct {
    Art_node *root;
    uint64 size;
  } Art_tree;

  typedef struct {
    const Art_node *node;  // current node
    int child_idx;         // current sub node idx
    const unsigned char *key;
    size_t key_len;
    void *value;  // value（if node is leaf node）
  } Art_iterator;

  Art_node *Alloc_node(NodeType type);
  void Destroy_node(Art_node *n);

 private:
  // 0 sucess.
  Art_node **Find_child(Art_node *n, unsigned char c);
  void Find_children(Art_node *n, unsigned char c, std::vector<Art_node *> &children);
  int Check_prefix(const Art_node *n, const unsigned char *key, int key_len, int depth);
  int Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  int Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  inline uint64 art_size() { return m_tree->size; }

  Art_leaf *Minimum(const Art_node *n);
  Art_leaf *Maximum(const Art_node *n);
  Art_leaf *Make_leaf(const unsigned char *key, int key_len, void *value, uint value_len);
  int Longest_common_prefix(Art_leaf *l1, Art_leaf *l2, int depth);
  void Copy_header(Art_node *dest, Art_node *src);
  void Add_child256(Art_node256 *n, Art_node **ref, unsigned char c, void *child);
  void Add_child48(Art_node48 *n, Art_node **ref, unsigned char c, void *child);
  void Add_child16(Art_node16 *n, Art_node **ref, unsigned char c, void *child);
  void Add_child4(Art_node4 *n, Art_node **ref, unsigned char c, void *child);
  void Add_child(Art_node *n, Art_node **ref, unsigned char c, void *child);
  int Prefix_mismatch(const Art_node *n, const unsigned char *key, int key_len, int depth);
  void *Recursive_insert(Art_node *n, Art_node **ref, const unsigned char *key, int key_len, void *value, int value_len,
                         int depth, int *old, int replace);
  void Remove_child256(Art_node256 *n, Art_node **ref, unsigned char c);
  void Remove_child48(Art_node48 *n, Art_node **ref, unsigned char c);
  void Remove_child16(Art_node16 *n, Art_node **ref, Art_node **l);
  void Remove_child4(Art_node4 *n, Art_node **ref, Art_node **l);
  void Remove_child(Art_node *n, Art_node **ref, unsigned char c, Art_node **l);
  Art_leaf *Recursive_delete(Art_node *n, Art_node **ref, const unsigned char *key, int key_len, int depth);
  int Recursive_iter(Art_node *n, ART_Func &cb, void *data);
  int Recursive_iter_with_key(Art_node *n, const unsigned char *key, int key_len);
  int Leaf_prefix_matches(const Art_leaf *n, const unsigned char *prefix, int prefix_len);
  int Leaf_prefix_matches2(const Art_leaf *n, const unsigned char *prefix, int prefix_len);

 public:
  inline int ART_tree_init() {
    if (!m_tree) {
      m_tree = new Art_tree();
    }
    m_tree->root = nullptr;
    m_tree->size = 0;
    m_inited = true;
    return 0;
  }

  inline int ART_tree_destroy() {
    Destroy_node(m_tree->root);
    if (m_tree) {
      delete m_tree;
      m_tree = nullptr;
      m_inited = false;
    }
    return 0;
  }

  inline bool Art_initialized() { return m_inited; }
  inline Art_node *root() const { return (m_inited) ? m_tree->root : nullptr; }

  void *ART_insert(const unsigned char *key, int key_len, void *value, uint value_len);
  void *ART_insert_with_replace(const unsigned char *key, int key_len, void *value, uint value_len);
  void *ART_delete(const unsigned char *key, int key_len);
  void *ART_search(const unsigned char *key, int key_len);

  Art_leaf *ART_minimum();
  Art_leaf *ART_maximum();

  int ART_iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len);
  int ART_iter(ART_Func &cb, void *data);

 private:
  Art_tree *m_tree{nullptr};
  bool m_inited{false};
  std::vector<Art_leaf *> m_current_values;
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_H__