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

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/varlen0data.h"

#include <algorithm>
#include <new>

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
VarlenDataPool::VarlenDataPool(size_t initial_size, std::shared_ptr<Utils::MemoryPool> mem_pool)
    : m_next_block_id(1), m_allocation_count(0), m_retired_count(0), m_memory_pool(mem_pool) {
  // Initialize free lists
  for (size_t i = 0; i < NUM_FREELISTS; i++) {
    m_freelists[i] = FreeList();
  }

  // Allocate initial block
  if (initial_size > 0) {
    allocate_new_block(initial_size);
  }
}

VarlenDataPool::~VarlenDataPool() {
  // Blocks are released by the BlockPtr deleters; no external overflow pages
  // exist in the current DRAM-only design.
}

bool VarlenDataPool::allocate(const uchar *data, size_t length, VarlenReference &ref) {
  if (!data || length == 0) return false;
  if (length > MAX_VARLEN_VALUE_SIZE) return false;  // would truncate in uint32 ref/block fields

  // 1. Determine storage type
  if (length < INLINE_THRESHOLD) {
    // Inline storage (caller responsible for storing in CU data area)
    ref.storage_type = VarlenReference::INLINE;
    ref.length = length;
    return true;
  }

  // Always use in-memory pool storage — overflow to disk requires a
  // pre-existing ./tmp/ directory and adds unnecessary I/O latency.
  return allocate_in_pool(data, length, ref);
}

void VarlenDataPool::retire(const VarlenReference &ref) {
  if (ref.is_inline()) return;  // inline needs no bookkeeping
  if (ref.is_pool()) retire_in_pool(ref);
}

size_t VarlenDataPool::read(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const {
  if (ref.is_inline()) {
    // Inline data handled by caller
    return 0;
  }

  if (ref.is_pool()) {
    return read_from_pool(ref, buffer, buffer_size);
  }

  return 0;
}

VarlenDataPool::VarlenReadGuard VarlenDataPool::get_data_ptr(const VarlenReference &ref) const {
  if (!ref.is_pool()) return {};

  std::shared_lock lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return {};

  DataBlock *block = it->second;
  if (!block || !block->header.is_valid()) return {};

  // Use uint64_t to prevent overflow when ref.offset + ref.length approaches UINT32_MAX.
  if (static_cast<uint64_t>(ref.offset) + ref.length > block->header.used_size) {
    return {};
  }

  return VarlenReadGuard(std::move(lock), block->data + ref.offset);
}

bool VarlenDataPool::copy_data(const VarlenReference &ref, void *out, size_t max_len, size_t &out_len) const {
  out_len = 0;
  if (!ref.is_pool()) return false;

  std::shared_lock lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return false;

  DataBlock *block = it->second;
  if (!block || !block->header.is_valid()) return false;
  if (static_cast<uint64_t>(ref.offset) + ref.length > block->header.used_size) return false;

  const size_t copy_len = std::min(static_cast<size_t>(ref.length), max_len);
  if (copy_len > 0 && out == nullptr) return false;
  if (copy_len > 0) std::memcpy(out, block->data + ref.offset, copy_len);
  out_len = copy_len;
  return true;
}

size_t VarlenDataPool::reclaim() {
  std::lock_guard lock(m_mutex);

  size_t reclaimed_capacity = 0;

  // Free whole blocks whose allocations have all been retired.  Append-only
  // arena semantics: a block is reusable only when no live reference points
  // into it.
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    DataBlock *block = it->get();

    if (block->header.live_allocations == 0) {
      const size_t released = sizeof(BlockHeader) + block->header.size;
      reclaimed_capacity += released;

      // Remove from index
      m_block_index.erase(block->header.block_id);

      // Drop the retired-allocation markers for this block so the idempotency
      // set does not grow without bound.
      for (auto rit = m_retired_refs.begin(); rit != m_retired_refs.end();) {
        if ((*rit >> 32) == static_cast<uint64_t>(block->header.block_id)) {
          rit = m_retired_refs.erase(rit);
        } else {
          ++rit;
        }
      }

      // ...and the reuse extents, which point into memory about to be freed.
      drop_retired_extents_for_block(block->header.block_id);

      // Remove from free list
      remove_from_freelist(&block->header);

      // No used_size adjustment: it counts *live* bytes, and every allocation
      // in this block has already been retired (live_allocations == 0), so
      // retire_in_pool() subtracted each one as it went.

      // Release block
      it = m_blocks.erase(it);
      m_header.block_count--;
      m_header.total_size -= released;
    } else {
      ++it;
    }
  }

  return reclaimed_capacity;
}

