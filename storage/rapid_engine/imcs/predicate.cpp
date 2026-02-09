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

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/innobase/include/mach0data.ic"

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/utils/utils.h"  //bit_array_xxx

namespace ShannonBase {
namespace Imcs {
// NEON has no movemask equivalent. This helper extracts a 4-bit integer from
// a uint32x4_t mask (each lane is either 0x00000000 or 0xFFFFFFFF) so the
// result can be tested with a simple bit-and, mirroring the AVX2 movemask
// pattern used in the x86 paths.
//
//  shrn:   narrow each 32-bit lane to 8 bits by taking bits [15:8]
//          (the highest bit of each 0xFFFF word becomes the sign bit).
//  vget_lane_u32: extract the 4 condensed bytes as a single uint32.
//  Result bit layout:  bit0=lane0, bit8=lane1, bit16=lane2, bit24=lane3.
//  We normalise to a dense 4-bit mask via the table lookup below.
#ifdef SHANNON_ARM_VECT_SUPPORTED
static inline uint8_t neon_mask4_from_u32x4(uint32x4_t m) {
  // Narrow 32→16, then 16→8, keeping MSB of each original lane.
  uint16x4_t narrow16 = vmovn_u32(m);  // 4 x uint16, lane[i] = 0x0000 or 0xFFFF
  uint8x8_t narrow8 = vmovn_u16(vcombine_u16(narrow16, vdup_n_u16(0)));
  // narrow8[0..3] is 0x00 or 0xFF per original lane.
  // Build a 4-bit scalar mask.
  uint32_t raw = vget_lane_u32(vreinterpret_u32_u8(narrow8), 0);
  // Each byte is 0x00 or 0xFF → compress to single bits.
  return static_cast<uint8_t>(((raw & 0x000000FF) ? 1u : 0u) | ((raw & 0x0000FF00) ? 2u : 0u) |
                              ((raw & 0x00FF0000) ? 4u : 0u) | ((raw & 0xFF000000) ? 8u : 0u));
}

// Same for uint64x2_t (2 lanes).
static inline uint8_t neon_mask2_from_u64x2(uint64x2_t m) {
  uint32x2_t narrow = vmovn_u64(m);  // 2 x uint32, lane[i] = 0x00000000 or 0xFFFFFFFF
  uint32_t lo = vget_lane_u32(narrow, 0);
  uint32_t hi = vget_lane_u32(narrow, 1);
  return static_cast<uint8_t>((lo ? 1u : 0u) | (hi ? 2u : 0u));
}

// float64x2_t variant (AArch64 only).
#if defined(__aarch64__)
static inline uint8_t neon_mask2_from_f64x2(uint64x2_t m) { return neon_mask2_from_u64x2(m); }
#endif  // __aarch64__
#endif  // SHANNON_ARM_VECT_SUPPORTED

void Simple_Predicate::evaluate_vecotrized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                           bit_array_t &result) {
  if (col_data.empty()) return;
  auto vector_instr{false};
#if defined(SHANNON_SSE_VECT_SUPPORTED)
  vector_instr = true;
#endif
  if (vector_instr) {
    switch (column_type) {
      case MYSQL_TYPE_LONG:
        evaluate_int32_vectorized(col_data, num_rows, result);
        return;
      case MYSQL_TYPE_LONGLONG:
        evaluate_int64_vectorized(col_data, num_rows, result);
        return;
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT:
        evaluate_double_vectorized(col_data, num_rows, result);
        return;
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_DECIMAL:
        evaluate_decimal_vectorized(col_data, num_rows, result);
        return;
      default:
        // Unsupported type, fall back to scalar
        break;
    }
  }
  // Scalar fallback
  evaluate_batch(col_data, result, num_rows);
}

void Simple_Predicate::evaluate_int32_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                 bit_array_t &result) {
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  const int32_t target = static_cast<int32_t>(value.as_int());
  const size_t simd_width = 8;  // AVX2: 8 x int32
  __m256i target_vec = _mm256_set1_epi32(target);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    alignas(32) int32_t values[8];
    for (size_t j = 0; j < 8; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const int32_t *>(col_data[i + j]) : 0;
    }

    __m256i vdata = _mm256_load_si256(reinterpret_cast<const __m256i *>(values));
    __m256i mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm256_cmpeq_epi32(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm256_cmpgt_epi32(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm256_cmpgt_epi32(target_vec, vdata);
        break;
      case PredicateOperator::GREATER_EQUAL: {
        __m256i gt = _mm256_cmpgt_epi32(vdata, target_vec);
        __m256i eq = _mm256_cmpeq_epi32(vdata, target_vec);
        mask = _mm256_or_si256(gt, eq);
        break;
      }
      case PredicateOperator::LESS_EQUAL: {
        __m256i lt = _mm256_cmpgt_epi32(target_vec, vdata);
        __m256i eq = _mm256_cmpeq_epi32(vdata, target_vec);
        mask = _mm256_or_si256(lt, eq);
        break;
      }
      case PredicateOperator::NOT_EQUAL: {
        __m256i eq = _mm256_cmpeq_epi32(vdata, target_vec);
        mask = _mm256_xor_si256(eq, _mm256_set1_epi32(-1));
        break;
      }
      default:
        mask = _mm256_setzero_si256();
        break;
    }

    for (size_t j = 0; j < 8; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        reinterpret_cast<int32_t *>(&mask)[j] ? Utils::Util::bit_array_set(&result, i + j)
                                              : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
  // NEON processes 4 x int32 per 128-bit register.
  const int32_t target = static_cast<int32_t>(value.as_int());
  const size_t simd_width = 4;  // NEON: 4 x int32
  const int32x4_t target_vec = vdupq_n_s32(target);
  // All-ones constant for NOT_EQUAL inversion.
  const uint32x4_t all_ones = vdupq_n_u32(0xFFFFFFFFu);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    // Gather 4 values; replace NULL slots with 0 (NULL handled separately).
    alignas(16) int32_t values[4];
    for (size_t j = 0; j < 4; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const int32_t *>(col_data[i + j]) : 0;
    }

    int32x4_t vdata = vld1q_s32(values);
    uint32x4_t mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        // vceqq_s32: lane = 0xFFFFFFFF if equal, else 0x00000000
        mask = vceqq_s32(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        // vcgtq_s32: lane = 0xFFFFFFFF if vdata > target
        mask = vcgtq_s32(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = vcltq_s32(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = vcgeq_s32(vdata, target_vec);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = vcleq_s32(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        // Invert equality mask with XOR.
        mask = veorq_u32(vceqq_s32(vdata, target_vec), all_ones);
        break;
      default:
        mask = vdupq_n_u32(0);
        break;
    }

    // Extract 4-bit result and update bit_array.
    uint8_t bits = neon_mask4_from_u32x4(mask);
    for (size_t j = 0; j < 4; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#else
  evaluate_batch(col_data, result);
#endif
}

void Simple_Predicate::evaluate_int64_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                 bit_array_t &result) {
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  const int64_t target = value.as_int();
  const size_t simd_width = 4;  // AVX2: 4 x int64
  __m256i target_vec = _mm256_set1_epi64x(target);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    alignas(32) int64_t values[4];
    for (size_t j = 0; j < 4; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0;
    }

    __m256i vdata = _mm256_load_si256(reinterpret_cast<const __m256i *>(values));
    __m256i mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm256_cmpeq_epi64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm256_cmpgt_epi64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm256_cmpgt_epi64(target_vec, vdata);
        break;
      case PredicateOperator::GREATER_EQUAL: {
        __m256i gt = _mm256_cmpgt_epi64(vdata, target_vec);
        __m256i eq = _mm256_cmpeq_epi64(vdata, target_vec);
        mask = _mm256_or_si256(gt, eq);
        break;
      }
      case PredicateOperator::LESS_EQUAL: {
        __m256i lt = _mm256_cmpgt_epi64(target_vec, vdata);
        __m256i eq = _mm256_cmpeq_epi64(vdata, target_vec);
        mask = _mm256_or_si256(lt, eq);
        break;
      }
      case PredicateOperator::NOT_EQUAL: {
        __m256i eq = _mm256_cmpeq_epi64(vdata, target_vec);
        mask = _mm256_xor_si256(eq, _mm256_set1_epi64x(-1LL));
        break;
      }
      default:
        mask = _mm256_setzero_si256();
        break;
    }

    for (size_t j = 0; j < 4; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        reinterpret_cast<int64_t *>(&mask)[j] ? Utils::Util::bit_array_set(&result, i + j)
                                              : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#elif defined(SHANNON_ARM_VECT_SUPPORTED)
  //
  // NEON 128-bit register holds 2 x int64 (int64x2_t).
  //
  // AArch64 (64-bit ARM): full signed 64-bit comparison intrinsics exist:
  //   vceqq_s64, vcgtq_s64, vcgeq_s64, vcltq_s64, vcleq_s64
  //
  // AArch32 (32-bit ARM): only vceqq_s64 is available. GT/GE/LT/LE are not
  // natively supported and must be emulated via a sign-bit trick:
  //   a > b  ⟺  (a - b) has a negative sign when no overflow, but overflow
  //               detection is complex for 64-bit. It's safer to fall back to
  //               scalar for those operators on AArch32.
  //
  const int64_t target = value.as_int();
  const size_t simd_width = 2;  // NEON: 2 x int64
  const int64x2_t target_vec = vdupq_n_s64(target);
  const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

  size_t i = 0;

#if defined(__aarch64__)
  // AArch64: full signed compare intrinsics available
  for (; i + simd_width <= num_rows; i += simd_width) {
    alignas(16) int64_t values[2];
    for (size_t j = 0; j < 2; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;
    }

    int64x2_t vdata = vld1q_s64(values);
    uint64x2_t mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        // vceqq_s64: lane = 0xFFFFFFFFFFFFFFFF if equal
        mask = vceqq_s64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        // vcgtq_s64: AArch64 only
        mask = vcgtq_s64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = vcltq_s64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = vcgeq_s64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = vcleq_s64(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = veorq_u64(vceqq_s64(vdata, target_vec), all_ones64);
        break;
      default:
        mask = vdupq_n_u64(0);
        break;
    }

    uint8_t bits = neon_mask2_from_u64x2(mask);
    for (size_t j = 0; j < 2; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

#else   // AArch32: only EQUAL and NOT_EQUAL have direct NEON support
  // For EQUAL / NOT_EQUAL we use NEON; for ordering operators we fall through
  // to the scalar tail immediately (i stays at 0).
  if (op == PredicateOperator::EQUAL || op == PredicateOperator::NOT_EQUAL) {
    for (; i + simd_width <= num_rows; i += simd_width) {
      alignas(16) int64_t values[2];
      for (size_t j = 0; j < 2; ++j) {
        values[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;
      }

      int64x2_t vdata = vld1q_s64(values);
      uint64x2_t eq_mask = vceqq_s64(vdata, target_vec);  // always available
      uint64x2_t mask = (op == PredicateOperator::NOT_EQUAL) ? veorq_u64(eq_mask, all_ones64) : eq_mask;

      uint8_t bits = neon_mask2_from_u64x2(mask);
      for (size_t j = 0; j < 2; ++j) {
        if (!col_data[i + j]) {
          (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                             : Utils::Util::bit_array_reset(&result, i + j);
        } else {
          (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j)
                             : Utils::Util::bit_array_reset(&result, i + j);
        }
      }
    }
  }
  // GT / GE / LT / LE on AArch32: fall through to scalar tail below.
#endif  // __aarch64__
  // Scalar tail (remaining rows after SIMD, or all rows for unsupported ops
  // on AArch32).
  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#else
  evaluate_batch(col_data, result);
#endif
}

void Simple_Predicate::evaluate_double_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                  bit_array_t &result) {
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  // x86 AVX2 path
  const double target = value.as_double();
  const size_t simd_width = 4;  // AVX2: 4 x double
  __m256d target_vec = _mm256_set1_pd(target);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    alignas(32) double values[4];
    for (size_t j = 0; j < 4; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const double *>(col_data[i + j]) : 0.0;
    }

    __m256d vdata = _mm256_load_pd(values);
    __m256d mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_EQ_OQ);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_GT_OQ);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_LT_OQ);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_GE_OQ);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_LE_OQ);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_NEQ_OQ);
        break;
      default:
        mask = _mm256_setzero_pd();
        break;
    }

    // _mm256_movemask_pd returns a 4-bit mask, one bit per lane
    int result_bits = _mm256_movemask_pd(mask);
    for (size_t j = 0; j < 4; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (result_bits & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j)
                                 : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#elif defined(SHANNON_ARM_VECT_SUPPORTED) && defined(__aarch64__)
  // float64x2_t and vcgtq_f64 / vcltq_f64 etc. are AArch64-only.
  // 32-bit ARM NEON has no double SIMD; those builds fall to scalar below.
  const double target = value.as_double();
  const size_t simd_width = 2;  // NEON AArch64: 2 x double
  const float64x2_t target_vec = vdupq_n_f64(target);
  const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    alignas(16) double values[2];
    for (size_t j = 0; j < 2; ++j) {
      values[j] = col_data[i + j] ? *reinterpret_cast<const double *>(col_data[i + j]) : 0.0;
    }

    float64x2_t vdata = vld1q_f64(values);
    uint64x2_t mask;

    switch (op) {
      case PredicateOperator::EQUAL:
        mask = vceqq_f64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = vcgtq_f64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = vcltq_f64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = vcgeq_f64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = vcleq_f64(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        // NOT(EQ): NaN → EQ=false → NOT=true, consistent with IEEE unordered.
        mask = veorq_u64(vceqq_f64(vdata, target_vec), all_ones64);
        break;
      default:
        mask = vdupq_n_u64(0);
        break;
    }

    uint8_t bits = neon_mask2_from_u64x2(mask);
    for (size_t j = 0; j < 2; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#else
  // AArch32 or no SIMD support: scalar fallback.
  evaluate_batch(col_data, result);
#endif
}

/**
 * DECIMAL is stored in MySQL's compact binary format (decimal_t / my_decimal).
 * No SIMD instruction can compare this format natively, so we decode each value
 * to double first via get_field_numeric<double>(), then reuse the AVX2 / NEON
 * double comparison path.
 *
 * Precision note: double has 53-bit mantissa (~15–16 significant decimal
 * digits).  MySQL DECIMAL supports up to 65 digits, so for very high-precision
 * values there may be rounding.  This is acceptable for the IMCS analytics
 * use-case.  Exact arithmetic would require scalar fallback for those columns.
 */
void Simple_Predicate::evaluate_decimal_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                   bit_array_t &result) {
  // Operators expressible as a single SIMD floating-point comparison.
  auto is_simd_comparable = [](PredicateOperator o) -> bool {
    switch (o) {
      case PredicateOperator::EQUAL:
      case PredicateOperator::NOT_EQUAL:
      case PredicateOperator::LESS_THAN:
      case PredicateOperator::LESS_EQUAL:
      case PredicateOperator::GREATER_THAN:
      case PredicateOperator::GREATER_EQUAL:
        return true;
      default:
        return false;  // BETWEEN, IN, LIKE, IS_NULL, etc. → scalar
    }
  };

  if (!is_simd_comparable(op)) {
    evaluate_batch(col_data, result, num_rows);
    return;
  }

  const double target [[maybe_unused]] = value.as_double();
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  // x86 AVX2: 4 x double per iteration
  const size_t simd_width = 4;
  const __m256d target_vec = _mm256_set1_pd(target);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    // Step 1: decode DECIMAL → double (4 lanes).
    alignas(32) double decoded[4];
    for (size_t j = 0; j < simd_width; ++j) {
      decoded[j] = col_data[i + j]
                       ? Utils::Util::get_field_numeric<double>(field_meta, col_data[i + j], nullptr, low_order)
                       : std::numeric_limits<double>::quiet_NaN();  // NULL → NaN, handled below
    }

    // Step 2: AVX2 double comparison.
    __m256d vdata = _mm256_load_pd(decoded);
    __m256d mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_EQ_OQ);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_NEQ_OQ);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_LT_OQ);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_LE_OQ);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_GT_OQ);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = _mm256_cmp_pd(vdata, target_vec, _CMP_GE_OQ);
        break;
      default:
        mask = _mm256_setzero_pd();
        break;
    }

    // Step 3: extract results and handle NULLs.
    int cmp_bits = _mm256_movemask_pd(mask);
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (cmp_bits & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j)
                              : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#elif defined(SHANNON_ARM_VECT_SUPPORTED) && defined(__aarch64__)
  // ARM NEON AArch64: 2 x double per iteration
  // float64x2_t intrinsics require AArch64. 32-bit ARM falls through to scalar.
  const size_t simd_width = 2;
  const float64x2_t target_vec = vdupq_n_f64(target);
  const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    // Step 1: decode DECIMAL → double (2 lanes).
    alignas(16) double decoded[2];
    for (size_t j = 0; j < simd_width; ++j) {
      decoded[j] = col_data[i + j]
                       ? Utils::Util::get_field_numeric<double>(field_meta, col_data[i + j], nullptr, low_order)
                       : std::numeric_limits<double>::quiet_NaN();
    }

    // Step 2: NEON double comparison.
    float64x2_t vdata = vld1q_f64(decoded);
    uint64x2_t mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = vceqq_f64(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = veorq_u64(vceqq_f64(vdata, target_vec), all_ones64);
        break;
      case PredicateOperator::LESS_THAN:
        mask = vcltq_f64(vdata, target_vec);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = vcleq_f64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = vcgtq_f64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = vcgeq_f64(vdata, target_vec);
        break;
      default:
        mask = vdupq_n_u64(0);
        break;
    }

    // Step 3: extract results and handle NULLs.
    uint8_t bits = neon_mask2_from_u64x2(mask);
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }

