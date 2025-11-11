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
#include "storage/rapid_engine/utils/memory_pool.h"

#include "sql/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ShannonBase {
namespace Utils {

MemoryPool::PoolStats::Snapshot MemoryPool::PoolStats::snapshot() const noexcept {
  size_t cap = total_capacity.load(std::memory_order_relaxed);
  size_t used = used_bytes.load(std::memory_order_relaxed);
  size_t alloc = allocated_bytes.load(std::memory_order_relaxed);

  // Calculate fragmentation ratio: wasted space due to alignment and free blocks
  double frag_ratio = 0.0;
  if (alloc > 0 && used > 0) {
    frag_ratio = 1.0 - ((double)used / (double)alloc);
  }

  return {cap,
          alloc,
          used,
          peak_usage.load(std::memory_order_relaxed),
          allocation_count.load(std::memory_order_relaxed),
          deallocation_count.load(std::memory_order_relaxed),
          expansion_count.load(std::memory_order_relaxed),
          defragmentation_count.load(std::memory_order_relaxed),
          failed_allocations.load(std::memory_order_relaxed),
          cap > 0 ? (double)used * 100.0 / cap : 0.0,
          frag_ratio};
}

MemoryPool::TenantConfig::TenantConfig(const std::string &n, size_t q) : name(n), quota(q) {}

MemoryPool::SubPool::SubPool(size_t size, size_t alignment)
    : memory_base(nullptr), total_size(size), current_offset(0), is_from_parent(false), parent_ref(nullptr) {
  // Allocate new memory for root pool
  memory_base = aligned_alloc_portable(alignment, size);
  if (!memory_base) {
    throw std::bad_alloc();
  }
}

MemoryPool::SubPool::SubPool(void *base, size_t size)
    : memory_base(base), total_size(size), current_offset(0), is_from_parent(true), parent_ref(nullptr) {
  // Use memory provided by parent pool (no allocation)
}

MemoryPool::SubPool::~SubPool() {
  if (memory_base && !is_from_parent) {
    // Only free memory if it was allocated by this pool
    free_aligned_portable(memory_base);
  }
  // If is_from_parent=true, memory will be freed by parent pool
}

MemoryPool::MemoryPool(const Config &config) : m_config(config), m_shutdown(false) {
  validate_config();

  if (!m_config.is_sub_pool) {
    // Root pool: allocate new memory
    initialize_pools(m_config.initial_size);

    // Start background monitoring thread
    if (m_config.allow_expansion || m_config.auto_defragmentation) {
      m_monitor_thread = std::thread(&MemoryPool::monitor_loop, this);
    }

    log(LogLevel::INFO, "MemoryPool initialized");
    log(LogLevel::INFO, "  Total: " + format_size(m_config.initial_size));
    log(LogLevel::INFO, "  Small Pool: " + format_size(m_subpools[0]->total_size));
    log(LogLevel::INFO, "  Large Pool: " + format_size(m_subpools[1]->total_size));
  }
  // Sub-pool initialization is deferred to initialize_as_sub_pool()
}

MemoryPool::~MemoryPool() noexcept {
  m_shutdown.store(true, std::memory_order_release);

  if (m_monitor_thread.joinable()) {
    m_monitor_thread.join();
  }

  cleanup();
  log(LogLevel::INFO, "MemoryPool shutdown");
}

void *MemoryPool::allocate(size_t size, SubPoolType pool_type, const std::string &tenant_id) {
  if (size == 0) [[unlikely]] {
    size = m_config.alignment;
  }

  try {
    // Check tenant quota
    if (!tenant_id.empty() && !check_tenant_quota(tenant_id, size)) {
      log(LogLevel::WARNING, "Tenant " + tenant_id + " quota exceeded");
      m_stats.failed_allocations.fetch_add(1, std::memory_order_relaxed);
      throw std::runtime_error("Tenant quota exceeded: " + tenant_id);
    }

    size_t aligned_size = align_up(size, m_config.alignment);
    int pool_idx = (pool_type == SubPoolType::SMALL_BLOCK) ? 0 : 1;

    void *ptr = allocate_from_pool(pool_idx, aligned_size, size, tenant_id);

    if (!ptr) {
      m_stats.failed_allocations.fetch_add(1, std::memory_order_relaxed);
      throw std::bad_alloc();
    }

    return ptr;

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "Allocation failed: " + std::string(e.what()));
    throw;
  }
}

