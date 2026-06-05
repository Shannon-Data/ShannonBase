/*
   Copyright (c) 2014, 2023, Oracle and/or its affiliates.

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

   Shannon Data AI.
*/
#ifndef __SHANNONBASE_RPD_TYPES_H__
#define __SHANNONBASE_RPD_TYPES_H__
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

#include "field_types.h"          //for MYSQL_TYPE_XXX
#include "include/my_alloc.h"     // MEM_ROOT
#include "include/my_inttypes.h"  //uint8_t
#include "sql/field.h"

#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {

// used for SHANNON_DB_TRX_ID FIELD.
class Mock_field_trxid : public Field_longlong {
 public:
  Mock_field_trxid()
      : Field_longlong(nullptr,                    // ptr_arg
                       8,                          // len_arg
                       &Field::dummy_null_buffer,  // null_ptr_arg
                       1,                          // null_bit_arg
                       Field::NONE,                // auto_flags_arg
                       SHANNON_DB_TRX_ID,          // field_name_arg
                       false,                      // zero_arg
                       true)                       // unsigned_arg
  {}

  void make_writable() { bitmap_set_bit(table->write_set, field_index()); }
  void make_readable() { bitmap_set_bit(table->read_set, field_index()); }
};

extern MEM_ROOT rapid_mem_root;
// the memory object for all rapid engine.
class MemoryObject {};

typedef struct alignas(CACHE_LINE_SIZE) BitArray {
  BitArray() = delete;

  explicit BitArray(size_t num_rows) : rows(num_rows), size((num_rows + 7) / 8), data(new uint8_t[size]) {
    std::memset(data, 0x0, size);
  }

  BitArray(const BitArray &other) : rows(other.rows), size(other.size) {
    if (size > 0) {
      data = new uint8_t[size];
      std::memcpy(data, other.data, size);
    }
  }

  BitArray(BitArray &&other) noexcept
      : rows(std::exchange(other.rows, 0)),
        size(std::exchange(other.size, 0)),
        data(std::exchange(other.data, nullptr)) {}
  ~BitArray() { delete[] data; }

  BitArray &operator=(const BitArray &other) {
    if (this != &other) {
      BitArray tmp(other);
      swap(tmp);
    }
    return *this;
  }

  BitArray &operator=(BitArray &&other) noexcept {
    if (this != &other) {
      delete[] data;
      data = std::exchange(other.data, nullptr);
      rows = std::exchange(other.rows, 0);
      size = std::exchange(other.size, 0);
    }
    return *this;
  }

  inline void set() {
    if (!data || !size) return;
    std::memset(data, 0xFF, size);
    mask_tail_byte();
  }

  inline void reset() {
    if (!data || !size) return;
    std::memset(data, 0x0, size);
  }

  inline BitArray clone_empty() const {
    BitArray b(rows);
    b.reset();
    return b;
  }

  void not_inplace() {
    if (!data || !size) return;
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const __m256i all_ones = _mm256_set1_epi8(static_cast<char>(0xFF));
    const size_t simd_width = 32;  // 256-bit / 8 = 32 bytes
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_xor_si256(chunk, all_ones));
    }
    for (; i < size; ++i) data[i] ^= 0xFF;
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    // SSE2: XOR with all-ones via _mm_xor_si128; 16 bytes per register.
    const __m128i all_ones = _mm_set1_epi8(static_cast<char>(0xFF));
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(data + i), _mm_xor_si128(chunk, all_ones));
    }
    for (; i < size; ++i) data[i] ^= 0xFF;
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t chunk = vld1q_u8(data + i);
      vst1q_u8(data + i, vmvnq_u8(chunk));
    }
    for (; i < size; ++i) data[i] ^= 0xFF;
#else
    for (size_t i = 0; i < size; ++i) data[i] ^= 0xFF;
#endif
    mask_tail_byte();
  }

  void and_with(const BitArray &other) {
    assert(rows == other.rows);  // [F6]
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const size_t simd_width = 32;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_and_si256(a, b));
    }
    for (; i < size; ++i) data[i] &= other.data[i];
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(other.data + i));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(data + i), _mm_and_si128(a, b));
    }
    for (; i < size; ++i) data[i] &= other.data[i];
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t a = vld1q_u8(data + i);
      uint8x16_t b = vld1q_u8(other.data + i);
      vst1q_u8(data + i, vandq_u8(a, b));
    }
    for (; i < size; ++i) data[i] &= other.data[i];
#else
    for (size_t i = 0; i < size; ++i) data[i] &= other.data[i];
#endif
  }

  void or_with(const BitArray &other) {
    assert(rows == other.rows);  // [F6]
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const size_t simd_width = 32;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_or_si256(a, b));
    }

    for (; i < size; ++i) data[i] |= other.data[i];
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(other.data + i));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(data + i), _mm_or_si128(a, b));
    }
    for (; i < size; ++i) data[i] |= other.data[i];
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t a = vld1q_u8(data + i);
      uint8x16_t b = vld1q_u8(other.data + i);
      vst1q_u8(data + i, vorrq_u8(a, b));
    }
    for (; i < size; ++i) data[i] |= other.data[i];
#else
    for (size_t i = 0; i < size; ++i) data[i] |= other.data[i];
#endif
  }

  void xor_with(const BitArray &other) {
    assert(rows == other.rows);  // [F6]
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const size_t simd_width = 32;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_xor_si256(a, b));
    }

    for (; i < size; ++i) data[i] ^= other.data[i];
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(other.data + i));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(data + i), _mm_xor_si128(a, b));
    }
    for (; i < size; ++i) data[i] ^= other.data[i];
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t a = vld1q_u8(data + i);
      uint8x16_t b = vld1q_u8(other.data + i);
      vst1q_u8(data + i, veorq_u8(a, b));
    }
    for (; i < size; ++i) data[i] ^= other.data[i];
