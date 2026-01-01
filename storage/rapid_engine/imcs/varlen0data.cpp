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

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
VarlenDataPool::VarlenDataPool(size_t initial_size, std::shared_ptr<Utils::MemoryPool> mem_pool)
    : m_next_block_id(1),
      m_next_overflow_page_id(1),
      m_allocation_count(0),
      m_deallocation_count(0),
      m_overflow_count(0),
      m_memory_pool(mem_pool) {
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
  // Release all overflow pages
  for (auto &[page_id, page] : m_overflow_pages) {
    if (page->mapped_data) {
      // Unmap memory
      // munmap(page->mapped_data, page->page_size);
    }
  }
}

bool VarlenDataPool::allocate(const uchar *data, size_t length, VarlenReference &ref) {
  if (!data || length == 0) return false;

  // 1. Determine storage type
  if (length < INLINE_THRESHOLD) {
    // Inline storage (caller responsible for storing in CU data area)
    ref.storage_type = VarlenReference::INLINE;
    ref.length = length;
    return true;
  }

  if (length < POOL_THRESHOLD) {
    // Pool storage
    return allocate_in_pool(data, length, ref);
  }

  // Overflow storage
  return allocate_overflow(data, length, ref);
}

bool VarlenDataPool::deallocate(const VarlenReference &ref) {
  if (ref.is_inline()) {
    // Inline data doesn't need deallocation
    return true;
  }

  if (ref.is_pool()) {
    return deallocate_from_pool(ref);
  }

  if (ref.is_overflow()) {
    return deallocate_overflow(ref);
  }

  return false;
}

size_t VarlenDataPool::read(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const {
  if (ref.is_inline()) {
    // Inline data handled by caller
    return 0;
  }

  if (ref.is_pool()) {
    return read_from_pool(ref, buffer, buffer_size);
  }

  if (ref.is_overflow()) {
    return read_from_overflow(ref, buffer, buffer_size);
  }

  return 0;
}

const uchar *VarlenDataPool::get_data_ptr(const VarlenReference &ref) const {
  if (!ref.is_pool()) return nullptr;

  std::lock_guard lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return nullptr;

  DataBlock *block = it->second;
  if (!block || !block->header.is_valid()) return nullptr;

  if (ref.offset + ref.length > block->header.used_size) {
    return nullptr;
  }

  return block->data + ref.offset;
}

size_t VarlenDataPool::compact() {
  std::lock_guard lock(m_mutex);

  size_t freed = 0;

  // 1. Merge adjacent free blocks
  // TODO: Implement defragmentation algorithm

  // 2. Release completely free blocks
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    DataBlock *block = it->get();

    if (block->header.used_size == 0) {
      // Completely free, can be released
      freed += block->header.size;

      // Remove from index
      m_block_index.erase(block->header.block_id);

      // Remove from free list
      remove_from_freelist(&block->header);

      // Release block
      it = m_blocks.erase(it);
      m_header.block_count--;
    } else {
      ++it;
    }
  }

  m_header.used_size -= freed;

  return freed;
}

size_t VarlenDataPool::get_total_size() const { return m_header.total_size; }

size_t VarlenDataPool::get_used_size() const { return m_header.used_size; }

double VarlenDataPool::get_fragmentation_ratio() const {
  if (m_header.total_size == 0) return 0.0;

  size_t wasted = m_header.total_size - m_header.used_size;
  return static_cast<double>(wasted) / m_header.total_size;
}

size_t VarlenDataPool::get_block_count() const { return m_header.block_count; }

size_t VarlenDataPool::get_overflow_page_count() const { return m_overflow_count.load(); }

