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
    : memory_base(nullptr), total_size(size), current_offset(0), is_from_parent(false), parent_ref() {
  // Allocate new memory for root pool
  memory_base = aligned_alloc_portable(alignment, size);
  if (!memory_base) {
    throw std::bad_alloc();
  }
}

MemoryPool::SubPool::SubPool(void *base, size_t size)
    : memory_base(base), total_size(size), current_offset(0), is_from_parent(true), parent_ref() {
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

MemoryPool::~MemoryPool() {
  m_shutdown.store(true, std::memory_order_release);
  if (m_monitor_thread.joinable()) {
    m_monitor_thread.join();
  }

  // 2. reclaim sub pool.
  if (m_config.is_sub_pool && m_subpool_base) {
    if (auto parent = m_config.parent_pool.lock()) {
      const size_t subpool_size = m_stats.total_capacity.load();
      size_t remaining_allocs = 0;
      {
        std::scoped_lock lock(parent->m_alloc_mutex);
        const char *subpool_start = static_cast<const char *>(m_subpool_base);
        const char *subpool_end = subpool_start + subpool_size;

        for (const auto &[ptr, info] : parent->m_allocations) {
          if (ptr >= subpool_start && ptr < subpool_end) {
            remaining_allocs++;
          }
        }
      }

      if (remaining_allocs > 0) {
        log(LogLevel::WARNING, "Sub-pool still has " + std::to_string(remaining_allocs) +
                                   " unreleased allocations. Memory leak possible!");
      }

      if (auto parent = m_config.parent_pool.lock()) {
        try {
          parent->deallocate(m_subpool_base, subpool_size);
        } catch (...) {
        }
      }
      log(LogLevel::INFO, "Returned sub-pool base block: " + format_size(subpool_size));

      // 3. Remove self from parent's child list
      {
        std::scoped_lock lock(parent->m_child_pools_mutex);
        auto &children = parent->m_child_pools;
        children.erase(std::remove_if(children.begin(), children.end(),
                                      [this](const std::weak_ptr<MemoryPool> &wp) {
                                        auto sp = wp.lock();
                                        return sp && sp.get() == this;
                                      }),
                       children.end());
      }
    } else {
      log(LogLevel::WARNING, "Parent pool expired, cannot return sub-pool memory");
    }
  }

  cleanup();
  log(LogLevel::INFO, "MemoryPool destroyed");
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
      log(LogLevel::WARNING, "deallocate: pointer " + std::to_string((uintptr_t)ptr) +
                                 " not found in m_allocations, size=" + format_size(size));
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

    log(LogLevel::DEBUG,
        "deallocate: freed " + format_size(info.aligned_size) + " at offset " + std::to_string(info.offset));

  } catch (const std::exception &e) {
    log(LogLevel::ERROR, "deallocate error: " + std::string(e.what()));
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
  // clean expired sub pool children.
  cleanup_expired_children();

  {
    std::scoped_lock lock(m_child_pools_mutex);
    if (!m_child_pools.empty()) {
      log(LogLevel::ERROR, "Cannot reset pool with active sub-pools");
      return;
    }
  }

  try {
    log(LogLevel::INFO, "Resetting memory pool...");

    // reset all subpool.
    for (auto &subpool : m_subpools) {
      std::scoped_lock lock(subpool->mutex);
      subpool->current_offset = 0;
      subpool->free_blocks.clear();
    }

    // cleanup allocated track
    {
      std::scoped_lock lock(m_alloc_mutex);
      m_allocations.clear();
    }

    // reset the statistics
    {
      std::scoped_lock lock(m_tenant_mutex);
      for (auto &[_, tenant] : m_tenants) {
        tenant->current_usage.store(0, std::memory_order_relaxed);
        tenant->peak_usage.store(0, std::memory_order_relaxed);
        tenant->allocation_count.store(0, std::memory_order_relaxed);
      }
    }

    // reset pool statistics.
    m_stats.allocated_bytes.store(0, std::memory_order_relaxed);
    m_stats.used_bytes.store(0, std::memory_order_relaxed);
    m_stats.peak_usage.store(0, std::memory_order_relaxed);
    m_stats.allocation_count.store(0, std::memory_order_relaxed);
    m_stats.deallocation_count.store(0, std::memory_order_relaxed);

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

  Config sub_config = m_config;
  sub_config.tenant_name = tenant_name;
  sub_config.initial_size = sub_pool_size;
  sub_config.is_sub_pool = true;
  sub_config.allow_expansion = false;
  // ===== set parent_pool =====
  sub_config.parent_pool = shared_from_this();

  auto sub_pool = std::make_shared<MemoryPool>(sub_config);

  // allocate memory
  void *sub_memory = allocate(sub_pool_size, SubPoolType::LARGE_BLOCK, tenant_name);
  if (!sub_memory) {
    throw std::bad_alloc();
  }

  sub_pool->m_subpool_base = sub_memory;
  sub_pool->initialize_as_sub_pool(sub_memory, sub_pool_size);

  {
    std::scoped_lock lock(m_child_pools_mutex);
    m_child_pools.push_back(sub_pool);
  }

  log(LogLevel::INFO, "Created sub-pool '" + tenant_name + "' size: " + format_size(sub_pool_size));
  return sub_pool;
}

std::shared_ptr<MemoryPool> MemoryPool::create_from_parent(const std::shared_ptr<MemoryPool> &parent_pool,
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
  m_subpool_base = parent_memory;
  m_subpools.clear();

  size_t required_alignment = std::max(m_config.alignment, sizeof(void *));

  size_t small_pool_size = static_cast<size_t>(size * m_config.small_pool_ratio);
  small_pool_size = align_up(small_pool_size, required_alignment);

  if (small_pool_size > size) {
    small_pool_size = align_down(size / 2, required_alignment);
  }

  size_t large_pool_size = size - small_pool_size;

  auto small_subpool = std::make_unique<SubPool>(parent_memory, small_pool_size);
  small_subpool->parent_ref = m_config.parent_pool;
  m_subpools.push_back(std::move(small_subpool));

  void *large_base = static_cast<char *>(parent_memory) + small_pool_size;
  auto large_subpool = std::make_unique<SubPool>(large_base, large_pool_size);
  large_subpool->parent_ref = m_config.parent_pool;
  m_subpools.push_back(std::move(large_subpool));

  m_stats.total_capacity.store(size, std::memory_order_relaxed);

  log(LogLevel::INFO, "Sub-pool initialized with " + format_size(size));
}

void *MemoryPool::allocate_from_pool(int pool_idx, size_t aligned_size, size_t actual_size,
                                     const std::string &tenant_id) {
  if (unlikely(aligned_size == 0)) aligned_size = m_config.alignment;

  auto &subpool = m_subpools[pool_idx];
  std::scoped_lock lock(subpool->mutex);

  void *ptr = try_allocate_from_free_blocks(subpool.get(), aligned_size);
  if (ptr) {
#ifndef NDEBUG
    const size_t req_align = std::max(m_config.alignment, sizeof(void *));
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr % req_align != 0) {
      log(LogLevel::ERROR, "BUG: Unaligned pointer from free_blocks: " + std::to_string(addr));
      assert(false && "Alignment violation");
      return nullptr;
    }
#endif
    record_allocation(ptr, aligned_size, actual_size, pool_idx, tenant_id);
    return ptr;
  }

  // allocate from consective memory space.
  const size_t required_alignment = std::max(m_config.alignment, sizeof(void *));
  const uintptr_t tail_addr = reinterpret_cast<uintptr_t>(subpool->memory_base) + subpool->current_offset;
  const uintptr_t aligned_tail = align_up(tail_addr, required_alignment);
  const size_t padding = aligned_tail - tail_addr;
  const size_t total_needed = padding + aligned_size;

  //  space check.
  if (subpool->current_offset + total_needed > subpool->total_size) {
    return nullptr;
  }

#ifndef NDEBUG
  assert(padding < required_alignment && "Padding overflow");
  assert(aligned_tail >= tail_addr && "Alignment regression");
  assert(total_needed >= aligned_size && "Size underflow");
#endif

  // update offset
  subpool->current_offset += total_needed;
  ptr = reinterpret_cast<void *>(aligned_tail);

#ifndef NDEBUG
  assert(reinterpret_cast<uintptr_t>(ptr) % required_alignment == 0 && "Final alignment failed");
  assert(ptr >= subpool->memory_base && ptr < static_cast<char *>(subpool->memory_base) + subpool->total_size);
  assert(static_cast<char *>(ptr) + aligned_size <=
         static_cast<char *>(subpool->memory_base) + subpool->current_offset);
#endif

  record_allocation(ptr, aligned_size, actual_size, pool_idx, tenant_id);
  return ptr;
}

void *MemoryPool::try_allocate_from_free_blocks(SubPool *subpool, size_t aligned_size) {
  size_t required_alignment = std::max(m_config.alignment, sizeof(void *));

  for (auto it = subpool->free_blocks.begin(); it != subpool->free_blocks.end(); ++it) {
    void *ptr = static_cast<char *>(subpool->memory_base) + it->offset;
    uintptr_t ptr_addr = reinterpret_cast<uintptr_t>(ptr);

    uintptr_t aligned_addr = align_up(ptr_addr, required_alignment);
    size_t alignment_padding = aligned_addr - ptr_addr;

    if (it->size >= aligned_size + alignment_padding) {
      it->offset += alignment_padding;
      it->size -= alignment_padding;

      ptr = reinterpret_cast<void *>(aligned_addr);

      if (it->size > aligned_size + required_alignment) {
        size_t new_offset = it->offset + aligned_size;
        new_offset = align_up(new_offset, required_alignment);
        size_t actual_used = new_offset - it->offset;

        if (it->size > actual_used) {
          it->offset = new_offset;
          it->size -= actual_used;
        } else {
          subpool->free_blocks.erase(it);
        }
      } else {
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

  std::sort(
      subpool->free_blocks.begin(), subpool->free_blocks.end(),
      [](const FreeBlock &a, const FreeBlock &b) constexpr { return a.offset < b.offset; });

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
    try {
      std::this_thread::sleep_for(std::chrono::seconds(5));

      cleanup_expired_children();

      auto s = m_stats.snapshot();
      if (m_config.allow_expansion && s.usage_percentage >= 85.0) {
        expand(std::max(m_config.initial_size / 4, m_config.min_expansion_size));
      }
      if (m_config.auto_defragmentation && s.fragmentation_ratio >= 0.3) {
        defragment();
      }
    } catch (...) {
    }
  }
}

void MemoryPool::cleanup() noexcept {
  m_subpools.clear();
  m_allocations.clear();
  m_tenants.clear();
  m_child_pools.clear();
}

void MemoryPool::cleanup_expired_children() noexcept {
  std::scoped_lock lock(m_child_pools_mutex);
  size_t old_size = m_child_pools.size();
  m_child_pools.erase(std::remove_if(m_child_pools.begin(), m_child_pools.end(),
                                     [](const std::weak_ptr<MemoryPool> &wp) { return wp.expired(); }),
                      m_child_pools.end());

  if (m_child_pools.size() < old_size) {
    log(LogLevel::DEBUG, "Cleaned " + std::to_string(old_size - m_child_pools.size()) + " expired sub-pools");
  }
}

void MemoryPool::log(LogLevel level, const std::string &message) const {
  DBUG_PRINT("memory_pool", ("[%d] %s", static_cast<int>(level), message.c_str()));
}

void *MemoryPool::aligned_alloc_portable(size_t alignment, size_t size) noexcept {
  size_t min_alignment = std::max(alignment, sizeof(void *));
  size_t adjusted_size = ((size + min_alignment - 1) / min_alignment) * min_alignment;

#if defined(_MSC_VER)
  return _aligned_malloc(adjusted_size, min_alignment);
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__APPLE__)
  return std::aligned_alloc(min_alignment, adjusted_size);
#else
  void *ptr = nullptr;
  if (posix_memalign(&ptr, min_alignment, adjusted_size) != 0) return nullptr;
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