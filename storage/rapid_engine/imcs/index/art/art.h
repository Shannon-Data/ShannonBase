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
 *  re-impl in c++.
 */

#ifndef __SHANNONBASE_ART_H__
#define __SHANNONBASE_ART_H__

#include <assert.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <cstring>
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

  ART(ART &&other) noexcept : m_tree(std::move(other.m_tree)), m_inited(other.m_inited) { other.m_inited = false; }

  ART &operator=(ART &&other) noexcept {
    if (this != &other) {
      if (m_inited) {
        ART_tree_destroy();
      }
      m_tree = std::move(other.m_tree);
      m_inited = other.m_inited;
      other.m_inited = false;
    }
    return *this;
  }

  enum NodeType { UNKNOWN = 0, NODE4 = 1, NODE16, NODE48, NODE256, LEAF };
  static constexpr uint32_t MAX_PREFIX_LEN = 512;
  static constexpr uint32_t VALUE_INITIAL_CAPACITY = 16;

  using ART_Func =
      std::function<int(void *data, const void *key, uint32_t key_len, const void *value, uint32_t value_len)>;

  struct Art_node {
    Art_node() {
      partial_len = 0;
      num_children = 0;
      std::memset(partial, 0x0, MAX_PREFIX_LEN);
    }

    virtual ~Art_node() = default;
    virtual NodeType type() const = 0;

    Art_node(const Art_node &other) : partial_len(other.partial_len), num_children(other.num_children) {
      std::memcpy(partial, other.partial, MAX_PREFIX_LEN);
    }

    Art_node(Art_node &&other) noexcept : partial_len(other.partial_len), num_children(other.num_children) {
      std::memcpy(partial, other.partial, MAX_PREFIX_LEN);
      other.partial_len = 0;
      other.num_children = 0;
      std::memset(other.partial, 0, MAX_PREFIX_LEN);
    }

    Art_node &operator=(const Art_node &other) {
      if (this != &other) {
        partial_len = other.partial_len;
        num_children = other.num_children;
        std::memcpy(partial, other.partial, MAX_PREFIX_LEN);
      }
      return *this;
    }

    Art_node &operator=(Art_node &&other) noexcept {
      if (this != &other) {
        partial_len = other.partial_len;
        num_children = other.num_children;
        std::memcpy(partial, other.partial, MAX_PREFIX_LEN);

        other.partial_len = 0;
        other.num_children = 0;
        std::memset(other.partial, 0, MAX_PREFIX_LEN);
      }
      return *this;
    }

    uint32_t partial_len{0};
    uint8_t num_children{0};
    mutable std::shared_mutex node_mutex;
    unsigned char partial[MAX_PREFIX_LEN];
  };

  struct Art_leaf : public Art_node {
    Art_leaf() = default;
    virtual ~Art_leaf() {}

    Art_leaf(const unsigned char *key_data, int key_length, void *value_data, uint32_t value_length) {
      key.assign(key_data, key_data + key_length);
      add_value(value_data, value_length);
    }

    Art_leaf(const Art_leaf &other) : Art_node(other), key(other.key), vcount(other.vcount.load()) {
      std::shared_lock lock(other.leaf_mutex);
      values = other.values;
    }

    Art_leaf(Art_leaf &&other) noexcept
        : Art_node(std::move(other)),
          key(std::move(other.key)),
          values(std::move(other.values)),
          vcount(other.vcount.load()) {
      other.vcount.store(0);
    }

    Art_leaf &operator=(const Art_leaf &other) {
      if (this != &other) {
        Art_node::operator=(other);

        std::unique_lock this_lock(leaf_mutex, std::defer_lock);
        std::shared_lock other_lock(other.leaf_mutex, std::defer_lock);
        std::lock(this_lock, other_lock);

        key = other.key;
        values = other.values;
        vcount.store(other.vcount.load());
      }
      return *this;
    }

    Art_leaf &operator=(Art_leaf &&other) noexcept {
      if (this != &other) {
        Art_node::operator=(std::move(other));

        std::unique_lock this_lock(leaf_mutex);

        key = std::move(other.key);
        values = std::move(other.values);
        vcount.store(other.vcount.load());
        other.vcount.store(0);
      }
      return *this;
    }

    NodeType type() const override { return LEAF; }

    mutable std::shared_mutex leaf_mutex;
    std::vector<unsigned char> key;
    std::vector<std::vector<uint8_t>> values;
    std::atomic<uint32_t> vcount{0};

    inline void add_value(const void *value, uint32_t value_len) {
      values.emplace_back(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + value_len);
      vcount.fetch_add(1, std::memory_order_acq_rel);
    }

    inline std::vector<uint8_t> *get_value(size_t index) {
      std::shared_lock lock(leaf_mutex);
      return (index < values.size()) ? &values[index] : nullptr;
    }

    inline size_t get_value_count() const {
      assert(values.size() == vcount);
      return vcount.load(std::memory_order_acquire);
    }
  };

  struct Art_node4 : public Art_node {
    Art_node4() {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }

    Art_node4(const Art_node4 &other) : Art_node(other) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = other.children;  // copy of shared_ptr
    }

    Art_node4(Art_node4 &&other) noexcept : Art_node(std::move(other)) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = std::move(other.children);
      std::memset(other.keys, 0, sizeof(keys));
    }

    Art_node4 &operator=(const Art_node4 &other) {
      if (this != &other) {
        Art_node::operator=(other);
        std::memcpy(keys, other.keys, sizeof(keys));
        children = other.children;
      }
      return *this;
    }

    Art_node4 &operator=(Art_node4 &&other) noexcept {
      if (this != &other) {
        Art_node::operator=(std::move(other));
        std::memcpy(keys, other.keys, sizeof(keys));
        children = std::move(other.children);
        std::memset(other.keys, 0, sizeof(keys));
      }
      return *this;
    }

    NodeType type() const override { return NODE4; }
    unsigned char keys[4];
    std::array<std::shared_ptr<Art_node>, 4> children;
  };

  struct Art_node16 : public Art_node {
    Art_node16() {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }

    Art_node16(const Art_node16 &other) : Art_node(other) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = other.children;
    }

    Art_node16(Art_node16 &&other) noexcept : Art_node(std::move(other)) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = std::move(other.children);
      std::memset(other.keys, 0, sizeof(keys));
    }

    Art_node16 &operator=(const Art_node16 &other) {
      if (this != &other) {
        Art_node::operator=(other);
        std::memcpy(keys, other.keys, sizeof(keys));
        children = other.children;
      }
      return *this;
    }

    Art_node16 &operator=(Art_node16 &&other) noexcept {
      if (this != &other) {
        Art_node::operator=(std::move(other));
        std::memcpy(keys, other.keys, sizeof(keys));
        children = std::move(other.children);
        std::memset(other.keys, 0, sizeof(keys));
      }
      return *this;
    }

    NodeType type() const override { return NODE16; }
    unsigned char keys[16];
    std::array<std::shared_ptr<Art_node>, 16> children;
  };

  struct Art_node48 : public Art_node {
    Art_node48() {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }

    Art_node48(const Art_node48 &other) : Art_node(other) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = other.children;
    }

    Art_node48(Art_node48 &&other) noexcept : Art_node(std::move(other)) {
      std::memcpy(keys, other.keys, sizeof(keys));
      children = std::move(other.children);
      std::memset(other.keys, 0, sizeof(keys));
    }

    Art_node48 &operator=(const Art_node48 &other) {
      if (this != &other) {
        Art_node::operator=(other);
        std::memcpy(keys, other.keys, sizeof(keys));
        children = other.children;
      }
      return *this;
    }

    Art_node48 &operator=(Art_node48 &&other) noexcept {
      if (this != &other) {
        Art_node::operator=(std::move(other));
        std::memcpy(keys, other.keys, sizeof(keys));
        children = std::move(other.children);
        std::memset(other.keys, 0, sizeof(keys));
      }
      return *this;
    }

    NodeType type() const override { return NODE48; }
    unsigned char keys[256];
    std::array<std::shared_ptr<Art_node>, 48> children;
  };

  struct Art_node256 : public Art_node {
    Art_node256() { children.fill(nullptr); }

    Art_node256(const Art_node256 &other) : Art_node(other) { children = other.children; }

    Art_node256(Art_node256 &&other) noexcept : Art_node(std::move(other)) { children = std::move(other.children); }

    Art_node256 &operator=(const Art_node256 &other) {
      if (this != &other) {
        Art_node::operator=(other);
        children = other.children;
      }
      return *this;
    }

    Art_node256 &operator=(Art_node256 &&other) noexcept {
      if (this != &other) {
        Art_node::operator=(std::move(other));
        children = std::move(other.children);
      }
      return *this;
    }

    NodeType type() const override { return NODE256; }
    std::array<std::shared_ptr<Art_node>, 256> children;
  };

  using ArtNodePtr = std::shared_ptr<Art_node>;
  using ArtNode4Ptr = std::shared_ptr<Art_node4>;
  using ArtNode16Ptr = std::shared_ptr<Art_node16>;
  using ArtNode48Ptr = std::shared_ptr<Art_node48>;
  using ArtNode256Ptr = std::shared_ptr<Art_node256>;
  using ArtLeafPtr = std::shared_ptr<Art_leaf>;

  template <typename T, typename... Args>
  std::shared_ptr<T> make_art_node(Args &&...args) {
    static_assert(std::is_base_of_v<Art_node, T>, "T must inherit from Art_node");
    return std::make_shared<T>(std::forward<Args>(args)...);
  }

  struct Art_tree {
    ArtNodePtr root;
    std::atomic<size_t> size{0};
    mutable std::shared_mutex tree_mutex;
  };
  using ArtTreePtr = std::unique_ptr<Art_tree>;

  static bool is_leaf(const Art_node *node) { return node && node->type() == LEAF; }

  static const Art_leaf *to_leaf(const Art_node *node) {
    return is_leaf(node) ? static_cast<const Art_leaf *>(node) : nullptr;
  }

  inline int ART_tree_init() {
    std::unique_lock lock(m_node_mutex);
    if (!m_tree) {
      m_tree = std::make_unique<Art_tree>();
    }
    m_tree->root = nullptr;
    m_tree->size.store(0, std::memory_order_release);
    m_inited = true;
    return 0;
  }

  inline int ART_tree_destroy() {
    std::unique_lock lock(m_node_mutex);
    if (m_tree) {
      m_tree->root = nullptr;
      m_tree.reset();
    }
    m_inited = false;
    return 0;
  }

  inline bool Art_initialized() const {
    std::shared_lock lock(m_node_mutex);
    return m_inited;
  }

  inline Art_tree *tree() const {
    std::shared_lock lock(m_node_mutex);
    return m_tree.get();
  }

  inline Art_node *root() const {
    std::shared_lock lock(m_node_mutex);
    return (m_inited && m_tree) ? m_tree->root.get() : nullptr;
  }

  void *ART_insert(const unsigned char *key, int key_len, void *value, uint32_t value_len);
  void *ART_delete(const unsigned char *key, int key_len);
  void *ART_search(const unsigned char *key, int key_len);
  std::vector<std::vector<uint8_t>> ART_search_all(const unsigned char *key, int key_len);
  int ART_iter(ART_Func cb, void *data);

  Art_leaf *ART_minimum();
  Art_leaf *ART_maximum();

 private:
  void *Recursive_insert(ArtNodePtr &node, const unsigned char *key, int key_len, void *value, uint32_t value_len,
                         int depth, int *old, int replace);

  ArtNodePtr Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth, void *&result);

  ArtLeafPtr Make_leaf(const unsigned char *key, int key_len, void *value, uint32_t value_len) {
    if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;

    auto leaf = make_art_node<Art_leaf>();

    leaf->key.assign(key, key + key_len);

    leaf->add_value(value, value_len);

    return leaf;
  }

  ArtNodePtr &Find_child(const ArtNodePtr &n, unsigned char c);
  void Find_children(const ArtNodePtr &n, unsigned char c, std::vector<ArtNodePtr> &children);

  inline uint64_t art_size() const { return m_tree ? m_tree->size.load(std::memory_order_acquire) : 0; }

  // the min key node.
  Art_leaf *Minimum(const ArtNodePtr &n);

  // the max key node.
  Art_leaf *Maximum(const ArtNodePtr &n);

  // copy header info.
  void Copy_header(Art_node *dest, const Art_node *src);

  // add a new node to some node.
  void Add_child(ArtNodePtr &new_node, ArtNodePtr &old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child256(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child48(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child16(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child4(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);

  // remove the child nodes.
  void Remove_child(ArtNodePtr &node, unsigned char c, const ArtNodePtr &child);
  void Remove_child256(ArtNodePtr &node, unsigned char c);
  void Remove_child48(ArtNodePtr &node, unsigned char c);
  void Remove_child16(ArtNodePtr &node, const ArtNodePtr &child);
  void Remove_child4(ArtNodePtr &node, const ArtNodePtr &child);

  int Recursive_iter(Art_node *node, ART_Func &cb, void *data);
  int Recursive_iter_with_key(const ArtNodePtr &n, const unsigned char *key, int key_len);
  int Iter_prefix(const unsigned char *key, int key_len, ART_Func &cb, void *data, int data_len);

  // the the prefix of a node is matched the key or not.
  int Check_prefix(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth);

  // return the pos of the mis-matched position.
  int Prefix_mismatch(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth);

  // the longest common prefix length
  int Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth);

  // to test the leaf node is match the key, return 0 matched, otherwise the memcmp result.
  int Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);

  // to test the leaf node's key is matched the key or not with depth. return 0 matched, otherwise the memcmp result.
  int Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);

 private:
  mutable std::shared_mutex m_node_mutex;
  std::unique_ptr<Art_tree> m_tree{nullptr};
  bool m_inited{false};

  static ArtNodePtr null_ptr;
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ART_H__