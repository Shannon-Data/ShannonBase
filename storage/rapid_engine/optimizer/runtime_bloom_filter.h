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
#ifndef SHANNONBASE_RUNTIME_BLOOM_FILTER_H
#define SHANNONBASE_RUNTIME_BLOOM_FILTER_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <vector>

namespace ShannonBase::Utils {
// Snapshot-based policy: no hot-loop locks and no new resource reservation.
// Active/queued AP and propagation work are Rapid's workload pressure signal.
struct RuntimeBloomPolicy {
  size_t max_bytes{0};
  unsigned min_reject_percent{12};

  // Placement hysteresis: compaction needs stronger filtering than probing.
  // Resume after a selective window instead of trusting the initial sample
  // for the entire query. Division avoids overflow for arbitrary counts.
  static bool scan_pushdown_after_sample(bool active, size_t checks, size_t rejects) noexcept {
    if (checks == 0) return active;
    const size_t required = active ? checks / 2 + checks % 2 : checks - checks / 4;
    return rejects >= required;
  }

  static RuntimeBloomPolicy choose(size_t operator_limit, size_t operator_used, size_t pool_capacity, size_t pool_used,
                                   size_t pool_reserved, size_t active, bool waiting) noexcept {
    if (operator_used >= operator_limit || pool_used >= pool_capacity) return {};
    const size_t headroom = pool_capacity - std::max(pool_used, std::min(pool_reserved, pool_capacity));
    const bool pressure = waiting || headroom < pool_capacity / 4;
    const size_t share = std::clamp<size_t>(active, 1, 64);
    const size_t cap = (1024 * 1024 / share) / (pressure ? 4 : 1);
    return {std::min({operator_limit - operator_used, operator_limit / 32, headroom / 32, cap}),
            pressure || active > 1 ? 50U : 12U};
  }
};

// Query-local blocked Bloom filter. Hashes must come from the exact same
// canonical key as the hash join. A positive always needs exact comparison.
// Each lookup touches one 256-bit block; portable scalar implementation.
class RuntimeBloomFilter {
 public:
  explicit RuntimeBloomFilter(std::pmr::memory_resource *resource) : m_blocks(resource) {}
  RuntimeBloomFilter(const RuntimeBloomFilter &) = delete;
  RuntimeBloomFilter &operator=(const RuntimeBloomFilter &) = delete;

  void reset() noexcept { decltype(m_blocks)(m_blocks.get_allocator()).swap(m_blocks); }
  bool prepare(size_t keys, size_t max_bytes) noexcept {
    reset();
    if (!keys || keys > std::numeric_limits<size_t>::max() / 2) return false;
    // At least 16 bits per key. Round up to whole blocks, without overflow.
    const size_t blocks = keys / 16 + (keys % 16 != 0);
    if (blocks > std::numeric_limits<uint32_t>::max() || blocks > max_bytes / sizeof(Block)) return false;
    try {
      m_blocks.resize(blocks, Block{});
    } catch (const std::bad_alloc &) {
      reset();
      return false;
    }
    return true;
  }
  void insert(uint64_t hash) noexcept {
    if (m_blocks.empty()) return;
    auto &block = m_blocks[block_index(hash)];
    const uint32_t key = static_cast<uint32_t>(hash);
    for (size_t i = 0; i < block.size(); ++i) block[i] |= mask(key, i);
  }
  bool may_contain(uint64_t hash) const noexcept {
    if (m_blocks.empty()) return true;  // disabled filters never reject
    const auto &block = m_blocks[block_index(hash)];
    const uint32_t key = static_cast<uint32_t>(hash);
    for (size_t i = 0; i < block.size(); ++i)
      if ((block[i] & mask(key, i)) == 0) return false;
    return true;
  }
  size_t bytes() const noexcept { return m_blocks.capacity() * sizeof(Block); }

 private:
  using Block = std::array<uint32_t, 8>;
  size_t block_index(uint64_t hash) const noexcept {
    return ((hash >> 32) * static_cast<uint64_t>(m_blocks.size())) >> 32;
  }
  static uint32_t mask(uint32_t key, size_t lane) noexcept {
    constexpr uint32_t salts[] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                  0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};
    return uint32_t{1} << ((key * salts[lane]) >> 27);
  }
  std::pmr::vector<Block> m_blocks;
};
}  // namespace ShannonBase::Utils
#endif
