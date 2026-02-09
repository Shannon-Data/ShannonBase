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
#include <cstring>

#include "field_types.h"          //for MYSQL_TYPE_XXX
#include "include/my_alloc.h"     // MEM_ROOT
#include "include/my_inttypes.h"  //uint8_t
#include "sql/field.h"

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
// the memor object for all rapid engine.
class MemoryObject {};

typedef struct alignas(CACHE_LINE_SIZE) BitArray {
  BitArray() = delete;

  explicit BitArray(size_t rows) {
    size = (rows + 7) / 8;
    data = new uint8_t[size];
    std::memset(data, 0x0, size);
  }

  BitArray(const BitArray &other) {
    size = other.size;
    if (size > 0) {
      data = new uint8_t[size];
      std::memcpy(data, other.data, size);
    }
  }

  BitArray(BitArray &&other) noexcept : data(std::exchange(other.data, nullptr)), size(std::exchange(other.size, 0)) {}

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
      size = std::exchange(other.size, 0);
    }
    return *this;
  }

  inline void set() {
    if (!data || !size) return;
    std::memset(data, 0xFF, size);
  }

  inline void reset() {
    if (!data || !size) return;
    std::memset(data, 0x0, size);
  }

  void not_inplace() {
    if (!data || !size) return;
#ifdef SHANNON_AVX_VECT_SUPPORTED
    // AVX2: XOR with all-ones flips every bit.
    const __m256i all_ones = _mm256_set1_epi8(static_cast<char>(0xFF));
    const size_t simd_width = 256 / 8;  // 32 bytes per register
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_xor_si256(chunk, all_ones));
    }
    for (; i < size; ++i) data[i] ^= 0xFF;

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    // NEON: vmvnq_u8 performs bitwise NOT on 16 bytes at once.
    const size_t simd_width = 128 / 8;  // 16 bytes per register
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t chunk = vld1q_u8(data + i);
      vst1q_u8(data + i, vmvnq_u8(chunk));
    }
    for (; i < size; ++i) data[i] ^= 0xFF;

#else
    for (size_t i = 0; i < size; ++i) data[i] ^= 0xFF;
#endif
  }

  void and_with(const BitArray &other) {
    assert(size == other.size);
#ifdef SHANNON_AVX_VECT_SUPPORTED
    const size_t simd_width = 256 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_and_si256(a, b));
    }
    for (; i < size; ++i) data[i] &= other.data[i];
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 128 / 8;
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
    assert(size == other.size);
#ifdef SHANNON_AVX_VECT_SUPPORTED
    const size_t simd_width = 256 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_or_si256(a, b));
    }
    for (; i < size; ++i) data[i] |= other.data[i];

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 128 / 8;
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
    assert(size == other.size);
#ifdef SHANNON_AVX_VECT_SUPPORTED
    const size_t simd_width = 256 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(other.data + i));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(data + i), _mm256_xor_si256(a, b));
    }
    for (; i < size; ++i) data[i] ^= other.data[i];

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 128 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t a = vld1q_u8(data + i);
      uint8x16_t b = vld1q_u8(other.data + i);
      vst1q_u8(data + i, veorq_u8(a, b));  // veorq_u8: bitwise XOR
    }
    for (; i < size; ++i) data[i] ^= other.data[i];

#else
    for (size_t i = 0; i < size; ++i) data[i] ^= other.data[i];
#endif
  }

  bool is_all_false() const {
    if (!data || !size) return true;
#ifdef SHANNON_AVX_VECT_SUPPORTED
    const size_t simd_width = 256 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      if (!_mm256_testz_si256(chunk, chunk)) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0) return false;
    }
    return true;

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 128 / 8;
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
#ifdef SHANNON_AVX_VECT_SUPPORTED
    const __m256i all_ones = _mm256_set1_epi8(static_cast<char>(0xFF));
    const size_t simd_width = 256 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      // _mm256_testz_si256(~chunk, ~chunk) == 0  iff all bits are 1.
      // Simpler: XOR with all_ones gives 0 only when chunk == all_ones.
      __m256i flipped = _mm256_xor_si256(chunk, all_ones);
      if (!_mm256_testz_si256(flipped, flipped)) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0xFF) return false;
    }
    return true;

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 128 / 8;
    size_t i = 0;
    for (; i + simd_width <= size; i += simd_width) {
      uint8x16_t chunk = vld1q_u8(data + i);
      // vminvq_u8: horizontal minimum across all lanes.
      // If all bytes are 0xFF the minimum is also 0xFF.
      if (vminvq_u8(chunk) != 0xFF) return false;
    }
    for (; i < size; ++i) {
      if (data[i] != 0xFF) return false;
    }
    return true;

#else
    for (size_t i = 0; i < size; ++i) {
      if (data[i] != 0xFF) return false;
    }
    return true;
#endif
  }

  size_t count_ones() const {
    if (!data) return 0;

    size_t count = 0;
    size_t full_bytes = (size + 7) / 8;
    size_t i = 0;

#if defined(SHANNON_AVX_VECT_SUPPORTED)
    const uint64_t *data64 = reinterpret_cast<const uint64_t *>(data);
    size_t num_qwords = full_bytes / 8;
    for (size_t q = 0; q < num_qwords; ++q) {
      count += __builtin_popcountll(data64[q]);
    }
    i = num_qwords * 8;
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
    const size_t simd_width = 16;
    for (; i + simd_width <= full_bytes; i += simd_width) {
      uint8x16_t v = vld1q_u8(data + i);
      uint8x16_t counts = vcntq_u8(v);
      count += vaddlvq_u8(counts);
    }
#endif

    for (; i < full_bytes; ++i) {
      count += __builtin_popcount(data[i]);
    }
    return count;
  }

  void swap(BitArray &other) noexcept {
    std::swap(data, other.data);
    std::swap(size, other.size);
  }

  // data of BA, where to store the real bitmap.
  uint8_t *data{nullptr};
  // size of BA.
  size_t size{0};
} BitArray_t;

using bit_array_t = BitArray_t;

using mysql_field_t = struct mysql_field_info {
  // whether field nullable or not.
  bool has_nullbit{false};
  // if is nullable, true is null, or none-null.
  bool is_null{false};

  // type in mysql type. DATA_MISSING = 0.
  uint mtype{0u};

  // mysql field lenght.
  size_t mlength{0};

  // physical length(innodb).
  size_t plength{0};
  // field data.
  std::unique_ptr<uchar[]> data{nullptr};
};

using key_info_t = std::pair<uint, std::unique_ptr<uchar[]>>;

}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_TYPES_H__