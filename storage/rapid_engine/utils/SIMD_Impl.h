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

// ============================================================================
// Gathered integer comparison: one compare primitive per (ISA, width) and one
// gather loop. The index-output filters below and the IMCS predicate's bitmap
// kernels both go through here, so the operator lattice is written once.
// ============================================================================
namespace detail {
// Rows per gathered block: a multiple of every lane width below, so a block
// never needs a partial vector.
constexpr size_t kCmpBlockRows = 64;

struct CmpRequest {
  CompareOp op{CompareOp::EQ};
  bool range{false};   // also require value <= upper
  bool negate{false};  // NOT BETWEEN
};

template <typename T>
inline uint64_t cmp_block_scalar(const T *vals, T lower, T upper, const CmpRequest &req) {
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; ++j) {
    if constexpr (std::is_floating_point_v<T>) {
      // Floating-point NOT BETWEEN is ordered LT || GT, not the complement
      // of GE && LE: NaN would otherwise become a matching row.
      if (req.range && req.negate) {
        if (vals[j] < lower || vals[j] > upper) mask |= static_cast<uint64_t>(1) << j;
        continue;
      }
      if (req.op == CompareOp::NE && (std::isnan(vals[j]) || std::isnan(lower))) continue;
    }
    bool matched = compare_scalar(vals[j], lower, req.op);
    if (req.range) matched = matched && compare_scalar(vals[j], upper, CompareOp::LE);
    if (req.negate) matched = !matched;
    if (matched) mask |= static_cast<uint64_t>(1) << j;
  }
  return mask;
}

#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)

SHANNON_TARGET_AVX2 inline __m256i cmp_avx2_i32(__m256i values, __m256i bound, CompareOp op) {
  const __m256i all_ones = _mm256_set1_epi32(-1);
  switch (op) {
    case CompareOp::EQ:
      return _mm256_cmpeq_epi32(values, bound);
    case CompareOp::NE:
      return _mm256_xor_si256(_mm256_cmpeq_epi32(values, bound), all_ones);
    case CompareOp::LT:
      return _mm256_cmpgt_epi32(bound, values);
    case CompareOp::LE:
      return _mm256_xor_si256(_mm256_cmpgt_epi32(values, bound), all_ones);
    case CompareOp::GT:
      return _mm256_cmpgt_epi32(values, bound);
    case CompareOp::GE:
      return _mm256_xor_si256(_mm256_cmpgt_epi32(bound, values), all_ones);
  }
  return _mm256_setzero_si256();
}

SHANNON_TARGET_AVX2 inline __m256i cmp_avx2_i64(__m256i values, __m256i bound, CompareOp op) {
  const __m256i all_ones = _mm256_set1_epi64x(-1);
  switch (op) {
    case CompareOp::EQ:
      return _mm256_cmpeq_epi64(values, bound);
    case CompareOp::NE:
      return _mm256_xor_si256(_mm256_cmpeq_epi64(values, bound), all_ones);
    case CompareOp::LT:
      return _mm256_cmpgt_epi64(bound, values);
    case CompareOp::LE:
      return _mm256_xor_si256(_mm256_cmpgt_epi64(values, bound), all_ones);
    case CompareOp::GT:
      return _mm256_cmpgt_epi64(values, bound);
    case CompareOp::GE:
      return _mm256_xor_si256(_mm256_cmpgt_epi64(bound, values), all_ones);
  }
  return _mm256_setzero_si256();
}

SHANNON_TARGET_AVX2 inline uint64_t cmp_block_avx2_i32(const int32_t *vals, int32_t lower, int32_t upper,
                                                       const CmpRequest &req) {
  const __m256i lo = _mm256_set1_epi32(lower);
  const __m256i hi = _mm256_set1_epi32(upper);
  const __m256i all_ones = _mm256_set1_epi32(-1);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 8) {
    const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(vals + j));
    __m256i cmp = cmp_avx2_i32(values, lo, req.op);
    if (req.range) cmp = _mm256_and_si256(cmp, _mm256_xor_si256(_mm256_cmpgt_epi32(values, hi), all_ones));
    if (req.negate) cmp = _mm256_xor_si256(cmp, all_ones);
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(cmp)))) << j;
  }
  return mask;
}