size_t VarlenDataPool::get_total_size() const {
  std::lock_guard lock(m_mutex);
  return m_header.total_size;
}

size_t VarlenDataPool::get_used_size() const {
  std::lock_guard lock(m_mutex);
  return m_header.used_size;
}

double VarlenDataPool::get_fragmentation_ratio() const {
  std::lock_guard lock(m_mutex);
  if (m_header.total_size == 0) return 0.0;

  size_t wasted = m_header.total_size - m_header.used_size;
  return static_cast<double>(wasted) / m_header.total_size;
}

size_t VarlenDataPool::get_block_count() const {
  std::lock_guard lock(m_mutex);
  return m_header.block_count;
}

VarlenDataPool::AllocationStats VarlenDataPool::get_stats() const {
  AllocationStats stats;
  stats.allocation_count = m_allocation_count.load();
  stats.retired_count = m_retired_count.load();
  stats.rejected_retire_count = m_rejected_retire_count.load();
  stats.lost_fragment_bytes = m_lost_fragment_bytes.load();

  std::lock_guard lock(m_mutex);
  stats.total_size = m_header.total_size;
  stats.used_size = m_header.used_size;
  stats.fragmentation_ratio = (m_header.total_size == 0)
                                  ? 0.0
                                  : static_cast<double>(m_header.total_size - m_header.used_size) / m_header.total_size;
  return stats;
}

bool VarlenDataPool::validate() const {
  std::lock_guard lock(m_mutex);

  for (const auto &block_ptr : m_blocks) {
    DataBlock *block = block_ptr.get();

    // Verify magic number
    if (!block->header.is_valid()) {
      return false;
    }

    // Verify block ID
    auto it = m_block_index.find(block->header.block_id);
    if (it == m_block_index.end() || it->second != block) {
      return false;
    }
  }

  return true;
}

void VarlenDataPool::dump_summary(std::ostream &out) const {
  std::lock_guard lock(m_mutex);

  out << "Varlen Data Pool Summary:\n";
  out << "  Total Size: " << m_header.total_size << " bytes\n";
  out << "  Used Size: " << m_header.used_size << " bytes\n";
  // Compute inline — get_fragmentation_ratio() would re-lock m_mutex and
  // deadlock (std::mutex is not recursive).
  const double fragmentation =
      (m_header.total_size == 0) ? 0.0
                                 : static_cast<double>(m_header.total_size - m_header.used_size) / m_header.total_size;
  out << "  Fragmentation: " << (fragmentation * 100) << "%\n";
  out << "  Blocks: " << m_header.block_count << "\n";
  out << "  Free Blocks: " << m_header.free_blocks << "\n";
  out << "  Allocations: " << m_allocation_count.load() << "\n";
  out << "  Retired: " << m_retired_count.load() << "\n";
  out << "  Rejected Retires: " << m_rejected_retire_count.load() << "\n";
  out << "  Lost Fragment Bytes: " << m_lost_fragment_bytes.load() << "\n";
}

