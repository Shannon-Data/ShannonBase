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
#include <algorithm>
#include <cstring>

#include "storage/rapid_engine/imcs/predicate.h"

#include "storage/rapid_engine/imcs/storage0index.h"
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

void Simple_Predicate::init_regex_if_needed() const {
  if (op == PredicateOperator::REGEXP || op == PredicateOperator::NOT_REGEXP) {
    get_regex();  // Force compilation
  } else if (op == PredicateOperator::LIKE || op == PredicateOperator::NOT_LIKE) {
    get_like_regex();                    // Force compilation
    analyze_pattern(value.as_string());  // Analyze for fast path
  }
}

const std::regex &Simple_Predicate::get_regex() const {
  std::call_once(m_regex_flag, [this]() {
    const std::string &pattern = value.as_string();
    // Use global cache for better sharing across predicates
    m_regex = std::make_unique<std::regex>(RegexCache::instance().get(pattern));
  });
  return *m_regex;
}

const std::regex &Simple_Predicate::get_like_regex() const {
  std::call_once(m_like_flag, [this]() {
    const std::string &pattern = value.as_string();
    std::string regex_pattern = pattern_to_regex(pattern);
    m_like_regex = std::make_unique<std::regex>(RegexCache::instance().get(regex_pattern, std::regex::icase));
  });
  return *m_like_regex;
}

PatternType Simple_Predicate::analyze_pattern(const std::string &pattern) const {
  std::call_once(m_pattern_flag, [this, &pattern]() {
    const std::string &pat = pattern;
    // Check for exact match (no wildcards)
    if (pat.find('%') == std::string::npos && pat.find('_') == std::string::npos) {
      m_pattern_type = PatternType::EXACT;
      return;
    }
    if (pat.back() == '%') {  // Check for prefix pattern (starts with X, ends with %)
      size_t first_wildcard = pat.find('%');
      if (first_wildcard == pat.size() - 1) {
        size_t underscore = pat.find('_');
        if (underscore == std::string::npos) {
          m_pattern_type = PatternType::PREFIX;
          return;
        }
      }
    }
    if (pat.front() == '%') {  // Check for suffix pattern (starts with %, ends with X)
      size_t last_wildcard = pat.rfind('%');
      if (last_wildcard == 0) {
        size_t underscore = pat.find('_');
        if (underscore == std::string::npos && pat.size() > 1) {
          m_pattern_type = PatternType::SUFFIX;
          return;
        }
      }
    }
    if (pat.front() == '%' && pat.back() == '%') {  // Check for contains pattern (%X%)
      size_t first_wildcard = pat.find('%', 1);
      if (first_wildcard == pat.size() - 1) {
        size_t underscore = pat.find('_');
        if (underscore == std::string::npos && pat.size() > 2) {
          m_pattern_type = PatternType::CONTAINS;
          return;
        }
      }
    }

    m_pattern_type = PatternType::COMPLEX;
  });

  return m_pattern_type;
}