SHANNON_TARGET_AVX2 inline uint64_t cmp_block_avx2_i64(const int64_t *vals, int64_t lower, int64_t upper,
                                                       const CmpRequest &req) {
  const __m256i lo = _mm256_set1_epi64x(lower);
  const __m256i hi = _mm256_set1_epi64x(upper);
  const __m256i all_ones = _mm256_set1_epi64x(-1);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 4) {
    const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(vals + j));
    __m256i cmp = cmp_avx2_i64(values, lo, req.op);
    if (req.range) cmp = _mm256_and_si256(cmp, _mm256_xor_si256(_mm256_cmpgt_epi64(values, hi), all_ones));
    if (req.negate) cmp = _mm256_xor_si256(cmp, all_ones);
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)))) << j;
  }
  return mask;
}

#endif  // AVX2

#if defined(SHANNON_HAVE_SSE2)

inline __m128i cmp_sse_i32(__m128i values, __m128i bound, CompareOp op) {
  const __m128i all_ones = _mm_set1_epi32(-1);
  switch (op) {
    case CompareOp::EQ:
      return _mm_cmpeq_epi32(values, bound);
    case CompareOp::NE:
      return _mm_xor_si128(_mm_cmpeq_epi32(values, bound), all_ones);
    case CompareOp::LT:
      return _mm_cmpgt_epi32(bound, values);
    case CompareOp::LE:
      return _mm_xor_si128(_mm_cmpgt_epi32(values, bound), all_ones);
    case CompareOp::GT:
      return _mm_cmpgt_epi32(values, bound);
    case CompareOp::GE:
      return _mm_xor_si128(_mm_cmpgt_epi32(bound, values), all_ones);
  }
  return _mm_setzero_si128();
}

inline uint64_t cmp_block_sse_i32(const int32_t *vals, int32_t lower, int32_t upper, const CmpRequest &req) {
  const __m128i lo = _mm_set1_epi32(lower);
  const __m128i hi = _mm_set1_epi32(upper);
  const __m128i all_ones = _mm_set1_epi32(-1);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 4) {
    const __m128i values = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vals + j));
    __m128i cmp = cmp_sse_i32(values, lo, req.op);
    if (req.range) cmp = _mm_and_si128(cmp, _mm_xor_si128(_mm_cmpgt_epi32(values, hi), all_ones));
    if (req.negate) cmp = _mm_xor_si128(cmp, all_ones);
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(cmp)))) << j;
  }
  return mask;
}

#if defined(SHANNON_HAVE_SSE42)

inline __m128i cmp_sse_i64(__m128i values, __m128i bound, CompareOp op) {
  const __m128i all_ones = _mm_set1_epi64x(-1);
  switch (op) {
    case CompareOp::EQ:
      return _mm_cmpeq_epi64(values, bound);
    case CompareOp::NE:
      return _mm_xor_si128(_mm_cmpeq_epi64(values, bound), all_ones);
    case CompareOp::LT:
      return _mm_cmpgt_epi64(bound, values);
    case CompareOp::LE:
      return _mm_xor_si128(_mm_cmpgt_epi64(values, bound), all_ones);
    case CompareOp::GT:
      return _mm_cmpgt_epi64(values, bound);
    case CompareOp::GE:
      return _mm_xor_si128(_mm_cmpgt_epi64(bound, values), all_ones);
  }
  return _mm_setzero_si128();
}

inline uint64_t cmp_block_sse_i64(const int64_t *vals, int64_t lower, int64_t upper, const CmpRequest &req) {
  const __m128i lo = _mm_set1_epi64x(lower);
  const __m128i hi = _mm_set1_epi64x(upper);
  const __m128i all_ones = _mm_set1_epi64x(-1);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 2) {
    const __m128i values = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vals + j));
    __m128i cmp = cmp_sse_i64(values, lo, req.op);
    if (req.range) cmp = _mm_and_si128(cmp, _mm_xor_si128(_mm_cmpgt_epi64(values, hi), all_ones));
    if (req.negate) cmp = _mm_xor_si128(cmp, all_ones);
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm_movemask_pd(_mm_castsi128_pd(cmp)))) << j;
  }
  return mask;
}

#endif  // SSE4.2
#endif  // SSE2

#if defined(SHANNON_HAVE_NEON)

inline uint8_t neon_lane_mask_u32(uint32x4_t mask) {
  alignas(16) uint32_t lanes[4];
  vst1q_u32(lanes, mask);
  uint8_t bits = 0;
  for (unsigned j = 0; j < 4; ++j)
    if (lanes[j] != 0) bits = static_cast<uint8_t>(bits | (1u << j));
  return bits;
}

