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

// ============================================================================
// SIMD Detection Implementation
// ============================================================================

inline SIMDType detect_simd_support() {
#if defined(SHANNON_X86_PLATFORM)
#if defined(__AVX2__)
  return SIMDType::AVX2;
#elif defined(__AVX__)
  return SIMDType::AVX;
#elif defined(__SSE4_2__)
  return SIMDType::SSE4_2;
#elif defined(__SSE4_1__)
  return SIMDType::SSE4_1;
#elif defined(__SSE2__)
  return SIMDType::SSE2;
#elif defined(__SSE__)
  return SIMDType::SSE;
#endif
#elif defined(SHANNON_ARM_PLATFORM) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
  return SIMDType::NEON;
#endif
  return SIMDType::NONE;
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

// ============================================================================
// SSE2 Helper: horizontal reductions
// ============================================================================

#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))

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

inline int64_t horizontal_sum_epi64(__m256i vec) {
  __m128i low = _mm256_extracti128_si256(vec, 0);
  __m128i high = _mm256_extracti128_si256(vec, 1);
  __m128i sum128 = _mm_add_epi64(low, high);
  return horizontal_sum_epi64(sum128);
}

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
  __m128i shuf = _mm_srli_si128(v, 8);
  v = _mm_min_epi32(v, shuf);
  shuf = _mm_srli_si128(v, 4);
  v = _mm_min_epi32(v, shuf);
  return _mm_cvtsi128_si32(v);
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
  __m128i shuf = _mm_srli_si128(v, 8);
  v = _mm_max_epi32(v, shuf);
  shuf = _mm_srli_si128(v, 4);
  v = _mm_max_epi32(v, shuf);
  return _mm_cvtsi128_si32(v);
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
#if defined(SHANNON_X86_PLATFORM) && defined(__SSE2__)
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

inline size_t popcount_bitmap_avx2(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  size_t sum = 0, i = 0;
  for (; i + 16 <= bytes; i += 16) sum += vaddvq_u8(vcntq_u8(vld1q_u8(data + i)));
  for (; i < bytes; ++i) sum += std::popcount(static_cast<unsigned>(data[i]));
  return sum;
#else
  return popcount_bitmap_common(data, bytes);
#endif
}

inline size_t popcount_bitmap(const std::vector<uint8_t> &bm) {
  if (bm.empty()) return 0;
  static SIMDType simd_type = detect_simd_support();
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
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

template <typename T>
size_t filter_generic(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                      std::vector<size_t> &out) {
  size_t cnt = 0;
  for (size_t i = 0; i < row_count; ++i)
    if (!is_null(null_mask, i) && predicate(data[i])) {
      out.push_back(i);
      ++cnt;
    }
  return cnt;
}

// ============================================================================
// SSE2 Sum
// ============================================================================

inline float sum_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0.0f;
  __m128 sv = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      chunk = _mm_and_ps(chunk, _mm_castsi128_ps(im));
    }
    sv = _mm_add_ps(sv, chunk);
  }
  float sum = horizontal_sum_ps(sv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline double sum_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0.0;
  __m128d sv = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 2; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m128i im = _mm_set_epi64x((vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm_and_pd(chunk, _mm_castsi128_pd(im));
    }
    sv = _mm_add_pd(sv, chunk);
  }
  double sum = horizontal_sum_pd(sv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int32_t sum_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0;
  __m128i sv = _mm_setzero_si128();
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      chunk = _mm_and_si128(chunk, im);
    }
    sv = _mm_add_epi32(sv, chunk);
  }
  int32_t sum = horizontal_sum_epi32(sv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int64_t sum_sse2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0;
  __m128i sv = _mm_setzero_si128();
  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 2; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m128i im = _mm_set_epi64x((vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm_and_si128(chunk, im);
    }
    sv = _mm_add_epi64(sv, chunk);
  }
  alignas(16) int64_t tmp[2];
  _mm_store_si128(reinterpret_cast<__m128i *>(tmp), sv);
  int64_t sum = tmp[0] + tmp[1];
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// SSE2 Min
// ============================================================================

inline float min_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<float>::max();
  __m128 mv = _mm_set1_ps(std::numeric_limits<float>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128 neutral = _mm_set1_ps(std::numeric_limits<float>::max());
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      __m128 fm = _mm_castsi128_ps(im);
      chunk = _mm_or_ps(_mm_and_ps(fm, chunk), _mm_andnot_ps(fm, neutral));
    } else {
      found = true;
    }
    mv = _mm_min_ps(mv, chunk);
  }
  float val = horizontal_min_ps(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline double min_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<double>::max();
  __m128d mv = _mm_set1_pd(std::numeric_limits<double>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 2; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128d neutral = _mm_set1_pd(std::numeric_limits<double>::max());
      __m128i im = _mm_set_epi64x((vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      __m128d fm = _mm_castsi128_pd(im);
      chunk = _mm_or_pd(_mm_and_pd(fm, chunk), _mm_andnot_pd(fm, neutral));
    } else {
      found = true;
    }
    mv = _mm_min_pd(mv, chunk);
  }
  double val = horizontal_min_pd(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<double>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline int32_t min_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<int32_t>::max();
  __m128i mv = _mm_set1_epi32(std::numeric_limits<int32_t>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128i neutral = _mm_set1_epi32(std::numeric_limits<int32_t>::max());
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      chunk = _mm_or_si128(_mm_and_si128(im, chunk), _mm_andnot_si128(im, neutral));
    } else {
      found = true;
    }
    mv = _mm_min_epi32(mv, chunk);
  }
  int32_t val = horizontal_min_epi32(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int32_t>::max();
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
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<float>::lowest();
  __m128 mv = _mm_set1_ps(std::numeric_limits<float>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128 neutral = _mm_set1_ps(std::numeric_limits<float>::lowest());
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      __m128 fm = _mm_castsi128_ps(im);
      chunk = _mm_or_ps(_mm_and_ps(fm, chunk), _mm_andnot_ps(fm, neutral));
    } else {
      found = true;
    }
    mv = _mm_max_ps(mv, chunk);
  }
  float val = horizontal_max_ps(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline double max_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<double>::lowest();
  __m128d mv = _mm_set1_pd(std::numeric_limits<double>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 2; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128d neutral = _mm_set1_pd(std::numeric_limits<double>::lowest());
      __m128i im = _mm_set_epi64x((vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      __m128d fm = _mm_castsi128_pd(im);
      chunk = _mm_or_pd(_mm_and_pd(fm, chunk), _mm_andnot_pd(fm, neutral));
    } else {
      found = true;
    }
    mv = _mm_max_pd(mv, chunk);
  }
  double val = horizontal_max_pd(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<double>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline int32_t max_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<int32_t>::lowest();
  __m128i mv = _mm_set1_epi32(std::numeric_limits<int32_t>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m128i neutral = _mm_set1_epi32(std::numeric_limits<int32_t>::lowest());
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      chunk = _mm_or_si128(_mm_and_si128(im, chunk), _mm_andnot_si128(im, neutral));
    } else {
      found = true;
    }
    mv = _mm_max_epi32(mv, chunk);
  }
  int32_t val = horizontal_max_epi32(mv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int32_t>::lowest();
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

inline float sum_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0.0f;
  __m256 sv = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256 chunk = _mm256_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_and_ps(chunk, _mm256_castsi256_ps(im));
    }
    sv = _mm256_add_ps(sv, chunk);
  }
  __m128 s128 = _mm_add_ps(_mm256_extractf128_ps(sv, 0), _mm256_extractf128_ps(sv, 1));
  float sum = horizontal_sum_ps(s128);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_sse2_float(data, null_mask, row_count);
#endif
}

inline double sum_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0.0;
  __m256d sv = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256d chunk = _mm256_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_and_pd(chunk, _mm256_castsi256_pd(im));
    }
    sv = _mm256_add_pd(sv, chunk);
  }
  __m128d s128 = _mm_add_pd(_mm256_extractf128_pd(sv, 0), _mm256_extractf128_pd(sv, 1));
  double sum = horizontal_sum_pd(s128);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_sse2_double(data, null_mask, row_count);
