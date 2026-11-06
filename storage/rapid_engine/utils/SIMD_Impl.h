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
#ifndef __SHANNONBASE_UTILS_SIMD_IMPL_H__
#define __SHANNONBASE_UTILS_SIMD_IMPL_H__

namespace ShannonBase {
namespace Utils {
namespace SIMD {
// SHANNON_TARGET_AVX2 and SHANNON_RUNTIME_AVX2 come from rapid_arch_inf.h; the
// fallback keeps a direct include of this file compilable.
#ifndef SHANNON_TARGET_AVX2
#define SHANNON_TARGET_AVX2
#endif
#if defined(SHANNON_X86_PLATFORM) && defined(_MSC_VER) && !defined(SHANNON_RUNTIME_AVX2)
// MSVC has the AVX2 intrinsics but no runtime probe here (__builtin_cpu_supports
// is GCC/Clang only), so the kernels could never be selected. Fail loudly until
// detect_simd_support() gains a __cpuidex branch.
static_assert(false,
              "ShannonBase: add a __cpuidex probe to SIMD::detect_simd_support() before building the Rapid SIMD "
              "kernels with MSVC");
#endif

// ============================================================================
// SIMD Detection Implementation
// ============================================================================

inline SIMDType detect_simd_support() {
#if defined(SHANNON_X86_PLATFORM) && (defined(__GNUC__) || defined(__clang__))
  // Dispatch only to an ISA the build compiled and this CPU implements. The
  // SSE4.x/SSE tiers are not reported separately: every dispatcher below maps
  // them to the same SSE2 kernel.
  __builtin_cpu_init();
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  if (__builtin_cpu_supports("avx2")) return SIMDType::AVX2;
#endif
#if defined(SHANNON_HAVE_AVX)
  if (__builtin_cpu_supports("avx")) return SIMDType::AVX;
#endif
#if defined(SHANNON_HAVE_SSE2)
  if (__builtin_cpu_supports("sse2")) return SIMDType::SSE2;
#endif
#elif defined(SHANNON_HAVE_NEON)
  return SIMDType::NEON;
#endif
  return SIMDType::NONE;
}

inline SIMDType simd_support() {
  static const SIMDType value = detect_simd_support();
  return value;
}

// ============================================================================
// Common Utilities
// ============================================================================

// bit=1 means NULL
inline bool is_null(const uint8_t *null_mask, size_t index) {
  return null_mask && (null_mask[index / 8] & (1 << (index % 8)));
}

inline uint8_t get_null_mask_byte(const uint8_t *null_mask, size_t byte_index) {
  return null_mask ? null_mask[byte_index] : 0x00;
}

// Validity of one vector's rows as bits, low bit = row i. A chunk never
// straddles a bitmap byte (lanes divides 8 and callers advance i by lanes), so
// one byte load covers the whole vector. Bit set = value present.
inline unsigned lane_valid_bits(const uint8_t *null_mask, size_t i, unsigned lanes) {
  const unsigned all = (1u << lanes) - 1u;
  if (null_mask == nullptr) return all;
  return (~static_cast<unsigned>(null_mask[i >> 3]) >> (i & 7)) & all;
}

// ============================================================================
// SSE2 Helper: horizontal reductions
// ============================================================================

#if defined(SHANNON_HAVE_SSE2)

inline float horizontal_sum_ps(__m128 v) {
  __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
  v = _mm_add_ps(v, shuf);
  shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2));
  v = _mm_add_ps(v, shuf);
  return _mm_cvtss_f32(v);
}

inline double horizontal_sum_pd(__m128d v) {
  __m128d shuf = _mm_unpackhi_pd(v, v);
  v = _mm_add_pd(v, shuf);
  return _mm_cvtsd_f64(v);
}

inline int32_t horizontal_sum_epi32(__m128i v) {
  __m128i sum = _mm_add_epi32(v, _mm_srli_si128(v, 8));
  sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
  return _mm_cvtsi128_si32(sum);
}

inline int64_t horizontal_sum_epi64(__m128i vec) {
  __m128i sum64 = _mm_add_epi64(vec, _mm_unpackhi_epi64(vec, vec));
  return _mm_cvtsi128_si64(sum64);
}

#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
SHANNON_TARGET_AVX2 inline int64_t horizontal_sum_epi64(__m256i vec) {
  __m128i low = _mm256_extracti128_si256(vec, 0);
  __m128i high = _mm256_extracti128_si256(vec, 1);
  __m128i sum128 = _mm_add_epi64(low, high);
  return horizontal_sum_epi64(sum128);
}
#endif

inline float horizontal_min_ps(__m128 v) {
  __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
  v = _mm_min_ps(v, shuf);
  shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2));
  v = _mm_min_ps(v, shuf);
  return _mm_cvtss_f32(v);
}

inline double horizontal_min_pd(__m128d v) {
  __m128d shuf = _mm_unpackhi_pd(v, v);
  v = _mm_min_pd(v, shuf);
  return _mm_cvtsd_f64(v);
}

inline int32_t horizontal_min_epi32(__m128i v) {
  // _mm_min_epi32 is SSE4.1, not SSE2. Keep the SSE2 helper truly SSE2-only.
  alignas(16) int32_t lanes[4];
  _mm_store_si128(reinterpret_cast<__m128i *>(lanes), v);
  return *std::min_element(lanes, lanes + 4);
}

inline float horizontal_max_ps(__m128 v) {
  __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
  v = _mm_max_ps(v, shuf);
  shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2));
  v = _mm_max_ps(v, shuf);
  return _mm_cvtss_f32(v);
}

inline double horizontal_max_pd(__m128d v) {
  __m128d shuf = _mm_unpackhi_pd(v, v);
  v = _mm_max_pd(v, shuf);
  return _mm_cvtsd_f64(v);
}

inline int32_t horizontal_max_epi32(__m128i v) {
  // _mm_max_epi32 is SSE4.1, not SSE2. Keep the SSE2 helper truly SSE2-only.
  alignas(16) int32_t lanes[4];
  _mm_store_si128(reinterpret_cast<__m128i *>(lanes), v);
  return *std::max_element(lanes, lanes + 4);
}

#endif  // SSE2 helpers

