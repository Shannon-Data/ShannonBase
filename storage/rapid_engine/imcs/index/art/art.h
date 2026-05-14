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
#include "storage/rapid_engine/utils/memory_pool.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

// Forward declare
class ART;

class ART {
 public:
  // Node allocator that can use either pool or heap allocation
  class ArtNodeAllocator {
    ShannonBase::Utils::MemoryPool *m_pool{nullptr};
    bool m_use_pool{false};

   public:
    ArtNodeAllocator() = default;

    // Enable pool allocation
    void enable_pool(ShannonBase::Utils::MemoryPool *pool) {
      m_pool = pool;
      m_use_pool = (pool != nullptr);
    }

    // Disable pool allocation (fall back to make_shared)
    void disable_pool() {
      m_use_pool = false;
      m_pool = nullptr;
    }

    template <typename T, typename... Args>
    std::shared_ptr<T> make(Args &&...args) {
      if (m_use_pool && m_pool) {
        // Pool allocation
        void *mem = m_pool->allocate(sizeof(T));
        if (!mem) {
          throw std::bad_alloc();
        }
        T *ptr = new (mem) T(std::forward<Args>(args)...);
        return std::shared_ptr<T>(ptr, [this](T *p) {
          if (p) {
            p->~T();
            m_pool->deallocate(p, sizeof(T));
          }
        });
      } else {
        // Fallback to heap allocation
        return std::make_shared<T>(std::forward<Args>(args)...);
      }
    }

    // Bulk allocation for multiple nodes
    template <typename T>
    T *allocate_bulk(size_t count) {
      if (m_use_pool && m_pool) {
        return static_cast<T *>(m_pool->allocate(sizeof(T) * count));
      }
      return static_cast<T *>(::operator new(sizeof(T) * count));
    }

    void deallocate_bulk(void *ptr, size_t size) {
      if (m_use_pool && m_pool) {
        m_pool->deallocate(ptr, size);
      } else {
        ::operator delete(ptr);
      }
    }
  };

  ART() : m_tree(nullptr), m_inited(false) {
    // Optional: Enable pool allocation for bulk loads
    // m_allocator.enable_pool(&m_memory_pool);
  }

  ~ART() {
    if (m_inited) ART_tree_destroy();
  }

  ART(const ART &) = delete;
  ART &operator=(const ART &) = delete;

  ART(ART &&other)
  noexcept : m_tree(std::move(other.m_tree)), m_inited(other.m_inited), m_allocator(std::move(other.m_allocator)) {
    other.m_inited = false;
  }

  ART &operator=(ART &&other) noexcept {
    if (this != &other) {
      if (m_inited) ART_tree_destroy();
      m_tree = std::move(other.m_tree);
      m_inited = other.m_inited;
      m_allocator = std::move(other.m_allocator);
      other.m_inited = false;
    }
    return *this;
  }

  // Enable pool allocation for bulk operations
  void enable_pool_allocation(ShannonBase::Utils::MemoryPool *pool) { m_allocator.enable_pool(pool); }

  void disable_pool_allocation() { m_allocator.disable_pool(); }

  enum NodeType { UNKNOWN = 0, NODE4 = 1, NODE16, NODE48, NODE256, LEAF };

  static constexpr uint32_t MAX_PREFIX_LEN = 512;
  static constexpr uint32_t VALUE_INITIAL_CAPACITY = 16;

  using ART_Func =
      std::function<int(void *data, const void *key, uint32_t key_len, const void *value, uint32_t value_len)>;

  struct Art_node {
    Art_node() {
      partial_len = 0;
      num_children = 0;
      std::memset(partial, 0, MAX_PREFIX_LEN);
    }
    virtual ~Art_node() = default;
    virtual NodeType type() const = 0;

