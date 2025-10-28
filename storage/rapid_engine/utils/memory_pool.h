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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_UTILS_MEMORY_POOL_H__
#define __SHANNONBASE_UTILS_MEMORY_POOL_H__

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace Utils {
/**
 * @file memory_pool.h
 * @brief Production-grade hierarchical memory pool implementation
 *
 * Design Concepts:
 * ================
 *
 * 1. Pre-Allocation Strategy (Oracle In-Memory Area Style)
 *    - Allocates large contiguous memory block at initialization
 *    - Subdivides into small-block pool (64KB) and large-block pool (1MB)
 *    - Linear allocation with offset tracking for cache-friendly access
 *
 * 2. Hierarchical Architecture
 *    - Global Pool: Top-level memory container (e.g., 20GB)
 *    - Sub-Pools: Isolated memory regions carved from parent pool
 *    - Multi-tenant Support: Each sub-pool can have quota limits
 *    - Example: Global → Department Pools → Team Pools
 *
 * 3. Memory Management Features
 *    - Thread-safe allocation/deallocation with mutex protection
 *    - Free-block reuse with first-fit strategy
 *    - Automatic defragmentation by merging adjacent free blocks
 *    - Dynamic expansion for root pools (not sub-pools)
 *
 * 4. Performance Optimizations
 *    - Minimal system calls (pre-allocated memory)
 *    - Lock-free statistics with atomic operations
 *    - Background monitoring thread for auto-expansion/defragmentation
 *    - Aligned memory allocation for SIMD/cache line optimization
 *
 * 5. Use Cases
 *    - Database buffer pools (similar to Oracle SGA)
 *    - Multi-tenant applications with resource isolation
 *    - Game engines with per-module memory budgets
 *    - High-performance computing with NUMA awareness
 *
 * Architecture Overview:
 * =====================
 *
 *   ┌─────────────────────────────────────────────┐
 *   │         Global Memory Pool (20GB)           │
 *   │  ┌──────────────┐  ┌──────────────────┐    │
 *   │  │ Small Pool   │  │  Large Pool      │    │
 *   │  │ (20%, 4GB)   │  │  (80%, 16GB)     │    │
 *   │  │ Metadata     │  │  Data Blocks     │    │
 *   │  └──────────────┘  └──────────────────┘    │
 *   └─────────────────────────────────────────────┘
 *             │                    │
 *             ├────────────────────┼──────────┐
 *             ▼                    ▼          ▼
 *   ┌─────────────┐      ┌─────────────┐  ┌─────────────┐
 *   │  Sub-Pool A │      │  Sub-Pool B │  │  Sub-Pool C │
 *   │   (500MB)   │      │    (1GB)    │  │    (2GB)    │
 *   │   Tenant A  │      │   Tenant B  │  │   Tenant C  │
 *   └─────────────┘      └─────────────┘  └─────────────┘
 *
 * Thread Safety:
 * ==============
 * - Per-subpool mutex for allocation operations
 * - Global mutex for allocation tracking
 * - Atomic counters for statistics
 * - Lock-free read operations where possible
 *
 * Memory Layout:
 * ==============
 *
 *   Memory Base                                    Memory End
 *   │                                                    │
 *   ├────────────┬──────────┬──────┬─────────────┬──────┤
 *   │ Allocated  │  Free    │ Free │  Allocated  │ Free │
 *   │  Block 1   │  Block 1 │ Blk2 │   Block 2   │ Blk3 │
 *   └────────────┴──────────┴──────┴─────────────┴──────┘
 *   │◄────────current_offset────────►│
 *                                     └─ Next allocation point
 *
 */

/**
 * @class MemoryPool
 * @brief Thread-safe, hierarchical memory pool with sub-pool support
 *
 * This class implements a production-grade memory pool that:
 * - Pre-allocates large memory blocks for efficient allocation
 * - Supports creating isolated sub-pools from parent pools
 * - Provides tenant-based quota management
 * - Includes automatic defragmentation and expansion
 * - Offers comprehensive statistics and monitoring
 */
class MemoryPool : public std::enable_shared_from_this<MemoryPool> {
 public:
  /**
   * @enum SubPoolType
   * @brief Type of sub-pool for allocation targeting
   */
  enum class SubPoolType {
    SMALL_BLOCK,  ///< Small block pool (~64KB blocks) for metadata
    LARGE_BLOCK   ///< Large block pool (~1MB blocks) for data
  };

  /**
   * @enum LogLevel
   * @brief Logging verbosity levels
   */
  enum class LogLevel {
    DEBUG,    ///< Detailed debug information
    INFO,     ///< General information messages
    WARNING,  ///< Warning messages
    ERROR     ///< Error messages
  };

  /**
   * @struct PoolStats
   * @brief Runtime statistics for memory pool monitoring
   */
  struct PoolStats {
    std::atomic<size_t> total_capacity{0};         ///< Total pool capacity in bytes
    std::atomic<size_t> allocated_bytes{0};        ///< Bytes allocated from pool
    std::atomic<size_t> used_bytes{0};             ///< Actual bytes in use
    std::atomic<size_t> peak_usage{0};             ///< Peak memory usage
    std::atomic<size_t> allocation_count{0};       ///< Number of allocations
    std::atomic<size_t> deallocation_count{0};     ///< Number of deallocations
    std::atomic<size_t> expansion_count{0};        ///< Number of expansions
    std::atomic<size_t> defragmentation_count{0};  ///< Number of defragmentations
    std::atomic<size_t> failed_allocations{0};     ///< Failed allocation attempts

    /**
     * @struct Snapshot
     * @brief Point-in-time statistics snapshot
     */
    struct Snapshot {
      size_t total_capacity;
      size_t allocated_bytes;
      size_t used_bytes;
      size_t peak_usage;
      size_t allocation_count;
      size_t deallocation_count;
      size_t expansion_count;
      size_t defragmentation_count;
      size_t failed_allocations;
      double usage_percentage;     ///< Percentage of pool in use
      double fragmentation_ratio;  ///< Ratio of wasted space
    };

    /**
     * @brief Capture current statistics snapshot
     * @return Snapshot containing current statistics
     */
    [[nodiscard]] Snapshot snapshot() const noexcept;
  };

  /**
   * @struct TenantConfig
   * @brief Configuration for tenant-specific quota management
   */
  struct TenantConfig {
    std::string name;                         ///< Tenant identifier
    size_t quota;                             ///< Memory quota in bytes (0 = disabled)
    std::atomic<size_t> current_usage{0};     ///< Current memory usage
    std::atomic<size_t> peak_usage{0};        ///< Peak memory usage
    std::atomic<size_t> allocation_count{0};  ///< Number of allocations

    TenantConfig(const std::string &n, size_t q);
  };

  struct Config {
    // basic configuration.
    size_t initial_size;
    double small_pool_ratio;
    size_t alignment;
    bool allow_expansion;
    size_t min_expansion_size;
    double expansion_trigger_threshold;
    bool auto_defragmentation;
    double defrag_trigger_threshold;
    LogLevel log_level;

    // Sub-pool specific configuration
    std::shared_ptr<MemoryPool> parent_pool;  ///< Parent pool (for sub-pools)
    bool is_sub_pool;                         ///< Is this a sub-pool?

    Config()
        : initial_size(SHANNON_DEFAULT_MEMRORY_SIZE),
          small_pool_ratio(0.2),
          alignment(CACHE_LINE_SIZE),
          allow_expansion(true),
          min_expansion_size(128 * SHANNON_MB),
          expansion_trigger_threshold(0.9),  // 90%
          auto_defragmentation(true),
          defrag_trigger_threshold(0.3),  // 30%
          log_level(LogLevel::INFO),
          parent_pool{nullptr},
          is_sub_pool(false) {}
  };

  explicit MemoryPool(const Config &config = Config());
  virtual ~MemoryPool() noexcept;

  // Non-copyable and non-movable
  MemoryPool(const MemoryPool &) = delete;
  MemoryPool &operator=(const MemoryPool &) = delete;

  // Memory Allocation/Deallocation Interface
  /**
   * @brief Allocate memory from the pool
   * @param size Size in bytes to allocate
   * @param pool_type Target sub-pool type (SMALL_BLOCK or LARGE_BLOCK)
   * @param tenant_id Optional tenant identifier for quota tracking
   * @return Pointer to allocated memory
   * @throws std::bad_alloc if allocation fails
   * @throws std::runtime_error if tenant quota exceeded
   */
  void *allocate(size_t size, SubPoolType pool_type = SubPoolType::LARGE_BLOCK, const std::string &tenant_id = "");

  /**
   * @brief Allocate memory with automatic pool selection
   * @param size Size in bytes to allocate
   * @param tenant_id Optional tenant identifier
   * @return Pointer to allocated memory
   * @note Automatically selects SMALL_BLOCK for size < 64KB, LARGE_BLOCK otherwise
   */
  void *allocate_auto(size_t size, const std::string &tenant_id = "");

  /**
   * @brief Deallocate previously allocated memory
   * @param ptr Pointer to memory to deallocate
   * @param size Original allocation size
   * @note This function never throws
   */
  void deallocate(void *ptr, size_t size) noexcept;

  // Pool Management Interface
  /**
   * @brief Expand the memory pool by adding more capacity
   * @param additional_size Size in bytes to add
   * @return true if expansion succeeded, false otherwise
   * @note Only works for root pools, not sub-pools
   * @note Requires allow_expansion=true in configuration
   */
  bool expand(size_t additional_size);

  /**
   * @brief Manually trigger memory defragmentation
   * @return true if defragmentation succeeded
   * @note Merges adjacent free blocks to reduce fragmentation
   */
  bool defragment();

  /**
   * @brief Reset pool to initial state (clears all allocations)
   * @warning All existing pointers become invalid after reset
   */
  void reset() noexcept;

  // Tenant/Quota Management Interface
  /**
   * @brief Set memory quota for a tenant
   * @param tenant_id Tenant identifier
   * @param quota Memory limit in bytes (0 to disable tenant)
   * @note Similar to Oracle PDB INMEMORY_SIZE setting
   */
  void set_tenant_quota(const std::string &tenant_id, size_t quota);

  /**
   * @brief Get current memory usage for a tenant
   * @param tenant_id Tenant identifier
   * @return Current usage in bytes
   */
  size_t get_tenant_usage(const std::string &tenant_id) const;

  // Sub-Pool Management Interface
  /**
   * @brief Create a sub-pool from this pool
   * @param sub_pool_size Size of sub-pool to create
   * @param tenant_name Name/identifier for the sub-pool
   * @return Shared pointer to newly created sub-pool
   * @throws std::runtime_error if called on a sub-pool
   * @throws std::bad_alloc if insufficient memory
   *
   * @note The sub-pool memory is allocated from this pool
   * @note Sub-pools cannot create further sub-pools
   * @note Sub-pools automatically return memory when destroyed
   *
   * Example:
   * @code
   * auto global_pool = std::make_shared<MemoryPool>(config);
   * auto table_pool = global_pool->create_sub_pool(500*1024*1024, "table_engine");
   * void* data = table_pool->allocate_auto(1024*1024);
   * @endcode
   */
  std::shared_ptr<MemoryPool> create_sub_pool(size_t sub_pool_size, const std::string &tenant_name = "");

  /**
   * @brief Static factory method to create sub-pool from parent
   * @param parent_pool Parent memory pool
   * @param tenant_name Tenant/sub-pool name
   * @param sub_pool_size Size of sub-pool
   * @return Shared pointer to sub-pool
   * @throws std::invalid_argument if parent_pool is null
   *
   * Example:
   * @code
   * auto sub_pool = MemoryPool::create_from_parent(
   *     global_pool, "my_module", 1024*1024*1024);
   * @endcode
   */
  static std::shared_ptr<MemoryPool> create_from_parent(std::shared_ptr<MemoryPool> parent_pool,
                                                        const std::string &tenant_name, size_t sub_pool_size);

  /**
   * @brief Check if this is a sub-pool
   * @return true if this is a sub-pool, false if root pool
   */
  bool is_sub_pool() const noexcept { return m_config.is_sub_pool; }

  /**
   * @brief Get parent pool (if this is a sub-pool)
   * @return Shared pointer to parent pool, or nullptr if root pool
   */
  std::shared_ptr<MemoryPool> get_parent_pool() const { return m_config.parent_pool; }

  // Statistics and Monitoring Interface
  /**
   * @brief Print detailed statistics to stdout
   * @note Includes pool stats, sub-pool details, and tenant usage
   */
  void print_stats() const noexcept;

  /**
   * @brief Get current statistics snapshot
   * @return Snapshot containing current statistics
   */
  [[nodiscard]] PoolStats::Snapshot stats() const noexcept;

  /**
   * @brief Set logging verbosity level
   * @param level New log level
   */
  void set_log_level(LogLevel level);

 private:
  /**
   * @struct FreeBlock
   * @brief Represents a free memory block
   */
  struct FreeBlock {
    size_t offset;  ///< Offset from pool base
    size_t size;    ///< Size of free block
  };

  /**
   * @struct SubPool
   * @brief Internal representation of a memory sub-pool region
   */
  struct SubPool {
    void *memory_base;                   ///< Base address of pool memory
    size_t total_size;                   ///< Total size of pool
    size_t current_offset;               ///< Current allocation offset
    std::vector<FreeBlock> free_blocks;  ///< List of free blocks
    mutable std::mutex mutex;            ///< Mutex for thread safety

    bool is_from_parent;                     ///< True if memory from parent pool
    std::shared_ptr<MemoryPool> parent_ref;  ///< Reference to parent pool

    SubPool(size_t size, size_t alignment);  ///< Constructor for root pool
    SubPool(void *base, size_t size);        ///< Constructor for sub-pool
    ~SubPool();
  };

  struct AllocationInfo {
    size_t offset;          ///< Offset from pool base
    size_t aligned_size;    ///< Aligned allocation size
    int pool_index;         ///< Index of sub-pool (0=small, 1=large)
    std::string tenant_id;  ///< Associated tenant ID
  };

  // Member Variables
  Config m_config;                                                           ///< Pool configuration
  std::vector<std::unique_ptr<SubPool>> m_subpools;                          ///< Sub-pool storage
  std::unordered_map<void *, AllocationInfo> m_allocations;                  ///< Allocation tracking
  std::unordered_map<std::string, std::unique_ptr<TenantConfig>> m_tenants;  ///< Tenant configs

  std::vector<std::shared_ptr<MemoryPool>> m_child_pools;  ///< Child sub-pools
  mutable std::mutex m_child_pools_mutex;                  ///< Child pool mutex

  mutable std::mutex m_alloc_mutex;   ///< Allocation mutex
  mutable std::mutex m_tenant_mutex;  ///< Tenant mutex
  PoolStats m_stats;                  ///< Pool statistics

  std::thread m_monitor_thread;  ///< Background monitor
  std::atomic<bool> m_shutdown;  ///< Shutdown flag

  // Internal Methods
  void validate_config();
  void initialize_pools(size_t total_size);
  void initialize_as_sub_pool(void *parent_memory, size_t size);
  void *allocate_from_pool(int pool_idx, size_t aligned_size, size_t actual_size, const std::string &tenant_id);
  bool expand_subpool(int pool_index, size_t additional_size);
  bool defragment_subpool(int pool_index);
  void merge_adjacent_free_blocks(SubPool *subpool);
  void *try_allocate_from_free_blocks(SubPool *subpool, size_t aligned_size);
  void record_allocation(void *ptr, size_t aligned_size, size_t actual_size, int pool_index,
                         const std::string &tenant_id);
  bool check_tenant_quota(const std::string &tenant_id, size_t size);
  void update_tenant_usage(const std::string &tenant_id, ssize_t delta);
  void update_peak_usage() noexcept;
  void monitor_loop();
  void cleanup() noexcept;
  void log(LogLevel level, const std::string &message) const;

  // Static Utility Methods
  static size_t align_up(size_t size, size_t alignment) noexcept;
  static void *aligned_alloc_portable(size_t alignment, size_t size) noexcept;
  static void free_aligned_portable(void *ptr) noexcept;
  static std::string format_size(size_t bytes);
};

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_MEMORY_POOL_H__