// ============================================================================
// Bitmap Operations
// ============================================================================

inline size_t popcount_bitmap_common(const uint8_t *data, size_t bytes) {
  size_t sum = 0;
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, sizeof(v));
    sum += std::popcount(v);
  }
  for (; i < bytes; ++i) sum += std::popcount(static_cast<unsigned>(data[i]));
  return sum;
}

inline size_t popcount_bitmap_sse2(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_HAVE_SSE2)
  size_t sum = 0;
  size_t i = 0;
  if (bytes >= 16) {
    __m128i mask1 = _mm_set1_epi8(0x55);
    __m128i mask2 = _mm_set1_epi8(0x33);
    __m128i mask4 = _mm_set1_epi8(0x0F);
    __m128i total = _mm_setzero_si128();
    for (; i + 16 <= bytes; i += 16) {
      __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
      __m128i t1 = _mm_sub_epi8(v, _mm_and_si128(_mm_srli_epi16(v, 1), mask1));
      __m128i t2 = _mm_add_epi8(_mm_and_si128(t1, mask2), _mm_and_si128(_mm_srli_epi16(t1, 2), mask2));
      __m128i t3 = _mm_and_si128(_mm_add_epi8(t2, _mm_srli_epi16(t2, 4)), mask4);
      total = _mm_add_epi64(total, _mm_sad_epu8(t3, _mm_setzero_si128()));
    }
    alignas(16) uint64_t temp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(temp), total);
    sum = temp[0] + temp[1];
  }
  for (; i < bytes; ++i) sum += std::popcount(static_cast<unsigned>(data[i]));
  return sum;
#else
  return popcount_bitmap_common(data, bytes);
#endif
}

SHANNON_TARGET_AVX2 inline size_t popcount_bitmap_avx2(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  size_t sum = 0;
  size_t i = 0;
  if (bytes >= 32) {
    alignas(32) static const uint8_t lut[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    __m256i vlut = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i *>(lut)));
    __m256i mask_low = _mm256_set1_epi8(0x0F);
    __m256i total = _mm256_setzero_si256();
    for (; i + 32 <= bytes; i += 32) {
      __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i lo = _mm256_shuffle_epi8(vlut, _mm256_and_si256(v, mask_low));
      __m256i hi = _mm256_shuffle_epi8(vlut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask_low));
      total = _mm256_add_epi64(total, _mm256_sad_epu8(_mm256_add_epi8(lo, hi), _mm256_setzero_si256()));
    }
    alignas(32) uint64_t temp[4];
    _mm256_store_si256(reinterpret_cast<__m256i *>(temp), total);
    sum = temp[0] + temp[1] + temp[2] + temp[3];
  }
  if (i < bytes) sum += popcount_bitmap_sse2(data + i, bytes - i);
  return sum;
#else
  return popcount_bitmap_sse2(data, bytes);
#endif
}

inline size_t popcount_bitmap_neon(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_HAVE_NEON_A64)
  size_t sum = 0, i = 0;
  for (; i + 16 <= bytes; i += 16) sum += vaddvq_u8(vcntq_u8(vld1q_u8(data + i)));
  for (; i < bytes; ++i) sum += std::popcount(static_cast<unsigned>(data[i]));
  return sum;
#else
  // vaddvq_u8 is A64-only; ARMv7 NEON has no horizontal add.
  return popcount_bitmap_common(data, bytes);
#endif
}

inline size_t popcount_bitmap(const std::vector<uint8_t> &bm) {
  if (bm.empty()) return 0;
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      return popcount_bitmap_avx2(bm.data(), bm.size());
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      return popcount_bitmap_sse2(bm.data(), bm.size());
#endif
#if defined(SHANNON_HAVE_NEON)
    case SIMDType::NEON:
      return popcount_bitmap_neon(bm.data(), bm.size());
#endif
    default:
      return popcount_bitmap_common(bm.data(), bm.size());
  }
}

// ============================================================================
// Generic scalar fallbacks
// ============================================================================

template <typename T>
T sum_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  T sum = 0;
  for (size_t i = 0; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
}

template <typename T>
T min_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  T val = std::numeric_limits<T>::max();
  bool found = false;
  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i) && (!found || data[i] < val)) {
      val = data[i];
      found = true;
    }
  }
  return found ? val : std::numeric_limits<T>::max();
}

template <typename T>
T max_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  T val = std::numeric_limits<T>::lowest();
  bool found = false;
  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i) && (!found || data[i] > val)) {
      val = data[i];
      found = true;
    }
  }
  return found ? val : std::numeric_limits<T>::lowest();
}

template <typename T, typename Allocator>
size_t filter_generic(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                      std::vector<size_t, Allocator> &out) {
  size_t cnt = 0;
  for (size_t i = 0; i < row_count; ++i)
    if (!is_null(null_mask, i) && predicate(data[i])) {
      out.push_back(i);
      ++cnt;
    }
  return cnt;
}

// ============================================================================
// Aggregate kernels: one lane description per (ISA, element type) and a single
// loop per ISA, instead of one copy of the loop per (ISA, type, operator).
// ============================================================================

struct SumOp {
  template <typename T>
  static constexpr T identity() {
    return T{0};
  }
};
struct MinOp {
  template <typename T>
  static constexpr T identity() {
    return (std::numeric_limits<T>::max)();
  }
};
struct MaxOp {
  template <typename T>
  static constexpr T identity() {
    return (std::numeric_limits<T>::lowest)();
  }
};

// Scalar tail fold. Integer sums wrap through the unsigned type so the tail
// cannot trip signed-overflow UB; the vector accumulators already wrap.
template <class Op, typename T>
inline T fold_scalar(T acc, T value) {
  if constexpr (std::is_same_v<Op, SumOp>) {
    if constexpr (std::is_integral_v<T>) {
      using U = std::make_unsigned_t<T>;
      return static_cast<T>(static_cast<U>(acc) + static_cast<U>(value));
    } else {
      return acc + value;
    }
  } else if constexpr (std::is_same_v<Op, MinOp>) {
    return value < acc ? value : acc;
  } else {
    return value > acc ? value : acc;
  }
}