inline uint64_t cmp_block_neon_i32(const int32_t *vals, int32_t lower, int32_t upper, const CmpRequest &req) {
  const int32x4_t lo = vdupq_n_s32(lower);
  const int32x4_t hi = vdupq_n_s32(upper);
  const uint32x4_t all_ones = vdupq_n_u32(0xFFFFFFFFu);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 4) {
    const int32x4_t values = vld1q_s32(vals + j);
    uint32x4_t cmp;
    switch (req.op) {
      case CompareOp::EQ:
        cmp = vceqq_s32(values, lo);
        break;
      case CompareOp::NE:
        cmp = veorq_u32(vceqq_s32(values, lo), all_ones);
        break;
      case CompareOp::LT:
        cmp = vcltq_s32(values, lo);
        break;
      case CompareOp::LE:
        cmp = vcleq_s32(values, lo);
        break;
      case CompareOp::GT:
        cmp = vcgtq_s32(values, lo);
        break;
      case CompareOp::GE:
        cmp = vcgeq_s32(values, lo);
        break;
      default:
        cmp = vdupq_n_u32(0);
        break;
    }
    if (req.range) cmp = vandq_u32(cmp, vcleq_s32(values, hi));
    if (req.negate) cmp = veorq_u32(cmp, all_ones);
    mask |= static_cast<uint64_t>(neon_lane_mask_u32(cmp)) << j;
  }
  return mask;
}

#if defined(SHANNON_HAVE_NEON_A64)
// AArch32 NEON has no 64-bit ordered compare; there cmp_block_scalar covers it.
inline uint8_t neon_lane_mask_u64(uint64x2_t mask) {
  alignas(16) uint64_t lanes[2];
  vst1q_u64(lanes, mask);
  uint8_t bits = 0;
  for (unsigned j = 0; j < 2; ++j)
    if (lanes[j] != 0) bits = static_cast<uint8_t>(bits | (1u << j));
  return bits;
}

inline uint64_t cmp_block_neon_i64(const int64_t *vals, int64_t lower, int64_t upper, const CmpRequest &req) {
  const int64x2_t lo = vdupq_n_s64(lower);
  const int64x2_t hi = vdupq_n_s64(upper);
  const uint64x2_t all_ones = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 2) {
    const int64x2_t values = vld1q_s64(vals + j);
    uint64x2_t cmp;
    switch (req.op) {
      case CompareOp::EQ:
        cmp = vceqq_s64(values, lo);
        break;
      case CompareOp::NE:
        cmp = veorq_u64(vceqq_s64(values, lo), all_ones);
        break;
      case CompareOp::LT:
        cmp = vcltq_s64(values, lo);
        break;
      case CompareOp::LE:
        cmp = vcleq_s64(values, lo);
        break;
      case CompareOp::GT:
        cmp = vcgtq_s64(values, lo);
        break;
      case CompareOp::GE:
        cmp = vcgeq_s64(values, lo);
        break;
      default:
        cmp = vdupq_n_u64(0);
        break;
    }
    if (req.range) cmp = vandq_u64(cmp, vcleq_s64(values, hi));
    if (req.negate) cmp = veorq_u64(cmp, all_ones);
    mask |= static_cast<uint64_t>(neon_lane_mask_u64(cmp)) << j;
  }
  return mask;
}
#endif  // NEON A64
#endif  // NEON

inline uint64_t cmp_block_i32(const int32_t *vals, int32_t lower, int32_t upper, const CmpRequest &req) {
  const SIMDType isa = simd_support();
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  if (isa == SIMDType::AVX2) return cmp_block_avx2_i32(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_SSE2)
  if (isa != SIMDType::NONE && isa != SIMDType::NEON) return cmp_block_sse_i32(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_NEON)
  if (isa == SIMDType::NEON) return cmp_block_neon_i32(vals, lower, upper, req);
#endif
  return cmp_block_scalar<int32_t>(vals, lower, upper, req);
}

inline uint64_t cmp_block_i64(const int64_t *vals, int64_t lower, int64_t upper, const CmpRequest &req) {
  const SIMDType isa = simd_support();
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  if (isa == SIMDType::AVX2) return cmp_block_avx2_i64(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_SSE42)
  if (isa != SIMDType::NONE && isa != SIMDType::NEON) return cmp_block_sse_i64(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_NEON_A64)
  if (isa == SIMDType::NEON) return cmp_block_neon_i64(vals, lower, upper, req);
#endif
  return cmp_block_scalar<int64_t>(vals, lower, upper, req);
}

