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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2026, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Impl of Ring Buffer of Rapid.
*/
#ifndef __SHANNONBASE_MPMC_RINGBUFFER_H__
#define __SHANNONBASE_MPMC_RINGBUFFER_H__
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include "storage/rapid_engine/utils/row_image_buffer.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"  // CACHE_LINE_SIZE
#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace Populate {
using ::ShannonBase::RowImageBufferPool;

/**
 * Multi-producer ring buffer with an exclusive consumer interface.
 *
 * Concurrency model:
 *   - try_put()  : any number of producers may call concurrently.
 *   - try_pop()  : ONLY ONE consumer may call try_pop().  If multiple
 *                  consumers are needed they must serialise on an
 *                  external mutex.  Calling try_pop() concurrently
 *                  with consume() or peek()+consume() on the same
 *                  ring is undefined behaviour.
 *   - consume() / peek() + consume() : alternative single-consumer
 *                  interface (the same exclusivity rule applies).
 *
 * This is the classic DPDK-style CAS-based ring where each slot has a
 * sequence number that encodes ownership.  Both head_ (producer) and
 * tail_ (consumer) advance independently via CAS.
 */
template <typename T,
          size_t BufferSize = 1 << 18,  // 256K changes records
          size_t CacheLine = CACHE_LINE_SIZE>
class Ringbuffer {
  static_assert((BufferSize & (BufferSize - 1)) == 0, "BufferSize must be power of 2");
  static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>,
                "T must be move constructible and move assignable");

 private:
  struct alignas(T) Storage {
    std::byte bytes[sizeof(T)];
  };
  static constexpr size_t chunk_size = BufferSize < 256 ? BufferSize : 256;
  static constexpr size_t chunk_count = BufferSize / chunk_size;
  bool ensure_chunk(size_t pos) noexcept {
    auto &slot = chunks_[pos / chunk_size];
    if (slot.load(std::memory_order_acquire)) return true;
    auto *chunk = new (std::nothrow) Storage[chunk_size];
    if (!chunk) return false;
    Storage *expected = nullptr;
    if (!slot.compare_exchange_strong(expected, chunk, std::memory_order_release, std::memory_order_acquire))
      delete[] chunk;
    return true;
  }
  T *slot_ptr(size_t pos) noexcept {
    return reinterpret_cast<T *>(&chunks_[pos / chunk_size].load(std::memory_order_acquire)[pos % chunk_size]);
  }
  const T *slot_ptr(size_t pos) const noexcept {
    return reinterpret_cast<const T *>(&chunks_[pos / chunk_size].load(std::memory_order_acquire)[pos % chunk_size]);
  }

 public:
  constexpr static size_t capacity = BufferSize;
  Ringbuffer() noexcept {
    for (size_t i = 0; i < BufferSize; ++i) {
      seqs_[i].store(i, std::memory_order_relaxed);
    }
  }

  ~Ringbuffer() noexcept {
    clear();
    for (auto &chunk : chunks_) delete[] chunk.load(std::memory_order_relaxed);
  }

  Ringbuffer(const Ringbuffer &) = delete;
  Ringbuffer &operator=(const Ringbuffer &) = delete;

  bool try_put(const T &item) noexcept { return try_put_impl(item); }
  bool try_put(T &&item) noexcept { return try_put_impl(std::move(item)); }
  size_t try_put_bulk(const T *items, size_t count) noexcept {
    size_t written = 0;
    while (written < count && try_put(items[written])) {
      ++written;
    }
    return written;
  }

  bool try_pop(T &item) noexcept {
    uint64_t tail = tail_.load(std::memory_order_relaxed);
    while (true) {
      uint64_t pos = tail & mask_;
      uint64_t seq = seqs_[pos].load(std::memory_order_acquire);
      int64_t diff = static_cast<int64_t>(seq - (tail + 1));
      if (diff == 0) {
        if (tail_.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed)) {
          item = std::move(*slot_ptr(pos));
          slot_ptr(pos)->~T();
          seqs_[pos].store(tail + BufferSize, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        tail = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  size_t try_pop_bulk(T *items, size_t count) noexcept {
    size_t read = 0;
    while (read < count && try_pop(items[read])) {
      ++read;
    }
    return read;
  }

  const T *peek() const noexcept {
    uint64_t tail = tail_.load(std::memory_order_acquire);
    uint64_t pos = tail & mask_;
    return (seqs_[pos].load(std::memory_order_acquire) == tail + 1) ? slot_ptr(pos) : nullptr;
  }
  T *peek() noexcept {
    uint64_t tail = tail_.load(std::memory_order_acquire);
    uint64_t pos = tail & mask_;
    return (seqs_[pos].load(std::memory_order_acquire) == tail + 1) ? slot_ptr(pos) : nullptr;
  }
  void consume(size_t n = 1) noexcept {
    uint64_t tail = tail_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < n; ++i) {
      uint64_t pos = (tail + i) & mask_;
      slot_ptr(pos)->~T();
      seqs_[pos].store(tail + BufferSize + i, std::memory_order_release);
    }

    tail_.store(tail + n, std::memory_order_release);
  }

  // Occupancy includes reserved, not-yet-published slots. Use try_pop()/peek()
  // to check readiness; a nonempty ring does not guarantee peek() is non-null.
  bool empty() const noexcept { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
  bool full() const noexcept {
    uint64_t head = head_.load(std::memory_order_acquire);
    uint64_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) >= BufferSize;
  }
  // Advisory only while producers/consumer are active.
  size_t size() const noexcept {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    return h >= t ? std::min<size_t>(h - t, BufferSize) : 0;
  }
  size_t available() const noexcept { return capacity - size(); }
  void clear() noexcept {
    T dummy;
    while (try_pop(dummy)) {
    }
  }

  void reset() noexcept {
    clear();
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < BufferSize; ++i) {
      seqs_[i].store(i, std::memory_order_relaxed);
    }
  }

 private:
  template <typename U>
  bool try_put_impl(U &&item) noexcept {
    uint64_t head = head_.load(std::memory_order_relaxed);
    while (true) {
      uint64_t pos = head & mask_;
      uint64_t seq = seqs_[pos].load(std::memory_order_acquire);
      int64_t diff = static_cast<int64_t>(seq - head);
      if (diff == 0) {
        if (!ensure_chunk(pos)) return false;
        if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
          new (slot_ptr(pos)) T(std::forward<U>(item));
          seqs_[pos].store(head + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        head = head_.load(std::memory_order_relaxed);
      }
    }
  }
  static constexpr uint64_t mask_ = BufferSize - 1;
  alignas(CacheLine) std::atomic<uint64_t> head_{0};
  alignas(CacheLine) std::atomic<uint64_t> tail_{0};
  alignas(CacheLine) std::atomic<uint64_t> seqs_[BufferSize]{};
  // Allocate row storage only as occupancy grows; cold tables retain only
  // sequence metadata rather than reserving all 256K heavyweight records.
  alignas(CacheLine) std::atomic<Storage *> chunks_[chunk_count]{};
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  // __SHANNONBASE_MPMC_RINGBUFFER_H__