// A Vec<Isa, T> supplies: Reg, lanes, load, splat, valid_mask, combine<Op>,
// reduce<Op> and keep<Op> (mask out invalid lanes).
template <class Isa, typename T>
struct Vec;

#if defined(SHANNON_HAVE_SSE2)

struct Sse2Tag {};

template <>
struct Vec<Sse2Tag, float> {
  using Reg = __m128;
  static constexpr unsigned lanes = 4;
  static Reg load(const float *p) { return _mm_loadu_ps(p); }
  static Reg splat(float v) { return _mm_set1_ps(v); }
  static Reg valid_mask(unsigned bits) {
    const __m128i sel = _mm_set_epi32(8, 4, 2, 1);
    return _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32(static_cast<int>(bits)), sel), sel));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_add_ps(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm_min_ps(a, b);
    return _mm_max_ps(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_and_ps(v, mask);
    return _mm_or_ps(_mm_and_ps(mask, v), _mm_andnot_ps(mask, neutral));
  }
  template <class Op>
  static float reduce(Reg v) {
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_ps(v);
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_ps(v);
    return horizontal_max_ps(v);
  }
};

template <>
struct Vec<Sse2Tag, double> {
  using Reg = __m128d;
  static constexpr unsigned lanes = 2;
  static Reg load(const double *p) { return _mm_loadu_pd(p); }
  static Reg splat(double v) { return _mm_set1_pd(v); }
  static Reg valid_mask(unsigned bits) {
    return _mm_castsi128_pd(_mm_set_epi64x((bits & 2) ? -1LL : 0, (bits & 1) ? -1LL : 0));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_add_pd(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm_min_pd(a, b);
    return _mm_max_pd(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_and_pd(v, mask);
    return _mm_or_pd(_mm_and_pd(mask, v), _mm_andnot_pd(mask, neutral));
  }
  template <class Op>
  static double reduce(Reg v) {
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_pd(v);
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_pd(v);
    return horizontal_max_pd(v);
  }
};

template <>
struct Vec<Sse2Tag, int32_t> {
  using Reg = __m128i;
  static constexpr unsigned lanes = 4;
  static Reg load(const int32_t *p) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p)); }
  static Reg splat(int32_t v) { return _mm_set1_epi32(v); }
  static Reg valid_mask(unsigned bits) {
    const __m128i sel = _mm_set_epi32(8, 4, 2, 1);
    return _mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32(static_cast<int>(bits)), sel), sel);
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_add_epi32(a, b);
    // SSE2 has no epi32 min/max; select on a signed compare instead.
    const __m128i greater = _mm_cmpgt_epi32(a, b);
    if constexpr (std::is_same_v<Op, MinOp>)
      return _mm_or_si128(_mm_and_si128(greater, b), _mm_andnot_si128(greater, a));
    return _mm_or_si128(_mm_and_si128(greater, a), _mm_andnot_si128(greater, b));
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_and_si128(v, mask);
    return _mm_or_si128(_mm_and_si128(mask, v), _mm_andnot_si128(mask, neutral));
  }
  template <class Op>
  static int32_t reduce(Reg v) {
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_epi32(v);
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_epi32(v);
    return horizontal_max_epi32(v);
  }
};

template <>
struct Vec<Sse2Tag, int64_t> {
  using Reg = __m128i;
  static constexpr unsigned lanes = 2;
  static Reg load(const int64_t *p) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p)); }
  static Reg splat(int64_t v) { return _mm_set1_epi64x(v); }
  static Reg valid_mask(unsigned bits) { return _mm_set_epi64x((bits & 2) ? -1LL : 0, (bits & 1) ? -1LL : 0); }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    // SSE2 also lacks an epi64 compare, so int64 min/max keep the scalar path.
    static_assert(std::is_same_v<Op, SumOp>, "SSE2 cannot fold 64-bit min/max");
    return _mm_add_epi64(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm_and_si128(v, mask);
    return _mm_or_si128(_mm_and_si128(mask, v), _mm_andnot_si128(mask, neutral));
  }
  template <class Op>
  static int64_t reduce(Reg v) {
    alignas(16) int64_t tmp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(tmp), v);
    if constexpr (std::is_same_v<Op, SumOp>) return tmp[0] + tmp[1];
    if constexpr (std::is_same_v<Op, MinOp>) return tmp[0] < tmp[1] ? tmp[0] : tmp[1];
    return tmp[0] > tmp[1] ? tmp[0] : tmp[1];
  }
};

template <class Op, typename T>
inline T aggregate_sse2(const T *data, const uint8_t *null_mask, size_t row_count) {
  using V = Vec<Sse2Tag, T>;
  constexpr unsigned kLanes = V::lanes;
  constexpr unsigned kAll = (1u << kLanes) - 1u;
  const typename V::Reg neutral = V::splat(Op::template identity<T>());
  typename V::Reg acc = neutral;
  size_t i = 0;
  // Separated: the per-vector null check halves the maskless path.
  if (null_mask == nullptr) {
    for (; i + kLanes <= row_count; i += kLanes) acc = V::template combine<Op>(acc, V::load(data + i));
    T result = V::template reduce<Op>(acc);
    for (; i < row_count; ++i) result = fold_scalar<Op>(result, data[i]);
    return result;
  }
  for (; i + kLanes <= row_count; i += kLanes) {
    typename V::Reg chunk = V::load(data + i);
    const unsigned valid = lane_valid_bits(null_mask, i, kLanes);
    if (valid != kAll) chunk = V::template keep<Op>(chunk, neutral, V::valid_mask(valid));
    acc = V::template combine<Op>(acc, chunk);
  }
  T result = V::template reduce<Op>(acc);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) result = fold_scalar<Op>(result, data[i]);
  return result;
}

#endif  // SHANNON_HAVE_SSE2

#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)

#if defined(__GNUC__) && !defined(__clang__)
// GCC reports the AVX ABI note while parsing the return type, before it applies
// the target attribute to the function.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"
#endif

struct Avx2Tag {};