// Writes one block's match bits, keeping the bits of the trailing byte that
// fall outside the requested rows.
inline void write_cmp_block_bits(uint64_t mask, size_t first, size_t count, uint8_t *out_bits) {
  for (size_t j = 0; j < count; j += 8) {
    const unsigned lanes = static_cast<unsigned>(std::min<size_t>(8, count - j));
    const uint8_t keep = static_cast<uint8_t>((1u << lanes) - 1u);
    uint8_t &byte = out_bits[(first + j) >> 3];
    byte = static_cast<uint8_t>((byte & ~keep) | ((mask >> j) & keep));
  }
}

// Floating-point compares are all ordered, so a NaN operand matches nothing and
// never satisfies NOT_EQUAL either.
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)

SHANNON_TARGET_AVX2 inline __m256d cmp_avx2_f64(__m256d values, __m256d bound, CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return _mm256_cmp_pd(values, bound, _CMP_EQ_OQ);
    case CompareOp::NE:
      return _mm256_cmp_pd(values, bound, _CMP_NEQ_OQ);
    case CompareOp::LT:
      return _mm256_cmp_pd(values, bound, _CMP_LT_OQ);
    case CompareOp::LE:
      return _mm256_cmp_pd(values, bound, _CMP_LE_OQ);
    case CompareOp::GT:
      return _mm256_cmp_pd(values, bound, _CMP_GT_OQ);
    case CompareOp::GE:
      return _mm256_cmp_pd(values, bound, _CMP_GE_OQ);
  }
  return _mm256_setzero_pd();
}

SHANNON_TARGET_AVX2 inline uint64_t cmp_block_avx2_f64(const double *vals, double lower, double upper,
                                                       const CmpRequest &req) {
  const __m256d lo = _mm256_set1_pd(lower);
  const __m256d hi = _mm256_set1_pd(upper);
  const __m256d all_ones = _mm256_castsi256_pd(_mm256_set1_epi64x(-1));
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 4) {
    const __m256d values = _mm256_loadu_pd(vals + j);
    __m256d cmp = cmp_avx2_f64(values, lo, req.op);
    if (req.range && req.negate)
      cmp = _mm256_or_pd(_mm256_cmp_pd(values, lo, _CMP_LT_OQ), _mm256_cmp_pd(values, hi, _CMP_GT_OQ));
    else {
      if (req.range) cmp = _mm256_and_pd(cmp, _mm256_cmp_pd(values, hi, _CMP_LE_OQ));
      if (req.negate) cmp = _mm256_xor_pd(cmp, all_ones);
    }
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm256_movemask_pd(cmp))) << j;
  }
  return mask;
}

#endif  // AVX2

#if defined(SHANNON_HAVE_SSE2)

inline uint64_t cmp_block_sse_f64(const double *vals, double lower, double upper, const CmpRequest &req) {
  const __m128d lo = _mm_set1_pd(lower);
  const __m128d hi = _mm_set1_pd(upper);
  const __m128d all_ones = _mm_castsi128_pd(_mm_set1_epi64x(-1));
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 2) {
    const __m128d values = _mm_loadu_pd(vals + j);
    __m128d cmp;
    switch (req.op) {
      case CompareOp::EQ:
        cmp = _mm_cmpeq_pd(values, lo);
        break;
      case CompareOp::NE:  // ordered, so a NaN row never matches
        cmp = _mm_andnot_pd(_mm_cmpeq_pd(values, lo), _mm_cmpord_pd(values, lo));
        break;
      case CompareOp::LT:
        cmp = _mm_cmplt_pd(values, lo);
        break;
      case CompareOp::LE:
        cmp = _mm_cmple_pd(values, lo);
        break;
      case CompareOp::GT:
        cmp = _mm_cmpgt_pd(values, lo);
        break;
      case CompareOp::GE:
        cmp = _mm_cmpge_pd(values, lo);
        break;
      default:
        cmp = _mm_setzero_pd();
        break;
    }
    if (req.range && req.negate)
      cmp = _mm_or_pd(_mm_cmplt_pd(values, lo), _mm_cmpgt_pd(values, hi));
    else {
      if (req.range) cmp = _mm_and_pd(cmp, _mm_cmple_pd(values, hi));
      if (req.negate) cmp = _mm_xor_pd(cmp, all_ones);
    }
    mask |= static_cast<uint64_t>(static_cast<unsigned>(_mm_movemask_pd(cmp))) << j;
  }
  return mask;
}

