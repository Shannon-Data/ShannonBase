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
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

#define IS_LEAF(x) (((uintptr_t)x & 1))
#define SET_LEAF(x) ((void *)((uintptr_t)x | 1))
#define LEAF_RAW(x) ((ART::Art_leaf *)((void *)((uintptr_t)x & ~1)))

class ART {
 public:
  ART() : m_tree(nullptr), m_inited(false) {}

  ~ART() {
    if (m_inited) {
      ART_tree_destroy();
    }
  }

  ART(const ART &other) = delete;
  ART &operator=(const ART &other) = delete;

  ART(ART &&other)
  noexcept : m_tree(other.m_tree), m_inited(other.m_inited), m_current_values(std::move(other.m_current_values)) {
    other.m_tree = nullptr;
    other.m_inited = false;
  }

  ART &operator=(ART &&other) noexcept {
    if (this != &other) {
      if (m_inited) {
        ART_tree_destroy();
      }
      m_tree = other.m_tree;
      m_inited = other.m_inited;
      m_current_values = std::move(other.m_current_values);
      other.m_tree = nullptr;
      other.m_inited = false;
    }
    return *this;
  }

  enum NodeType { UNKNOWN = 0, NODE4 = 1, NODE16, NODE48, NODE256 };
  static constexpr uint MAX_PREFIX_LEN = 512;
  static constexpr uint initial_capacity = 16;
  using ART_Func = std::function<int(void *data, void *leaf, const unsigned char *key, uint32 key_len, void *value,
                                     uint32 value_len)>;
  typedef struct {
    std::atomic<uint32> ref_count{1};
    uint32 partial_len{0};
    uint8 type{NodeType::UNKNOWN};
    uint8 num_children{0};
    mutable std::shared_mutex node_mutex;
    unsigned char partial[ART::MAX_PREFIX_LEN];
  } Art_node;

  typedef struct {
    Art_node n;
    unsigned char keys[4];
    std::atomic<Art_node *> children[4];
  } Art_node4;

  typedef struct {
    Art_node n;
    unsigned char keys[16];
    std::atomic<Art_node *> children[16];
  } Art_node16;

  typedef struct {
    Art_node n;
    unsigned char keys[256];
    std::atomic<Art_node *> children[48];
  } Art_node48;

  typedef struct {
    Art_node n;
    std::atomic<Art_node *> children[256];
  } Art_node256;

  typedef struct {
    std::atomic<uint32> ref_count{1};
    mutable std::shared_mutex leaf_mutex;
    std::atomic<void **> values;
    std::atomic<uint32_t> value_len;
    std::atomic<uint32_t> vcount;
    std::atomic<uint32_t> capacity;
    uint32_t key_len;
    unsigned char key[];
  } Art_leaf;