#else
  // AArch32 or no SIMD: pure scalar.
  evaluate_batch(col_data, result, num_rows);
#endif
}

void Simple_Predicate::evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                                      size_t batch_num) const {
  size_t i = 0;
  auto values_sz = input_values.size();
  // Process with loop unrolling
  for (; i + batch_num <= values_sz; i += batch_num) {
    for (size_t j = 0; j < batch_num; j++) {
      auto input_value = input_values[i + j];
      bool match = evaluate(input_value);
      (match) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
    }
  }

  // Process remainder
  for (; i < values_sz; i++) {
    auto input_value = input_values[i];
    bool match = evaluate(input_value);
    (match) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
}

bool Simple_Predicate::evaluate(const uchar *&input_value) const {
  // Handle NULL
  if (!input_value) {
    switch (op) {
      case PredicateOperator::IS_NULL:
        return true;
      case PredicateOperator::IS_NOT_NULL:
        return false;
      default:
        return false;  // NULL compared with any value is false
    }
  }

  // Extract column value
  PredicateValue col_value = extract_value(input_value, low_order);
  // Evaluate based on operator
  switch (op) {
    case PredicateOperator::EQUAL:
      return col_value == value;
    case PredicateOperator::NOT_EQUAL:
      return col_value != value;
    case PredicateOperator::LESS_THAN:
      return col_value < value;
    case PredicateOperator::LESS_EQUAL:
      return col_value <= value;
    case PredicateOperator::GREATER_THAN:
      return col_value > value;
    case PredicateOperator::GREATER_EQUAL:
      return col_value >= value;
    case PredicateOperator::BETWEEN:
      return col_value >= value && col_value <= value2;
    case PredicateOperator::NOT_BETWEEN:
      return col_value < value || col_value > value2;
    case PredicateOperator::IN:
      return std::find(value_list.begin(), value_list.end(), col_value) != value_list.end();
    case PredicateOperator::NOT_IN:
      return std::find(value_list.begin(), value_list.end(), col_value) == value_list.end();
    case PredicateOperator::IS_NULL:
      return false;  // NULL already handled
    case PredicateOperator::IS_NOT_NULL:
      return true;
    case PredicateOperator::LIKE:
      return evaluate_like(col_value.as_string(), value.as_string());
    case PredicateOperator::NOT_LIKE:
      return !evaluate_like(col_value.as_string(), value.as_string());
    case PredicateOperator::REGEXP:
      return evaluate_regexp(col_value.as_string(), value.as_string());
    case PredicateOperator::NOT_REGEXP:
      return !evaluate_regexp(col_value.as_string(), value.as_string());
    default:
      return false;
  }
}

std::string Simple_Predicate::to_string() const {
  std::ostringstream oss;
  oss << "col_" << column_id << " ";

  switch (op) {
    case PredicateOperator::EQUAL:
      oss << "= ";
      break;
    case PredicateOperator::NOT_EQUAL:
      oss << "!= ";
      break;
    case PredicateOperator::LESS_THAN:
      oss << "< ";
      break;
    case PredicateOperator::LESS_EQUAL:
      oss << "<= ";
      break;
    case PredicateOperator::GREATER_THAN:
      oss << "> ";
      break;
    case PredicateOperator::GREATER_EQUAL:
      oss << ">= ";
      break;
    case PredicateOperator::BETWEEN:
      oss << "BETWEEN " << value.as_string() << " AND " << value2.as_string();
      return oss.str();
    case PredicateOperator::IN:
      oss << "IN (...)";
      return oss.str();
    case PredicateOperator::IS_NULL:
      oss << "IS NULL";
      return oss.str();
    case PredicateOperator::IS_NOT_NULL:
      oss << "IS NOT NULL";
      return oss.str();
    case PredicateOperator::LIKE:
      oss << "LIKE ";
      break;
    default:
      oss << "? ";
      break;
  }

  oss << value.as_string();
  return oss.str();
}

double Simple_Predicate::estimate_selectivity(const StorageIndex *storage_index) const {
  if (!storage_index) {
    // No statistics, use empirical defaults
    switch (op) {
      case PredicateOperator::EQUAL:
        return 0.1;  // 10%
      case PredicateOperator::NOT_EQUAL:
        return 0.9;  // 90%
      case PredicateOperator::LESS_THAN:
      case PredicateOperator::LESS_EQUAL:
      case PredicateOperator::GREATER_THAN:
      case PredicateOperator::GREATER_EQUAL:
        return 0.33;  // 33%
      case PredicateOperator::BETWEEN:
        return 0.25;  // 25%
      case PredicateOperator::IN:
        return std::min(0.5, 0.1 * value_list.size());
      case PredicateOperator::NOT_IN:
        return std::max(0.5, 1.0 - 0.1 * value_list.size());
      case PredicateOperator::IS_NULL:
        return 0.05;  // 5%
      case PredicateOperator::IS_NOT_NULL:
        return 0.95;  // 95%
      case PredicateOperator::LIKE:
      case PredicateOperator::REGEXP:
        return 0.2;  // 20%
      default:
        return 0.5;  // 50%
    }
  }

  // Use storage index statistics
  const double min_val = storage_index->get_min_value(column_id);
  const double max_val = storage_index->get_max_value(column_id);
  const size_t null_count = storage_index->get_null_count(column_id);
  const bool has_null = storage_index->get_has_null(column_id);

  // Get column stats for distinct count
  const auto *stats = storage_index->get_column_stats_snapshot(column_id);
  const size_t distinct_count = stats ? stats->distinct_count.load(std::memory_order_acquire) : 0;

  // Estimate total rows (this should ideally be passed in or stored)
  const size_t estimated_total_rows = std::max(distinct_count * 10, size_t(1000));
  const double non_null_rows =
      estimated_total_rows > null_count ? estimated_total_rows - null_count : estimated_total_rows;

  double pred_value = 0.0;
  double range = max_val - min_val;

  switch (op) {
    case PredicateOperator::EQUAL: {
      pred_value = value.as_double();

      // Value out of range
      if (pred_value < min_val || pred_value > max_val) return 0.0;
      // Use distinct count if available
      return (distinct_count > 0) ? 1.0 / distinct_count : 0.1;  // Default
    } break;
    case PredicateOperator::NOT_EQUAL: {
      pred_value = value.as_double();

      // Value out of range - all rows match
      if (pred_value < min_val || pred_value > max_val) return 1.0;
      return (distinct_count > 0) ? (distinct_count - 1.0) / distinct_count : 0.9;
    } break;
    case PredicateOperator::LESS_THAN: {
      pred_value = value.as_double();

      if (pred_value <= min_val) return 0.0;
      if (pred_value >= max_val) return 1.0;

      return (range > 0) ? (pred_value - min_val) / range : 0.5;
    } break;
    case PredicateOperator::LESS_EQUAL: {
      pred_value = value.as_double();

      if (pred_value < min_val) return 0.0;
      if (pred_value >= max_val) return 1.0;

      if (range > 0) {
        double sel = (pred_value - min_val) / range;
        // Add small adjustment for equality
        if (distinct_count > 0) {
          sel += 0.5 / distinct_count;
        }
        return std::min(1.0, sel);
      }
      return 1.0;  // Single value
    } break;
    case PredicateOperator::GREATER_THAN: {
      pred_value = value.as_double();

      if (pred_value >= max_val) return 0.0;
      if (pred_value <= min_val) return 1.0;
      return (range > 0) ? (max_val - pred_value) / range : 0.5;
    } break;
    case PredicateOperator::GREATER_EQUAL: {
      pred_value = value.as_double();
      if (pred_value > max_val) return 0.0;
      if (pred_value <= min_val) return 1.0;

      if (range > 0) {
        double sel = (max_val - pred_value) / range;
        if (distinct_count > 0) {
          sel += 0.5 / distinct_count;
        }
        return std::min(1.0, sel);
      }
      return 1.0;
    } break;
    case PredicateOperator::BETWEEN: {
      double low = value.as_double();
      double high = value2.as_double();

      // No overlap
      if (high < min_val || low > max_val) return 0.0;
      // Full coverage
      if (low <= min_val && high >= max_val) return 1.0;

      // Partial overlap
      if (range > 0) {
        double overlap_min = std::max(low, min_val);
        double overlap_max = std::min(high, max_val);
        double overlap_range = overlap_max - overlap_min;
        return overlap_range / range;
      }
      return 0.5;
    } break;
    case PredicateOperator::NOT_BETWEEN: {
      // Complement of BETWEEN
      double between_sel = 0.0;

      double low = value.as_double();
      double high = value2.as_double();

      if (high < min_val || low > max_val) {
        between_sel = 0.0;
      } else if (low <= min_val && high >= max_val) {
        between_sel = 1.0;
      } else if (range > 0) {
        double overlap_min = std::max(low, min_val);
        double overlap_max = std::min(high, max_val);
        double overlap_range = overlap_max - overlap_min;
        between_sel = overlap_range / range;
      } else {
        between_sel = 0.5;
      }
      return 1.0 - between_sel;
    } break;
    case PredicateOperator::IN: {
      if (value_list.empty()) return 0.0;

      return (distinct_count > 0)
                 ? std::min(1.0, static_cast<double>(value_list.size()) /
                                     distinct_count)        // Estimate: min(1.0, num_values / distinct_count)
                 : std::min(1.0, 0.1 * value_list.size());  // Heuristic: 10% per value, capped at 1.0;
    } break;
    case PredicateOperator::NOT_IN: {
      if (value_list.empty()) return 1.0;

      if (distinct_count > 0) {
        double in_sel = std::min(1.0, static_cast<double>(value_list.size()) / distinct_count);
        return 1.0 - in_sel;
      }
      return std::max(0.0, 1.0 - 0.1 * value_list.size());
    } break;
    case PredicateOperator::IS_NULL: {
      if (!has_null) return 0.0;
      return (estimated_total_rows > 0) ? static_cast<double>(null_count) / estimated_total_rows : 0.05;
    }
    case PredicateOperator::IS_NOT_NULL: {
      if (!has_null) return 1.0;
      return (estimated_total_rows > 0) ? non_null_rows / estimated_total_rows : 0.95;
    } break;
    case PredicateOperator::LIKE:
    case PredicateOperator::NOT_LIKE: {
      // Pattern analysis could be done here
      // For now, use rough estimate
      double like_sel = 0.2;  // 20% for LIKE
      return (op == PredicateOperator::LIKE) ? like_sel : (1.0 - like_sel);
    } break;
    case PredicateOperator::REGEXP:
    case PredicateOperator::NOT_REGEXP: {
      double regexp_sel = 0.15;  // 15% for REGEXP
      return (op == PredicateOperator::REGEXP) ? regexp_sel : (1.0 - regexp_sel);
    } break;
    default:
      return 0.5;
  }
}

PredicateValue Simple_Predicate::extract_value(const uchar *data, bool low_order) const {
  assert(field_meta->field_index() == column_id && field_meta->type() == column_type);
  switch (column_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      auto val = Utils::Util::get_field_numeric<int64_t>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DECIMAL: {
      auto val = Utils::Util::get_field_numeric<double>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_STRING: {  // padding with ` ` in store formate, so trim the extra space
      uint32 pred_length = value.as_string().length();
      auto length = std::min(pred_length, field_meta->pack_length());
      std::string val(reinterpret_cast<const char *>(data), length);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      std::string val(reinterpret_cast<const char *>(data));
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      auto val = Utils::Util::get_field_numeric<int64_t>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      // TODO: ref: row0row.cpp: RowBuffer::extract_field_data
      assert(false);
    } break;
    default:
      return PredicateValue::null_value();
  }
  return PredicateValue::null_value();
}

bool Simple_Predicate::evaluate_like(const std::string &str, const std::string &pattern) const {
  // Simplified implementation: convert LIKE pattern to regex
  std::string regex_pattern = pattern;

  // Escape special characters
  std::string escaped;
  for (char c : regex_pattern) {
    switch (c) {
      case '%':
        escaped += ".*";
        break;
      case '_':
        escaped += ".";
        break;
      case '.':
      case '*':
      case '+':
      case '?':
      case '|':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '^':
      case '$':
      case '\\':
        escaped += '\\';
        escaped += c;
        break;
      default:
        escaped += c;
    }
  }

  try {
    std::regex re(escaped, std::regex::icase);
    return std::regex_match(str, re);
  } catch (const std::regex_error &) {
    return false;
  }
}

bool Simple_Predicate::evaluate_regexp(const std::string &str, const std::string &pattern) const {
  try {
    std::regex re(pattern);
    return std::regex_search(str, re);
  } catch (const std::regex_error &) {
    return false;
  }
}

bool Compound_Predicate::evaluate(const uchar *&input_value) const {
  switch (op) {
    case PredicateOperator::AND: {
      for (const auto &child : children) {
        if (!child->evaluate(input_value)) return false;  // Short-circuit evaluation
      }
      return true;
    } break;
    case PredicateOperator::OR: {
      for (const auto &child : children) {
        if (child->evaluate(input_value)) return true;  // Short-circuit evaluation
      }
      return false;
    } break;
    case PredicateOperator::NOT: {
      if (children.empty()) return false;
      return !children[0]->evaluate(input_value);
    } break;
    default:
      return false;
  }
}

void Compound_Predicate::evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                                        size_t batch_num) const {
  if (children.empty()) return;

  auto input_sz = input_values.size();
  switch (op) {
    case PredicateOperator::AND: {
      // Initialize to all 1s
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_set(&result, i);
      }
      // Evaluate each child predicate and AND with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(input_values, child_result, batch_num);
        // result = result AND child_result
        for (size_t i = 0; i < input_sz; i++) {
          if (!Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_reset(&result, i);
          }
        }
      }
    } break;
    case PredicateOperator::OR: {
      // Initialize to all 0s
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_reset(&result, i);
      }
      // Evaluate each child predicate and OR with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(input_values, child_result, batch_num);
        // result = result OR child_result
        for (size_t i = 0; i < input_sz; i++) {
          if (Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_set(&result, i);
          }
        }
      }
    } break;
    case PredicateOperator::NOT: {
      if (children.empty()) return;
      // Evaluate child predicate
      children[0]->evaluate_batch(input_values, result, batch_num);
      // Invert
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_get(&result, i) ? Utils::Util::bit_array_reset(&result, i)
                                               : Utils::Util::bit_array_set(&result, i);
      }
    } break;
    default:
      break;
  }
}