#endif  // SSE2

#if defined(SHANNON_HAVE_NEON_A64)

inline uint64_t cmp_block_neon_f64(const double *vals, double lower, double upper, const CmpRequest &req) {
  const float64x2_t lo = vdupq_n_f64(lower);
  const float64x2_t hi = vdupq_n_f64(upper);
  const uint64x2_t all_ones = vdupq_n_u64(0xFFFFFFFFFFFFFFFFull);
  uint64_t mask = 0;
  for (size_t j = 0; j < kCmpBlockRows; j += 2) {
    const float64x2_t values = vld1q_f64(vals + j);
    uint64x2_t cmp;
    switch (req.op) {
      case CompareOp::EQ:
        cmp = vceqq_f64(values, lo);
        break;
      case CompareOp::NE:
        // Ordered NE: unordered (NaN) lanes must not match.
        cmp = vandq_u64(veorq_u64(vceqq_f64(values, lo), all_ones),
                        vandq_u64(vceqq_f64(values, values), vceqq_f64(lo, lo)));
        break;
      case CompareOp::LT:
        cmp = vcltq_f64(values, lo);
        break;
      case CompareOp::LE:
        cmp = vcleq_f64(values, lo);
        break;
      case CompareOp::GT:
        cmp = vcgtq_f64(values, lo);
        break;
      case CompareOp::GE:
        cmp = vcgeq_f64(values, lo);
        break;
      default:
        cmp = vdupq_n_u64(0);
        break;
    }
    if (req.range && req.negate)
      cmp = vorrq_u64(vcltq_f64(values, lo), vcgtq_f64(values, hi));
    else {
      if (req.range) cmp = vandq_u64(cmp, vcleq_f64(values, hi));
      if (req.negate) cmp = veorq_u64(cmp, all_ones);
    }
    mask |= static_cast<uint64_t>(neon_lane_mask_u64(cmp)) << j;
  }
  return mask;
}

#endif  // NEON A64

inline uint64_t cmp_block_f64(const double *vals, double lower, double upper, const CmpRequest &req) {
  const SIMDType isa = simd_support();
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  if (isa == SIMDType::AVX2) return cmp_block_avx2_f64(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_SSE2)
  if (isa != SIMDType::NONE && isa != SIMDType::NEON) return cmp_block_sse_f64(vals, lower, upper, req);
#endif
#if defined(SHANNON_HAVE_NEON_A64)
  if (isa == SIMDType::NEON) return cmp_block_neon_f64(vals, lower, upper, req);
#endif
  return cmp_block_scalar<double>(vals, lower, upper, req);
}

template <typename T>
inline uint64_t cmp_block(const T *vals, T lower, T upper, const CmpRequest &req) {
  if constexpr (std::is_same_v<T, int32_t>)
    return cmp_block_i32(vals, lower, upper, req);
  else
    return cmp_block_i64(vals, lower, upper, req);
}

// Gathers `rows` cell pointers into fixed blocks, compares them and writes the
// matches as a LSB-first bitmap. `bias` XORs both sides, which is how an
// unsigned column keeps its ordering under signed compare instructions.
template <typename T>
inline size_t gather_compare_bitmap(const unsigned char *const *cells, size_t rows, T lower, T upper, T bias,
                                    const CmpRequest &req, uint8_t *out_bits) {
  if (cells == nullptr || out_bits == nullptr || rows == 0) return 0;
  const T lo = static_cast<T>(lower ^ bias);
  const T hi = static_cast<T>(upper ^ bias);
  alignas(32) T vals[kCmpBlockRows];
  size_t matched = 0;
  for (size_t first = 0; first < rows; first += kCmpBlockRows) {
    const size_t count = std::min(kCmpBlockRows, rows - first);
    uint64_t valid = 0;
    for (size_t j = 0; j < kCmpBlockRows; ++j) {
      if (j >= count) {
        vals[j] = bias;
        continue;
      }
      const unsigned char *cell = cells[first + j];
      T raw{};
      if (cell != nullptr) {
        std::memcpy(&raw, cell, sizeof(T));
        valid |= static_cast<uint64_t>(1) << j;
      }
      vals[j] = static_cast<T>(raw ^ bias);
    }
    uint64_t mask = cmp_block<T>(vals, lo, req.range ? hi : lo, req) & valid;
    if (count < kCmpBlockRows) mask &= (static_cast<uint64_t>(1) << count) - 1;
    matched += static_cast<size_t>(std::popcount(mask));
    write_cmp_block_bits(mask, first, count, out_bits);
  }
  return matched;
}

