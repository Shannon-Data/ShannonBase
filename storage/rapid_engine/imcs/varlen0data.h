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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Column Unit.
*/
#ifndef __SHANNONBASE_VAR_LEN_DATA_POOL_H__
#define __SHANNONBASE_VAR_LEN_DATA_POOL_H__

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"
#include "storage/rapid_engine/utils/memory_pool.h"
/**
 * VarlenDataPool (Variable-length Data Pool)
 *
 * Purpose:
 * 1. Manage very long variable-length data (VARCHAR, TEXT, BLOB)
 * 2. Memory-efficient variable-length data storage
 *
 * Design Philosophy (current implementation):
 * - Data below INLINE_THRESHOLD is stored inline in the CU data area.
 * - Everything else is stored in this in-memory, append-only arena (bump
 *   allocator).  A reference returned by allocate() stays valid until the
 *   owning CU is destroyed (IMCU compact / GC); retire() only marks the slot
 *   as no longer referenced and never reuses its memory, so zero-copy readers
 *   are safe.
 * - reclaim() frees whole blocks whose allocations have all been retired.
 *   The arena is also reclaimed wholesale when its owning CU is destroyed.
 *
 * Memory Layout:
 * ┌─────────────────────────────────────┐
 * │  Pool Header                        │
 * ├─────────────────────────────────────┤
 * │  Free List (Free Block Linked List) │
 * ├─────────────────────────────────────┤
 * │  Data Blocks                        │
 * │  ┌─────────┬─────────┬─────────┐    │
 * │  │ Block 1 │ Block 2 │ Block 3 │    │
 * │  └─────────┴─────────┴─────────┘    │
 * └─────────────────────────────────────┘
 */

class Field;
namespace ShannonBase {
namespace Imcs {
class CU;
class MemoryPool;
class VarlenDataPool : public MemoryObject {
 public:
  // Only the tightly-coupled CU read path may obtain a zero-copy pointer into
  // the pool; external callers must use copy_data().
  friend class CU;

  static constexpr size_t INLINE_THRESHOLD = 256;     // Inline threshold
  static constexpr size_t DEFAULT_BLOCK_SIZE = 1024;  // Default block size
  static constexpr size_t MIN_BLOCK_SIZE = 64;        // Minimum block size
  static constexpr size_t MAX_BLOCK_SIZE = 65536;     // Maximum block size
  static constexpr size_t ALIGNMENT = 8;              // Memory alignment

  // Largest payload that can be represented end-to-end.  VarlenReference.length
  // and BlockHeader.{size,used_size} are all uint32_t, and align_size() adds up
  // to ALIGNMENT-1 bytes before masking, so the safe upper bound is
  // UINT32_MAX - (ALIGNMENT-1).
  static constexpr size_t MAX_VARLEN_VALUE_SIZE = std::numeric_limits<uint32_t>::max() - (ALIGNMENT - 1);

  // Data Block
  /**
   * Data block header
   */
  struct SHANNON_ALIGNAS BlockHeader {
    uint32_t block_id{0};          // Block ID
    uint32_t size{0};              // Block size (including header)
    uint32_t used_size{0};         // Used size
    uint32_t magic{MAGIC_NUMBER};  // Magic number (for validation)
    uint32_t live_allocations{0};  // Number of live allocations in this block

    BlockHeader *next_free{nullptr};  // Next free block (for free list)

    static constexpr uint32_t MAGIC_NUMBER = 0xDEADBEEF;

    BlockHeader() = default;

    bool is_valid() const { return magic == MAGIC_NUMBER; }

    size_t available_space() const { return size - used_size; }
  };

  /**
   * Data block
   */
  struct SHANNON_ALIGNAS DataBlock {
    BlockHeader header;
    uchar data[1];  // Flexible array (actual size determined at runtime)

    // Disable copying
    DataBlock(const DataBlock &) = delete;
    DataBlock &operator=(const DataBlock &) = delete;
  };

  /**
   * Variable-length data reference (stored in CU's main data area).
   *
   * The on-disk / in-slot footprint of this struct is exactly
   * VARLEN_REF_SIZE bytes (see static_assert below).
   */
  struct VarlenReference {
    uint32_t block_id{0};          // Block ID
    uint32_t offset{0};            // Offset within block
    uint32_t length{0};            // Data length
    uint8_t storage_type{INLINE};  // Storage type