  typedef struct {
    std::atomic<Art_node *> root;
    std::atomic<size_t> size;
    mutable std::shared_mutex tree_mutex;
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

  int Check_prefix(const Art_node *n, const unsigned char *key, int key_len, int depth);
  std::atomic<Art_node *> *Find_child(Art_node *n, unsigned char c);

 private:
  // add ref.
  void AddRef(Art_node *node);
  void Release(Art_node *node);
  void AddRefLeaf(Art_leaf *leaf);
  void ReleaseLeaf(Art_leaf *leaf);

  // safe node access.
  Art_node *GetChildSafe(Art_node *parent, unsigned char c);
  void SetChildSafe(Art_node *parent, unsigned char c, Art_node *child);

  void Find_children(Art_node *n, unsigned char c, std::vector<Art_node *> &children);

  int Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  int Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  inline uint64 art_size() { return m_tree->size; }

  Art_leaf *Minimum(const Art_node *n);
  Art_leaf *Maximum(const Art_node *n);
  Art_leaf *Make_leaf(const unsigned char *key, int key_len, void *value, uint value_len);
  int Longest_common_prefix(Art_leaf *l1, Art_leaf *l2, int depth);
  void Copy_header(Art_node *dest, Art_node *src);

  void Add_child256(Art_node256 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child);
  void Add_child48(Art_node48 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child);
  void Add_child16(Art_node16 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child);
  void Add_child4(Art_node4 *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child);
  void Add_child(Art_node *n, std::atomic<Art_node *> *ref, unsigned char c, Art_node *child);

  int Prefix_mismatch(const Art_node *n, const unsigned char *key, int key_len, int depth);
  void *Recursive_insert(Art_node *n, std::atomic<Art_node *> *ref, const unsigned char *key, int key_len, void *value,
                         int value_len, int depth, int *old, int replace);

  void Remove_child256(Art_node256 *n, std::atomic<Art_node *> *ref, unsigned char c);
  void Remove_child48(Art_node48 *n, std::atomic<Art_node *> *ref, unsigned char c);
  void Remove_child16(Art_node16 *n, std::atomic<Art_node *> *ref, std::atomic<Art_node *> *l);
  void Remove_child4(Art_node4 *n, std::atomic<Art_node *> *ref, std::atomic<Art_node *> *l);
  void Remove_child(Art_node *n, std::atomic<Art_node *> *ref, unsigned char c, std::atomic<Art_node *> *l);
  void RemoveChildSafe(Art_node *parent, unsigned char c);

  Art_leaf *Recursive_delete(Art_node *n, std::atomic<Art_node *> *ref, const unsigned char *key, int key_len,
                             int depth);
  int Recursive_iter(Art_node *n, ART_Func &cb, void *data);
  int Recursive_iter_with_key(Art_node *n, const unsigned char *key, int key_len);
  int Leaf_prefix_matches(const Art_leaf *n, const unsigned char *prefix, int prefix_len);
  int Leaf_prefix_matches2(const Art_leaf *n, const unsigned char *prefix, int prefix_len);

  void Free_leaf(Art_leaf *l);

  void *safe_malloc(size_t size);
  void *safe_calloc(size_t count, size_t size);
  void *safe_realloc(void *ptr, size_t size);

 public:
  inline int ART_tree_init() {
    std::unique_lock lock(m_node_mutex);
    if (!m_tree) {
      m_tree = new Art_tree();
    }
    m_tree->root.store(nullptr, std::memory_order_release);
    m_tree->size.store(0, std::memory_order_release);
    m_inited = true;
    return 0;
  }

  inline int ART_tree_destroy() {
    std::unique_lock lock(m_node_mutex);
    if (m_tree) {
      Art_node *root = m_tree->root.load(std::memory_order_acquire);
      if (root) {
        Destroy_node(root);
        m_tree->root.store(nullptr, std::memory_order_release);
      }
      delete m_tree;
      m_tree = nullptr;
    }
    m_inited = false;
    return 0;
  }

  inline bool Art_initialized() {
    std::shared_lock lock(m_node_mutex);
    return m_inited;
  }
  inline Art_tree *tree() const {
    std::shared_lock lock(m_node_mutex);
    return m_tree;
  }
  inline Art_node *root() const {
    std::shared_lock lock(m_node_mutex);
    return (m_inited && m_tree) ? m_tree->root.load(std::memory_order_acquire) : nullptr;
  }

  void *ART_insert(const unsigned char *key, int key_len, void *value, uint value_len);
  void *ART_insert_with_replace(const unsigned char *key, int key_len, void *value, uint value_len);
  void *ART_delete(const unsigned char *key, int key_len);
  void *ART_search(const unsigned char *key, int key_len);
  std::vector<void *> ART_search_all(const unsigned char *key, int key_len);

  Art_leaf *ART_minimum();
  Art_leaf *ART_maximum();

  int ART_iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len);
  int ART_iter(ART_Func cb, void *data);

 private:
  mutable std::shared_mutex m_node_mutex;
  Art_tree *m_tree{nullptr};
  bool m_inited{false};
  std::vector<Art_leaf *> m_current_values;
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_H__