VarlenDataPool::AllocationStats VarlenDataPool::get_stats() const {
  AllocationStats stats;
  stats.allocation_count = m_allocation_count.load();
  stats.deallocation_count = m_deallocation_count.load();
  stats.overflow_count = m_overflow_count.load();
  stats.total_size = m_header.total_size;
  stats.used_size = m_header.used_size;
  stats.fragmentation_ratio = get_fragmentation_ratio();
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
  out << "  Fragmentation: " << (get_fragmentation_ratio() * 100) << "%\n";
  out << "  Blocks: " << m_header.block_count << "\n";
  out << "  Free Blocks: " << m_header.free_blocks << "\n";
  out << "  Overflow Pages: " << m_overflow_count.load() << "\n";
  out << "  Allocations: " << m_allocation_count.load() << "\n";
  out << "  Deallocations: " << m_deallocation_count.load() << "\n";
}

bool VarlenDataPool::allocate_in_pool(const uchar *data, size_t length, VarlenReference &ref) {
  std::lock_guard lock(m_mutex);

  // Align length
  size_t aligned_length = align_size(length);

  // 1. Find suitable free block
  DataBlock *block = find_free_block(aligned_length);

  if (!block) {
    // 2. No suitable free block found, allocate new block
    size_t block_size = std::max(aligned_length * 2, DEFAULT_BLOCK_SIZE);
    block = allocate_new_block(block_size);

    if (!block) return false;
  }

  // 3. Allocate space in block
  uint32_t offset = block->header.used_size;

  if (offset + aligned_length > block->header.size - sizeof(BlockHeader)) {
    return false;  // Should not happen
  }

  // 4. Copy data
  std::memcpy(block->data + offset, data, length);

  // 5. Update block header
  block->header.used_size += aligned_length;

  // 6. If block is full, remove from free list
  if (block->header.available_space() < MIN_BLOCK_SIZE) {
    remove_from_freelist(&block->header);
  }

  // 7. Set reference
  ref.block_id = block->header.block_id;
  ref.offset = offset;
  ref.length = length;
  ref.storage_type = VarlenReference::POOL;

  // 8. Update statistics
  m_header.used_size += aligned_length;
  m_allocation_count.fetch_add(1);

  return true;
}

bool VarlenDataPool::deallocate_from_pool(const VarlenReference &ref) {
  std::lock_guard lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return false;

  DataBlock *block = it->second;

  // Simplified implementation: mark as available space
  // Complete implementation needs to maintain free space list

  size_t aligned_length = align_size(ref.length);

  // If it's the last allocated data in the block, can roll back
  if (ref.offset + aligned_length == block->header.used_size) {
    block->header.used_size -= aligned_length;
    m_header.used_size -= aligned_length;

    // If block becomes empty, add to free list
    if (block->header.used_size == 0) {
      add_to_freelist(&block->header);
    }
  }

  m_deallocation_count.fetch_add(1);

  return true;
}