    enum Storage_Type : uint8_t {
      INLINE = 0,   // Inline storage (stored in CU data area)
      POOL = 1,     // Pool storage (stored in VarlenDataPool)
      OVERFLOW = 2  // Overflow storage (external file)
    };

    VarlenReference() = default;

    bool is_inline() const { return storage_type == INLINE; }
    bool is_pool() const { return storage_type == POOL; }
    bool is_overflow() const { return storage_type == OVERFLOW; }
  };

  /**
   * In-memory size of a VarlenReference (may include tail padding).
   *
   * NOTE: this is NOT a persistence format.  If VarlenReference ever needs a
   * stable on-disk / CU-slot binary layout it must be encoded field-by-field
   * using VARLEN_REF_DISK_SIZE below, never sizeof(VarlenReference) (padding
   * and alignment are compiler/architecture dependent).
   */
  static constexpr size_t VARLEN_REF_SIZE = sizeof(VarlenReference);

  /** Stable binary footprint of a VarlenReference: 4+4+4+1 bytes, no padding. */
  static constexpr size_t VARLEN_REF_DISK_SIZE = 4 + 4 + 4 + 1;

  /**
   * Allocation statistics
   */
  struct SHANNON_ALIGNAS AllocationStats {
    size_t allocation_count{0};
    size_t retired_count{0};
    size_t total_size{0};
    size_t used_size{0};
    double fragmentation_ratio{0.0};
  };

  /**
   * Constructor
   * @param initial_size: Initial size (bytes)
   * @param mem_pool: Memory pool (optional)
   */
  explicit VarlenDataPool(size_t initial_size = 1024 * 1024, std::shared_ptr<Utils::MemoryPool> mem_pool = nullptr);

  virtual ~VarlenDataPool();

  // Disable copying
  VarlenDataPool(const VarlenDataPool &) = delete;
  VarlenDataPool &operator=(const VarlenDataPool &) = delete;

  /**
   * Allocate variable-length data
   * @param data: Data pointer
   * @param length: Data length
   * @param ref: Output reference
   * @return: true if successful
   */
  bool allocate(const uchar *data, size_t length, VarlenReference &ref);

  /**
   * Retire a previously allocated reference.
   *
   * Arena semantics: the slot is marked as no longer referenced but its
   * memory is NOT reused.  It is reclaimed only when the whole block becomes
   * fully retired (see reclaim()) or when the owning CU is destroyed.
   *
   * Must be called exactly once per allocation; double-retiring a reference
   * would under-count the owning block's live allocations.
   */
  void retire(const VarlenReference &ref);