#endif
}

inline int32_t sum_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0;
  __m256i sv = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_and_si256(chunk, im);
    }
    sv = _mm256_add_epi32(sv, chunk);
  }
  __m128i s128 = _mm_add_epi32(_mm256_extracti128_si256(sv, 0), _mm256_extracti128_si256(sv, 1));
  int32_t sum = horizontal_sum_epi32(s128);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_sse2_int32(data, null_mask, row_count);
#endif
}

inline int64_t sum_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0;
  __m256i sv = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_and_si256(chunk, im);
    }
    sv = _mm256_add_epi64(sv, chunk);
  }
  int64_t sum = horizontal_sum_epi64(sv);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// AVX2 Min
// ============================================================================

inline float min_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<float>::max();
  __m256 mv = _mm256_set1_ps(std::numeric_limits<float>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256 chunk = _mm256_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256 neutral = _mm256_set1_ps(std::numeric_limits<float>::max());
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_blendv_ps(neutral, chunk, _mm256_castsi256_ps(im));
    } else {
      found = true;
    }
    mv = _mm256_min_ps(mv, chunk);
  }
  __m128 lo = _mm256_extractf128_ps(mv, 0), hi = _mm256_extractf128_ps(mv, 1);
  float val = horizontal_min_ps(_mm_min_ps(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::max();
#else
  return min_sse2_float(data, null_mask, row_count);
#endif
}

inline double min_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<double>::max();
  __m256d mv = _mm256_set1_pd(std::numeric_limits<double>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256d chunk = _mm256_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256d neutral = _mm256_set1_pd(std::numeric_limits<double>::max());
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_blendv_pd(neutral, chunk, _mm256_castsi256_pd(im));
    } else {
      found = true;
    }
    mv = _mm256_min_pd(mv, chunk);
  }
  __m128d lo = _mm256_extractf128_pd(mv, 0), hi = _mm256_extractf128_pd(mv, 1);
  double val = horizontal_min_pd(_mm_min_pd(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<double>::max();
#else
  return min_sse2_double(data, null_mask, row_count);
#endif
}

inline int32_t min_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<int32_t>::max();
  __m256i mv = _mm256_set1_epi32(std::numeric_limits<int32_t>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256i neutral = _mm256_set1_epi32(std::numeric_limits<int32_t>::max());
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_blendv_epi8(neutral, chunk, im);
    } else {
      found = true;
    }
    mv = _mm256_min_epi32(mv, chunk);
  }
  __m128i lo = _mm256_extracti128_si256(mv, 0), hi = _mm256_extracti128_si256(mv, 1);
  int32_t val = horizontal_min_epi32(_mm_min_epi32(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int32_t>::max();
#else
  return min_sse2_int32(data, null_mask, row_count);
#endif
}

// AVX2 int64 min: _mm256_cmpgt_epi64 available, use blendv to select smaller
inline int64_t min_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<int64_t>::max();
  __m256i mv = _mm256_set1_epi64x(std::numeric_limits<int64_t>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256i neutral = _mm256_set1_epi64x(std::numeric_limits<int64_t>::max());
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_blendv_epi8(neutral, chunk, im);
    } else {
      found = true;
    }
    // chunk < mv  →  gt = (mv > chunk), select chunk where gt is set
    __m256i gt = _mm256_cmpgt_epi64(mv, chunk);
    mv = _mm256_blendv_epi8(mv, chunk, gt);
  }
  alignas(32) int64_t tmp[4];
  _mm256_store_si256(reinterpret_cast<__m256i *>(tmp), mv);
  int64_t val = *std::min_element(tmp, tmp + 4);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int64_t>::max();