size_t VarlenDataPool::read_from_pool(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const {
  std::lock_guard lock(m_mutex);

  auto it = m_block_index.find(ref.block_id);
  if (it == m_block_index.end()) return 0;

  DataBlock *block = it->second;

  if (ref.offset + ref.length > block->header.used_size) {
    return 0;
  }

  size_t copy_len = std::min(static_cast<size_t>(ref.length), buffer_size);
  std::memcpy(buffer, block->data + ref.offset, copy_len);

  return copy_len;
}

bool VarlenDataPool::allocate_overflow(const uchar *data, size_t length, VarlenReference &ref) {
  std::lock_guard lock(m_mutex);

  // 1. Create overflow page
  auto page = std::make_unique<OverflowPage>();
  page->page_id = m_next_overflow_page_id.fetch_add(1);
  page->page_size = align_size(length);
  page->data_length = length;

  // 2. Generate file path
  // Format: overflow_<imcu_id>_<page_id>.dat
  page->file_path = generate_overflow_file_path(page->page_id);
  page->file_offset = 0;

  // 3. Write to file
  if (!write_overflow_to_file(page.get(), data, length)) {
    return false;
  }

  // 4. Set reference
  ref.block_id = static_cast<uint32_t>(page->page_id);  // Reuse block_id field
  ref.offset = 0;
  ref.length = length;
  ref.storage_type = VarlenReference::OVERFLOW;

  // 5. Save overflow page
  m_overflow_pages[page->page_id] = std::move(page);

  // 6. Update statistics
  m_overflow_count.fetch_add(1);
  m_allocation_count.fetch_add(1);

  return true;
}

bool VarlenDataPool::deallocate_overflow(const VarlenReference &ref) {
  std::lock_guard lock(m_mutex);

  uint64_t page_id = ref.block_id;

  auto it = m_overflow_pages.find(page_id);
  if (it == m_overflow_pages.end()) return false;

  OverflowPage *page = it->second.get();

  // Delete file
  if (!page->file_path.empty()) {
    // std::remove(page->file_path.c_str());
  }

  // Remove overflow page
  m_overflow_pages.erase(it);

  m_overflow_count.fetch_sub(1);
  m_deallocation_count.fetch_add(1);

  return true;
}

size_t VarlenDataPool::read_from_overflow(const VarlenReference &ref, uchar *buffer, size_t buffer_size) const {
  std::lock_guard lock(m_mutex);

  uint64_t page_id = ref.block_id;

  auto it = m_overflow_pages.find(page_id);
  if (it == m_overflow_pages.end()) return 0;

  OverflowPage *page = it->second.get();

  // If already memory mapped, copy directly
  if (page->mapped_data) {
    size_t copy_len = std::min(static_cast<size_t>(ref.length), buffer_size);
    std::memcpy(buffer, page->mapped_data + ref.offset, copy_len);
    return copy_len;
  }

  // Read from file
  return read_overflow_from_file(page, ref.offset, buffer, buffer_size);
}

VarlenDataPool::DataBlock *VarlenDataPool::allocate_new_block(size_t size) {
  // Align size
  size_t aligned_size = align_size(size);
  size_t total_size = sizeof(BlockHeader) + aligned_size;

  // Allocate memory
  void *mem = nullptr;
  if (m_memory_pool) {
    mem = m_memory_pool->allocate(total_size);
  } else {
    mem = ::operator new(total_size);
  }

  if (!mem) return nullptr;

  // Initialize block
  DataBlock *block = reinterpret_cast<DataBlock *>(mem);
  block->header.block_id = m_next_block_id.fetch_add(1);
  block->header.size = aligned_size;
  block->header.used_size = 0;
  block->header.magic = BlockHeader::MAGIC_NUMBER;
  block->header.next_free = nullptr;

  BlockDeleter deleter = [this](DataBlock *b) {
    if (m_memory_pool) {
      m_memory_pool->deallocate(b, sizeof(BlockHeader) + b->header.size);
    } else {
      ::operator delete(b);
    }
  };

  m_blocks.emplace_back(block, std::move(deleter));

  // Add to index
  m_block_index[block->header.block_id] = block;

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
    if (m_freelists[i].head) {
      BlockHeader *header = m_freelists[i].head;

      // Check if there's enough space
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

std::string VarlenDataPool::generate_overflow_file_path(uint64_t page_id) const {
  // Simplified implementation
  return "./tmp/shannonbase_overflow_" + std::to_string(page_id) + ".dat";
}

bool VarlenDataPool::write_overflow_to_file(OverflowPage *page, const uchar *data, size_t length) const {
  // Simplified implementation: actual implementation should use mmap or async I/O
  FILE *fp = fopen(page->file_path.c_str(), "wb");
  if (!fp) return false;

  size_t written = fwrite(data, 1, length, fp);
  fclose(fp);

  return written == length;
}

size_t VarlenDataPool::read_overflow_from_file(const OverflowPage *page, uint64_t offset, uchar *buffer,
                                               size_t buffer_size) const {
  FILE *fp = fopen(page->file_path.c_str(), "rb");
  if (!fp) return 0;

  if (fseek(fp, page->file_offset + offset, SEEK_SET) != 0) {
    fclose(fp);
    return 0;
  }

  size_t to_read = std::min(page->data_length - offset, buffer_size);
  size_t read = fread(buffer, 1, to_read, fp);

  fclose(fp);
  return read;
}
}  // namespace Imcs
}  // namespace ShannonBase