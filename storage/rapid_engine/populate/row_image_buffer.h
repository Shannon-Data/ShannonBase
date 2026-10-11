// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef SHANNONBASE_ROW_IMAGE_BUFFER_H
#define SHANNONBASE_ROW_IMAGE_BUFFER_H

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

namespace ShannonBase::Populate {
// Producer-local cache; ownership follows the record across asynchronous apply
// and producer exit. Returning a buffer never allocates. Large rows bypass it.
class RowImageBufferPool {
 public:
  using Buffer = unsigned char[];

  static std::shared_ptr<Buffer> acquire(size_t bytes) {
    if (!bytes) return {};
    if (bytes > kMaxBuffer) return std::shared_ptr<Buffer>(new unsigned char[bytes]);
    thread_local auto pool = std::make_shared<RowImageBufferPool>();
    size_t capacity = 256;
    while (capacity < bytes) capacity *= 2;
    auto buffer = pool->take(capacity);
    if (!buffer) buffer = std::make_unique<Buffer>(capacity);
    return std::shared_ptr<Buffer>(buffer.release(), [owner = pool, capacity](unsigned char *data) {
      owner->put(std::unique_ptr<Buffer>(data), capacity);
    });
  }

 private:
  static constexpr size_t kMaxBuffer = 64 * 1024;
  static constexpr size_t kMaxCachedBytes = 1024 * 1024;
  struct Slot {
    std::unique_ptr<Buffer> buffer;
    size_t capacity{0};
  };
  std::unique_ptr<Buffer> take(size_t capacity) {
    std::lock_guard guard(m_mutex);
    for (size_t i = m_count; i > 0; --i) {
      auto &slot = m_slots[i - 1];
      if (slot.capacity == capacity) {
        m_cached_bytes -= capacity;
        auto buffer = std::move(slot.buffer);
        --m_count;
        if (i - 1 != m_count) slot = std::move(m_slots[m_count]);
        return buffer;
      }
    }
    return {};
  }
  void put(std::unique_ptr<Buffer> buffer, size_t capacity) {
    std::lock_guard guard(m_mutex);
    if (m_cached_bytes + capacity > kMaxCachedBytes || m_count == m_slots.size()) return;
    auto &slot = m_slots[m_count++];
    slot.capacity = capacity;
    slot.buffer = std::move(buffer);
    m_cached_bytes += capacity;
  }
  std::mutex m_mutex;
  std::array<Slot, 256> m_slots;
  size_t m_count{0};
  size_t m_cached_bytes{0};
};
}  // namespace ShannonBase::Populate
#endif