#else
  return min_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// AVX2 Max
// ============================================================================

inline float max_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<float>::lowest();
  __m256 mv = _mm256_set1_ps(std::numeric_limits<float>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256 chunk = _mm256_loadu_ps(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256 neutral = _mm256_set1_ps(std::numeric_limits<float>::lowest());
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_blendv_ps(neutral, chunk, _mm256_castsi256_ps(im));
    } else {
      found = true;
    }
    mv = _mm256_max_ps(mv, chunk);
  }
  __m128 lo = _mm256_extractf128_ps(mv, 0), hi = _mm256_extractf128_ps(mv, 1);
  float val = horizontal_max_ps(_mm_max_ps(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::lowest();
#else
  return max_sse2_float(data, null_mask, row_count);
#endif
}

inline double max_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<double>::lowest();
  __m256d mv = _mm256_set1_pd(std::numeric_limits<double>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256d chunk = _mm256_loadu_pd(data + i);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256d neutral = _mm256_set1_pd(std::numeric_limits<double>::lowest());
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_blendv_pd(neutral, chunk, _mm256_castsi256_pd(im));
    } else {
      found = true;
    }
    mv = _mm256_max_pd(mv, chunk);
  }
  __m128d lo = _mm256_extractf128_pd(mv, 0), hi = _mm256_extractf128_pd(mv, 1);
  double val = horizontal_max_pd(_mm_max_pd(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<double>::lowest();
#else
  return max_sse2_double(data, null_mask, row_count);
#endif
}

inline int32_t max_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<int32_t>::lowest();
  __m256i mv = _mm256_set1_epi32(std::numeric_limits<int32_t>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 8 <= row_count; i += 8) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 8; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256i neutral = _mm256_set1_epi32(std::numeric_limits<int32_t>::lowest());
      __m256i im =
          _mm256_set_epi32((vm & 0x80) ? -1 : 0, (vm & 0x40) ? -1 : 0, (vm & 0x20) ? -1 : 0, (vm & 0x10) ? -1 : 0,
                           (vm & 0x08) ? -1 : 0, (vm & 0x04) ? -1 : 0, (vm & 0x02) ? -1 : 0, (vm & 0x01) ? -1 : 0);
      chunk = _mm256_blendv_epi8(neutral, chunk, im);
    } else {
      found = true;
    }
    mv = _mm256_max_epi32(mv, chunk);
  }
  __m128i lo = _mm256_extracti128_si256(mv, 0), hi = _mm256_extracti128_si256(mv, 1);
  int32_t val = horizontal_max_epi32(_mm_max_epi32(lo, hi));
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int32_t>::lowest();
#else
  return max_sse2_int32(data, null_mask, row_count);
