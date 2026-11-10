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

#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"

// The platform/ISA macros come from rapid_arch_inf.h, so every translation unit
// that includes this header sees the same kernel set.
#if defined(SHANNON_X86_PLATFORM)
#include <immintrin.h>  // SSE2..AVX2 intrinsics
#elif defined(SHANNON_ARM_PLATFORM)
#include <arm_neon.h>  // NEON intrinsics
#endif

#include <algorithm>
#include <bit>  // for std::countr_zero/popcount
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
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

enum class CompareOp : uint8_t { EQ = 0, NE, LT, LE, GT, GE };

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

// Sum Oper. The per-ISA entry points are non-template overloads in
// SIMD_Impl.h (one per element type); only the scalar reference stays a
// template, because it is instantiated for every supported T.
template <typename T>
T sum_generic(const T *data, const uint8_t *null_mask, size_t row_count);

// Min Oper
template <typename T>
T min_generic(const T *data, const uint8_t *null_mask, size_t row_count);

// Max Oper
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

template <typename T, typename Allocator>
size_t filter_generic(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                      std::vector<size_t, Allocator> &output_indices);

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

template <typename Allocator>
size_t filter_compare_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                            CompareOp op, std::vector<size_t, Allocator> &output_indices);
template <typename Allocator>
size_t filter_compare_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count, int64_t value,
                            CompareOp op, std::vector<size_t, Allocator> &output_indices);

// Whether the compare kernels for this width actually run vectorized on the
// current CPU. Every tier with a kernel covers all six operators.
bool integer_compare_simd(bool is_int32, CompareOp op);

// ============================================================================
// Gathered integer filters (Rapid column batches)
// ============================================================================
// `cells[i]` points at row i's packed value, or is nullptr when the row is
// NULL. The result is written as a LSB-first bitmap: bit i set means row i
// matched, and a NULL row never matches. Bits of the trailing byte beyond
// `rows` are left untouched. These entry points let the IMCS predicate and the
// executor filter share one set of per-ISA kernels.
size_t filter_compare_int32_bitmap(const unsigned char *const *cells, size_t rows, int32_t value, CompareOp op,
                                   bool is_unsigned, uint8_t *out_bits);
size_t filter_compare_int64_bitmap(const unsigned char *const *cells, size_t rows, int64_t value, CompareOp op,
                                   bool is_unsigned, uint8_t *out_bits);
// lower <= value <= upper, or its negation when `negate` is set (NOT BETWEEN).
size_t filter_range_int32_bitmap(const unsigned char *const *cells, size_t rows, int32_t lower, int32_t upper,
                                 bool is_unsigned, bool negate, uint8_t *out_bits);
size_t filter_range_int64_bitmap(const unsigned char *const *cells, size_t rows, int64_t lower, int64_t upper,
                                 bool is_unsigned, bool negate, uint8_t *out_bits);
// IS NULL, or IS NOT NULL when `negate` is set.
size_t filter_null_bitmap(const unsigned char *const *cells, size_t rows, bool negate, uint8_t *out_bits);

// Double-valued variants. `decode` turns one cell into a double (FLOAT, DOUBLE
// and DECIMAL each need their own reader), so the compare kernel stays shared.
template <typename Decode>
size_t filter_compare_double_bitmap(const unsigned char *const *cells, size_t rows, double value, CompareOp op,
                                    Decode decode, uint8_t *out_bits);
template <typename Decode>
size_t filter_range_double_bitmap(const unsigned char *const *cells, size_t rows, double lower, double upper,
                                  bool negate, Decode decode, uint8_t *out_bits);

}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase

#include "storage/rapid_engine/utils/SIMD_Impl.h"
#endif  //__SHANNONBASE_UTILS_SIMD_H__