template <>
struct Vec<Avx2Tag, float> {
  using Reg = __m256;
  static constexpr unsigned lanes = 8;
  SHANNON_TARGET_AVX2 static Reg load(const float *p) { return _mm256_loadu_ps(p); }
  SHANNON_TARGET_AVX2 static Reg splat(float v) { return _mm256_set1_ps(v); }
  SHANNON_TARGET_AVX2 static Reg valid_mask(unsigned bits) {
    const __m256i sel = _mm256_set_epi32(128, 64, 32, 16, 8, 4, 2, 1);
    return _mm256_castsi256_ps(
        _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(static_cast<int>(bits)), sel), sel));
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_add_ps(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm256_min_ps(a, b);
    return _mm256_max_ps(a, b);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_and_ps(v, mask);
    return _mm256_blendv_ps(neutral, v, mask);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static float reduce(Reg v) {
    const __m128 lo = _mm256_extractf128_ps(v, 0), hi = _mm256_extractf128_ps(v, 1);
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_ps(_mm_add_ps(lo, hi));
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_ps(_mm_min_ps(lo, hi));
    return horizontal_max_ps(_mm_max_ps(lo, hi));
  }
};

template <>
struct Vec<Avx2Tag, double> {
  using Reg = __m256d;
  static constexpr unsigned lanes = 4;
  SHANNON_TARGET_AVX2 static Reg load(const double *p) { return _mm256_loadu_pd(p); }
  SHANNON_TARGET_AVX2 static Reg splat(double v) { return _mm256_set1_pd(v); }
  SHANNON_TARGET_AVX2 static Reg valid_mask(unsigned bits) {
    const __m256i sel = _mm256_set_epi64x(8, 4, 2, 1);
    return _mm256_castsi256_pd(
        _mm256_cmpeq_epi64(_mm256_and_si256(_mm256_set1_epi64x(static_cast<long long>(bits)), sel), sel));
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_add_pd(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm256_min_pd(a, b);
    return _mm256_max_pd(a, b);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_and_pd(v, mask);
    return _mm256_blendv_pd(neutral, v, mask);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static double reduce(Reg v) {
    const __m128d lo = _mm256_extractf128_pd(v, 0), hi = _mm256_extractf128_pd(v, 1);
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_pd(_mm_add_pd(lo, hi));
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_pd(_mm_min_pd(lo, hi));
    return horizontal_max_pd(_mm_max_pd(lo, hi));
  }
};

template <>
struct Vec<Avx2Tag, int32_t> {
  using Reg = __m256i;
  static constexpr unsigned lanes = 8;
  SHANNON_TARGET_AVX2 static Reg load(const int32_t *p) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
  }
  SHANNON_TARGET_AVX2 static Reg splat(int32_t v) { return _mm256_set1_epi32(v); }
  SHANNON_TARGET_AVX2 static Reg valid_mask(unsigned bits) {
    const __m256i sel = _mm256_set_epi32(128, 64, 32, 16, 8, 4, 2, 1);
    return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(static_cast<int>(bits)), sel), sel);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_add_epi32(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm256_min_epi32(a, b);
    return _mm256_max_epi32(a, b);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_and_si256(v, mask);
    return _mm256_blendv_epi8(neutral, v, mask);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static int32_t reduce(Reg v) {
    const __m128i lo = _mm256_extracti128_si256(v, 0), hi = _mm256_extracti128_si256(v, 1);
    if constexpr (std::is_same_v<Op, SumOp>) return horizontal_sum_epi32(_mm_add_epi32(lo, hi));
    if constexpr (std::is_same_v<Op, MinOp>) return horizontal_min_epi32(_mm_min_epi32(lo, hi));
    return horizontal_max_epi32(_mm_max_epi32(lo, hi));
  }
};

template <>
struct Vec<Avx2Tag, int64_t> {
  using Reg = __m256i;
  static constexpr unsigned lanes = 4;
  SHANNON_TARGET_AVX2 static Reg load(const int64_t *p) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
  }
  SHANNON_TARGET_AVX2 static Reg splat(int64_t v) { return _mm256_set1_epi64x(v); }
  SHANNON_TARGET_AVX2 static Reg valid_mask(unsigned bits) {
    const __m256i sel = _mm256_set_epi64x(8, 4, 2, 1);
    return _mm256_cmpeq_epi64(_mm256_and_si256(_mm256_set1_epi64x(static_cast<long long>(bits)), sel), sel);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_add_epi64(a, b);
    // gt has all-ones where a > b, so blending on it picks the smaller/larger lane.
    const __m256i gt = _mm256_cmpgt_epi64(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return _mm256_blendv_epi8(a, b, gt);
    return _mm256_blendv_epi8(b, a, gt);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>) return _mm256_and_si256(v, mask);
    return _mm256_blendv_epi8(neutral, v, mask);
  }
  template <class Op>
  SHANNON_TARGET_AVX2 static int64_t reduce(Reg v) {
    alignas(32) int64_t tmp[4];
    _mm256_store_si256(reinterpret_cast<__m256i *>(tmp), v);
    if constexpr (std::is_same_v<Op, SumOp>) return tmp[0] + tmp[1] + tmp[2] + tmp[3];
    int64_t best = tmp[0];
    for (int j = 1; j < 4; ++j) {
      if constexpr (std::is_same_v<Op, MinOp>)
        best = tmp[j] < best ? tmp[j] : best;
      else
        best = tmp[j] > best ? tmp[j] : best;
    }
    return best;
  }
};

template <class Op, typename T>
SHANNON_TARGET_AVX2 inline T aggregate_avx2(const T *data, const uint8_t *null_mask, size_t row_count) {
  using V = Vec<Avx2Tag, T>;
  constexpr unsigned kLanes = V::lanes;
  constexpr unsigned kAll = (1u << kLanes) - 1u;
  const typename V::Reg neutral = V::splat(Op::template identity<T>());
  typename V::Reg acc = neutral;
  size_t i = 0;
  // Separated: the per-vector null check halves the maskless path.
  if (null_mask == nullptr) {
    for (; i + kLanes <= row_count; i += kLanes) acc = V::template combine<Op>(acc, V::load(data + i));
    T result = V::template reduce<Op>(acc);
    for (; i < row_count; ++i) result = fold_scalar<Op>(result, data[i]);
    return result;
  }
  for (; i + kLanes <= row_count; i += kLanes) {
    typename V::Reg chunk = V::load(data + i);
    const unsigned valid = lane_valid_bits(null_mask, i, kLanes);
    if (valid != kAll) chunk = V::template keep<Op>(chunk, neutral, V::valid_mask(valid));
    acc = V::template combine<Op>(acc, chunk);
  }
  T result = V::template reduce<Op>(acc);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) result = fold_scalar<Op>(result, data[i]);
  return result;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif  // SHANNON_HAVE_AVX2 || SHANNON_RUNTIME_AVX2