#endif
}

inline int64_t max_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return std::numeric_limits<int64_t>::lowest();
  __m256i mv = _mm256_set1_epi64x(std::numeric_limits<int64_t>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          vm |= (1 << j);
          found = true;
        }
      __m256i neutral = _mm256_set1_epi64x(std::numeric_limits<int64_t>::lowest());
      __m256i im =
          _mm256_set_epi64x((vm & 8) ? -1LL : 0, (vm & 4) ? -1LL : 0, (vm & 2) ? -1LL : 0, (vm & 1) ? -1LL : 0);
      chunk = _mm256_blendv_epi8(neutral, chunk, im);
    } else {
      found = true;
    }
    // chunk > mv  →  gt = (chunk > mv), select chunk where gt is set
    __m256i gt = _mm256_cmpgt_epi64(chunk, mv);
    mv = _mm256_blendv_epi8(mv, chunk, gt);
  }
  alignas(32) int64_t tmp[4];
  _mm256_store_si256(reinterpret_cast<__m256i *>(tmp), mv);
  int64_t val = *std::max_element(tmp, tmp + 4);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<int64_t>::lowest();
#else
  return max_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// NEON Sum / Min / Max
// ============================================================================

inline float sum_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return 0.0f;
  float32x4_t sv = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);
    if (null_mask) {
      uint32x4_t mask = vdupq_n_u32(0);
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
      chunk = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(chunk), mask));
    }
    sv = vaddq_f32(sv, chunk);
  }
  float32x2_t s2 = vadd_f32(vget_high_f32(sv), vget_low_f32(sv));
  float sum = vget_lane_f32(vpadd_f32(s2, s2), 0);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) sum += data[i];
  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline float min_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return std::numeric_limits<float>::max();
  float32x4_t mv = vdupq_n_f32(std::numeric_limits<float>::max());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);
    if (null_mask) {
      uint32x4_t mask = vdupq_n_u32(0);
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
          found = true;
        }
      chunk = vbslq_f32(mask, chunk, vdupq_n_f32(std::numeric_limits<float>::max()));
    } else {
      found = true;
    }
    mv = vminq_f32(mv, chunk);
  }
  float32x2_t m2 = vmin_f32(vget_high_f32(mv), vget_low_f32(mv));
  float val = vget_lane_f32(vpmin_f32(m2, m2), 0);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline float max_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return std::numeric_limits<float>::lowest();
  float32x4_t mv = vdupq_n_f32(std::numeric_limits<float>::lowest());
  bool found = false;
  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);
    if (null_mask) {
      uint32x4_t mask = vdupq_n_u32(0);
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) {
          mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
          found = true;
        }
      chunk = vbslq_f32(mask, chunk, vdupq_n_f32(std::numeric_limits<float>::lowest()));
    } else {
      found = true;
    }
    mv = vmaxq_f32(mv, chunk);
  }
  float32x2_t m2 = vmax_f32(vget_high_f32(mv), vget_low_f32(mv));
  float val = vget_lane_f32(vpmax_f32(m2, m2), 0);
  for (; i < row_count; ++i)
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > val) {
        val = data[i];
        found = true;
      }
    }
  return found ? val : std::numeric_limits<float>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// count_non_null
