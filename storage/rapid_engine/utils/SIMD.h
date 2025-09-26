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
#include <functional>
#include <type_traits>
#include <vector>

namespace ShannonBase {
namespace Utils {
namespace SIMD {
// ============================================================================
// SIMD Capability Detection
// ============================================================================
enum class SIMDType {
  NONE = 0,
  SSE,     // Basic SSE support
  SSE2,    // SSE2 support
  SSE4_1,  // SSE4.1 support
  SSE4_2,  // SSE4.2 support
  AVX,     // AVX support
  AVX2,    // AVX2 support
  NEON     // ARM NEON support
};

SIMDType detect_simd_support();

// ============================================================================
// Bitmap Operations
// ============================================================================

size_t popcount_bitmap_common(const uint8_t *data, size_t bytes);
size_t popcount_bitmap_avx2(const uint8_t *data, size_t bytes);
size_t popcount_bitmap_neon(const uint8_t *data, size_t bytes);
size_t popcount_bitmap(const std::vector<uint8_t> &bm);

// ============================================================================
// Arithmetic Operations
// ============================================================================

// Sum Oper
template <typename T>
T sum_avx2(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T sum_sse4(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T sum_neon(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T sum_generic(const T *data, const uint8_t *null_mask, size_t row_count);

// Min Oper
template <typename T>
T min_avx2(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T min_sse4(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T min_neon(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T min_generic(const T *data, const uint8_t *null_mask, size_t row_count);

// Max Oper
template <typename T>
T max_avx2(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T max_sse4(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T max_neon(const T *data, const uint8_t *null_mask, size_t row_count);
template <typename T>
T max_generic(const T *data, const uint8_t *null_mask, size_t row_count);

// ============================================================================
// Counting Operations
// ============================================================================

size_t count_non_null_avx2(const uint8_t *null_mask, size_t row_count);
size_t count_non_null_sse4(const uint8_t *null_mask, size_t row_count);
size_t count_non_null_neon(const uint8_t *null_mask, size_t row_count);
size_t count_non_null_generic(const uint8_t *null_mask, size_t row_count);

// ============================================================================
// Filtering Operations
// ============================================================================

template <typename T>
size_t filter_avx2(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                   std::vector<size_t> &output_indices);
template <typename T>
size_t filter_sse4(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                   std::vector<size_t> &output_indices);
template <typename T>
size_t filter_neon(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                   std::vector<size_t> &output_indices);
template <typename T>
size_t filter_generic(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                      std::vector<size_t> &output_indices);

// ============================================================================
// High-level Interface
// ============================================================================

// To choose the corresponding instrcutions to execute.
template <typename T>
T sum(const T *data, const uint8_t *null_mask, size_t row_count);

template <typename T>
T min(const T *data, const uint8_t *null_mask, size_t row_count);

template <typename T>
T max(const T *data, const uint8_t *null_mask, size_t row_count);

size_t count_non_null(const uint8_t *null_mask, size_t row_count);

template <typename T>
size_t filter(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
              std::vector<size_t> &output_indices);

}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase

#include "storage/rapid_engine/utils/SIMD_Impl.h"
#endif  //__SHANNONBASE_UTILS_SIMD_H__