std::vector<uint32> Compound_Predicate::get_columns() const {
  std::vector<uint32> columns;
  for (const auto &child : children) {
    auto child_cols = child->get_columns();
    columns.insert(columns.end(), child_cols.begin(), child_cols.end());
  }

  // Remove duplicates
  std::sort(columns.begin(), columns.end());
  columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
  return columns;
}

std::unique_ptr<Predicate> Compound_Predicate::clone() const {
  auto cloned = std::make_unique<Compound_Predicate>(op);
  for (const auto &child : children) {
    cloned->add_child(child->clone());
  }
  return cloned;
}

std::string Compound_Predicate::to_string() const {
  if (children.empty()) return "()";

  std::ostringstream oss;
  switch (op) {
    case PredicateOperator::AND:
      oss << "(";
      for (size_t i = 0; i < children.size(); i++) {
        if (i > 0) oss << " AND ";
        oss << children[i]->to_string();
      }
      oss << ")";
      break;
    case PredicateOperator::OR:
      oss << "(";
      for (size_t i = 0; i < children.size(); i++) {
        if (i > 0) oss << " OR ";
        oss << children[i]->to_string();
      }
      oss << ")";
      break;
    case PredicateOperator::NOT:
      oss << "NOT (" << children[0]->to_string() << ")";
      break;
    default:
      oss << "(?)";
      break;
  }

  return oss.str();
}