// ============================================================================

inline size_t count_non_null_generic(const uint8_t *null_mask, size_t row_count) {
  if (!null_mask) return row_count;
  size_t cnt = 0;
  for (size_t i = 0; i < row_count; ++i)
    if (!is_null(null_mask, i)) ++cnt;
  return cnt;
}

inline size_t count_non_null_sse2(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (!null_mask) return row_count;
  size_t null_cnt = 0;
  size_t bytes = (row_count + 7) / 8, i = 0;
  for (; i + 16 <= bytes; i += 16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(null_mask + i));
    alignas(16) uint8_t tmp[16];
    _mm_store_si128(reinterpret_cast<__m128i *>(tmp), v);
    for (int j = 0; j < 16; ++j) null_cnt += std::popcount(static_cast<unsigned>(tmp[j]));
  }
  for (; i < bytes; ++i) null_cnt += std::popcount(static_cast<unsigned>(null_mask[i]));
  return row_count - null_cnt;
#else
  return count_non_null_generic(null_mask, row_count);
#endif
}

inline size_t count_non_null_sse4(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE4_1__) || defined(__SSE4_2__))
  if (!null_mask) return row_count;
  size_t null_cnt = 0;
  size_t bytes = (row_count + 7) / 8, i = 0;
  for (; i + 16 <= bytes; i += 16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(null_mask + i));
    alignas(16) uint64_t tmp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(tmp), v);
    null_cnt += _mm_popcnt_u64(tmp[0]) + _mm_popcnt_u64(tmp[1]);
  }
  for (; i < bytes; ++i) null_cnt += std::popcount(static_cast<unsigned>(null_mask[i]));
  return row_count - null_cnt;
#else
  return count_non_null_sse2(null_mask, row_count);