void *MemoryPool::allocate_auto(size_t size, const std::string &tenant_id) {
  SubPoolType type = (size < 64 * 1024) ? SubPoolType::SMALL_BLOCK : SubPoolType::LARGE_BLOCK;
  return allocate(size, type, tenant_id);
}

void MemoryPool::deallocate(void *ptr, size_t size) noexcept {
  if (!ptr) return;

  try {
    std::scoped_lock lock(m_alloc_mutex);
    auto it = m_allocations.find(ptr);
    if (it == m_allocations.end()) {
      log(LogLevel::WARNING, "Attempted to deallocate invalid pointer");
      return;
    }

    const AllocationInfo &info = it->second;
    auto &subpool = m_subpools[info.pool_index];

    {
      std::scoped_lock pool_lock(subpool->mutex);
      FreeBlock block{info.offset, info.aligned_size};
      subpool->free_blocks.push_back(block);
      merge_adjacent_free_blocks(subpool.get());
    }

    if (!info.tenant_id.empty()) {
      update_tenant_usage(info.tenant_id, -(ssize_t)size);
    }

    m_stats.used_bytes.fetch_sub(size, std::memory_order_relaxed);
    m_stats.deallocation_count.fetch_add(1, std::memory_order_relaxed);

    m_allocations.erase(it);

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "Deallocation error: " + std::string(e.what()));
  }
}

bool MemoryPool::expand(size_t additional_size) {
  if (!m_config.allow_expansion) {
    log(LogLevel::WARNING, "Expansion not allowed");
    return false;
  }

  if (m_config.is_sub_pool) {
    log(LogLevel::WARNING, "Sub-pools cannot be expanded");
    return false;
  }

  if (additional_size < m_config.min_expansion_size) {
    log(LogLevel::WARNING, "Expansion size too small, minimum: " + format_size(m_config.min_expansion_size));
    return false;
  }

  try {
    size_t small_pool_addition = static_cast<size_t>(additional_size * m_config.small_pool_ratio);
    size_t large_pool_addition = additional_size - small_pool_addition;

    bool success = true;
    if (small_pool_addition > 0) {
      success &= expand_subpool(0, small_pool_addition);
    }
    if (large_pool_addition > 0) {
      success &= expand_subpool(1, large_pool_addition);
    }

    if (success) {
      m_stats.expansion_count.fetch_add(1, std::memory_order_relaxed);
      log(LogLevel::INFO, "Expanded by " + format_size(additional_size));
    }

    return success;

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "Expansion failed: " + std::string(e.what()));
    return false;
  }
}

bool MemoryPool::defragment() {
  if (!m_config.auto_defragmentation) {
    log(LogLevel::WARNING, "Defragmentation not enabled");
    return false;
  }

  try {
    log(LogLevel::INFO, "Starting defragmentation...");

    bool success = true;
    for (size_t i = 0; i < m_subpools.size(); ++i) {
      success &= defragment_subpool(i);
    }

    if (success) {
      m_stats.defragmentation_count.fetch_add(1, std::memory_order_relaxed);
      log(LogLevel::INFO, "Defragmentation completed");
    }

    return success;

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "Defragmentation failed: " + std::string(e.what()));
    return false;
  }
}