double Compound_Predicate::estimate_selectivity(const StorageIndex *storage_index) const {
  if (children.empty()) return 1.0;

  switch (op) {
    case PredicateOperator::AND: {
      // AND: Multiply selectivities
      double selectivity = 1.0;
      for (const auto &child : children) {
        selectivity *= child->estimate_selectivity(storage_index);
      }
      return selectivity;
    } break;
    case PredicateOperator::OR: {
      // OR: 1 - (1-s1) * (1-s2) * ...
      double prob_none = 1.0;
      for (const auto &child : children) {
        double s = child->estimate_selectivity(storage_index);
        prob_none *= (1.0 - s);
      }
      return 1.0 - prob_none;
    } break;
    case PredicateOperator::NOT: {
      double s = children[0]->estimate_selectivity(storage_index);
      return 1.0 - s;
    } break;
    default:
      return 0.5;
  }
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_simple(uint32 col_id, PredicateOperator op,
                                                                   const PredicateValue &value, enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, op, value, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_between(uint32 col_id, const PredicateValue &min_val,
                                                                    const PredicateValue &max_val,
                                                                    enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, min_val, max_val, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_in(uint32 col_id, const std::vector<PredicateValue> &values,
                                                               bool is_not_in, enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, values, is_not_in, type);
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_and(std::vector<std::unique_ptr<Predicate>> predicates) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::AND);
  for (auto &pred : predicates) {
    compound->add_child(std::move(pred));
  }
  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_or(std::vector<std::unique_ptr<Predicate>> predicates) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::OR);
  for (auto &pred : predicates) {
    compound->add_child(std::move(pred));
  }
  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_not(std::unique_ptr<Predicate> predicate) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::NOT);
  compound->add_child(std::move(predicate));
  return compound;
}
}  // namespace Imcs
}  // namespace ShannonBase