// Same loop for decoded floating-point cells. The decoder is only called for
// non-NULL rows, so a FLOAT/DOUBLE/DECIMAL reader never has to invent a value.
template <typename Decode>
inline size_t gather_compare_double_bitmap(const unsigned char *const *cells, size_t rows, double lower, double upper,
                                           const CmpRequest &req, Decode decode, uint8_t *out_bits) {
  if (cells == nullptr || out_bits == nullptr || rows == 0) return 0;
  alignas(32) double vals[kCmpBlockRows];
  size_t matched = 0;
  for (size_t first = 0; first < rows; first += kCmpBlockRows) {
    const size_t count = std::min(kCmpBlockRows, rows - first);
    uint64_t valid = 0;
    for (size_t j = 0; j < kCmpBlockRows; ++j) {
      if (j >= count) {
        vals[j] = 0.0;
        continue;
      }
      const unsigned char *cell = cells[first + j];
      if (cell != nullptr) {
        vals[j] = decode(cell);
        valid |= static_cast<uint64_t>(1) << j;
      } else {
        vals[j] = 0.0;
      }
    }
    uint64_t mask = cmp_block_f64(vals, lower, req.range ? upper : lower, req) & valid;
    if (count < kCmpBlockRows) mask &= (static_cast<uint64_t>(1) << count) - 1;
    matched += static_cast<size_t>(std::popcount(mask));
    write_cmp_block_bits(mask, first, count, out_bits);
  }
  return matched;
}

// Index-output variant for a contiguous column: same kernels, same null
// contract (bit set = NULL), pushes the matching row numbers.
template <typename T, typename Allocator>
inline size_t filter_compare_indexed(const T *data, const uint8_t *null_mask, size_t row_count, T value, CompareOp op,
                                     std::vector<size_t, Allocator> &out) {
  if (data == nullptr || row_count == 0) return 0;
  const CmpRequest req{op, false, false};
  size_t matched = 0;
  size_t i = 0;
  for (; i + kCmpBlockRows <= row_count; i += kCmpBlockRows) {
    uint64_t mask = cmp_block<T>(data + i, value, value, req);
    if (mask == 0) continue;
    if (null_mask != nullptr) {
      // kCmpBlockRows is a multiple of 8, so one block covers whole mask bytes.
      uint64_t valid = 0;
      for (unsigned byte = 0; byte < kCmpBlockRows / 8; ++byte)
        valid |= static_cast<uint64_t>(static_cast<uint8_t>(~null_mask[(i >> 3) + byte])) << (byte * 8);
      mask &= valid;
    }
    while (mask != 0) {
      out.push_back(i + static_cast<unsigned>(std::countr_zero(mask)));
      mask &= mask - 1;
      ++matched;
    }
  }
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i) && compare_scalar(data[i], value, op)) {
      out.push_back(i);
      ++matched;
    }
  return matched;
}

}  // namespace detail

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

template <typename Allocator>
inline size_t filter_compare_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                                   CompareOp op, std::vector<size_t, Allocator> &out) {
  return detail::filter_compare_indexed<int32_t>(data, null_mask, row_count, value, op, out);
}

template <typename Allocator>
inline size_t filter_compare_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count, int64_t value,
                                   CompareOp op, std::vector<size_t, Allocator> &out) {
  return detail::filter_compare_indexed<int64_t>(data, null_mask, row_count, value, op, out);
}

inline size_t filter_eq_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                              std::vector<size_t> &out) {
  return filter_compare_int32(data, null_mask, row_count, value, CompareOp::EQ, out);
}