void MemoryPool::reset() noexcept {
  try {
    log(LogLevel::INFO, "Resetting memory pool...");

    // Clear all sub-pools
    for (auto &subpool : m_subpools) {
      std::scoped_lock lock(subpool->mutex);
      subpool->current_offset = 0;
      subpool->free_blocks.clear();
    }

    // Clear allocation tracking
    {
      std::scoped_lock lock(m_alloc_mutex);
      m_allocations.clear();
    }

    // Reset tenant statistics
    {
      std::scoped_lock lock(m_tenant_mutex);
      for (auto &[_, tenant] : m_tenants) {
        tenant->current_usage.store(0, std::memory_order_relaxed);
        tenant->peak_usage.store(0, std::memory_order_relaxed);
      }
    }

    // Reset pool statistics
    m_stats.allocated_bytes.store(0, std::memory_order_relaxed);
    m_stats.used_bytes.store(0, std::memory_order_relaxed);

    log(LogLevel::INFO, "Reset completed");

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "Reset failed: " + std::string(e.what()));
  }
}

void MemoryPool::set_tenant_quota(const std::string &tenant_id, size_t quota) {
  std::scoped_lock lock(m_tenant_mutex);

  auto it = m_tenants.find(tenant_id);
  if (it != m_tenants.end()) {
    it->second->quota = quota;
  } else {
    m_tenants[tenant_id] = std::make_unique<TenantConfig>(tenant_id, quota);
  }

  log(LogLevel::INFO, "Tenant '" + tenant_id + "' quota set to " + format_size(quota));
}

size_t MemoryPool::get_tenant_usage(const std::string &tenant_id) const {
  std::scoped_lock lock(m_tenant_mutex);
  auto it = m_tenants.find(tenant_id);
  if (it != m_tenants.end()) {
    return it->second->current_usage.load(std::memory_order_relaxed);
  }
  return 0;
}

std::shared_ptr<MemoryPool> MemoryPool::create_sub_pool(size_t sub_pool_size, const std::string &tenant_name) {
  if (m_config.is_sub_pool) {
    throw std::runtime_error("Cannot create sub-pool from another sub-pool");
  }

  // Allocate memory for sub-pool from this pool
  void *sub_memory = allocate(sub_pool_size, SubPoolType::LARGE_BLOCK, tenant_name);
  if (!sub_memory) {
    throw std::bad_alloc();
  }

  // Create sub-pool configuration
  Config sub_config;
  sub_config.initial_size = sub_pool_size;
  sub_config.parent_pool = shared_from_this();
  sub_config.is_sub_pool = true;
  sub_config.allow_expansion = false;  // Sub-pools cannot expand
  sub_config.auto_defragmentation = m_config.auto_defragmentation;
  sub_config.log_level = m_config.log_level;
  sub_config.small_pool_ratio = m_config.small_pool_ratio;
  sub_config.alignment = m_config.alignment;

  // Create sub-pool instance
  auto sub_pool = std::make_shared<MemoryPool>(sub_config);
  sub_pool->initialize_as_sub_pool(sub_memory, sub_pool_size);

  // Register child pool for tracking
  {
    std::scoped_lock lock(m_child_pools_mutex);
    m_child_pools.push_back(sub_pool);
  }

  log(LogLevel::INFO, "Created sub-pool '" + tenant_name + "' with size " + format_size(sub_pool_size));

  return sub_pool;
}

std::shared_ptr<MemoryPool> MemoryPool::create_from_parent(std::shared_ptr<MemoryPool> parent_pool,
                                                           const std::string &tenant_name, size_t sub_pool_size) {
  if (!parent_pool) {
    throw std::invalid_argument("Parent pool cannot be null");
  }

  return parent_pool->create_sub_pool(sub_pool_size, tenant_name);
}

