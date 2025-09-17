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
#ifndef __SHANNONBASE_UTILS_SIMD_H__
#define __SHANNONBASE_UTILS_SIMD_H__

#include "storage/rapid_engine/include/rapid_const.h"

#if defined(SHANNON_X86_PLATFORM)
#include <immintrin.h>  // for AVX2 path
#elif defined(SHANNON_ARM_PLATFORM)
#include <arm_neon.h>  // for NEON path
#endif

#include <algorithm>
#include <bit>  // for std::countr_zero/popcount
#include <cstdint>
#include <cstring>
#include <vector>

namespace ShannonBase {
namespace Utils {
namespace SIMD {
// ============================================================================
// SIMD Capability Detection
// ============================================================================

inline size_t popcount_bitmap_common(const uint8_t *data, size_t bytes) {
  size_t sum = 0;
  // process 8 bytes -> uint64_t for faster popcount on 64-bit
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, sizeof(v));
    sum += __builtin_popcountll(v);
  }
  for (; i < bytes; ++i) sum += std::popcount((unsigned)data[i]);
  return sum;
}

inline size_t popcount_bitmap_avx2(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__) && defined(__SSE4_1__)
  static const uint8_t lut_arr[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
  __m128i lut128 = _mm_loadu_si128((const __m128i *)lut_arr);

  size_t i = 0;
  const size_t block = 32;
  __m256i total = _mm256_setzero_si256();

  for (; i + block <= bytes; i += block) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(data + i));
  }

  uint64_t lanes[4];
  _mm256_storeu_si256((__m256i *)lanes, total);
  size_t sum = (size_t)lanes[0] + (size_t)lanes[1] + (size_t)lanes[2] + (size_t)lanes[3];

  for (; i < bytes; ++i) sum += std::popcount((unsigned)data[i]);
  return sum;
#elif defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  size_t i = 0;
  const size_t block = 16;
  uint32x4_t total = vdupq_n_u32(0);

  for (; i + block <= bytes; i += block) {
    uint8x16_t v = vld1q_u8(data + i);

    uint8x16_t v0 = vandq_u8(v, vdupq_n_u8(0x55));  // 0x55 = 01010101
    uint8x16_t v1 = vandq_u8(v, vdupq_n_u8(0xAA));  // 0xAA = 10101010
    v1 = vshrq_n_u8(v1, 1);

    uint8x16_t sum8 = vaddq_u8(v0, v1);

    uint16x8_t sum16 = vpaddlq_u8(sum8);
    uint32x4_t sum32 = vpaddlq_u16(sum16);

    total = vaddq_u32(total, sum32);
  }

  uint32_t sum =
      vgetq_lane_u32(total, 0) + vgetq_lane_u32(total, 1) + vgetq_lane_u32(total, 2) + vgetq_lane_u32(total, 3);

  for (; i < bytes; ++i) {
    sum += std::popcount((unsigned)data[i]);
  }
  return sum;
#else
  return popcount_bitmap_common(data, bytes);
#endif
}

inline size_t popcount_bitmap_neon(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  size_t sum = 0;
  size_t i = 0;

  for (; i + 16 <= bytes; i += 16) {
    uint8x16_t v = vld1q_u8(data + i);
    sum += vaddvq_u8(vcntq_u8(v));
  }

  for (; i < bytes; ++i) {
    sum += std::popcount((unsigned)data[i]);
  }
  return sum;
#else
  return popcount_bitmap_common(data, bytes);
#endif
}

inline size_t popcount_bitmap(const std::vector<uint8_t> &bm) {
  if (bm.empty()) return 0;

#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  return popcount_bitmap_avx2(bm.data(), bm.size());
#elif defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  return popcount_bitmap_neon(bm.data(), bm.size());
#else
  return popcount_bitmap_common(bm.data(), bm.size());
#endif
}

}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_H__