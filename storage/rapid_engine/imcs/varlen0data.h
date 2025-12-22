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
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"  //uintxxx

#include "storage/innobase/include/ut0dbg.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/memory_pool.h"
/**
 * VarlenDataPool (Variable-length Data Pool)
 *
 * Purpose:
 * 1. Manage very long variable-length data (VARCHAR, TEXT, BLOB)
 * 2. Support overflow page mechanism
 * 3. Memory-efficient variable-length data storage
 *
 * Design Philosophy:
 * - Small data (< 256 bytes): Inline storage in CU data area
 * - Medium data (256 - 4096 bytes): Stored in Varlen Pool
 * - Large data (> 4096 bytes): Use overflow pages (external storage)
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
class MemoryPool;
class VarlenDataPool : public MemoryObject {
 public:
  static constexpr size_t INLINE_THRESHOLD = 256;     // Inline threshold
  static constexpr size_t POOL_THRESHOLD = 4096;      // Pool threshold
  static constexpr size_t DEFAULT_BLOCK_SIZE = 1024;  // Default block size
  static constexpr size_t MIN_BLOCK_SIZE = 64;        // Minimum block size
  static constexpr size_t MAX_BLOCK_SIZE = 65536;     // Maximum block size
  static constexpr size_t ALIGNMENT = 8;              // Memory alignment

  // Data Block
  /**
   * Data block header
   */
  struct SHANNON_ALIGNAS BlockHeader {
    uint32_t block_id{0};          // Block ID
    uint32_t size{0};              // Block size (including header)
    uint32_t used_size{0};         // Used size
    uint32_t magic{MAGIC_NUMBER};  // Magic number (for validation)

    BlockHeader *next_free{nullptr};  // Next free block (for free list)

    static constexpr uint32_t MAGIC_NUMBER = 0xDEADBEEF;

    BlockHeader() = default;

    bool is_valid() const { return magic == MAGIC_NUMBER; }

    size_t available_space() const { return size - sizeof(BlockHeader) - used_size; }
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
   * Variable-length data reference (stored in CU's main data area)
   */
  struct SHANNON_ALIGNAS VarlenReference {
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
   * Overflow page (external storage)
   */
  struct SHANNON_ALIGNAS OverflowPage {
    uint64_t page_id{0};      // Page ID
    uint32_t page_size{0};    // Page size
    uint32_t data_length{0};  // Actual data length

    std::string file_path;    // File path
    uint64_t file_offset{0};  // File offset

    // Optional: memory mapping
    uchar *mapped_data{nullptr};  // Mapped data pointer

    OverflowPage() = default;
  };

  /**
   * Allocation statistics
   */
  struct SHANNON_ALIGNAS AllocationStats {
    size_t allocation_count{0};
    size_t deallocation_count{0};
    size_t overflow_count{0};
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
   * Deallocate variable-length data
   * @param ref: Data reference
   * @return: true if successful
   */
  bool deallocate(const VarlenReference &ref);

  /**
   * Read variable-length data
   * @param ref: Data reference
   * @param buffer: Output buffer
   * @param buffer_size: Buffer size
   * @return: Actual read length
   */
  size_t read(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const;

  /**
   * Get data pointer (zero-copy, only for Pool storage)
   * @param ref: Data reference
   * @return: Data pointer, nullptr if failed
   */
  const uchar *get_data_ptr(const VarlenReference &ref) const;

  /**
   * Compact Pool (defragment)
   * @return: Number of bytes reclaimed
   */
  size_t compact();

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
   * Get overflow page count
   */
  size_t get_overflow_page_count() const;

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

  // Overflow page management
  std::unordered_map<uint64_t, std::unique_ptr<OverflowPage>> m_overflow_pages;

  // Next block ID
  std::atomic<uint32_t> m_next_block_id;

  // Next overflow page ID
  std::atomic<uint64_t> m_next_overflow_page_id;

  // Concurrency control
  mutable std::mutex m_mutex;

  // Statistics
  std::atomic<size_t> m_allocation_count;
  std::atomic<size_t> m_deallocation_count;
  std::atomic<size_t> m_overflow_count;

  // Memory pool (for block allocation)
  std::shared_ptr<Utils::MemoryPool> m_memory_pool;

  /**
   * Allocate in Pool
   */
  bool allocate_in_pool(const uchar *data, size_t length, VarlenReference &ref);

  /**
   * Deallocate from Pool
   */
  bool deallocate_from_pool(const VarlenReference &ref);

  /**
   * Read from Pool
   */
  size_t read_from_pool(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const;

  /**
   * Allocate overflow page
   */
  bool allocate_overflow(const uchar *data, size_t length, VarlenReference &ref);

  /**
   * Deallocate overflow page
   */
  bool deallocate_overflow(const VarlenReference &ref);

  /**
   * Read from overflow page
   */
  size_t read_from_overflow(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const;

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
   * Get free list index
   */
  size_t get_freelist_index(size_t size) const;

  /**
   * Align size
   */
  static size_t align_size(size_t size);

  /**
   * Generate overflow file path
   */
  std::string generate_overflow_file_path(uint64_t page_id) const;

  /**
   * Write to overflow file
   */
  bool write_overflow_to_file(OverflowPage *page, const uchar *data, size_t length) const;

  /**
   * Read from overflow file
   */
  size_t read_overflow_from_file(const OverflowPage *page, uint64_t offset, uchar *buffer, size_t buffer_size) const;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_VAR_LEN_DATA_POOL_H__