#else
    for (size_t i = 0; i < size; ++i) data[i] ^= other.data[i];
#endif
  }

  bool is_all_false() const {
    if (!data || !size) return true;
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const size_t simd_width = 32;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      if (!_mm256_testz_si256(chunk, chunk)) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0) return false;
    }
    return true;
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    // SSE4.1: _mm_testz_si128 returns 1 when (a AND b) == 0.
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      if (!_mm_testz_si128(chunk, chunk)) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0) return false;
    }
    return true;
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t chunk = vld1q_u8(data + i);
      if (vmaxvq_u8(chunk) != 0) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0) return false;
    }
    return true;
#else
    for (size_t i = 0; i < size; ++i) {
      if (data[i] != 0) return false;
    }
    return true;
#endif
  }

  bool is_all_true() const {
    if (!data || !size) return true;

    // Number of full bytes that must all be 0xFF.
    // If rows % 8 == 0 → tail_bits = 0 → all `size` bytes are full.
    const size_t tail_bits = rows % 8;
    const size_t full_bytes = (tail_bits == 0) ? size : (size - 1);
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    {
      const __m256i all_ones = _mm256_set1_epi8(static_cast<char>(0xFF));
      const size_t simd_width = 32;
      size_t i = 0;
      for (; i + simd_width <= full_bytes; i += simd_width) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
        __m256i flipped = _mm256_xor_si256(chunk, all_ones);
        if (!_mm256_testz_si256(flipped, flipped)) return false;
      }
      for (; i < full_bytes; ++i) {
        if (data[i] != 0xFF) return false;
      }
    }
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    {
      // _mm_cmpeq_epi8 + _mm_movemask_epi8: all 16 bytes 0xFF → movemask == 0xFFFF.
      const __m128i all_ones = _mm_set1_epi8(static_cast<char>(0xFF));
      const size_t simd_width = 16;
      size_t i = 0;
      for (; i + simd_width <= full_bytes; i += simd_width) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
        // XOR with all-ones: result is 0 iff chunk == 0xFF for every byte.
        __m128i flipped = _mm_xor_si128(chunk, all_ones);
        if (!_mm_testz_si128(flipped, flipped)) return false;
      }
      for (; i < full_bytes; ++i) {
        if (data[i] != 0xFF) return false;
      }
    }
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    {
      const size_t simd_width = 16;
      size_t i = 0;
      for (; i + simd_width <= full_bytes; i += simd_width) {
        uint8x16_t chunk = vld1q_u8(data + i);
        if (vminvq_u8(chunk) != 0xFF) return false;
      }
      for (; i < full_bytes; ++i) {
        if (data[i] != 0xFF) return false;
      }
    }
#else
    for (size_t i = 0; i < full_bytes; ++i) {
      if (data[i] != 0xFF) return false;
    }
#endif
    // Check the tail byte (only valid bits).
    if (tail_bits != 0) {
      const uint8_t tail_mask = static_cast<uint8_t>((1u << tail_bits) - 1u);
      if ((data[size - 1] & tail_mask) != tail_mask) return false;
    }
    return true;
  }

  size_t count_ones() const {
    if (!data) return 0;

    size_t count = 0;
    size_t i = 0;
#if defined(SHANNON_AVX_VECT_SUPPORTED)
    // Process 8 bytes (64 bits) per __builtin_popcountll call.
    // Unaligned access via uint64_t pointer is safe on x86.
    const size_t qwords = size / 8;
    const uint64_t *data64 = reinterpret_cast<const uint64_t *>(data);
    for (size_t q = 0; q < qwords; ++q) {
      count += __builtin_popcountll(data64[q]);
    }
    i = qwords * 8;
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
    // vcntq_u8 is NEON-only; on SSE fall through to the byte-by-byte popcount
    // below, which is already fast thanks to __builtin_popcount being compiled
    // to POPCNT when -mpopcnt / -msse4.2 is active.
    // (Alternatively one could process 8 bytes at a time via popcountll here
    //  too, but let's keep it simple and consistent.)
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    // vcntq_u8: per-byte popcount; vaddlvq_u8: horizontal sum.
    const size_t simd_width = 16;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t v = vld1q_u8(data + i);
      uint8x16_t counts = vcntq_u8(v);
      count += vaddlvq_u8(counts);
    }
#endif
    // Scalar tail (or full loop for SSE path).
    for (; i < size; ++i) {
      count += static_cast<size_t>(__builtin_popcount(data[i]));
    }
    return count;
  }

  void swap(BitArray &other) noexcept {
    std::swap(data, other.data);
    std::swap(rows, other.rows);
    std::swap(size, other.size);
  }

  size_t rows{0};
  size_t size{0};
  uint8_t *data{nullptr};

 private:
  inline void mask_tail_byte() noexcept {
    const size_t tail_bits = rows % 8;
    if (tail_bits != 0 && size > 0) {
      const uint8_t tail_mask = static_cast<uint8_t>((1u << tail_bits) - 1u);
      data[size - 1] &= tail_mask;
    }
  }
} BitArray_t;

using bit_array_t = BitArray_t;

using mysql_field_t = struct mysql_field_info {
  // whether field nullable or not.
  bool has_nullbit{false};
  // if is nullable, true is null, or none-null.
  bool is_null{false};

  // type in mysql type. DATA_MISSING = 0.
  uint mtype{0u};

  // mysql field length.
  size_t mlength{0};

  // physical length (innodb).
  size_t plength{0};

  // field data.
  std::unique_ptr<uchar[]> data{nullptr};
};

using key_info_t = std::pair<uint, std::unique_ptr<uchar[]>>;

}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_TYPES_H__