    Art_node(const Art_node &o) : partial_len(o.partial_len), num_children(o.num_children) {
      std::memcpy(partial, o.partial, MAX_PREFIX_LEN);
    }
    Art_node(Art_node &&o) noexcept : partial_len(o.partial_len), num_children(o.num_children) {
      std::memcpy(partial, o.partial, MAX_PREFIX_LEN);
      o.partial_len = 0;
      o.num_children = 0;
      std::memset(o.partial, 0, MAX_PREFIX_LEN);
    }
    Art_node &operator=(const Art_node &o) {
      if (this != &o) {
        partial_len = o.partial_len;
        num_children = o.num_children;
        std::memcpy(partial, o.partial, MAX_PREFIX_LEN);
      }
      return *this;
    }
    Art_node &operator=(Art_node &&o) noexcept {
      if (this != &o) {
        partial_len = o.partial_len;
        num_children = o.num_children;
        std::memcpy(partial, o.partial, MAX_PREFIX_LEN);
        o.partial_len = 0;
        o.num_children = 0;
        std::memset(o.partial, 0, MAX_PREFIX_LEN);
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
    ~Art_leaf() override = default;

    Art_leaf(const unsigned char *key_data, int key_length, void *value_data, uint32_t value_length) {
      key.assign(key_data, key_data + key_length);
      add_value(value_data, value_length);
    }

    Art_leaf(const Art_leaf &o) : Art_node(o), key(o.key) {
      std::shared_lock lk(o.leaf_mutex);
      values = o.values;
    }

    Art_leaf(Art_leaf &&o) noexcept : Art_node(std::move(o)), key(std::move(o.key)), values(std::move(o.values)) {}

    Art_leaf &operator=(const Art_leaf &o) {
      if (this != &o) {
        Art_node::operator=(o);
        std::unique_lock lk1(leaf_mutex, std::defer_lock);
        std::shared_lock lk2(o.leaf_mutex, std::defer_lock);
        std::lock(lk1, lk2);
        key = o.key;
        values = o.values;
      }
      return *this;
    }

    Art_leaf &operator=(Art_leaf &&o) noexcept {
      if (this != &o) {
        Art_node::operator=(std::move(o));
        std::unique_lock lk(leaf_mutex);
        key = std::move(o.key);
        values = std::move(o.values);
      }
      return *this;
    }

    NodeType type() const override { return LEAF; }

    mutable std::shared_mutex leaf_mutex;
    std::vector<unsigned char> key;
    std::vector<std::vector<uint8_t>> values;

    inline void add_value(const void *value, uint32_t value_len) {
      std::unique_lock lk(leaf_mutex);
      values.emplace_back(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + value_len);
    }

    inline void add_value_unsafe(const void *value, uint32_t value_len) {
      values.emplace_back(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + value_len);
    }

    inline std::vector<uint8_t> *get_value(size_t index) {
      std::shared_lock lk(leaf_mutex);
      return (index < values.size()) ? &values[index] : nullptr;
    }

    inline size_t get_value_count() const {
      std::shared_lock lk(leaf_mutex);
      return values.size();
    }
  };

  struct Art_node4 : public Art_node {
    Art_node4() {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }
    Art_node4(const Art_node4 &o) : Art_node(o) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = o.children;
    }
    Art_node4(Art_node4 &&o) noexcept : Art_node(std::move(o)) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = std::move(o.children);
      std::memset(o.keys, 0, sizeof(keys));
    }
    Art_node4 &operator=(const Art_node4 &o) {
      if (this != &o) {
        Art_node::operator=(o);
        std::memcpy(keys, o.keys, sizeof(keys));
        children = o.children;
      }
      return *this;
    }
    Art_node4 &operator=(Art_node4 &&o) noexcept {
      if (this != &o) {
        Art_node::operator=(std::move(o));
        std::memcpy(keys, o.keys, sizeof(keys));
        children = std::move(o.children);
        std::memset(o.keys, 0, sizeof(keys));
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
    Art_node16(const Art_node16 &o) : Art_node(o) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = o.children;
    }
    Art_node16(Art_node16 &&o) noexcept : Art_node(std::move(o)) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = std::move(o.children);
      std::memset(o.keys, 0, sizeof(keys));
    }
    Art_node16 &operator=(const Art_node16 &o) {
      if (this != &o) {
        Art_node::operator=(o);
        std::memcpy(keys, o.keys, sizeof(keys));
        children = o.children;
      }
      return *this;
    }
    Art_node16 &operator=(Art_node16 &&o) noexcept {
      if (this != &o) {
        Art_node::operator=(std::move(o));
        std::memcpy(keys, o.keys, sizeof(keys));
        children = std::move(o.children);
        std::memset(o.keys, 0, sizeof(keys));
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
    Art_node48(const Art_node48 &o) : Art_node(o) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = o.children;
    }
    Art_node48(Art_node48 &&o) noexcept : Art_node(std::move(o)) {
      std::memcpy(keys, o.keys, sizeof(keys));
      children = std::move(o.children);
      std::memset(o.keys, 0, sizeof(keys));
    }
    Art_node48 &operator=(const Art_node48 &o) {
      if (this != &o) {
        Art_node::operator=(o);
        std::memcpy(keys, o.keys, sizeof(keys));
        children = o.children;
      }
      return *this;
    }
    Art_node48 &operator=(Art_node48 &&o) noexcept {
      if (this != &o) {
        Art_node::operator=(std::move(o));
        std::memcpy(keys, o.keys, sizeof(keys));
        children = std::move(o.children);
        std::memset(o.keys, 0, sizeof(keys));
      }
      return *this;
    }
    NodeType type() const override { return NODE48; }
    unsigned char keys[256];
    std::array<std::shared_ptr<Art_node>, 48> children;
  };