// True when this width really runs vectorized on the current CPU. Every tier
// with a kernel covers all six operators, so only the width and the ISA decide;
// callers use this to skip setup that would not pay off.
inline bool integer_compare_simd(bool is_int32, CompareOp op) {
  (void)op;
  const auto isa = simd_support();
#if defined(SHANNON_HAVE_AVX2) || defined(SHANNON_RUNTIME_AVX2)
  if (isa == SIMDType::AVX2) return true;
#endif
  if (isa == SIMDType::NEON) {
    if (is_int32) {
#if defined(SHANNON_HAVE_NEON)
      return true;
#endif
    } else {
#if defined(SHANNON_HAVE_NEON_A64)
      return true;
#endif
    }
    return false;
  }
  if (isa == SIMDType::NONE) return false;
  if (is_int32) {
#if defined(SHANNON_HAVE_SSE2)
    return true;
#endif
  } else {
#if defined(SHANNON_HAVE_SSE42)
    return true;
#endif
  }
  return false;
}

// ============================================================================
// Gathered integer filters
// ============================================================================

inline size_t filter_compare_int32_bitmap(const unsigned char *const *cells, size_t rows, int32_t value, CompareOp op,
                                          bool is_unsigned, uint8_t *out_bits) {
  const int32_t bias = is_unsigned ? static_cast<int32_t>(0x80000000u) : 0;
  return detail::gather_compare_bitmap<int32_t>(cells, rows, value, value, bias, detail::CmpRequest{op, false, false},
                                                out_bits);
}

inline size_t filter_compare_int64_bitmap(const unsigned char *const *cells, size_t rows, int64_t value, CompareOp op,
                                          bool is_unsigned, uint8_t *out_bits) {
  const int64_t bias = is_unsigned ? static_cast<int64_t>(0x8000000000000000ull) : 0;
  return detail::gather_compare_bitmap<int64_t>(cells, rows, value, value, bias, detail::CmpRequest{op, false, false},
                                                out_bits);
}

inline size_t filter_range_int32_bitmap(const unsigned char *const *cells, size_t rows, int32_t lower, int32_t upper,
                                        bool is_unsigned, bool negate, uint8_t *out_bits) {
  const int32_t bias = is_unsigned ? static_cast<int32_t>(0x80000000u) : 0;
  return detail::gather_compare_bitmap<int32_t>(cells, rows, lower, upper, bias,
                                                detail::CmpRequest{CompareOp::GE, true, negate}, out_bits);
}

inline size_t filter_range_int64_bitmap(const unsigned char *const *cells, size_t rows, int64_t lower, int64_t upper,
                                        bool is_unsigned, bool negate, uint8_t *out_bits) {
  const int64_t bias = is_unsigned ? static_cast<int64_t>(0x8000000000000000ull) : 0;
  return detail::gather_compare_bitmap<int64_t>(cells, rows, lower, upper, bias,
                                                detail::CmpRequest{CompareOp::GE, true, negate}, out_bits);
}

inline size_t filter_null_bitmap(const unsigned char *const *cells, size_t rows, bool negate, uint8_t *out_bits) {
  if (cells == nullptr || out_bits == nullptr || rows == 0) return 0;
  size_t matched = 0;
  for (size_t j = 0; j < rows; j += 8) {
    const unsigned lanes = static_cast<unsigned>(std::min<size_t>(8, rows - j));
    const uint8_t keep = static_cast<uint8_t>((1u << lanes) - 1u);
    uint8_t bits = 0;
    for (unsigned lane = 0; lane < lanes; ++lane)
      if ((cells[j + lane] == nullptr) != negate) bits = static_cast<uint8_t>(bits | (1u << lane));
    uint8_t &byte = out_bits[j >> 3];
    byte = static_cast<uint8_t>((byte & ~keep) | bits);
    matched += static_cast<size_t>(std::popcount(static_cast<unsigned>(bits)));
  }
  return matched;
}

template <typename Decode>
inline size_t filter_compare_double_bitmap(const unsigned char *const *cells, size_t rows, double value, CompareOp op,
                                           Decode decode, uint8_t *out_bits) {
  return detail::gather_compare_double_bitmap(cells, rows, value, value, detail::CmpRequest{op, false, false}, decode,
                                              out_bits);
}

template <typename Decode>
inline size_t filter_range_double_bitmap(const unsigned char *const *cells, size_t rows, double lower, double upper,
                                         bool negate, Decode decode, uint8_t *out_bits) {
  return detail::gather_compare_double_bitmap(cells, rows, lower, upper,
                                              detail::CmpRequest{CompareOp::GE, true, negate}, decode, out_bits);
}
}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_IMPL_H__