bool VarlenDataPool::allocate_in_pool(const uchar *data, size_t length, VarlenReference &ref) {
  if (!data || length == 0) return false;
  if (length > MAX_VARLEN_VALUE_SIZE) return false;

  std::lock_guard lock(m_mutex);

  // Align length
  size_t aligned_length = align_size(length);

  // 0. Reuse a retired extent before taking fresh space.  Without this the
  //    pool only ever grew: retire() freed nothing a later allocate() could
  //    see, so a repeatedly-updated BLOB/JSON/VECTOR column consumed memory in
  //    proportion to every value ever written.
  {
    uint32_t reuse_block_id = 0, reuse_offset = 0;
    if (take_retired_extent(aligned_length, reuse_block_id, reuse_offset)) {
      auto rit = m_block_index.find(reuse_block_id);
      if (rit != m_block_index.end() && rit->second != nullptr && rit->second->header.is_valid()) {
        DataBlock *reuse_block = rit->second;
        std::memcpy(reuse_block->data + reuse_offset, data, length);

        ref.block_id = reuse_block_id;
        ref.offset = reuse_offset;
        ref.length = length;
        ref.storage_type = VarlenReference::POOL;
        reuse_block->header.live_allocations++;

        m_header.used_size += aligned_length;
        m_allocation_count.fetch_add(1);
        return true;
      }
      // The block vanished under us (reclaimed); fall through and allocate
      // fresh rather than trusting a stale extent.
    }
  }

  // 1. Find suitable free block
  DataBlock *block = find_free_block(aligned_length);

  if (!block) {
    // 2. No suitable free block found, allocate new block.  Keep the block
    //    capacity representable in BlockHeader::size (uint32_t): doubling a
    //    near-4GiB payload would otherwise overflow the 32-bit field.
    size_t block_size = std::max(aligned_length * 2, DEFAULT_BLOCK_SIZE);
    if (block_size > MAX_VARLEN_VALUE_SIZE) block_size = MAX_VARLEN_VALUE_SIZE;
    block = allocate_new_block(block_size);

    if (!block) return false;
  }

  // 3. Allocate space in block
  uint32_t offset = block->header.used_size;

  if (offset + aligned_length > block->header.size) {
    return false;  // Should not happen
  }

  // 4. Copy data
  std::memcpy(block->data + offset, data, length);

  // 5. Snapshot the freelist bucket this block is currently chained in
  //    BEFORE changing used_size (which changes available_space() and
  //    thus the bucket index).
  size_t old_freelist_idx = get_freelist_index(block->header.available_space());

  // 6. Update block header
  block->header.used_size += aligned_length;

  // 7. Migrate the block to its new freelist bucket so the bucket index keeps
  //    reflecting remaining capacity (a block that is no longer full must not
  //    stay in the old, larger bucket).
  if (block->header.available_space() < MIN_BLOCK_SIZE) {
    remove_from_freelist_at(&block->header, old_freelist_idx);
  } else {
    size_t new_freelist_idx = get_freelist_index(block->header.available_space());
    if (new_freelist_idx != old_freelist_idx) {
      remove_from_freelist_at(&block->header, old_freelist_idx);
      add_to_freelist(&block->header);
    }
  }

  // 7. Set reference
  ref.block_id = block->header.block_id;
  ref.offset = offset;
  ref.length = length;
  ref.storage_type = VarlenReference::POOL;
  block->header.live_allocations++;

  // 8. Update statistics
  m_header.used_size += aligned_length;
  m_allocation_count.fetch_add(1);

  return true;
}