  /**
   * Read variable-length data
   * @param ref: Data reference
   * @param buffer: Output buffer
   * @param buffer_size: Buffer size
   * @return: Actual read length
   */
  size_t read(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const;

  /**
   * Copy variable-length payload out of the pool under the pool lock.
   *
   * @param ref      Data reference (must be POOL storage).
   * @param out      Destination buffer (may be nullptr when max_len == 0).
   * @param max_len  Destination capacity.
   * @param out_len  [out] Number of bytes copied.
   * @return true on success.
   */
  bool copy_data(const VarlenReference &ref, void *out, size_t max_len, size_t &out_len) const;

  /**
   * RAII guard returned by get_data_ptr().  Holds a shared lock on the pool so
   * the underlying block cannot be reclaimed while the caller uses the pointer.
   */
  class VarlenReadGuard {
   public:
    VarlenReadGuard() = default;
    explicit VarlenReadGuard(const uchar *ptr) : m_ptr(ptr) {}
    VarlenReadGuard(std::shared_lock<std::shared_mutex> lock, const uchar *ptr) : m_lock(std::move(lock)), m_ptr(ptr) {}

    VarlenReadGuard(VarlenReadGuard &&) = default;
    VarlenReadGuard &operator=(VarlenReadGuard &&) = default;
    VarlenReadGuard(const VarlenReadGuard &) = delete;
    VarlenReadGuard &operator=(const VarlenReadGuard &) = delete;

    const uchar *get() const { return m_ptr; }
    operator const uchar *() const { return m_ptr; }

    VarlenReadGuard &operator=(const uchar *ptr) {
      m_lock = std::shared_lock<std::shared_mutex>{};
      m_ptr = ptr;
      return *this;
    }

   private:
    std::shared_lock<std::shared_mutex> m_lock;
    const uchar *m_ptr{nullptr};
  };

  /**
   * Reclaim blocks whose allocations have all been retired.
   * @return: Number of bytes reclaimed
   */
  size_t reclaim();

  /**
   * Get Pool size
   */
  size_t get_total_size() const;

  /**
   * Get used size
   */
  size_t get_used_size() const;

  /**
   * Get fragmentation ratio
   */
  double get_fragmentation_ratio() const;

  /**
   * Get block count
   */
  size_t get_block_count() const;

  /**
   * Get allocation statistics
   */
  AllocationStats get_stats() const;

  /**
   * Verify Pool integrity
   */
  bool validate() const;

  /**
   * Print statistics summary
   */
  void dump_summary(std::ostream &out) const;

  /**
   * Allocate in Pool (public — caller can force pool storage when inline
   * is not suitable, e.g. when the CU slot is only large enough for the
   * VarlenReference itself).
   */
  bool allocate_in_pool(const uchar *data, size_t length, VarlenReference &ref);

 private:
  // Pool metadata
  struct SHANNON_ALIGNAS PoolHeader {
    size_t total_size{0};   // Total size
    size_t used_size{0};    // Used size
    size_t block_count{0};  // Block count
    size_t free_blocks{0};  // Free block count

    PoolHeader() = default;
  };

  PoolHeader m_header;

  // Data block list
  using BlockDeleter = std::function<void(DataBlock *)>;
  using BlockPtr = std::unique_ptr<DataBlock, BlockDeleter>;
  std::vector<BlockPtr> m_blocks;

  // Free block linked list (grouped by size)
  struct SHANNON_ALIGNAS FreeList {
    BlockHeader *head{nullptr};
    size_t count{0};

    FreeList() = default;
  };

  // Free lists (different size levels)
  static constexpr size_t NUM_FREELISTS = 8;
  FreeList m_freelists[NUM_FREELISTS];

  // Block index (block_id -> block)
  std::unordered_map<uint32_t, DataBlock *> m_block_index;

  // Next block ID
  std::atomic<uint32_t> m_next_block_id;

  // Concurrency control
  mutable std::shared_mutex m_mutex;

  // Statistics
  std::atomic<size_t> m_allocation_count;
  std::atomic<size_t> m_retired_count;

  // Idempotent retire bookkeeping: keys are (block_id << 32) | offset.
  // Guarded by m_mutex.  Ensures a double-retire cannot under-count
  // live_allocations and make reclaim() free a block with live references.
  std::unordered_set<uint64_t> m_retired_refs;

  // Memory pool (for block allocation)
  std::shared_ptr<Utils::MemoryPool> m_memory_pool;

  /**
   * Retire a pool reference (internal).
   */
  void retire_in_pool(const VarlenReference &ref);

  /**
   * Read from Pool
   */
  size_t read_from_pool(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const;

  /**
   * Zero-copy pointer into the pool (internal — CU read path only).  External
   * callers must use copy_data().
   */
  VarlenReadGuard get_data_ptr(const VarlenReference &ref) const;

  /**
   * Allocate new block
   */
  DataBlock *allocate_new_block(size_t size);

  /**
   * Find free block
   */
  DataBlock *find_free_block(size_t required_size);

  /**
   * Add to free list
   */
  void add_to_freelist(BlockHeader *header);

  /**
   * Remove from free list
   */
  void remove_from_freelist(BlockHeader *header);

  /**
   * Remove from free list using an explicit (pre-computed) bucket index.
   * Use this when available_space() has already been changed and the
   * bucket index implied by the current header state no longer matches
   * the bucket the block is actually chained into.
   */
  void remove_from_freelist_at(BlockHeader *header, size_t idx);

  /**
   * Get free list index
   */
  size_t get_freelist_index(size_t size) const;

  /**
   * Align size
   */
  static size_t align_size(size_t size);
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_VAR_LEN_DATA_POOL_H__