void MemoryPool::print_stats() const noexcept {
  auto s = m_stats.snapshot();

  std::cout << "\n╔═══════════════════════════════════════════════════════════╗\n";
  std::cout << "║              MemoryPool Statistics                        ║\n";
  std::cout << "╠═══════════════════════════════════════════════════════════╣\n";
  std::cout << "║ Total Capacity:     " << std::setw(30) << std::left << format_size(s.total_capacity) << "    ║\n";
  std::cout << "║ Allocated:          " << std::setw(30) << std::left << format_size(s.allocated_bytes) << "    ║\n";
  std::cout << "║ Used:               " << std::setw(20) << std::left << format_size(s.used_bytes) << " (" << std::fixed
            << std::setprecision(1) << s.usage_percentage << "%)   ║\n";
  std::cout << "║ Peak Usage:         " << std::setw(30) << std::left << format_size(s.peak_usage) << "    ║\n";
  std::cout << "║ Fragmentation:      " << std::setw(30) << std::left
            << (std::to_string((int)(s.fragmentation_ratio * 100)) + "%") << "    ║\n";
  std::cout << "║                                                           ║\n";
  std::cout << "║ Allocations:        " << std::setw(30) << std::left << s.allocation_count << "    ║\n";
  std::cout << "║ Deallocations:      " << std::setw(30) << std::left << s.deallocation_count << "    ║\n";
  std::cout << "║ Failed Allocations: " << std::setw(30) << std::left << s.failed_allocations << "    ║\n";
  std::cout << "║ Expansions:         " << std::setw(30) << std::left << s.expansion_count << "    ║\n";
  std::cout << "║ Defragmentations:   " << std::setw(30) << std::left << s.defragmentation_count << "    ║\n";
  std::cout << "╚═══════════════════════════════════════════════════════════╝\n";

  // Sub-pool details
  std::cout << "\n╔═══════════════════════════════════════════════════════════╗\n";
  std::cout << "║                    SubPool Details                        ║\n";
  std::cout << "╠═══════════════════════════════════════════════════════════╣\n";

  const char *pool_names[] = {"SMALL_BLOCK (64KB)", "LARGE_BLOCK (1MB)"};
  for (size_t i = 0; i < m_subpools.size(); ++i) {
    auto &subpool = m_subpools[i];
    std::scoped_lock lock(subpool->mutex);

    size_t used = subpool->current_offset;
    size_t free_blocks_size = 0;
    for (const auto &fb : subpool->free_blocks) {
      free_blocks_size += fb.size;
    }

    double usage_pct = subpool->total_size > 0 ? (double)used * 100.0 / subpool->total_size : 0.0;

    std::cout << "║ " << std::setw(20) << std::left << pool_names[i] << "                                   ║\n";
    std::cout << "║   Total:          " << std::setw(30) << std::left << format_size(subpool->total_size) << "    ║\n";
    std::cout << "║   Used:           " << std::setw(20) << std::left << format_size(used) << " (" << std::fixed
              << std::setprecision(1) << usage_pct << "%)   ║\n";
    std::cout << "║   Free Blocks:    " << std::setw(10) << std::left << subpool->free_blocks.size() << " ("
              << std::setw(15) << std::left << format_size(free_blocks_size) << ")   ║\n";

    if (i < m_subpools.size() - 1) {
      std::cout << "║                                                           ║\n";
    }
  }
  std::cout << "╚═══════════════════════════════════════════════════════════╝\n";

  // Tenant statistics
  {
    std::scoped_lock lock(m_tenant_mutex);
    if (!m_tenants.empty()) {
      std::cout << "\n╔═══════════════════════════════════════════════════════════╗\n";
      std::cout << "║                    Tenant Usage                           ║\n";
      std::cout << "╠═══════════════════════════════════════════════════════════╣\n";

      for (const auto &[id, tenant] : m_tenants) {
        size_t usage = tenant->current_usage.load(std::memory_order_relaxed);
        size_t peak = tenant->peak_usage.load(std::memory_order_relaxed);

        std::cout << "║ " << std::setw(15) << std::left << id;

        if (tenant->quota > 0) {
          double pct = (double)usage * 100.0 / tenant->quota;
          std::cout << std::setw(12) << std::left << format_size(usage) << " / " << std::setw(12) << std::left
                    << format_size(tenant->quota) << " (" << std::fixed << std::setprecision(1) << pct << "%)  ║\n";
        } else {
          std::cout << std::setw(20) << std::left << "(disabled)"
                    << "                          ║\n";
        }

        std::cout << "║   Peak:         " << std::setw(30) << std::left << format_size(peak) << "    ║\n";
        std::cout << "║   Allocations:  " << std::setw(30) << std::left
                  << tenant->allocation_count.load(std::memory_order_relaxed) << "    ║\n";
      }

      std::cout << "╚═══════════════════════════════════════════════════════════╝\n";
    }
  }

  std::cout << std::endl;
}

