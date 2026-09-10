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

  // Inlined prefix bytes per inner node. Path compression stays correct beyond
  // this bound -- partial_len keeps the true length and the search paths fall
  // back to a leaf key (see Prefix_mismatch) -- so this trades a rare extra key
  // probe for a much smaller index. 16 covers the composite keys in practice
  // (TPC-H's widest primary key encodes to 12 bytes). Leaves do not carry it.
  static constexpr uint32_t MAX_PREFIX_LEN = 16;

  using ART_Func =
      std::function<int(void *data, const void *key, uint32_t key_len, const void *value, uint32_t value_len)>;

  /**
   * Common base of every node: the type tag, and nothing else.
   *
   * There are no virtual functions here on purpose.  shared_ptr records the
   * concrete deleter in its control block, so a shared_ptr<Art_node> built
   * from make_shared<Art_node4>() destroys an Art_node4 correctly without a
   * vtable.  Dropping the vptr takes 8 bytes off every node -- paid once per
   * indexed row for leaves -- and turns type(), which runs on every step of
   * every descent, from an indirect call into a byte load.
   */
  struct Art_node {
    explicit Art_node(NodeType t) : node_type(static_cast<uint8_t>(t)) {}
    Art_node(const Art_node &) = default;
    Art_node &operator=(const Art_node &) = default;

    NodeType type() const { return static_cast<NodeType>(node_type); }

    uint8_t node_type{UNKNOWN};
  };

  /**
   * Base of the four inner-node fan-outs.  Only inner nodes own a compressed
   * path prefix and a child count; leaves used to inherit both, plus a
   * shared_mutex nothing ever locked, at a cost of 76 bytes per indexed row.
   *
   * Concurrency is the tree-wide Art_tree::tree_mutex, not per node.  The
   * eventual Optimistic Lock Coupling migration wants a version counter here,
   * not a mutex, so no lock word is reserved.
   */
  struct Art_inner_node : public Art_node {
    explicit Art_inner_node(NodeType t) : Art_node(t) { std::memset(partial, 0, MAX_PREFIX_LEN); }

    uint8_t num_children{0};
    uint32_t partial_len{0};
    unsigned char partial[MAX_PREFIX_LEN];
  };

  /**
   * ART leaf: one key and the values stored under it.
   *
   * Rapid's indexes are non-unique, so a key can carry several row ids.  This
   * is the classic ART leaf shape -- the key bytes live inside the node rather
   * than behind a std::vector -- with a small inline buffer that holds the key
   * and the first values together:
   *
   *     buffer: [ key_len bytes of key ][ value_count * value_len bytes ]
   *
   * The buffer is m_inline while it fits and one heap block afterwards.  A
   * 12-byte composite key plus one 8-byte row id therefore costs a single
   * 64-byte heap chunk (40-byte node + make_shared's 16-byte control block,
   * rounded up by the allocator).  The previous vector-of-vectors leaf cost
   * four allocations totalling roughly 330 bytes for the same row.
   *
   * All values under one key share a width: they are row ids.  The width is
   * taken from the first value stored and a later value of a different width
   * is refused rather than silently packed wrong.
   *
   * Not thread-safe on its own.  Every mutation runs under Art_tree::tree_mutex
   * held exclusively and every read under it held at least shared; the leaf's
   * own shared_mutex was redundant with that and has been removed.  Callers
   * that walk leaves outside ART's own methods -- ARTIterator -- must hold that
   * lock themselves.
   */
  struct Art_leaf : public Art_node {
    // Bytes of the node reserved for key + values before spilling to the heap.
    // 24 is the largest value that keeps sizeof(Art_leaf) at 40, which is what
    // make_shared turns into a single 64-byte allocation.
    static constexpr uint32_t INLINE_CAPACITY = 24;

    Art_leaf() : Art_node(LEAF) { m_inline[0] = 0; }

    Art_leaf(const unsigned char *key_data, int key_length, const void *value_data, uint32_t value_length)
        : Art_node(LEAF) {
      m_inline[0] = 0;  // makes m_inline the active union member
      const uint32_t klen = (key_data && key_length > 0) ? static_cast<uint32_t>(key_length) : 0;
      const uint32_t vlen = (value_data && value_length && value_length <= UINT16_MAX) ? value_length : 0;
      Grow_to(static_cast<uint64_t>(klen) + vlen);  // throws bad_alloc, as the vector leaf did
      if (klen) std::memcpy(Buffer(), key_data, klen);
      m_key_len = klen;
      if (vlen) {
        std::memcpy(Buffer() + klen, value_data, vlen);
        m_value_len = static_cast<uint16_t>(vlen);
        m_value_count = 1;
      }
    }

    ~Art_leaf() {
      if (!Is_inline()) ::operator delete(m_heap);
    }

    // A leaf owns a heap buffer and is only ever reached through shared_ptr.
    Art_leaf(const Art_leaf &) = delete;
    Art_leaf &operator=(const Art_leaf &) = delete;

    const unsigned char *key() const { return m_key_len ? Buffer() : nullptr; }
    uint32_t key_length() const { return m_key_len; }
    uint32_t value_count() const { return m_value_count; }
    uint32_t value_length() const { return m_value_len; }

    const unsigned char *value_at(uint32_t index) const {
      return (index < m_value_count) ? Buffer() + m_key_len + index * m_value_len : nullptr;
    }
    unsigned char *mutable_value_at(uint32_t index) {
      return (index < m_value_count) ? Buffer() + m_key_len + index * m_value_len : nullptr;
    }

    /**
     * Append one value under this key.  False when the width does not match
     * the values already stored, or when the leaf would exceed 4GB; throws
     * std::bad_alloc out of memory, which is what the vector leaf did.
     */
    bool add_value(const void *value, uint32_t value_len) {
      if (!value || value_len == 0 || value_len > UINT16_MAX) return false;
      if (m_value_count == 0) m_value_len = static_cast<uint16_t>(value_len);
      if (value_len != m_value_len) return false;

      const uint64_t need = uint64_t(m_key_len) + uint64_t(m_value_count + 1) * m_value_len;
      if (need > m_capacity) {
        // Double the value slots so appending N duplicates costs O(log N)
        // reallocations instead of N.
        const uint64_t slots = std::max<uint64_t>(uint64_t(m_value_count) * 2, 1);
        if (!Grow_to(uint64_t(m_key_len) + slots * m_value_len)) return false;
      }
      std::memcpy(Buffer() + m_key_len + m_value_count * m_value_len, value, m_value_len);
      ++m_value_count;
      return true;
    }

    /** Remove the first value equal to `value`. False when there is no match. */
    bool remove_value(const void *value, uint32_t value_len) {
      if (!value || m_value_count == 0 || value_len != m_value_len) return false;
      unsigned char *base = Buffer() + m_key_len;
      for (uint32_t i = 0; i < m_value_count; ++i) {
        if (std::memcmp(base + i * m_value_len, value, m_value_len) != 0) continue;
        const uint32_t tail = (m_value_count - i - 1) * m_value_len;
        if (tail) std::memmove(base + i * m_value_len, base + (i + 1) * m_value_len, tail);
        if (--m_value_count == 0) m_value_len = 0;
        return true;
      }
      return false;
    }

    /** Overwrite the first value in place (the `replace` insert mode). */
    bool replace_first_value(const void *value, uint32_t value_len) {
      if (!value || m_value_count == 0 || value_len != m_value_len) return false;
      std::memcpy(Buffer() + m_key_len, value, m_value_len);
      return true;
    }

    /**
     * Drop the key and every value, releasing the heap buffer.
     *
     * The node itself stays alive while any shared_ptr still points at it --
     * an in-flight ARTIterator, say -- and reads as an empty leaf: key() is
     * null and value_count() is zero.
     */
    void clear() {
      if (!Is_inline()) ::operator delete(m_heap);
      m_key_len = 0;
      m_value_count = 0;
      m_value_len = 0;
      m_capacity = INLINE_CAPACITY;
      m_inline[0] = 0;  // makes m_inline the active union member again
    }

   private:
    bool Is_inline() const { return m_capacity <= INLINE_CAPACITY; }
    unsigned char *Buffer() { return Is_inline() ? m_inline : m_heap; }
    const unsigned char *Buffer() const { return Is_inline() ? m_inline : m_heap; }

    /**
     * Move to a buffer of at least `need` bytes, keeping the live prefix.
     * False only when `need` does not fit m_capacity's 32 bits; allocation
     * failure throws std::bad_alloc rather than leaving a half-built leaf.
     */
    bool Grow_to(uint64_t need) {
      if (need <= m_capacity) return true;
      if (need > UINT32_MAX) return false;
      auto *fresh = static_cast<unsigned char *>(::operator new(static_cast<size_t>(need)));
      const uint32_t keep = m_key_len + m_value_count * m_value_len;
      if (keep) std::memcpy(fresh, Buffer(), keep);
      unsigned char *stale = Is_inline() ? nullptr : m_heap;  // read before m_capacity moves
      m_heap = fresh;                                         // makes m_heap the active union member
      m_capacity = static_cast<uint32_t>(need);
      ::operator delete(stale);
      return true;
    }

    uint16_t m_value_len{0};
    uint32_t m_key_len{0};
    uint32_t m_value_count{0};
    uint32_t m_capacity{INLINE_CAPACITY};
    // Exactly one member is ever live, selected by Is_inline(): m_inline until
    // key + values outgrow it, m_heap from then on. Nothing reads the other.
    union {
      unsigned char *m_heap;
      unsigned char m_inline[INLINE_CAPACITY];
    };
  };

  struct Art_node4 : public Art_inner_node {
    Art_node4() : Art_inner_node(NODE4) {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }
    unsigned char keys[4];
    std::array<std::shared_ptr<Art_node>, 4> children;
  };

  struct Art_node16 : public Art_inner_node {
    Art_node16() : Art_inner_node(NODE16) {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }
    unsigned char keys[16];
    std::array<std::shared_ptr<Art_node>, 16> children;
  };

  struct Art_node48 : public Art_inner_node {
    Art_node48() : Art_inner_node(NODE48) {
      std::memset(keys, 0, sizeof(keys));
      children.fill(nullptr);
    }
    unsigned char keys[256];
    std::array<std::shared_ptr<Art_node>, 48> children;
  };

  struct Art_node256 : public Art_inner_node {
    Art_node256() : Art_inner_node(NODE256) { children.fill(nullptr); }
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
  static Art_leaf *to_leaf(Art_node *node) { return is_leaf(node) ? static_cast<Art_leaf *>(node) : nullptr; }

  /** Narrow to the inner-node base, which alone owns partial[]/num_children. */
  static Art_inner_node *to_inner(Art_node *node) {
    return (node && node->type() != LEAF) ? static_cast<Art_inner_node *>(node) : nullptr;
  }
  static const Art_inner_node *to_inner(const Art_node *node) {
    return (node && node->type() != LEAF) ? static_cast<const Art_inner_node *>(node) : nullptr;
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
  /**
   * Remove the leaf addressed by `key` with every value under it.  Returns a
   * non-null marker when the key was present, nullptr otherwise -- the value
   * bytes are gone with the leaf, so there is nothing to hand back.
   */
  void *ART_delete(const unsigned char *key, int key_len);
  /**
   * Remove a single value from the leaf addressed by `key`, leaving the leaf
   * (and its other duplicate values) in place.  The leaf is removed only when
   * its last value is deleted.  Returns a non-null marker when a matching
   * value was found and removed, nullptr otherwise.
   */
  void *ART_delete_value(const unsigned char *key, int key_len, const void *value, uint32_t value_len);
  /**
   * Address of the last value stored under `key`, or nullptr.
   *
   * The pointer is into the leaf and stops being safe the moment this call
   * returns: the tree lock it took is released, and a concurrent writer may
   * free the leaf.  Prefer ART_search_copy unless the caller can prove no
   * writer can run.
   */
  void *ART_search(const unsigned char *key, int key_len);
  /**
   * Copy the last value stored under `key` into `out`, under the tree lock.
   * False when the key is absent or the stored values are not `out_len` wide.
   */
  bool ART_search_copy(const unsigned char *key, int key_len, void *out, uint32_t out_len);
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
    enable_pool_allocation(&m_bulk_pool);

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
  ArtLeafPtr MakeBulkLeaf(const unsigned char *key, int key_len, const void *value, uint32_t value_len) {
    if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;
    return m_allocator.make<Art_leaf>(key, key_len, value, value_len);
  }

  bool InsertBulkLeaf(ArtNodePtr &node, const ArtLeafPtr &leaf, int depth) {
    // TODO: Implement bulk insert logic that directly places the leaf in the correct position in the tree
    return false;  // Placeholder
  }

  void *Recursive_insert(ArtNodePtr &node, const unsigned char *key, int key_len, void *value, uint32_t value_len,
                         int depth, int *old, int replace);

  ArtNodePtr Recursive_delete(ArtNodePtr &node, const unsigned char *key, int key_len, int depth, void *&result);

  ArtLeafPtr Make_leaf(const unsigned char *key, int key_len, const void *value, uint32_t value_len) {
    if (!key || key_len <= 0 || !value || value_len == 0) return nullptr;
    // Use allocator instead of direct make_shared
    auto leaf = m_allocator.make<Art_leaf>(key, key_len, value, value_len);
    return leaf;
  }

  /** Leaf holding exactly `key`, or nullptr. Caller must hold tree_mutex. */
  Art_leaf *Find_leaf(const unsigned char *key, int key_len);

  ArtNodePtr Find_child(const ArtNodePtr &n, unsigned char c);

  /**
   * Address of the slot inside `n` that holds the child labelled `c`, so a
   * caller can replace the child in place (leaf -> Node4 on insert, or a
   * collapsed node on delete).  nullptr when `n` is a leaf or has no such
   * edge; the slot itself may hold nullptr.  Valid only while tree_mutex is
   * held and no structural change intervenes.
   */
  ArtNodePtr *Find_child_slot(Art_node *n, unsigned char c);

  inline uint64_t art_size() const { return m_tree ? m_tree->size.load(std::memory_order_acquire) : 0; }

  Art_leaf *Minimum(const Art_node *n);
  Art_leaf *Maximum(const Art_node *n);
  void Copy_header(Art_inner_node *dest, const Art_inner_node *src);

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
  int Check_prefix(const Art_inner_node *n, const unsigned char *key, int key_len, int depth);
  int Prefix_mismatch(const Art_inner_node *n, const unsigned char *key, int key_len, int depth);
  int Longest_common_prefix(const Art_leaf *l1, const Art_leaf *l2, int depth);
  int Leaf_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);
  int Leaf_partial_matches(const Art_leaf *n, const unsigned char *key, int key_len, int depth);

  mutable std::shared_mutex m_node_mutex;
  std::unique_ptr<Art_tree> m_tree{nullptr};
  bool m_inited{false};
  ArtNodeAllocator m_allocator;
  ShannonBase::Utils::MemoryPool m_bulk_pool;
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_ART_H__