std::string Simple_Predicate::pattern_to_regex(const std::string &pattern) const {
  std::string escaped;
  escaped.reserve(pattern.size() * 2);

  for (char c : pattern) {
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

  return escaped;
}

bool Simple_Predicate::evaluate_like_fast(const std::string &str, const std::string &pattern, PatternType type) const {
  switch (type) {
    case PatternType::EXACT:
      return str == pattern;
    case PatternType::PREFIX: {
      // pattern ends with '%'
      std::string prefix = pattern.substr(0, pattern.size() - 1);
      return str.compare(0, prefix.size(), prefix) == 0;
    }
    case PatternType::SUFFIX: {
      // pattern starts with '%'
      std::string suffix = pattern.substr(1);
      if (str.size() < suffix.size()) return false;
      return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    case PatternType::CONTAINS: {
      // pattern is '%X%'
      std::string substr = pattern.substr(1, pattern.size() - 2);
      return str.find(substr) != std::string::npos;
    }
    default:
      return false;  // Should not reach here
  }
}

bool Simple_Predicate::evaluate_like(const std::string &str, const std::string &pattern) const {
  // Fast path for simple patterns
  PatternType type = analyze_pattern(pattern);
  if (type != PatternType::COMPLEX) return evaluate_like_fast(str, pattern, type);

  // Complex pattern - use cached regex
  const std::regex &re = get_like_regex();
  return std::regex_match(str, re);
}

bool Simple_Predicate::evaluate_regexp(const std::string &str, const std::string &pattern) const {
  // Use cached regex
  const std::regex &re = get_regex();
  return std::regex_search(str, re);
}

void Simple_Predicate::evaluate_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                           bit_array_t &result) {
  if (col_data.empty()) return;
  auto simd_eligible = [](PredicateOperator o) -> bool {
    switch (o) {
      case PredicateOperator::EQUAL:
      case PredicateOperator::NOT_EQUAL:
      case PredicateOperator::LESS_THAN:
      case PredicateOperator::LESS_EQUAL:
      case PredicateOperator::GREATER_THAN:
      case PredicateOperator::GREATER_EQUAL:
      case PredicateOperator::BETWEEN:
      case PredicateOperator::NOT_BETWEEN:
      case PredicateOperator::IS_NULL:
      case PredicateOperator::IS_NOT_NULL:
        return true;
      default:
        return false;
    }
  };
#if defined(SHANNON_VECTORIZE_SUPPORT)
  if (simd_eligible(op)) {
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
#endif  // Scalar fallback: IN / NOT_IN / LIKE / REGEXP / unsupported types
  evaluate_batch(col_data, result, num_rows);
}

void Simple_Predicate::evaluate_int32_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                 bit_array_t &result) {
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  const int32_t target = static_cast<int32_t>(value.as_int());
  const int32_t target2 = static_cast<int32_t>(value2.as_int());  // for tow operands oper.
  constexpr size_t simd_width = 8;                                // AVX2: 8 x int32
  __m256i target_vec = _mm256_set1_epi32(target);
  __m256i target_vec2 = _mm256_set1_epi32(target2);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    int32_t v0 = col_data[i + 0] ? *reinterpret_cast<const int32_t *>(col_data[i + 0]) : 0;
    int32_t v1 = col_data[i + 1] ? *reinterpret_cast<const int32_t *>(col_data[i + 1]) : 0;
    int32_t v2 = col_data[i + 2] ? *reinterpret_cast<const int32_t *>(col_data[i + 2]) : 0;
    int32_t v3 = col_data[i + 3] ? *reinterpret_cast<const int32_t *>(col_data[i + 3]) : 0;
    int32_t v4 = col_data[i + 4] ? *reinterpret_cast<const int32_t *>(col_data[i + 4]) : 0;
    int32_t v5 = col_data[i + 5] ? *reinterpret_cast<const int32_t *>(col_data[i + 5]) : 0;
    int32_t v6 = col_data[i + 6] ? *reinterpret_cast<const int32_t *>(col_data[i + 6]) : 0;
    int32_t v7 = col_data[i + 7] ? *reinterpret_cast<const int32_t *>(col_data[i + 7]) : 0;

    __m256i vdata = _mm256_setr_epi32(v0, v1, v2, v3, v4, v5, v6, v7);
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
      } break;
      case PredicateOperator::LESS_EQUAL: {
        __m256i lt = _mm256_cmpgt_epi32(target_vec, vdata);
        __m256i eq = _mm256_cmpeq_epi32(vdata, target_vec);
        mask = _mm256_or_si256(lt, eq);
      } break;
      case PredicateOperator::NOT_EQUAL: {
        __m256i eq = _mm256_cmpeq_epi32(vdata, target_vec);
        mask = _mm256_xor_si256(eq, _mm256_set1_epi32(-1));
      } break;
      case PredicateOperator::BETWEEN: {
        // value <= data <= value2
        // ge_lo: data >= value  ↔  NOT (data < value)  ↔  NOT (target_vec > data)
        __m256i lt_lo = _mm256_cmpgt_epi32(target_vec, vdata);  // data < value
        __m256i ge_lo = _mm256_xor_si256(lt_lo, _mm256_set1_epi32(-1));
        // le_hi: data <= value2 ↔  NOT (data > value2)
        __m256i gt_hi = _mm256_cmpgt_epi32(vdata, target_vec2);  // data > value2
        __m256i le_hi = _mm256_xor_si256(gt_hi, _mm256_set1_epi32(-1));
        mask = _mm256_and_si256(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        __m256i lt_lo = _mm256_cmpgt_epi32(target_vec, vdata);
        __m256i gt_hi = _mm256_cmpgt_epi32(vdata, target_vec2);
        mask = _mm256_or_si256(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm256_setzero_si256();
        break;
    }

    uint8_t lane_mask = static_cast<uint8_t>(_mm256_movemask_ps(_mm256_castsi256_ps(mask)));
    uint8_t null_mask = 0;
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) null_mask |= static_cast<uint8_t>(1u << j);
    }

    for (size_t j = 0; j < simd_width; ++j) {
      bool set;
      if (op == PredicateOperator::IS_NULL)
        set = (null_mask >> j) & 1u;
      else if (op == PredicateOperator::IS_NOT_NULL)
        set = !((null_mask >> j) & 1u);
      else
        set = ((lane_mask >> j) & 1u) && !((null_mask >> j) & 1u);

      set ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
    }
  }

  for (; i < num_rows; ++i) {  // Scalar remainder
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
  const int32_t target = static_cast<int32_t>(value.as_int());
  const int32_t target2 = static_cast<int32_t>(value2.as_int());
  constexpr size_t simd_width = 4;  // SSE: 4 x int32
  const __m128i target_vec = _mm_set1_epi32(target);
  const __m128i target_vec2 = _mm_set1_epi32(target2);
  const __m128i all_ones = _mm_set1_epi32(-1);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int32_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int32_t *>(col_data[i + j]) : 0;

    __m128i vdata = _mm_load_si128(reinterpret_cast<const __m128i *>(vals));
    __m128i mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm_cmpeq_epi32(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm_cmpgt_epi32(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm_cmpgt_epi32(target_vec, vdata);
        break;
      case PredicateOperator::GREATER_EQUAL: {
        __m128i gt = _mm_cmpgt_epi32(vdata, target_vec);
        __m128i eq = _mm_cmpeq_epi32(vdata, target_vec);
        mask = _mm_or_si128(gt, eq);
      } break;
      case PredicateOperator::LESS_EQUAL: {
        __m128i lt = _mm_cmpgt_epi32(target_vec, vdata);
        __m128i eq = _mm_cmpeq_epi32(vdata, target_vec);
        mask = _mm_or_si128(lt, eq);
      } break;
      case PredicateOperator::NOT_EQUAL: {
        __m128i eq = _mm_cmpeq_epi32(vdata, target_vec);
        mask = _mm_xor_si128(eq, all_ones);
      } break;
      case PredicateOperator::BETWEEN: {
        // ge_lo: data >= value
        __m128i lt_lo = _mm_cmpgt_epi32(target_vec, vdata);
        __m128i ge_lo = _mm_xor_si128(lt_lo, all_ones);
        // le_hi: data <= value2
        __m128i gt_hi = _mm_cmpgt_epi32(vdata, target_vec2);
        __m128i le_hi = _mm_xor_si128(gt_hi, all_ones);
        mask = _mm_and_si128(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        __m128i lt_lo = _mm_cmpgt_epi32(target_vec, vdata);
        __m128i gt_hi = _mm_cmpgt_epi32(vdata, target_vec2);
        mask = _mm_or_si128(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm_setzero_si128();
        break;
    }

    uint8_t lane_mask = static_cast<uint8_t>(_mm_movemask_ps(_mm_castsi128_ps(mask)));
    uint8_t null_mask = 0;
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) null_mask |= static_cast<uint8_t>(1u << j);
    }

    for (size_t j = 0; j < simd_width; ++j) {
      bool set{false};
      if (op == PredicateOperator::IS_NULL)
        set = (null_mask >> j) & 1u;
      else if (op == PredicateOperator::IS_NOT_NULL)
        set = !((null_mask >> j) & 1u);
      else
        set = ((lane_mask >> j) & 1u) && !((null_mask >> j) & 1u);

      set ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
  const int32_t target = static_cast<int32_t>(value.as_int());
  const int32_t target2 = static_cast<int32_t>(value2.as_int());
  constexpr size_t simd_width = 4;  // NEON: 4 x int32
  const int32x4_t target_vec = vdupq_n_s32(target);
  const int32x4_t target_vec2 = vdupq_n_s32(target2);
  const uint32x4_t all_ones = vdupq_n_u32(0xFFFFFFFFu);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int32_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int32_t *>(col_data[i + j]) : 0;

    int32x4_t vdata = vld1q_s32(vals);
    uint32x4_t mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = vceqq_s32(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
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
        mask = veorq_u32(vceqq_s32(vdata, target_vec), all_ones);
        break;
      case PredicateOperator::BETWEEN: {
        // value <= data <= value2
        uint32x4_t ge_lo = vcgeq_s32(vdata, target_vec);
        uint32x4_t le_hi = vcleq_s32(vdata, target_vec2);
        mask = vandq_u32(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        uint32x4_t lt_lo = vcltq_s32(vdata, target_vec);
        uint32x4_t gt_hi = vcgtq_s32(vdata, target_vec2);
        mask = vorrq_u32(lt_lo, gt_hi);
      } break;
      default:
        mask = vdupq_n_u32(0);
        break;
    }

    uint8_t bits = neon_mask4_from_u32x4(mask);
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
  evaluate_batch(col_data, result, num_rows);
#endif
}

void Simple_Predicate::evaluate_int64_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                 bit_array_t &result) {
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  const int64_t target = value.as_int();
  const int64_t target2 = value2.as_int();
  constexpr size_t simd_width = 4;  // AVX2 path: 4 x int64
  __m256i target_vec = _mm256_set1_epi64x(target);
  __m256i target_vec2 = _mm256_set1_epi64x(target2);
  const __m256i all_ones64 = _mm256_set1_epi64x(-1LL);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int64_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;

    __m256i vdata = _mm256_load_si256(reinterpret_cast<const __m256i *>(vals));
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
      } break;
      case PredicateOperator::LESS_EQUAL: {
        __m256i lt = _mm256_cmpgt_epi64(target_vec, vdata);
        __m256i eq = _mm256_cmpeq_epi64(vdata, target_vec);
        mask = _mm256_or_si256(lt, eq);
      } break;
      case PredicateOperator::NOT_EQUAL: {
        __m256i eq = _mm256_cmpeq_epi64(vdata, target_vec);
        mask = _mm256_xor_si256(eq, all_ones64);
      } break;
      case PredicateOperator::BETWEEN: {
        // ge_lo: data >= value  ↔  NOT (value > data)
        __m256i lt_lo = _mm256_cmpgt_epi64(target_vec, vdata);
        __m256i ge_lo = _mm256_xor_si256(lt_lo, all_ones64);
        // le_hi: data <= value2 ↔  NOT (data > value2)
        __m256i gt_hi = _mm256_cmpgt_epi64(vdata, target_vec2);
        __m256i le_hi = _mm256_xor_si256(gt_hi, all_ones64);
        mask = _mm256_and_si256(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        __m256i lt_lo = _mm256_cmpgt_epi64(target_vec, vdata);
        __m256i gt_hi = _mm256_cmpgt_epi64(vdata, target_vec2);
        mask = _mm256_or_si256(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm256_setzero_si256();
        break;
    }

    int mm = _mm256_movemask_pd(_mm256_castsi256_pd(mask));
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
  const int64_t target = value.as_int();
  const int64_t target2 = value2.as_int();
  constexpr size_t simd_width = 2;  // SSE: 2 x int64
  const __m128i target_vec = _mm_set1_epi64x(target);
  const __m128i target_vec2 = _mm_set1_epi64x(target2);
  const __m128i all_ones = _mm_set1_epi32(-1);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int64_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;

    __m128i vdata = _mm_load_si128(reinterpret_cast<const __m128i *>(vals));
    __m128i mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm_cmpeq_epi64(vdata, target_vec);  // SSE4.1
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm_cmpgt_epi64(vdata, target_vec);  // SSE4.2
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm_cmpgt_epi64(target_vec, vdata);  // SSE4.2，exchange operand
        break;
      case PredicateOperator::GREATER_EQUAL: {
        __m128i gt = _mm_cmpgt_epi64(vdata, target_vec);
        __m128i eq = _mm_cmpeq_epi64(vdata, target_vec);
        mask = _mm_or_si128(gt, eq);
      } break;
      case PredicateOperator::LESS_EQUAL: {
        __m128i lt = _mm_cmpgt_epi64(target_vec, vdata);
        __m128i eq = _mm_cmpeq_epi64(vdata, target_vec);
        mask = _mm_or_si128(lt, eq);
      } break;
      case PredicateOperator::NOT_EQUAL: {
        __m128i eq = _mm_cmpeq_epi64(vdata, target_vec);
        mask = _mm_xor_si128(eq, all_ones);
      } break;
      case PredicateOperator::BETWEEN: {
        // ge_lo: data >= value  ↔  NOT (value > data)
        __m128i lt_lo = _mm_cmpgt_epi64(target_vec, vdata);
        __m128i ge_lo = _mm_xor_si128(lt_lo, all_ones);
        // le_hi: data <= value2 ↔  NOT (data > value2)
        __m128i gt_hi = _mm_cmpgt_epi64(vdata, target_vec2);
        __m128i le_hi = _mm_xor_si128(gt_hi, all_ones);
        mask = _mm_and_si128(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        __m128i lt_lo = _mm_cmpgt_epi64(target_vec, vdata);
        __m128i gt_hi = _mm_cmpgt_epi64(vdata, target_vec2);
        mask = _mm_or_si128(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm_setzero_si128();
        break;
    }

    // _mm_movemask_pd: 2-bit lane mask：bit 0 = lane 0 MSB，bit 1 = lane 1 MSB
    int mm = _mm_movemask_pd(_mm_castsi128_pd(mask));
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_ARM_VECT_SUPPORTED)
  const int64_t target = value.as_int();
  const int64_t target2 = value2.as_int();
  constexpr size_t simd_width = 2;  // NEON 2 x int64
  const int64x2_t target_vec = vdupq_n_s64(target);
  const int64x2_t target_vec2 = vdupq_n_s64(target2);
  const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

  size_t i = 0;
#if defined(__aarch64__)
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int64_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;

    int64x2_t vdata = vld1q_s64(vals);
    uint64x2_t mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = vceqq_s64(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
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
      case PredicateOperator::BETWEEN: {
        // value <= data <= value2
        uint64x2_t ge_lo = vcgeq_s64(vdata, target_vec);
        uint64x2_t le_hi = vcleq_s64(vdata, target_vec2);
        mask = vandq_u64(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        uint64x2_t lt_lo = vcltq_s64(vdata, target_vec);
        uint64x2_t gt_hi = vcgtq_s64(vdata, target_vec2);
        mask = vorrq_u64(lt_lo, gt_hi);
      } break;
      default:
        mask = vdupq_n_u64(0);
        break;
    }

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
#else
  // AArch32: vceqq_s64 / vcgtq_s64 / vcgeq_s64 are available;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS int64_t vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j)
      vals[j] = col_data[i + j] ? *reinterpret_cast<const int64_t *>(col_data[i + j]) : 0LL;

    int64x2_t vdata = vld1q_s64(vals);
    uint64x2_t mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = vceqq_s64(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = veorq_u64(vceqq_s64(vdata, target_vec), all_ones64);
        break;
      case PredicateOperator::BETWEEN: {
        uint64x2_t ge_lo = vcgeq_s64(vdata, target_vec);
        uint64x2_t le_hi = vcleq_s64(vdata, target_vec2);
        mask = vandq_u64(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        uint64x2_t lt_lo = vcltq_s64(vdata, target_vec);
        uint64x2_t gt_hi = vcgtq_s64(vdata, target_vec2);
        mask = vorrq_u64(lt_lo, gt_hi);
      } break;
      default:
        // AArch32 has no vcgt/vcge for 64-bit lanes; fall through to scalar.
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
#endif  // __aarch64__
  for (; i < num_rows; ++i) {  // remainder fallback
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#else
  evaluate_batch(col_data, result);
#endif
}

void Simple_Predicate::evaluate_double_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                  bit_array_t &result) {
  auto load_fp = [&](const uchar *p) -> double {
    if (!p) return std::numeric_limits<double>::quiet_NaN();
    if (column_type == MYSQL_TYPE_FLOAT) {
      float f;
      memcpy(&f, p, 4);
      return static_cast<double>(f);
    }
    double d;
    memcpy(&d, p, 8);
    return d;
  };
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  const double target = value.as_double();
  const double target2 = value2.as_double();  // only BETWEEN / NOT_BETWEEN
  constexpr size_t simd_width = 4;            // AVX2 path: 4 x double
  __m256d target_vec = _mm256_set1_pd(target);
  __m256d target_vec2 = _mm256_set1_pd(target2);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS double vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j) vals[j] = load_fp(col_data[i + j]);

    __m256d vdata = _mm256_load_pd(vals);
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
      case PredicateOperator::BETWEEN: {
        // value <= data <= value2  (ordered: NaN → false on both sides)
        __m256d ge_lo = _mm256_cmp_pd(vdata, target_vec, _CMP_GE_OQ);
        __m256d le_hi = _mm256_cmp_pd(vdata, target_vec2, _CMP_LE_OQ);
        mask = _mm256_and_pd(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        __m256d lt_lo = _mm256_cmp_pd(vdata, target_vec, _CMP_LT_OQ);
        __m256d gt_hi = _mm256_cmp_pd(vdata, target_vec2, _CMP_GT_OQ);
        mask = _mm256_or_pd(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm256_setzero_pd();
        break;
    }

    int mm = _mm256_movemask_pd(mask);
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
  const double target = value.as_double();
  const double target2 = value2.as_double();
  constexpr size_t simd_width = 2;  // SSE: 2 x double
  const __m128d target_vec = _mm_set1_pd(target);
  const __m128d target_vec2 = _mm_set1_pd(target2);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS double vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j) vals[j] = load_fp(col_data[i + j]);

    __m128d vdata = _mm_load_pd(vals);
    __m128d mask;
    switch (op) {
      case PredicateOperator::EQUAL:
        mask = _mm_cmpeq_pd(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_THAN:
        mask = _mm_cmpgt_pd(vdata, target_vec);
        break;
      case PredicateOperator::LESS_THAN:
        mask = _mm_cmplt_pd(vdata, target_vec);
        break;
      case PredicateOperator::GREATER_EQUAL:
        mask = _mm_cmpge_pd(vdata, target_vec);
        break;
      case PredicateOperator::LESS_EQUAL:
        mask = _mm_cmple_pd(vdata, target_vec);
        break;
      case PredicateOperator::NOT_EQUAL:
        mask = _mm_cmpneq_pd(vdata, target_vec);
        break;
      case PredicateOperator::BETWEEN: {
        __m128d ge_lo = _mm_cmpge_pd(vdata, target_vec);
        __m128d le_hi = _mm_cmple_pd(vdata, target_vec2);
        mask = _mm_and_pd(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        __m128d lt_lo = _mm_cmplt_pd(vdata, target_vec);
        __m128d gt_hi = _mm_cmpgt_pd(vdata, target_vec2);
        mask = _mm_or_pd(lt_lo, gt_hi);
      } break;
      default:
        mask = _mm_setzero_pd();
        break;
    }

    // _mm_movemask_pd：bit 0 = lane 0 MSB，bit 1 = lane 1 MSB
    int mm = _mm_movemask_pd(mask);
    for (size_t j = 0; j < simd_width; ++j) {
      if (!col_data[i + j]) {
        (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                           : Utils::Util::bit_array_reset(&result, i + j);
      } else {
        (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  for (; i < num_rows; ++i) {
    const uchar *v = col_data[i];
    evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
#elif defined(SHANNON_ARM_VECT_SUPPORTED) && defined(__aarch64__)
  // float64x2_t is AArch64-only; AArch32 falls through to scalar below.
  const double target = value.as_double();
  const double target2 = value2.as_double();
  constexpr size_t simd_width = 2;  // float64x2_t：AArch64-only
  const float64x2_t target_vec = vdupq_n_f64(target);
  const float64x2_t target_vec2 = vdupq_n_f64(target2);
  const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

  size_t i = 0;
  for (; i + simd_width <= num_rows; i += simd_width) {
    SHANNON_SIMD_ALIGNAS double vals[simd_width];
    for (size_t j = 0; j < simd_width; ++j) vals[j] = load_fp(col_data[i + j]);

    float64x2_t vdata = vld1q_f64(vals);
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
        mask = veorq_u64(vceqq_f64(vdata, target_vec), all_ones64);
        break;
      case PredicateOperator::BETWEEN: {
        // value <= data <= value2
        uint64x2_t ge_lo = vcgeq_f64(vdata, target_vec);
        uint64x2_t le_hi = vcleq_f64(vdata, target_vec2);
        mask = vandq_u64(ge_lo, le_hi);
      } break;
      case PredicateOperator::NOT_BETWEEN: {
        // data < value  OR  data > value2
        uint64x2_t lt_lo = vcltq_f64(vdata, target_vec);
        uint64x2_t gt_hi = vcgtq_f64(vdata, target_vec2);
        mask = vorrq_u64(lt_lo, gt_hi);
      } break;
      default:
        mask = vdupq_n_u64(0);
        break;
    }

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
  evaluate_batch(col_data, result, num_rows);
#endif
}

/**
 * DECIMAL is stored in MySQL's compact binary format (decimal_t / my_decimal).
 * No SIMD instruction can compare this format natively, so we decode each value
 * to double first via get_field_numeric<double>(), then reuse the AVX2 / NEON
 * double comparison path.
 *
 * Precision note: double has 53-bit mantissa (~15–16 significant decimal
 * digits). MySQL DECIMAL supports up to 65 digits, so for very high-precision
 * values there may be rounding. This is acceptable for the IMCS analytics
 * use-case, since IMCS currently evaluates DECIMAL predicates using double.
 * Exact arithmetic would require scalar fallback or a dedicated decimal type.
 */
void Simple_Predicate::evaluate_decimal_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows,
                                                   bit_array_t &result) {
  auto is_simd_comparable =
      [](PredicateOperator o) -> bool {  // Operators expressible via SIMD floating-point comparison(s).
    switch (o) {
      case PredicateOperator::EQUAL:
      case PredicateOperator::NOT_EQUAL:
      case PredicateOperator::LESS_THAN:
      case PredicateOperator::LESS_EQUAL:
      case PredicateOperator::GREATER_THAN:
      case PredicateOperator::GREATER_EQUAL:
      case PredicateOperator::BETWEEN:      // two-compare path below
      case PredicateOperator::NOT_BETWEEN:  // two-compare path below
        return true;
      default:
        return false;
    }
  };

  if (!is_simd_comparable(op)) {
    evaluate_batch(col_data, result, num_rows);
    return;
  }

  const double target [[maybe_unused]] = value.as_double();
  const double target2 [[maybe_unused]] = value2.as_double();
#if defined(SHANNON_AVX_VECT_SUPPORTED)
  {
    constexpr size_t simd_width = 4;  // AVX2 path: 4 x double
    const __m256d target_vec = _mm256_set1_pd(target);
    const __m256d target_vec2 = _mm256_set1_pd(target2);

    size_t i = 0;
    for (; i + simd_width <= num_rows; i += simd_width) {
      SHANNON_SIMD_ALIGNAS double decoded[simd_width];
      for (size_t j = 0; j < simd_width; ++j)
        decoded[j] = col_data[i + j]
                         ? Utils::Util::get_field_numeric<double>(field_meta, col_data[i + j], nullptr, low_order)
                         : std::numeric_limits<double>::quiet_NaN();

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
        case PredicateOperator::BETWEEN: {
          __m256d ge_lo = _mm256_cmp_pd(vdata, target_vec, _CMP_GE_OQ);
          __m256d le_hi = _mm256_cmp_pd(vdata, target_vec2, _CMP_LE_OQ);
          mask = _mm256_and_pd(ge_lo, le_hi);
        } break;
        case PredicateOperator::NOT_BETWEEN: {
          __m256d lt_lo = _mm256_cmp_pd(vdata, target_vec, _CMP_LT_OQ);
          __m256d gt_hi = _mm256_cmp_pd(vdata, target_vec2, _CMP_GT_OQ);
          mask = _mm256_or_pd(lt_lo, gt_hi);
        } break;
        default:
          mask = _mm256_setzero_pd();
          break;
      }

      int mm = _mm256_movemask_pd(mask);
      for (size_t j = 0; j < simd_width; ++j) {
        if (!col_data[i + j]) {
          (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                             : Utils::Util::bit_array_reset(&result, i + j);
        } else {
          (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
        }
      }
    }

    for (; i < num_rows; ++i) {
      const uchar *v = col_data[i];
      evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
    }
  }
#elif defined(SHANNON_SSE_VECT_SUPPORTED)
  {
    constexpr size_t simd_width = 2;  // SSE4.2 path: 2 x double
    const __m128d target_vec = _mm_set1_pd(target);
    const __m128d target_vec2 = _mm_set1_pd(target2);

    size_t i = 0;
    for (; i + simd_width <= num_rows; i += simd_width) {
      SHANNON_SIMD_ALIGNAS double decoded[simd_width];
      for (size_t j = 0; j < simd_width; ++j)
        decoded[j] = col_data[i + j]
                         ? Utils::Util::get_field_numeric<double>(field_meta, col_data[i + j], nullptr, low_order)
                         : std::numeric_limits<double>::quiet_NaN();

      __m128d vdata = _mm_load_pd(decoded);
      __m128d mask;
      switch (op) {
        case PredicateOperator::EQUAL:
          mask = _mm_cmpeq_pd(vdata, target_vec);
          break;
        case PredicateOperator::NOT_EQUAL:
          mask = _mm_cmpneq_pd(vdata, target_vec);
          break;
        case PredicateOperator::LESS_THAN:
          mask = _mm_cmplt_pd(vdata, target_vec);
          break;
        case PredicateOperator::LESS_EQUAL:
          mask = _mm_cmple_pd(vdata, target_vec);
          break;
        case PredicateOperator::GREATER_THAN:
          mask = _mm_cmpgt_pd(vdata, target_vec);
          break;
        case PredicateOperator::GREATER_EQUAL:
          mask = _mm_cmpge_pd(vdata, target_vec);
          break;
        case PredicateOperator::BETWEEN: {
          __m128d ge_lo = _mm_cmpge_pd(vdata, target_vec);
          __m128d le_hi = _mm_cmple_pd(vdata, target_vec2);
          mask = _mm_and_pd(ge_lo, le_hi);
        } break;
        case PredicateOperator::NOT_BETWEEN: {
          __m128d lt_lo = _mm_cmplt_pd(vdata, target_vec);
          __m128d gt_hi = _mm_cmpgt_pd(vdata, target_vec2);
          mask = _mm_or_pd(lt_lo, gt_hi);
        } break;
        default:
          mask = _mm_setzero_pd();
          break;
      }

      int mm = _mm_movemask_pd(mask);
      for (size_t j = 0; j < simd_width; ++j) {
        if (!col_data[i + j]) {
          (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                             : Utils::Util::bit_array_reset(&result, i + j);
        } else {
          (mm & (1 << j)) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
        }
      }
    }

    for (; i < num_rows; ++i) {
      const uchar *v = col_data[i];
      evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
    }
  }
#elif defined(SHANNON_ARM_VECT_SUPPORTED) && defined(__aarch64__)
  {
    constexpr size_t simd_width = 2;
    const float64x2_t target_vec = vdupq_n_f64(target);
    const float64x2_t target_vec2 = vdupq_n_f64(target2);
    const uint64x2_t all_ones64 = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);

    size_t i = 0;
    for (; i + simd_width <= num_rows; i += simd_width) {
      SHANNON_SIMD_ALIGNAS double decoded[simd_width];
      for (size_t j = 0; j < simd_width; ++j)
        decoded[j] = col_data[i + j]
                         ? Utils::Util::get_field_numeric<double>(field_meta, col_data[i + j], nullptr, low_order)
                         : std::numeric_limits<double>::quiet_NaN();

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
        case PredicateOperator::BETWEEN: {
          uint64x2_t ge_lo = vcgeq_f64(vdata, target_vec);
          uint64x2_t le_hi = vcleq_f64(vdata, target_vec2);
          mask = vandq_u64(ge_lo, le_hi);
        } break;
        case PredicateOperator::NOT_BETWEEN: {
          uint64x2_t lt_lo = vcltq_f64(vdata, target_vec);
          uint64x2_t gt_hi = vcgtq_f64(vdata, target_vec2);
          mask = vorrq_u64(lt_lo, gt_hi);
        } break;
        default:
          mask = vdupq_n_u64(0);
          break;
      }

      uint8_t bits = neon_mask2_from_u64x2(mask);
      for (size_t j = 0; j < simd_width; ++j) {
        if (!col_data[i + j]) {
          (op == PredicateOperator::IS_NULL) ? Utils::Util::bit_array_set(&result, i + j)
                                             : Utils::Util::bit_array_reset(&result, i + j);
        } else {
          (bits & (1u << j)) ? Utils::Util::bit_array_set(&result, i + j)
                             : Utils::Util::bit_array_reset(&result, i + j);
        }
      }
    }

    for (; i < num_rows; ++i) {
      const uchar *v = col_data[i];
      evaluate(v) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
    }
  }
#else
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
  if (value.is_null()) {
    switch (op) {
      case PredicateOperator::IS_NULL:
      case PredicateOperator::IS_NOT_NULL:
        break;  // these are handled normally below
      default:
        return false;  // col = NULL, col != NULL, col < NULL, etc. → always false
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
    case MYSQL_TYPE_DOUBLE: {
      auto val = Utils::Util::get_field_numeric<double>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
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
      for (size_t i = 0; i < input_sz; i++) Utils::Util::bit_array_set(&result, i);

      // Evaluate each child predicate and AND with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result before each child evaluation.
        for (size_t i = 0; i < input_sz; i++) Utils::Util::bit_array_reset(&child_result, i);

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
      for (size_t i = 0; i < input_sz; i++) Utils::Util::bit_array_reset(&result, i);

      // Evaluate each child predicate and OR with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result before each child evaluation.
        for (size_t i = 0; i < input_sz; i++) Utils::Util::bit_array_reset(&child_result, i);

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
  for (auto &pred : predicates) compound->add_child(std::move(pred));

  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_or(std::vector<std::unique_ptr<Predicate>> predicates) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::OR);
  for (auto &pred : predicates) compound->add_child(std::move(pred));

  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_not(std::unique_ptr<Predicate> predicate) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::NOT);
  compound->add_child(std::move(predicate));
  return compound;
}
}  // namespace Imcs
}  // namespace ShannonBase