MemoryPool::PoolStats::Snapshot MemoryPool::stats() const noexcept { return m_stats.snapshot(); }

void MemoryPool::set_log_level(LogLevel level) { m_config.log_level = level; }

void MemoryPool::validate_config() {
  if (m_config.initial_size < 10 * 1024 * 1024) {
    throw std::invalid_argument("Initial size must be at least 10MB");
  }
  if (m_config.small_pool_ratio < 0.0 || m_config.small_pool_ratio > 0.5) {
    throw std::invalid_argument("Small pool ratio must be between 0.0 and 0.5");
  }
  if (m_config.expansion_trigger_threshold <= 0.0 || m_config.expansion_trigger_threshold > 1.0) {
    throw std::invalid_argument("Expansion trigger must be between 0.0 and 1.0");
  }
}

void MemoryPool::initialize_pools(size_t total_size) {
  size_t small_pool_size = static_cast<size_t>(total_size * m_config.small_pool_ratio);
  size_t large_pool_size = total_size - small_pool_size;

  m_subpools.push_back(std::make_unique<SubPool>(small_pool_size, m_config.alignment));
  m_subpools.push_back(std::make_unique<SubPool>(large_pool_size, m_config.alignment));

  m_stats.total_capacity.store(total_size, std::memory_order_relaxed);
}

void MemoryPool::initialize_as_sub_pool(void *parent_memory, size_t size) {
  // Clear any default-initialized pools
  m_subpools.clear();

  // Create sub-pools using parent's memory
  size_t small_pool_size = static_cast<size_t>(size * m_config.small_pool_ratio);
  size_t large_pool_size = size - small_pool_size;

  // Small pool at start of parent memory
  auto small_subpool = std::make_unique<SubPool>(parent_memory, small_pool_size);
  small_subpool->parent_ref = m_config.parent_pool;
  m_subpools.push_back(std::move(small_subpool));

  // Large pool follows small pool
  void *large_base = static_cast<char *>(parent_memory) + small_pool_size;
  auto large_subpool = std::make_unique<SubPool>(large_base, large_pool_size);
  large_subpool->parent_ref = m_config.parent_pool;
  m_subpools.push_back(std::move(large_subpool));

  m_stats.total_capacity.store(size, std::memory_order_relaxed);

  log(LogLevel::INFO, "Sub-pool initialized with " + format_size(size));
}

void *MemoryPool::allocate_from_pool(int pool_idx, size_t aligned_size, size_t actual_size,
                                     const std::string &tenant_id) {
  auto &subpool = m_subpools[pool_idx];
  std::scoped_lock lock(subpool->mutex);

  void *ptr = try_allocate_from_free_blocks(subpool.get(), aligned_size);
  if (ptr) {
    record_allocation(ptr, aligned_size, actual_size, pool_idx, tenant_id);
    return ptr;
  }

  if (subpool->current_offset + aligned_size <= subpool->total_size) {
    ptr = static_cast<char *>(subpool->memory_base) + subpool->current_offset;
    subpool->current_offset += aligned_size;
    record_allocation(ptr, aligned_size, actual_size, pool_idx, tenant_id);
    return ptr;
  }

  return nullptr;
}