// ============================================================================
// SSE2 Sum
// ============================================================================

inline float sum_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline double sum_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int32_t sum_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int64_t sum_sse2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// SSE2 Min
// ============================================================================

inline float min_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MinOp>(data, null_mask, row_count);
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline double min_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MinOp>(data, null_mask, row_count);
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline int32_t min_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MinOp>(data, null_mask, row_count);
#else
  return min_generic(data, null_mask, row_count);
#endif
}

// int64 min: no native SSE epi64 min, fall through to generic
inline int64_t min_sse2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
  return min_generic(data, null_mask, row_count);
}

// ============================================================================
// SSE2 Max
// ============================================================================

inline float max_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MaxOp>(data, null_mask, row_count);
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline double max_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MaxOp>(data, null_mask, row_count);
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline int32_t max_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_SSE2)
  return aggregate_sse2<MaxOp>(data, null_mask, row_count);
#else
  return max_generic(data, null_mask, row_count);
#endif
}

// int64 max: no native SSE epi64 max, fall through to generic
inline int64_t max_sse2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
  return max_generic(data, null_mask, row_count);
}

// ============================================================================
// AVX2 Sum
// ============================================================================

SHANNON_TARGET_AVX2 inline float sum_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<SumOp>(data, null_mask, row_count);
#else
  return sum_sse2_float(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline double sum_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<SumOp>(data, null_mask, row_count);
#else
  return sum_sse2_double(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline int32_t sum_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<SumOp>(data, null_mask, row_count);
#else
  return sum_sse2_int32(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline int64_t sum_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<SumOp>(data, null_mask, row_count);
#else
  return sum_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// AVX2 Min
// ============================================================================

SHANNON_TARGET_AVX2 inline float min_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MinOp>(data, null_mask, row_count);
#else
  return min_sse2_float(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline double min_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MinOp>(data, null_mask, row_count);
#else
  return min_sse2_double(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline int32_t min_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MinOp>(data, null_mask, row_count);
#else
  return min_sse2_int32(data, null_mask, row_count);
#endif
}

// AVX2 int64 min: _mm256_cmpgt_epi64 available, use blendv to select smaller
SHANNON_TARGET_AVX2 inline int64_t min_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MinOp>(data, null_mask, row_count);
#else
  return min_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// AVX2 Max
// ============================================================================

SHANNON_TARGET_AVX2 inline float max_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MaxOp>(data, null_mask, row_count);
#else
  return max_sse2_float(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline double max_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MaxOp>(data, null_mask, row_count);
#else
  return max_sse2_double(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline int32_t max_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MaxOp>(data, null_mask, row_count);
#else
  return max_sse2_int32(data, null_mask, row_count);
#endif
}

SHANNON_TARGET_AVX2 inline int64_t max_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  return aggregate_avx2<MaxOp>(data, null_mask, row_count);
#else
  return max_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// NEON Sum / Min / Max
// ============================================================================

#if defined(SHANNON_HAVE_NEON)

struct NeonTag {};

// Reductions store the register and fold in scalars: vaddvq_*/vminvq_* are
// AArch64-only, and the final fold is a few instructions either way.
template <>
struct Vec<NeonTag, float> {
  using Reg = float32x4_t;
  static constexpr unsigned lanes = 4;
  static Reg load(const float *p) { return vld1q_f32(p); }
  static Reg splat(float v) { return vdupq_n_f32(v); }
  static Reg valid_mask(unsigned bits) {
    const uint32x4_t sel = {1u, 2u, 4u, 8u};
    return vreinterpretq_f32_u32(vceqq_u32(vandq_u32(vdupq_n_u32(bits), sel), sel));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return vaddq_f32(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return vminq_f32(a, b);
    return vmaxq_f32(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>)
      return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v), vreinterpretq_u32_f32(mask)));
    return vbslq_f32(vreinterpretq_u32_f32(mask), v, neutral);
  }
  template <class Op>
  static float reduce(Reg v) {
    alignas(16) float tmp[4];
    vst1q_f32(tmp, v);
    if constexpr (std::is_same_v<Op, SumOp>) return tmp[0] + tmp[1] + tmp[2] + tmp[3];
    if constexpr (std::is_same_v<Op, MinOp>) return std::min(std::min(tmp[0], tmp[1]), std::min(tmp[2], tmp[3]));
    return std::max(std::max(tmp[0], tmp[1]), std::max(tmp[2], tmp[3]));
  }
};

template <>
struct Vec<NeonTag, int32_t> {
  using Reg = int32x4_t;
  static constexpr unsigned lanes = 4;
  static Reg load(const int32_t *p) { return vld1q_s32(p); }
  static Reg splat(int32_t v) { return vdupq_n_s32(v); }
  static Reg valid_mask(unsigned bits) {
    const uint32x4_t sel = {1u, 2u, 4u, 8u};
    return vreinterpretq_s32_u32(vceqq_u32(vandq_u32(vdupq_n_u32(bits), sel), sel));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return vaddq_s32(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return vminq_s32(a, b);
    return vmaxq_s32(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>)
      return vreinterpretq_s32_u32(vandq_u32(vreinterpretq_u32_s32(v), vreinterpretq_u32_s32(mask)));
    return vbslq_s32(vreinterpretq_u32_s32(mask), v, neutral);
  }
  template <class Op>
  static int32_t reduce(Reg v) {
    alignas(16) int32_t tmp[4];
    vst1q_s32(tmp, v);
    if constexpr (std::is_same_v<Op, SumOp>) {
      using U = uint32_t;
      return static_cast<int32_t>(static_cast<U>(tmp[0]) + static_cast<U>(tmp[1]) + static_cast<U>(tmp[2]) +
                                  static_cast<U>(tmp[3]));
    }
    if constexpr (std::is_same_v<Op, MinOp>) return std::min(std::min(tmp[0], tmp[1]), std::min(tmp[2], tmp[3]));
    return std::max(std::max(tmp[0], tmp[1]), std::max(tmp[2], tmp[3]));
  }
};

#if defined(SHANNON_HAVE_NEON_A64)

// 64-bit lanes and vminq_f64/vbslq_f64 are AArch64-only.
template <>
struct Vec<NeonTag, double> {
  using Reg = float64x2_t;
  static constexpr unsigned lanes = 2;
  static Reg load(const double *p) { return vld1q_f64(p); }
  static Reg splat(double v) { return vdupq_n_f64(v); }
  static Reg valid_mask(unsigned bits) {
    const uint64x2_t sel = {1ull, 2ull};
    return vreinterpretq_f64_u64(vceqq_u64(vandq_u64(vdupq_n_u64(bits), sel), sel));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return vaddq_f64(a, b);
    if constexpr (std::is_same_v<Op, MinOp>) return vminq_f64(a, b);
    return vmaxq_f64(a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>)
      return vreinterpretq_f64_u64(vandq_u64(vreinterpretq_u64_f64(v), vreinterpretq_u64_f64(mask)));
    return vbslq_f64(vreinterpretq_u64_f64(mask), v, neutral);
  }
  template <class Op>
  static double reduce(Reg v) {
    alignas(16) double tmp[2];
    vst1q_f64(tmp, v);
    if constexpr (std::is_same_v<Op, SumOp>) return tmp[0] + tmp[1];
    if constexpr (std::is_same_v<Op, MinOp>) return tmp[0] < tmp[1] ? tmp[0] : tmp[1];
    return tmp[0] > tmp[1] ? tmp[0] : tmp[1];
  }
};

template <>
struct Vec<NeonTag, int64_t> {
  using Reg = int64x2_t;
  static constexpr unsigned lanes = 2;
  static Reg load(const int64_t *p) { return vld1q_s64(p); }
  static Reg splat(int64_t v) { return vdupq_n_s64(v); }
  static Reg valid_mask(unsigned bits) {
    const uint64x2_t sel = {1ull, 2ull};
    return vreinterpretq_s64_u64(vceqq_u64(vandq_u64(vdupq_n_u64(bits), sel), sel));
  }
  template <class Op>
  static Reg combine(Reg a, Reg b) {
    if constexpr (std::is_same_v<Op, SumOp>) return vaddq_s64(a, b);
    // AArch64 has no 64-bit integer SMIN/SMAX: select through a compare mask.
    if constexpr (std::is_same_v<Op, MinOp>) return vbslq_s64(vcltq_s64(a, b), a, b);
    return vbslq_s64(vcgtq_s64(a, b), a, b);
  }
  template <class Op>
  static Reg keep(Reg v, Reg neutral, Reg mask) {
    if constexpr (std::is_same_v<Op, SumOp>)
      return vreinterpretq_s64_u64(vandq_u64(vreinterpretq_u64_s64(v), vreinterpretq_u64_s64(mask)));
    return vbslq_s64(vreinterpretq_u64_s64(mask), v, neutral);
  }
  template <class Op>
  static int64_t reduce(Reg v) {
    alignas(16) int64_t tmp[2];
    vst1q_s64(tmp, v);
    if constexpr (std::is_same_v<Op, SumOp>)
      return static_cast<int64_t>(static_cast<uint64_t>(tmp[0]) + static_cast<uint64_t>(tmp[1]));
    if constexpr (std::is_same_v<Op, MinOp>) return tmp[0] < tmp[1] ? tmp[0] : tmp[1];
    return tmp[0] > tmp[1] ? tmp[0] : tmp[1];
  }
};

#endif  // SHANNON_HAVE_NEON_A64

template <class Op, typename T>
inline T aggregate_neon(const T *data, const uint8_t *null_mask, size_t row_count) {
  using V = Vec<NeonTag, T>;
  constexpr unsigned kLanes = V::lanes;
  constexpr unsigned kAll = (1u << kLanes) - 1u;
  const typename V::Reg neutral = V::splat(Op::template identity<T>());
  typename V::Reg acc = neutral;
  size_t i = 0;
  // Separated: the per-vector null check halves the maskless path.
  if (null_mask == nullptr) {
    for (; i + kLanes <= row_count; i += kLanes) acc = V::template combine<Op>(acc, V::load(data + i));
    T result = V::template reduce<Op>(acc);
    for (; i < row_count; ++i) result = fold_scalar<Op>(result, data[i]);
    return result;
  }
  for (; i + kLanes <= row_count; i += kLanes) {
    typename V::Reg chunk = V::load(data + i);
    const unsigned valid = lane_valid_bits(null_mask, i, kLanes);
    if (valid != kAll) chunk = V::template keep<Op>(chunk, neutral, V::valid_mask(valid));
    acc = V::template combine<Op>(acc, chunk);
  }
  T result = V::template reduce<Op>(acc);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) result = fold_scalar<Op>(result, data[i]);
  return result;
}

#endif  // SHANNON_HAVE_NEON

inline float sum_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline float min_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<MinOp>(data, null_mask, row_count);
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline float max_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<MaxOp>(data, null_mask, row_count);
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline int32_t sum_neon_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<SumOp>(data, null_mask, row_count);
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int32_t min_neon_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<MinOp>(data, null_mask, row_count);
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline int32_t max_neon_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_HAVE_NEON)
  return aggregate_neon<MaxOp>(data, null_mask, row_count);
#else
  return max_generic(data, null_mask, row_count);
#endif
}

#if defined(SHANNON_HAVE_NEON_A64)

inline double sum_neon_double(const double *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<SumOp>(data, null_mask, row_count);
}

inline double min_neon_double(const double *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<MinOp>(data, null_mask, row_count);
}

inline double max_neon_double(const double *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<MaxOp>(data, null_mask, row_count);
}

inline int64_t sum_neon_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<SumOp>(data, null_mask, row_count);
}