  struct Art_node256 : public Art_node {
    Art_node256() { children.fill(nullptr); }
    Art_node256(const Art_node256 &o) : Art_node(o) { children = o.children; }
    Art_node256(Art_node256 &&o) noexcept : Art_node(std::move(o)) { children = std::move(o.children); }
    Art_node256 &operator=(const Art_node256 &o) {
      if (this != &o) {
        Art_node::operator=(o);
        children = o.children;
      }
      return *this;
    }
    Art_node256 &operator=(Art_node256 &&o) noexcept {
      if (this != &o) {
        Art_node::operator=(std::move(o));
        children = std::move(o.children);
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

  // Updated make_art_node to use the allocator
  template <typename T, typename... Args>
  std::shared_ptr<T> make_art_node(Args &&...args) {
    static_assert(std::is_base_of_v<Art_node, T>, "T must derive from Art_node");
    return m_allocator.make<T>(std::forward<Args>(args)...);
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
    std::unique_lock lk(m_node_mutex);
    if (!m_tree) m_tree = std::make_unique<Art_tree>();
    m_tree->root = nullptr;
    m_tree->size.store(0, std::memory_order_release);
    m_inited = true;
    return 0;
  }

  inline int ART_tree_destroy() {
    std::unique_lock lk(m_node_mutex);
    if (m_tree) {
      m_tree->root = nullptr;
      m_tree.reset();
    }
    m_inited = false;
    return 0;
  }

  inline bool Art_initialized() const {
    std::shared_lock lk(m_node_mutex);
    return m_inited;
  }

  inline Art_tree *tree() const {
    std::shared_lock lk(m_node_mutex);
    return m_tree.get();
  }

  inline Art_node *root() const {
    std::shared_lock lk(m_node_mutex);
    return (m_inited && m_tree) ? m_tree->root.get() : nullptr;
  }

  void *ART_insert(const unsigned char *key, int key_len, void *value, uint32_t value_len);
  void *ART_delete(const unsigned char *key, int key_len);
  void *ART_search(const unsigned char *key, int key_len);
  std::vector<std::vector<uint8_t>> ART_search_all(const unsigned char *key, int key_len);
  int ART_iter(ART_Func cb, void *data);

  Art_leaf *ART_minimum();
  Art_leaf *ART_maximum();

  // Bulk insert for sorted data - bypasses recursive insert for better performance
  template <typename Iterator>
  int ART_bulk_insert(Iterator begin, Iterator end) {
    if (!m_inited || !m_tree) return -1;

    std::unique_lock lock(m_tree->tree_mutex);

    // Enable pool allocation for bulk operation
    ShannonBase::Utils::MemoryPool bulk_pool;
    enable_pool_allocation(&bulk_pool);

    int count = 0;
    for (auto it = begin; it != end; ++it) {
      // Direct leaf creation without recursive insert
      auto leaf = MakeBulkLeaf(it->key, it->key_len, it->value, it->value_len);
      if (leaf) {
        // Insert leaf directly
        if (!m_tree->root) {
          m_tree->root = leaf;
        } else {
          // Insert into existing tree
          if (!InsertBulkLeaf(m_tree->root, leaf, 0)) {
            disable_pool_allocation();
            return -1;
          }
        }
        count++;
      }
    }

    m_tree->size.fetch_add(count, std::memory_order_release);
    disable_pool_allocation();
    return count;
  }

 private:
  // Helper methods for bulk operations
  ArtLeafPtr MakeBulkLeaf(const unsigned char *key, int key_len, void *value, uint32_t value_len) {
    if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;
    return m_allocator.make<Art_leaf>(key, key_len, value, value_len);
  }

  bool InsertBulkLeaf(ArtNodePtr &node, const ArtLeafPtr &leaf, int depth);

  void *Recursive_insert(ArtNodePtr &node, const unsigned char *key, int key_len, void *value, uint32_t value_len,
                         int depth, int *old, int replace);

  ArtNodePtr Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth, void *&result);

  ArtLeafPtr Make_leaf(const unsigned char *key, int key_len, void *value, uint32_t value_len) {
    if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;
    // Use allocator instead of direct make_shared
    auto leaf = m_allocator.make<Art_leaf>(key, key_len, value, value_len);
    leaf->key.assign(key, key + key_len);
    leaf->add_value_unsafe(value, value_len);
    return leaf;
  }

  ArtNodePtr Find_child(const ArtNodePtr &n, unsigned char c);

  inline uint64_t art_size() const { return m_tree ? m_tree->size.load(std::memory_order_acquire) : 0; }

  Art_leaf *Minimum(const ArtNodePtr &n);
  Art_leaf *Maximum(const ArtNodePtr &n);
  void Copy_header(Art_node *dest, const Art_node *src);

  void Add_child(ArtNodePtr &new_node, ArtNodePtr &old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child256(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child48(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child16(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);
  ArtNodePtr Add_child4(ArtNodePtr old_node, unsigned char c, const ArtNodePtr &child);

  void Remove_child(ArtNodePtr &node, unsigned char c, const ArtNodePtr &child);
  void Remove_child256(ArtNodePtr &node, unsigned char c);
  void Remove_child48(ArtNodePtr &node, unsigned char c);
  void Remove_child16(ArtNodePtr &node, const ArtNodePtr &child);
  void Remove_child4(ArtNodePtr &node, const ArtNodePtr &child);

  int Recursive_iter(Art_node *node, ART_Func &cb, void *data);
  int Check_prefix(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth);
  int Prefix_mismatch(const ArtNodePtr &n, const unsigned char *key, int key_len, int depth);
  int Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth);
  int Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  int Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);

  mutable std::shared_mutex m_node_mutex;
  std::unique_ptr<Art_tree> m_tree{nullptr};
  bool m_inited{false};
  ArtNodeAllocator m_allocator;
  static ArtNodePtr null_ptr;
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ART_H__