void *MemoryPool::try_allocate_from_free_blocks(SubPool *subpool, size_t aligned_size) {
  // First-fit strategy: find first free block that fits
  for (auto it = subpool->free_blocks.begin(); it != subpool->free_blocks.end(); ++it) {
    if (it->size >= aligned_size) {
      void *ptr = static_cast<char *>(subpool->memory_base) + it->offset;

      if (it->size > aligned_size + m_config.alignment) {
        // Split block: keep remainder in free list
        it->offset += aligned_size;
        it->size -= aligned_size;
      } else {
        // Use entire block
        subpool->free_blocks.erase(it);
      }

      return ptr;
    }
  }
  return nullptr;
}

void MemoryPool::record_allocation(void *ptr, size_t aligned_size, size_t actual_size, int pool_index,
                                   const std::string &tenant_id) {
  size_t offset = static_cast<char *>(ptr) - static_cast<char *>(m_subpools[pool_index]->memory_base);

  // Record allocation metadata
  {
    std::scoped_lock lock(m_alloc_mutex);
    m_allocations[ptr] = AllocationInfo{offset, aligned_size, pool_index, tenant_id};
  }

  // Update tenant usage
  if (!tenant_id.empty()) {
    update_tenant_usage(tenant_id, actual_size);

    std::scoped_lock lock(m_tenant_mutex);
    auto it = m_tenants.find(tenant_id);
    if (it != m_tenants.end()) {
      it->second->allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Update pool statistics
  m_stats.allocated_bytes.fetch_add(aligned_size, std::memory_order_relaxed);
  m_stats.used_bytes.fetch_add(actual_size, std::memory_order_relaxed);
  m_stats.allocation_count.fetch_add(1, std::memory_order_relaxed);
  update_peak_usage();
}

bool MemoryPool::expand_subpool(int pool_index, size_t additional_size) {
  auto &subpool = m_subpools[pool_index];
  std::scoped_lock lock(subpool->mutex);

  size_t new_size = subpool->total_size + additional_size;
  void *new_memory = aligned_alloc_portable(m_config.alignment, new_size);
  if (!new_memory) {
    return false;
  }

  std::memcpy(new_memory, subpool->memory_base, subpool->current_offset);
  free_aligned_portable(subpool->memory_base);

  subpool->memory_base = new_memory;
  subpool->total_size = new_size;

  m_stats.total_capacity.fetch_add(additional_size, std::memory_order_relaxed);

  return true;
}

bool MemoryPool::defragment_subpool(int pool_index) {
  auto &subpool = m_subpools[pool_index];
  std::scoped_lock lock(subpool->mutex);

  if (subpool->free_blocks.empty()) {
    return true;  // Nothing to defragment
  }

  merge_adjacent_free_blocks(subpool.get());
  return true;
}

void MemoryPool::merge_adjacent_free_blocks(SubPool *subpool) {
  if (subpool->free_blocks.size() < 2) {
    return;  // Need at least 2 blocks to merge
  }

  std::sort(subpool->free_blocks.begin(), subpool->free_blocks.end(),
            [](const FreeBlock &a, const FreeBlock &b) { return a.offset < b.offset; });

  std::vector<FreeBlock> merged;
  merged.reserve(subpool->free_blocks.size());

  FreeBlock current = subpool->free_blocks[0];
  for (size_t i = 1; i < subpool->free_blocks.size(); ++i) {
    const auto &next = subpool->free_blocks[i];

    if (current.offset + current.size == next.offset) {
      current.size += next.size;
    } else {
      merged.push_back(current);
      current = next;
    }
  }
  merged.push_back(current);

  subpool->free_blocks = std::move(merged);
}

bool MemoryPool::check_tenant_quota(const std::string &tenant_id, size_t size) {
  std::scoped_lock lock(m_tenant_mutex);
  auto it = m_tenants.find(tenant_id);
  if (it == m_tenants.end()) {
    return true;  // No quota configured, allow allocation
  }

  auto &tenant = it->second;
  if (tenant->quota == 0) {
    return false;  // Tenant disabled
  }

  size_t current = tenant->current_usage.load(std::memory_order_relaxed);
  return (current + size <= tenant->quota);
}

void MemoryPool::update_tenant_usage(const std::string &tenant_id, ssize_t delta) {
  std::scoped_lock lock(m_tenant_mutex);
  auto it = m_tenants.find(tenant_id);
  if (it != m_tenants.end()) {
    if (delta > 0) {
      size_t new_usage = it->second->current_usage.fetch_add(delta, std::memory_order_relaxed) + delta;

      size_t peak = it->second->peak_usage.load(std::memory_order_relaxed);
      while (new_usage > peak &&
             !it->second->peak_usage.compare_exchange_weak(peak, new_usage, std::memory_order_relaxed)) {
      }
    } else {
      it->second->current_usage.fetch_sub(-delta, std::memory_order_relaxed);
    }
  }
}

void MemoryPool::update_peak_usage() noexcept {
  size_t cur = m_stats.used_bytes.load(std::memory_order_relaxed);
  size_t peak = m_stats.peak_usage.load(std::memory_order_relaxed);
  while (cur > peak && !m_stats.peak_usage.compare_exchange_weak(peak, cur, std::memory_order_relaxed)) {
  }
}

void MemoryPool::monitor_loop() {
  while (!m_shutdown.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::seconds(5));

    try {
      auto s = m_stats.snapshot();

      if (m_config.allow_expansion && s.usage_percentage >= m_config.expansion_trigger_threshold * 100.0) {
        size_t expand_size = m_config.initial_size / 2;
        if (expand_size < m_config.min_expansion_size) {
          expand_size = m_config.min_expansion_size;
        }

        log(LogLevel::INFO, "Auto-expansion triggered at " + std::to_string((int)s.usage_percentage) + "% usage");
        expand(expand_size);
      }

      if (m_config.auto_defragmentation && s.fragmentation_ratio >= m_config.defrag_trigger_threshold) {
        log(LogLevel::INFO, "Auto-defragmentation triggered at " + std::to_string((int)(s.fragmentation_ratio * 100)) +
                                "% fragmentation");
        defragment();
      }

    } catch (const std::exception &e) {
      log(LogLevel::ERROR, "Monitor loop error: " + std::string(e.what()));
    }
  }
}

void MemoryPool::cleanup() noexcept {
  m_subpools.clear();
  m_allocations.clear();
  m_tenants.clear();
  m_child_pools.clear();
}

void MemoryPool::log(LogLevel level, const std::string &message) const {
  DBUG_PRINT("memory_pool", ("[%d] %s", static_cast<int>(level), message.c_str()));
}

size_t MemoryPool::align_up(size_t size, size_t alignment) noexcept {
  return (size + alignment - 1) & ~(alignment - 1);
}

void *MemoryPool::aligned_alloc_portable(size_t alignment, size_t size) noexcept {
#if defined(_MSC_VER)
  return _aligned_malloc(size, alignment);
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__APPLE__)
  // C11 aligned_alloc requires size to be multiple of alignment
  size_t adjusted = ((size + alignment - 1) / alignment) * alignment;
  return std::aligned_alloc(alignment, adjusted);
#else
  // POSIX posix_memalign
  void *ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
  return ptr;
#endif
}

void MemoryPool::free_aligned_portable(void *ptr) noexcept {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

std::string MemoryPool::format_size(size_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit = 0;
  double size = static_cast<double>(bytes);
  while (size >= 1024.0 && unit < 4) {
    size /= 1024.0;
    unit++;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
  return oss.str();
}
}  // namespace Utils
}  // namespace ShannonBase