inline int64_t min_neon_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<MinOp>(data, null_mask, row_count);
}

inline int64_t max_neon_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
  return aggregate_neon<MaxOp>(data, null_mask, row_count);
}

#endif  // SHANNON_HAVE_NEON_A64

// ============================================================================
// count_non_null
// ============================================================================

// Present rows = rows minus the popcount of the null bitmap. Only complete bytes
// are counted in bulk: the padding bits of the last partial byte are not
// required to be zero by this API, and counting them would underflow the result.
inline size_t count_non_null_with(size_t (*popcount)(const uint8_t *, size_t), const uint8_t *null_mask,
                                  size_t row_count) {
  if (null_mask == nullptr) return row_count;
  const size_t full_bytes = row_count / 8;
  size_t null_cnt = popcount(null_mask, full_bytes);
  const unsigned tail_bits = static_cast<unsigned>(row_count & 7U);
  if (tail_bits != 0) {
    const uint8_t valid_mask = static_cast<uint8_t>((1U << tail_bits) - 1U);
    null_cnt += std::popcount(static_cast<unsigned>(null_mask[full_bytes] & valid_mask));
  }
  return row_count - null_cnt;
}

inline size_t count_non_null_generic(const uint8_t *null_mask, size_t row_count) {
  return count_non_null_with(&popcount_bitmap_common, null_mask, row_count);
}