void VarlenDataPool::retire_in_pool(const VarlenReference &ref) {
  std::lock_guard lock(m_mutex);

  // Every rejection below drops a retire on the floor: the extent stays
  // counted in used_size and can never be reused. That is the safe outcome --
  // trusting an unrecognised reference would hand live bytes to a second
  // allocation -- but it must be countable, so get_stats() can show that a
  // pool is leaking because its callers are retiring references it never
  // issued, rather than because the live set really grew.
  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) {
    m_rejected_retire_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  DataBlock *block = it->second;
  if (!block || !block->header.is_valid()) {
    m_rejected_retire_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Validate the reference still points at a live allocation.
  if (static_cast<uint64_t>(ref.offset) + ref.length > block->header.used_size) {
    m_rejected_retire_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const uint64_t key = (static_cast<uint64_t>(ref.block_id) << 32) | ref.offset;
  if (!m_retired_refs.insert(key).second) {
    m_rejected_retire_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (block->header.live_allocations > 0) {
    --block->header.live_allocations;
  }
  m_retired_count.fetch_add(1);

  // Hand the bytes back so a later allocate() can use them.  allocate_in_pool()
  // rounded the payload up to ALIGNMENT, so that is the extent's real size.
  const size_t aligned_length = align_size(ref.length);
  if (m_header.used_size >= aligned_length) {
    m_header.used_size -= aligned_length;
  } else {
    m_header.used_size = 0;
  }
  add_retired_extent(ref.block_id, ref.offset, static_cast<uint32_t>(aligned_length));
}

size_t VarlenDataPool::read_from_pool(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const {
  std::shared_lock lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return 0;

  DataBlock *block = it->second;
  if (!block || !block->header.is_valid()) return 0;

  // Use uint64_t to prevent overflow when ref.offset + ref.length approaches UINT32_MAX.
  if (static_cast<uint64_t>(ref.offset) + ref.length > block->header.used_size) return 0;

  const size_t copy_len = std::min(static_cast<size_t>(ref.length), buffer_size);
  if (copy_len > 0 && buffer == nullptr) return 0;
  if (copy_len > 0) std::memcpy(buffer, block->data + ref.offset, copy_len);

  return copy_len;
}

VarlenDataPool::DataBlock *VarlenDataPool::allocate_new_block(size_t size) {
  // Align size
  size_t aligned_size = align_size(size);
  if (aligned_size > MAX_VARLEN_VALUE_SIZE) return nullptr;  // must fit BlockHeader::size (uint32_t)
  size_t total_size = sizeof(BlockHeader) + aligned_size;

  void *mem = nullptr;
  try {
    if (m_memory_pool) {
      mem = m_memory_pool->allocate(total_size);
    } else {
      mem = ::operator new(total_size, std::nothrow);
    }
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
  if (!mem) return nullptr;

  // Initialize block
  DataBlock *block = reinterpret_cast<DataBlock *>(mem);
  block->header.block_id = m_next_block_id.fetch_add(1);
  block->header.size = aligned_size;
  block->header.used_size = 0;
  block->header.magic = BlockHeader::MAGIC_NUMBER;
  block->header.live_allocations = 0;
  block->header.next_free = nullptr;

  // BlockPtr objects outlive members declared after m_blocks during object
  // destruction. Capturing `this` would read m_memory_pool after destruction.
  auto pool = m_memory_pool;
  BlockDeleter deleter = [pool = std::move(pool)](DataBlock *b) {
    if (pool) {
      pool->deallocate(b, sizeof(BlockHeader) + b->header.size);
    } else {
      ::operator delete(b);
    }
  };

  BlockPtr owned(block, std::move(deleter));
  try {
    m_blocks.emplace_back(std::move(owned));
    const auto [_, inserted] = m_block_index.emplace(block->header.block_id, block);
    if (!inserted) {
      m_blocks.pop_back();
      return nullptr;
    }
  } catch (const std::bad_alloc &) {
    if (!m_blocks.empty() && m_blocks.back().get() == block) m_blocks.pop_back();
    return nullptr;
  }

  // Add to free list
  add_to_freelist(&block->header);

  // Update statistics
  m_header.total_size += total_size;
  m_header.block_count++;

  return block;
}

VarlenDataPool::DataBlock *VarlenDataPool::find_free_block(size_t required_size) {
  // Find free list of appropriate size
  size_t list_idx = get_freelist_index(required_size);

  for (size_t i = list_idx; i < NUM_FREELISTS; i++) {
    // Traverse the linked list within this bucket to find the first
    // block with enough space, rather than only checking the head.
    for (BlockHeader *header = m_freelists[i].head; header; header = header->next_free) {
      if (header->available_space() >= required_size) {
        return reinterpret_cast<DataBlock *>(reinterpret_cast<uchar *>(header) - offsetof(DataBlock, header));
      }
    }
  }

  return nullptr;
}

void VarlenDataPool::add_to_freelist(BlockHeader *header) {
  size_t idx = get_freelist_index(header->available_space());

  header->next_free = m_freelists[idx].head;
  m_freelists[idx].head = header;
  m_freelists[idx].count++;
  m_header.free_blocks++;
}

void VarlenDataPool::remove_from_freelist(BlockHeader *header) {
  size_t idx = get_freelist_index(header->available_space());
  remove_from_freelist_at(header, idx);
}

void VarlenDataPool::remove_from_freelist_at(BlockHeader *header, size_t idx) {
  BlockHeader **current = &m_freelists[idx].head;

  while (*current) {
    if (*current == header) {
      *current = header->next_free;
      header->next_free = nullptr;
      m_freelists[idx].count--;
      m_header.free_blocks--;
      return;
    }
    current = &(*current)->next_free;
  }
}

bool VarlenDataPool::take_retired_extent(size_t aligned_size, uint32_t &block_id_out, uint32_t &offset_out) {
  if (aligned_size == 0 || aligned_size > std::numeric_limits<uint32_t>::max()) return false;

  // Search this size class and every larger one, exactly as find_free_block()
  // does for whole blocks. Within a class the extents are unordered, so take
  // the first that fits rather than the best -- the classes are narrow enough
  // that the difference is not worth an ordered structure on the write path.
  for (size_t idx = get_freelist_index(aligned_size); idx < NUM_FREELISTS; ++idx) {
    auto &bucket = m_retired_extents[idx];
    for (size_t i = 0; i < bucket.size(); ++i) {
      if (bucket[i].size < aligned_size) continue;

      const RetiredExtent taken = bucket[i];
      bucket[i] = bucket.back();
      bucket.pop_back();

      // The extent is live again, so a future retire of the new allocation is
      // legitimate and must not be rejected as a double-retire.
      m_retired_refs.erase((static_cast<uint64_t>(taken.block_id) << 32) | taken.offset);

      const size_t leftover = taken.size - aligned_size;
      if (leftover >= MIN_REUSE_SPLIT) {
        add_retired_extent(taken.block_id, static_cast<uint32_t>(taken.offset + aligned_size),
                           static_cast<uint32_t>(leftover));
      }

      block_id_out = taken.block_id;
      offset_out = taken.offset;
      return true;
    }
  }
  return false;
}

void VarlenDataPool::add_retired_extent(uint32_t block_id, uint32_t offset, uint32_t size) {
  if (size < MIN_REUSE_SPLIT) {
    // Too small to ever satisfy a request, so it is dropped -- but it stays in
    // used_size for the life of the block. Count the loss; enough of these and
    // the pool grows for a reason no other counter explains.
    m_lost_fragment_bytes.fetch_add(size, std::memory_order_relaxed);
    return;
  }
  m_retired_extents[get_freelist_index(size)].push_back({block_id, offset, size});
}

void VarlenDataPool::drop_retired_extents_for_block(uint32_t block_id) {
  for (size_t idx = 0; idx < NUM_FREELISTS; ++idx) {
    auto &bucket = m_retired_extents[idx];
    bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                [block_id](const RetiredExtent &e) { return e.block_id == block_id; }),
                 bucket.end());
  }
}

size_t VarlenDataPool::get_freelist_index(size_t size) const {
  // Group by powers of 2
  // [0, 128), [128, 256), [256, 512), [512, 1024), ...

  if (size < 128) return 0;
  if (size < 256) return 1;
  if (size < 512) return 2;
  if (size < 1024) return 3;
  if (size < 2048) return 4;
  if (size < 4096) return 5;
  if (size < 8192) return 6;
  return 7;
}

size_t VarlenDataPool::align_size(size_t size) { return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1); }
}  // namespace Imcs
}  // namespace ShannonBase