#endif
}

inline size_t count_non_null_avx2(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (!null_mask) return row_count;
  size_t null_cnt = 0;
  size_t bytes = (row_count + 7) / 8, i = 0;
  if (bytes >= 32) {
    alignas(32) static const uint8_t lut[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    __m256i vlut = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i *>(lut)));
    __m256i mask_low = _mm256_set1_epi8(0x0F);
    for (; i + 32 <= bytes; i += 32) {
      __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(null_mask + i));
      __m256i lo = _mm256_shuffle_epi8(vlut, _mm256_and_si256(v, mask_low));
      __m256i hi = _mm256_shuffle_epi8(vlut, _mm256_and_si256(_mm256_srli_epi16(v, 4), mask_low));
      __m256i s = _mm256_sad_epu8(_mm256_add_epi8(lo, hi), _mm256_setzero_si256());
      alignas(32) uint64_t tmp[4];
      _mm256_store_si256(reinterpret_cast<__m256i *>(tmp), s);
      null_cnt += tmp[0] + tmp[1] + tmp[2] + tmp[3];
    }
  }
  for (; i < bytes; ++i) null_cnt += std::popcount(static_cast<unsigned>(null_mask[i]));
  return row_count - null_cnt;
#else
  return count_non_null_sse4(null_mask, row_count);
#endif
}

inline size_t count_non_null_neon(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (!null_mask) return row_count;
  size_t null_cnt = 0;
  size_t bytes = (row_count + 7) / 8, i = 0;
  for (; i + 16 <= bytes; i += 16) null_cnt += vaddvq_u8(vcntq_u8(vld1q_u8(null_mask + i)));
  for (; i < bytes; ++i) null_cnt += std::popcount(static_cast<unsigned>(null_mask[i]));
  return row_count - null_cnt;
#else
  return count_non_null_generic(null_mask, row_count);
#endif
}

// ============================================================================
// Filter (SSE2 int32 equality specialization)
// ============================================================================

inline size_t filter_sse2_int32_eq(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                                   std::vector<size_t> &out) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  size_t cnt = 0;
  size_t i = 0;
  __m128i target = _mm_set1_epi32(value);
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    __m128i cmp = _mm_cmpeq_epi32(chunk, target);
    if (null_mask) {
      uint32_t vm = 0;
      for (int j = 0; j < 4; ++j)
        if (!is_null(null_mask, i + j)) vm |= (1 << j);
      __m128i im = _mm_set_epi32((vm & 8) ? -1 : 0, (vm & 4) ? -1 : 0, (vm & 2) ? -1 : 0, (vm & 1) ? -1 : 0);
      cmp = _mm_and_si128(cmp, im);
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

// ============================================================================
// High-level dispatch
// ============================================================================

template <typename T>
T sum(const T *data, const uint8_t *null_mask, size_t row_count) {
  static SIMDType simd_type = detect_simd_support();
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) return sum_neon_float(data, null_mask, row_count);
      break;
#endif
    default:
      break;
  }
  return sum_generic<T>(data, null_mask, row_count);
}

template <typename T>
T min(const T *data, const uint8_t *null_mask, size_t row_count) {
  static SIMDType simd_type = detect_simd_support();
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) return min_neon_float(data, null_mask, row_count);
      break;
#endif
    default:
      break;
  }
  return min_generic<T>(data, null_mask, row_count);
}

template <typename T>
T max(const T *data, const uint8_t *null_mask, size_t row_count) {
  static SIMDType simd_type = detect_simd_support();
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) return max_neon_float(data, null_mask, row_count);
      break;
#endif
    default:
      break;
  }
  return max_generic<T>(data, null_mask, row_count);
}

inline size_t count_non_null(const uint8_t *null_mask, size_t row_count) {
  static SIMDType simd_type = detect_simd_support();
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
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
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
  static SIMDType simd_type = detect_simd_support();
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
}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_IMPL_H__