inline size_t count_non_null_sse2(const uint8_t *null_mask, size_t row_count) {
  return count_non_null_with(&popcount_bitmap_sse2, null_mask, row_count);
}

inline size_t count_non_null_sse4(const uint8_t *null_mask, size_t row_count) {
  // POPCNT is an independent x86 capability; SSE4.x alone does not imply it.
  return count_non_null_sse2(null_mask, row_count);
}

SHANNON_TARGET_AVX2 inline size_t count_non_null_avx2(const uint8_t *null_mask, size_t row_count) {
  return count_non_null_with(&popcount_bitmap_avx2, null_mask, row_count);
}

inline size_t count_non_null_neon(const uint8_t *null_mask, size_t row_count) {
  return count_non_null_with(&popcount_bitmap_neon, null_mask, row_count);
}

// ============================================================================
// Filter (SSE2 int32 equality specialization)
// ============================================================================

template <typename Allocator>
inline size_t filter_sse2_int32_eq(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                                   std::vector<size_t, Allocator> &out) {
#if defined(SHANNON_HAVE_SSE2)
  size_t cnt = 0;
  size_t i = 0;
  __m128i target = _mm_set1_epi32(value);
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    __m128i cmp = _mm_cmpeq_epi32(chunk, target);
    if (null_mask) {
      const unsigned valid = lane_valid_bits(null_mask, i, 4);
      if (valid != 0xFu) cmp = _mm_and_si128(cmp, Vec<Sse2Tag, int32_t>::valid_mask(valid));
    }
    int rm = _mm_movemask_ps(_mm_castsi128_ps(cmp));
    for (int j = 0; j < 4; ++j)
      if (rm & (1 << j)) {
        out.push_back(i + j);
        ++cnt;
      }
  }
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i) && data[i] == value) {
      out.push_back(i);
      ++cnt;
    }
  return cnt;
#else
  return filter_generic<int32_t>(
      data, null_mask, row_count, [value](int32_t v) { return v == value; }, out);
#endif
}

template <typename T>
inline bool compare_scalar(T lhs, T rhs, CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return lhs == rhs;
    case CompareOp::NE:
      return lhs != rhs;
    case CompareOp::LT:
      return lhs < rhs;
    case CompareOp::LE:
      return lhs <= rhs;
    case CompareOp::GT:
      return lhs > rhs;
    case CompareOp::GE:
      return lhs >= rhs;
  }
  return false;
}

template <typename Allocator>
SHANNON_TARGET_AVX2 inline size_t filter_avx2_int32_compare(const int32_t *data, const uint8_t *null_mask,
                                                            size_t row_count, int32_t value, CompareOp op,
                                                            std::vector<size_t, Allocator> &out) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  size_t cnt = 0;
  size_t i = 0;
  const __m256i target = _mm256_set1_epi32(value);
  const __m256i all_ones = _mm256_set1_epi32(-1);
  for (; i + 8 <= row_count; i += 8) {
    const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    __m256i cmp = _mm256_setzero_si256();
    switch (op) {
      case CompareOp::EQ:
        cmp = _mm256_cmpeq_epi32(values, target);
        break;
      case CompareOp::NE:
        cmp = _mm256_xor_si256(_mm256_cmpeq_epi32(values, target), all_ones);
        break;
      case CompareOp::LT:
        cmp = _mm256_cmpgt_epi32(target, values);
        break;
      case CompareOp::LE:
        cmp = _mm256_xor_si256(_mm256_cmpgt_epi32(values, target), all_ones);
        break;
      case CompareOp::GT:
        cmp = _mm256_cmpgt_epi32(values, target);
        break;
      case CompareOp::GE:
        cmp = _mm256_xor_si256(_mm256_cmpgt_epi32(target, values), all_ones);
        break;
    }
    if (null_mask) {
      const unsigned valid = lane_valid_bits(null_mask, i, 8);
      if (valid != 0xFFu) cmp = _mm256_and_si256(cmp, Vec<Avx2Tag, int32_t>::valid_mask(valid));
    }
    int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
    for (int lane = 0; lane < 8; ++lane) {
      if (mask & (1 << lane)) {
        out.push_back(i + lane);
        ++cnt;
      }
    }
  }
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i) && compare_scalar(data[i], value, op)) {
      out.push_back(i);
      ++cnt;
    }
  }
  return cnt;
#else
  return filter_generic<int32_t>(
      data, null_mask, row_count, [value, op](int32_t v) { return compare_scalar(v, value, op); }, out);
#endif
}

