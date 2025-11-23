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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Impl of Ring Buffer of Rapid.
*/
#ifndef __SHANNONBASE_MPMC_RINGBUFFER_H__
#define __SHANNONBASE_MPMC_RINGBUFFER_H__
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace Populate {
template <typename T,
          size_t BufferSize = 1 << 18,  // 256K changes records
          size_t CacheLine = CACHE_LINE_SIZE>
class Ringbuffer {
  static_assert((BufferSize & (BufferSize - 1)) == 0, "BufferSize must be power of 2");
  static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>,
                "T must be move constructible and move assignable");

 private:
  using Storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
  T *slot_ptr(size_t pos) noexcept { return reinterpret_cast<T *>(&buffer_[pos]); }
  const T *slot_ptr(size_t pos) const noexcept { return reinterpret_cast<const T *>(&buffer_[pos]); }

 public:
  constexpr static size_t capacity = BufferSize;
  Ringbuffer() noexcept {
    for (size_t i = 0; i < BufferSize; ++i) {
      seqs_[i].store(i, std::memory_order_relaxed);
    }
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
          seqs_[pos].store(tail + BufferSize + 1, std::memory_order_release);
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

  bool empty() const noexcept { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
  bool full() const noexcept {
    uint64_t head = head_.load(std::memory_order_acquire);
    uint64_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) >= BufferSize;
  }
  size_t size() const noexcept {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    return static_cast<size_t>(h - t);
  }
  size_t available() const noexcept { return capacity - size(); }
  void clear() noexcept {
    T dummy;
    while (try_pop(dummy)) {
    }
  }

  void reset() noexcept {
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
  alignas(CacheLine) Storage buffer_[BufferSize];
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  // __SHANNONBASE_MPMC_RINGBUFFER_H__