template <typename Allocator>
SHANNON_TARGET_AVX2 inline size_t filter_avx2_int64_compare(const int64_t *data, const uint8_t *null_mask,
                                                            size_t row_count, int64_t value, CompareOp op,
                                                            std::vector<size_t, Allocator> &out) {
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  size_t cnt = 0;
  size_t i = 0;
  const __m256i target = _mm256_set1_epi64x(value);
  const __m256i all_ones = _mm256_set1_epi64x(-1);
  for (; i + 4 <= row_count; i += 4) {
    const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    __m256i cmp = _mm256_setzero_si256();
    switch (op) {
      case CompareOp::EQ:
        cmp = _mm256_cmpeq_epi64(values, target);
        break;
      case CompareOp::NE:
        cmp = _mm256_xor_si256(_mm256_cmpeq_epi64(values, target), all_ones);
        break;
      case CompareOp::LT:
        cmp = _mm256_cmpgt_epi64(target, values);
        break;
      case CompareOp::LE:
        cmp = _mm256_xor_si256(_mm256_cmpgt_epi64(values, target), all_ones);
        break;
      case CompareOp::GT:
        cmp = _mm256_cmpgt_epi64(values, target);
        break;
      case CompareOp::GE:
        cmp = _mm256_xor_si256(_mm256_cmpgt_epi64(target, values), all_ones);
        break;
    }
    if (null_mask) {
      const unsigned valid = lane_valid_bits(null_mask, i, 4);
      if (valid != 0xFu) cmp = _mm256_and_si256(cmp, Vec<Avx2Tag, int64_t>::valid_mask(valid));
    }
    int mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp));
    for (int lane = 0; lane < 4; ++lane) {
      if (mask & (1 << lane)) {
        out.push_back(i + lane);
        ++cnt;
      }
    }
  }
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i) && compare_scalar(data[i], value, op)) {
      out.push_back(i);
      ++cnt;
    }
  }
  return cnt;
#else
  return filter_generic<int64_t>(
      data, null_mask, row_count, [value, op](int64_t v) { return compare_scalar(v, value, op); }, out);
#endif
}

// ============================================================================
// High-level dispatch
// ============================================================================

template <typename T>
T sum(const T *data, const uint8_t *null_mask, size_t row_count) {
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      if constexpr (std::is_same_v<T, float>)
        return sum_avx2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return sum_avx2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return sum_avx2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return sum_avx2_int64(data, null_mask, row_count);
      [[fallthrough]];
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>)
        return sum_sse2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return sum_sse2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return sum_sse2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return sum_sse2_int64(data, null_mask, row_count);
      break;
#endif
#if defined(SHANNON_HAVE_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>)
        return sum_neon_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return sum_neon_int32(data, null_mask, row_count);
#if defined(SHANNON_HAVE_NEON_A64)
      else if constexpr (std::is_same_v<T, double>)
        return sum_neon_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return sum_neon_int64(data, null_mask, row_count);
#endif
      break;
#endif
    default:
      break;
  }
  return sum_generic<T>(data, null_mask, row_count);
}

template <typename T>
T min(const T *data, const uint8_t *null_mask, size_t row_count) {
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      if constexpr (std::is_same_v<T, float>)
        return min_avx2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return min_avx2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return min_avx2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return min_avx2_int64(data, null_mask, row_count);
      [[fallthrough]];
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>)
        return min_sse2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return min_sse2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return min_sse2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return min_sse2_int64(data, null_mask, row_count);
      break;
#endif
#if defined(SHANNON_HAVE_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>)
        return min_neon_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return min_neon_int32(data, null_mask, row_count);
#if defined(SHANNON_HAVE_NEON_A64)
      else if constexpr (std::is_same_v<T, double>)
        return min_neon_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return min_neon_int64(data, null_mask, row_count);
#endif
      break;
#endif
    default:
      break;
  }
  return min_generic<T>(data, null_mask, row_count);
}

template <typename T>
T max(const T *data, const uint8_t *null_mask, size_t row_count) {
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      if constexpr (std::is_same_v<T, float>)
        return max_avx2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return max_avx2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return max_avx2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return max_avx2_int64(data, null_mask, row_count);
      [[fallthrough]];
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>)
        return max_sse2_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, double>)
        return max_sse2_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return max_sse2_int32(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return max_sse2_int64(data, null_mask, row_count);
      break;
#endif
#if defined(SHANNON_HAVE_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>)
        return max_neon_float(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int32_t>)
        return max_neon_int32(data, null_mask, row_count);
#if defined(SHANNON_HAVE_NEON_A64)
      else if constexpr (std::is_same_v<T, double>)
        return max_neon_double(data, null_mask, row_count);
      else if constexpr (std::is_same_v<T, int64_t>)
        return max_neon_int64(data, null_mask, row_count);
#endif
      break;
#endif
    default:
      break;
  }
  return max_generic<T>(data, null_mask, row_count);
}

inline size_t count_non_null(const uint8_t *null_mask, size_t row_count) {
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      return count_non_null_avx2(null_mask, row_count);
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
      return count_non_null_sse4(null_mask, row_count);
    case SIMDType::SSE2:
    case SIMDType::SSE:
      return count_non_null_sse2(null_mask, row_count);
#endif
#if defined(SHANNON_HAVE_NEON)
    case SIMDType::NEON:
      return count_non_null_neon(null_mask, row_count);
#endif
    default:
      return count_non_null_generic(null_mask, row_count);
  }
}

template <typename T>
size_t filter(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
              std::vector<size_t> &out) {
  return filter_generic<T>(data, null_mask, row_count, predicate, out);
}

inline size_t filter_eq_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                              std::vector<size_t> &out) {
  const SIMDType simd_type = simd_support();
  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      return filter_sse2_int32_eq(data, null_mask, row_count, value, out);
#endif
    default:
      return filter_generic<int32_t>(
          data, null_mask, row_count, [value](int32_t v) { return v == value; }, out);
  }
}

template <typename Allocator>
inline size_t filter_compare_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                                   CompareOp op, std::vector<size_t, Allocator> &out) {
  if (simd_support() == SIMDType::AVX2) return filter_avx2_int32_compare(data, null_mask, row_count, value, op, out);
  if (op == CompareOp::EQ) return filter_sse2_int32_eq(data, null_mask, row_count, value, out);
  return filter_generic<int32_t>(
      data, null_mask, row_count, [value, op](int32_t v) { return compare_scalar(v, value, op); }, out);
}

template <typename Allocator>
inline size_t filter_compare_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count, int64_t value,
                                   CompareOp op, std::vector<size_t, Allocator> &out) {
  if (simd_support() == SIMDType::AVX2) return filter_avx2_int64_compare(data, null_mask, row_count, value, op, out);
  return filter_generic<int64_t>(
      data, null_mask, row_count, [value, op](int64_t v) { return compare_scalar(v, value, op); }, out);
}
}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_IMPL_H__