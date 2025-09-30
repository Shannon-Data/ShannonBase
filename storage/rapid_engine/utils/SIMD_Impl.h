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

// Null mask semantics: bit=1 means NULL, bit=0 means NOT NULL
inline bool is_null(const uint8_t *null_mask, size_t index) {
  return null_mask && (null_mask[index / 8] & (1 << (index % 8)));
}

// Get a byte from null mask. Returns 0x00 (all non-NULL) if null_mask is nullptr
inline uint8_t get_null_mask_byte(const uint8_t *null_mask, size_t byte_index) {
  return null_mask ? null_mask[byte_index] : 0x00;
}

// ============================================================================
// SIMD Helper Functions
// ============================================================================

#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
// Horizontal sum operations
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

// Horizontal min operations
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

// Horizontal max operations
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
#endif

// ============================================================================
// Bitmap Operations Implementation
// ============================================================================

inline size_t popcount_bitmap_common(const uint8_t *data, size_t bytes) {
  size_t sum = 0;
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, sizeof(v));
    sum += std::popcount(v);
  }
  for (; i < bytes; ++i) {
    sum += std::popcount(static_cast<unsigned>(data[i]));
  }
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
      __m128i sum_16 = _mm_sad_epu8(t3, _mm_setzero_si128());
      total = _mm_add_epi64(total, sum_16);
    }

    alignas(16) uint64_t temp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(temp), total);
    sum = temp[0] + temp[1];
  }

  for (; i < bytes; ++i) {
    sum += std::popcount(static_cast<unsigned>(data[i]));
  }

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
    alignas(32) static const uint8_t popcount_lut[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    __m256i lut = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i *>(popcount_lut)));
    __m256i mask_low = _mm256_set1_epi8(0x0F);
    __m256i total = _mm256_setzero_si256();

    for (; i + 32 <= bytes; i += 32) {
      __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
      __m256i low = _mm256_and_si256(v, mask_low);
      __m256i pop_low = _mm256_shuffle_epi8(lut, low);
      __m256i high = _mm256_and_si256(_mm256_srli_epi16(v, 4), mask_low);
      __m256i pop_high = _mm256_shuffle_epi8(lut, high);
      __m256i pop_sum = _mm256_add_epi8(pop_low, pop_high);
      __m256i sum_16 = _mm256_sad_epu8(pop_sum, _mm256_setzero_si256());
      total = _mm256_add_epi64(total, sum_16);
    }

    alignas(32) uint64_t temp[4];
    _mm256_store_si256(reinterpret_cast<__m256i *>(temp), total);
    sum = temp[0] + temp[1] + temp[2] + temp[3];
  }

  if (i < bytes) {
    sum += popcount_bitmap_sse2(data + i, bytes - i);
  }

  return sum;
#else
  return popcount_bitmap_sse2(data, bytes);
#endif
}

inline size_t popcount_bitmap_neon(const uint8_t *data, size_t bytes) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  size_t sum = 0;
  size_t i = 0;

  if (bytes >= 16) {
    for (; i + 16 <= bytes; i += 16) {
      uint8x16_t v = vld1q_u8(data + i);
      sum += vaddvq_u8(vcntq_u8(v));
    }
  }

  for (; i < bytes; ++i) {
    sum += std::popcount(static_cast<unsigned>(data[i]));
  }

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
// Arithmetic Operations - Generic Implementations
// ============================================================================

template <typename T>
T sum_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  T sum = 0;
  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }
  return sum;
}

template <typename T>
T min_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  if (row_count == 0) return std::numeric_limits<T>::max();

  T min_val = std::numeric_limits<T>::max();
  bool found = false;

  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < min_val) {
        min_val = data[i];
        found = true;
      }
    }
  }

  return found ? min_val : std::numeric_limits<T>::max();
}

template <typename T>
T max_generic(const T *data, const uint8_t *null_mask, size_t row_count) {
  if (row_count == 0) return std::numeric_limits<T>::lowest();

  T max_val = std::numeric_limits<T>::lowest();
  bool found = false;

  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > max_val) {
        max_val = data[i];
        found = true;
      }
    }
  }

  return found ? max_val : std::numeric_limits<T>::lowest();
}

// ============================================================================
// Arithmetic Operations - SSE2 Implementations
// ============================================================================

inline float sum_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0.0f;

  if (!null_mask) {
    // Fast path: no NULL values
    __m128 sum_vec = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      sum_vec = _mm_add_ps(sum_vec, _mm_loadu_ps(data + i));
    }
    float sum = horizontal_sum_ps(sum_vec);
    for (; i < row_count; ++i) {
      sum += *(const float *)(data + i);
    }
    return sum;
  }

  __m128 sum_vec = _mm_setzero_ps();
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);

    // Extract NULL status for 4 elements (bit=1 means NULL)
    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      // Set bit if NOT NULL
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    // Build SIMD mask: 0xFFFFFFFF for valid, 0 for NULL (to be zeroed)
    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    // Zero out NULL positions
    chunk = _mm_and_ps(chunk, _mm_castsi128_ps(imask));
    sum_vec = _mm_add_ps(sum_vec, chunk);
  }

  float sum = horizontal_sum_ps(sum_vec);

  // Process remaining elements
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += *(const float *)(data + i);
    }
  }

  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline double sum_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0.0;

  if (!null_mask) {
    __m128d sum_vec = _mm_setzero_pd();
    size_t i = 0;
    for (; i + 2 <= row_count; i += 2) {
      sum_vec = _mm_add_pd(sum_vec, _mm_loadu_pd(data + i));
    }
    double sum = horizontal_sum_pd(sum_vec);
    for (; i < row_count; ++i) {
      sum += *(const double *)(data + i);
    }
    return sum;
  }

  __m128d sum_vec = _mm_setzero_pd();
  size_t i = 0;

  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 2; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m128i imask = _mm_set_epi64x((valid_mask & 2) ? -1LL : 0, (valid_mask & 1) ? -1LL : 0);

    chunk = _mm_and_pd(chunk, _mm_castsi128_pd(imask));
    sum_vec = _mm_add_pd(sum_vec, chunk);
  }

  double result = horizontal_sum_pd(sum_vec);

  // the remains
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      result += *(const double *)(data + i);
    }
  }

  return result;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int32_t sum_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0;

  if (!null_mask) {
    __m128i sum_vec = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      sum_vec = _mm_add_epi32(sum_vec, _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i)));
    }
    int32_t sum = horizontal_sum_epi32(sum_vec);
    for (; i < row_count; ++i) {
      sum += *(const int32_t *)(data + i);
    }
    return sum;
  }

  __m128i sum_vec = _mm_setzero_si128();
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    chunk = _mm_and_si128(chunk, imask);
    sum_vec = _mm_add_epi32(sum_vec, chunk);
  }

  int32_t result = horizontal_sum_epi32(sum_vec);

  // the remains.
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      result += *(const int32_t *)(data + i);
    }
  }

  return result;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline int64_t sum_sse2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return 0;

  if (!null_mask) {
    __m128i sum_vec = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 2 <= row_count; i += 2) {
      sum_vec = _mm_add_epi64(sum_vec, _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i)));
    }
    alignas(16) int64_t temp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(temp), sum_vec);
    int64_t sum = temp[0] + temp[1];
    for (; i < row_count; ++i) {
      sum += *(const int64_t *)(data + i);
    }
    return sum;
  }

  __m128i sum_vec = _mm_setzero_si128();
  size_t i = 0;

  for (; i + 2 <= row_count; i += 2) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 2; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m128i imask = _mm_set_epi64x((valid_mask & 2) ? -1LL : 0, (valid_mask & 1) ? -1LL : 0);

    chunk = _mm_and_si128(chunk, imask);
    sum_vec = _mm_add_epi64(sum_vec, chunk);
  }

  alignas(16) int64_t temp[2];
  _mm_store_si128(reinterpret_cast<__m128i *>(temp), sum_vec);
  int64_t result = temp[0] + temp[1];

  // the remains.
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      result += *(const int64_t *)(data + i);
    }
  }

  return result;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// Min/Max Operations - SSE2 Implementations
// ============================================================================

inline float min_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<float>::max();

  if (!null_mask) {
    __m128 min_vec = _mm_set1_ps(std::numeric_limits<float>::max());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      min_vec = _mm_min_ps(min_vec, _mm_loadu_ps(data + i));
    }
    float min_val = horizontal_min_ps(min_vec);
    for (; i < row_count; ++i) {
      if (data[i] < min_val) min_val = data[i];
    }
    return min_val;
  }

  __m128 min_vec = _mm_set1_ps(std::numeric_limits<float>::max());
  bool found = false;

  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    // Replace NULL elements with max value to not affect min calculation
    __m128 max_val = _mm_set1_ps(std::numeric_limits<float>::max());
    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    __m128 fmask = _mm_castsi128_ps(imask);
    // blend: chunk where valid, max where NULL
    chunk = _mm_or_ps(_mm_and_ps(fmask, chunk), _mm_andnot_ps(fmask, max_val));

    min_vec = _mm_min_ps(min_vec, chunk);
  }

  float min_val = horizontal_min_ps(min_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < min_val) {
        min_val = data[i];
        found = true;
      }
    }
  }

  return found ? min_val : std::numeric_limits<float>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline double min_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<double>::max();

  if (!null_mask) {
    __m128d min_vec = _mm_set1_pd(std::numeric_limits<double>::max());
    size_t i = 0;
    for (; i + 2 <= row_count; i += 2) {
      min_vec = _mm_min_pd(min_vec, _mm_loadu_pd(data + i));
    }
    double min_val = horizontal_min_pd(min_vec);
    for (; i < row_count; ++i) {
      if (data[i] < min_val) min_val = data[i];
    }
    return min_val;
  }

  __m128d min_vec = _mm_set1_pd(std::numeric_limits<double>::max());
  bool found = false;

  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 2; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    __m128d max_val = _mm_set1_pd(std::numeric_limits<double>::max());
    __m128i imask = _mm_set_epi64x((valid_mask & 2) ? -1LL : 0, (valid_mask & 1) ? -1LL : 0);

    __m128d fmask = _mm_castsi128_pd(imask);
    chunk = _mm_or_pd(_mm_and_pd(fmask, chunk), _mm_andnot_pd(fmask, max_val));

    min_vec = _mm_min_pd(min_vec, chunk);
  }

  double min_val = horizontal_min_pd(min_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < min_val) {
        min_val = data[i];
        found = true;
      }
    }
  }

  return found ? min_val : std::numeric_limits<double>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline int32_t min_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<int32_t>::max();

  if (!null_mask) {
    __m128i min_vec = _mm_set1_epi32(std::numeric_limits<int32_t>::max());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      min_vec = _mm_min_epi32(min_vec, _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i)));
    }
    int32_t min_val = horizontal_min_epi32(min_vec);
    for (; i < row_count; ++i) {
      if (data[i] < min_val) min_val = data[i];
    }
    return min_val;
  }

  __m128i min_vec = _mm_set1_epi32(std::numeric_limits<int32_t>::max());
  bool found = false;

  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    __m128i max_val = _mm_set1_epi32(std::numeric_limits<int32_t>::max());
    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    chunk = _mm_or_si128(_mm_and_si128(imask, chunk), _mm_andnot_si128(imask, max_val));
    min_vec = _mm_min_epi32(min_vec, chunk);
  }

  int32_t min_val = horizontal_min_epi32(min_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < min_val) {
        min_val = data[i];
        found = true;
      }
    }
  }

  return found ? min_val : std::numeric_limits<int32_t>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline float max_sse2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<float>::lowest();

  if (!null_mask) {
    __m128 max_vec = _mm_set1_ps(std::numeric_limits<float>::lowest());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      max_vec = _mm_max_ps(max_vec, _mm_loadu_ps(data + i));
    }
    float max_val = horizontal_max_ps(max_vec);
    for (; i < row_count; ++i) {
      if (data[i] > max_val) max_val = data[i];
    }
    return max_val;
  }

  __m128 max_vec = _mm_set1_ps(std::numeric_limits<float>::lowest());
  bool found = false;

  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128 chunk = _mm_loadu_ps(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    __m128 min_val = _mm_set1_ps(std::numeric_limits<float>::lowest());
    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    __m128 fmask = _mm_castsi128_ps(imask);
    chunk = _mm_or_ps(_mm_and_ps(fmask, chunk), _mm_andnot_ps(fmask, min_val));

    max_vec = _mm_max_ps(max_vec, chunk);
  }

  float max_val = horizontal_max_ps(max_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > max_val) {
        max_val = data[i];
        found = true;
      }
    }
  }

  return found ? max_val : std::numeric_limits<float>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline double max_sse2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<double>::lowest();

  if (!null_mask) {
    __m128d max_vec = _mm_set1_pd(std::numeric_limits<double>::lowest());
    size_t i = 0;
    for (; i + 2 <= row_count; i += 2) {
      max_vec = _mm_max_pd(max_vec, _mm_loadu_pd(data + i));
    }
    double max_val = horizontal_max_pd(max_vec);
    for (; i < row_count; ++i) {
      if (data[i] > max_val) max_val = data[i];
    }
    return max_val;
  }

  __m128d max_vec = _mm_set1_pd(std::numeric_limits<double>::lowest());
  bool found = false;

  size_t i = 0;
  for (; i + 2 <= row_count; i += 2) {
    __m128d chunk = _mm_loadu_pd(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 2; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    __m128d min_val = _mm_set1_pd(std::numeric_limits<double>::lowest());
    __m128i imask = _mm_set_epi64x((valid_mask & 2) ? -1LL : 0, (valid_mask & 1) ? -1LL : 0);

    __m128d fmask = _mm_castsi128_pd(imask);
    chunk = _mm_or_pd(_mm_and_pd(fmask, chunk), _mm_andnot_pd(fmask, min_val));

    max_vec = _mm_max_pd(max_vec, chunk);
  }

  double max_val = horizontal_max_pd(max_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > max_val) {
        max_val = data[i];
        found = true;
      }
    }
  }

  return found ? max_val : std::numeric_limits<double>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

inline int32_t max_sse2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (row_count == 0) return std::numeric_limits<int32_t>::lowest();

  if (!null_mask) {
    __m128i max_vec = _mm_set1_epi32(std::numeric_limits<int32_t>::lowest());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      max_vec = _mm_max_epi32(max_vec, _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i)));
    }
    int32_t max_val = horizontal_max_epi32(max_vec);
    for (; i < row_count; ++i) {
      if (data[i] > max_val) max_val = data[i];
    }
    return max_val;
  }

  __m128i max_vec = _mm_set1_epi32(std::numeric_limits<int32_t>::lowest());
  bool found = false;

  size_t i = 0;
  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
        found = true;
      }
    }

    __m128i min_val = _mm_set1_epi32(std::numeric_limits<int32_t>::lowest());
    __m128i imask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                  (valid_mask & 1) ? -1 : 0);

    chunk = _mm_or_si128(_mm_and_si128(imask, chunk), _mm_andnot_si128(imask, min_val));
    max_vec = _mm_max_epi32(max_vec, chunk);
  }

  int32_t max_val = horizontal_max_epi32(max_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > max_val) {
        max_val = data[i];
        found = true;
      }
    }
  }

  return found ? max_val : std::numeric_limits<int32_t>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// Arithmetic Operations - AVX2 Implementations
// ============================================================================

inline float sum_avx2_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0.0f;

  if (!null_mask) {
    __m256 sum_vec = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= row_count; i += 8) {
      sum_vec = _mm256_add_ps(sum_vec, _mm256_loadu_ps(data + i));
    }
    __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(sum_vec, 0), _mm256_extractf128_ps(sum_vec, 1));
    float sum = horizontal_sum_ps(sum128);
    for (; i < row_count; ++i) {
      sum += data[i];
    }
    return sum;
  }

  __m256 sum_vec = _mm256_setzero_ps();
  size_t i = 0;

  for (; i + 8 <= row_count; i += 8) {
    __m256 chunk = _mm256_loadu_ps(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 8; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m256i imask =
        _mm256_set_epi32((valid_mask & 0x80) ? -1 : 0, (valid_mask & 0x40) ? -1 : 0, (valid_mask & 0x20) ? -1 : 0,
                         (valid_mask & 0x10) ? -1 : 0, (valid_mask & 0x08) ? -1 : 0, (valid_mask & 0x04) ? -1 : 0,
                         (valid_mask & 0x02) ? -1 : 0, (valid_mask & 0x01) ? -1 : 0);

    chunk = _mm256_and_ps(chunk, _mm256_castsi256_ps(imask));
    sum_vec = _mm256_add_ps(sum_vec, chunk);
  }

  __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(sum_vec, 0), _mm256_extractf128_ps(sum_vec, 1));
  float sum = horizontal_sum_ps(sum128);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }

  return sum;
#else
  return sum_sse2_float(data, null_mask, row_count);
#endif
}

inline double sum_avx2_double(const double *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0.0;

  if (!null_mask) {
    __m256d sum_vec = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      sum_vec = _mm256_add_pd(sum_vec, _mm256_loadu_pd(data + i));
    }
    __m128d sum128 = _mm_add_pd(_mm256_extractf128_pd(sum_vec, 0), _mm256_extractf128_pd(sum_vec, 1));
    double sum = horizontal_sum_pd(sum128);
    for (; i < row_count; ++i) {
      sum += data[i];
    }
    return sum;
  }

  __m256d sum_vec = _mm256_setzero_pd();
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    __m256d chunk = _mm256_loadu_pd(data + i);

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m256i imask = _mm256_set_epi64x((valid_mask & 8) ? -1LL : 0, (valid_mask & 4) ? -1LL : 0,
                                      (valid_mask & 2) ? -1LL : 0, (valid_mask & 1) ? -1LL : 0);

    chunk = _mm256_and_pd(chunk, _mm256_castsi256_pd(imask));
    sum_vec = _mm256_add_pd(sum_vec, chunk);
  }

  __m128d sum128 = _mm_add_pd(_mm256_extractf128_pd(sum_vec, 0), _mm256_extractf128_pd(sum_vec, 1));
  double sum = horizontal_sum_pd(sum128);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }

  return sum;
#else
  return sum_sse2_double(data, null_mask, row_count);
#endif
}

inline int32_t sum_avx2_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0;

  if (!null_mask) {
    __m256i sum_vec = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= row_count; i += 8) {
      sum_vec = _mm256_add_epi32(sum_vec, _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i)));
    }
    __m128i sum128 = _mm_add_epi32(_mm256_extracti128_si256(sum_vec, 0), _mm256_extracti128_si256(sum_vec, 1));
    int32_t sum = horizontal_sum_epi32(sum128);
    for (; i < row_count; ++i) {
      sum += data[i];
    }
    return sum;
  }

  __m256i sum_vec = _mm256_setzero_si256();
  size_t i = 0;

  for (; i + 8 <= row_count; i += 8) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 8; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    __m256i imask =
        _mm256_set_epi32((valid_mask & 0x80) ? -1 : 0, (valid_mask & 0x40) ? -1 : 0, (valid_mask & 0x20) ? -1 : 0,
                         (valid_mask & 0x10) ? -1 : 0, (valid_mask & 0x08) ? -1 : 0, (valid_mask & 0x04) ? -1 : 0,
                         (valid_mask & 0x02) ? -1 : 0, (valid_mask & 0x01) ? -1 : 0);

    chunk = _mm256_and_si256(chunk, imask);
    sum_vec = _mm256_add_epi32(sum_vec, chunk);
  }

  __m128i sum128 = _mm_add_epi32(_mm256_extracti128_si256(sum_vec, 0), _mm256_extracti128_si256(sum_vec, 1));
  int32_t sum = horizontal_sum_epi32(sum128);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }

  return sum;
#else
  return sum_sse2_int32(data, null_mask, row_count);
#endif
}

inline int64_t sum_avx2_int64(const int64_t *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (row_count == 0) return 0;

  if (!null_mask) {
    __m256i sum_vec = _mm256_setzero_si256();
    size_t i = 0;
    // AVX2 can handler 4 int64_ts
    for (; i + 4 <= row_count; i += 4) {
      sum_vec = _mm256_add_epi64(sum_vec, _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i)));
    }

    // horizontal sum 4 int64_t
    int64_t sum = horizontal_sum_epi64(sum_vec);

    // remains
    for (; i < row_count; ++i) {
      sum += data[i];
    }
    return sum;
  }

  __m256i sum_vec = _mm256_setzero_si256();
  size_t i = 0;

  // dela with null mask, each step to deal 4 elems.
  for (; i + 4 <= row_count; i += 4) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));

    uint32_t valid_mask = 0;
    for (int j = 0; j < 4; ++j) {
      size_t idx = i + j;
      if (!is_null(null_mask, idx)) {
        valid_mask |= (1 << j);
      }
    }

    // build up mask， each int64_t needs same mask
    __m256i imask = _mm256_set_epi64x((valid_mask & 0x08) ? -1 : 0, (valid_mask & 0x04) ? -1 : 0,
                                      (valid_mask & 0x02) ? -1 : 0, (valid_mask & 0x01) ? -1 : 0);

    chunk = _mm256_and_si256(chunk, imask);
    sum_vec = _mm256_add_epi64(sum_vec, chunk);
  }

  int64_t sum = horizontal_sum_epi64(sum_vec);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }

  return sum;
#else
  return sum_sse2_int64(data, null_mask, row_count);
#endif
}

// ============================================================================
// Arithmetic Operations - NEON Implementations
// ============================================================================

inline float sum_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return 0.0f;

  if (!null_mask) {
    float32x4_t sum_vec = vdupq_n_f32(0.0f);
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      sum_vec = vaddq_f32(sum_vec, vld1q_f32(data + i));
    }
    float32x2_t sum2 = vadd_f32(vget_high_f32(sum_vec), vget_low_f32(sum_vec));
    float sum = vget_lane_f32(vpadd_f32(sum2, sum2), 0);
    for (; i < row_count; ++i) {
      sum += data[i];
    }
    return sum;
  }

  float32x4_t sum_vec = vdupq_n_f32(0.0f);
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);

    uint32x4_t mask = vdupq_n_u32(0);
    for (int j = 0; j < 4; ++j) {
      if (!is_null(null_mask, i + j)) {
        mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
      }
    }

    chunk = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(chunk), mask));
    sum_vec = vaddq_f32(sum_vec, chunk);
  }

  float32x2_t sum2 = vadd_f32(vget_high_f32(sum_vec), vget_low_f32(sum_vec));
  float sum = vget_lane_f32(vpadd_f32(sum2, sum2), 0);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      sum += data[i];
    }
  }

  return sum;
#else
  return sum_generic(data, null_mask, row_count);
#endif
}

inline float min_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return std::numeric_limits<float>::max();

  if (!null_mask) {
    float32x4_t min_vec = vdupq_n_f32(std::numeric_limits<float>::max());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      min_vec = vminq_f32(min_vec, vld1q_f32(data + i));
    }
    float32x2_t min2 = vmin_f32(vget_high_f32(min_vec), vget_low_f32(min_vec));
    float min_val = vget_lane_f32(vpmin_f32(min2, min2), 0);
    for (; i < row_count; ++i) {
      if (data[i] < min_val) min_val = data[i];
    }
    return min_val;
  }

  float32x4_t min_vec = vdupq_n_f32(std::numeric_limits<float>::max());
  bool found = false;
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);
    float32x4_t max_val = vdupq_n_f32(std::numeric_limits<float>::max());

    uint32x4_t mask = vdupq_n_u32(0);
    for (int j = 0; j < 4; ++j) {
      if (!is_null(null_mask, i + j)) {
        mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
        found = true;
      }
    }

    chunk = vbslq_f32(mask, chunk, max_val);
    min_vec = vminq_f32(min_vec, chunk);
  }

  float32x2_t min2 = vmin_f32(vget_high_f32(min_vec), vget_low_f32(min_vec));
  float min_val = vget_lane_f32(vpmin_f32(min2, min2), 0);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] < min_val) {
        min_val = data[i];
        found = true;
      }
    }
  }

  return found ? min_val : std::numeric_limits<float>::max();
#else
  return min_generic(data, null_mask, row_count);
#endif
}

inline float max_neon_float(const float *data, const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (row_count == 0) return std::numeric_limits<float>::lowest();

  if (!null_mask) {
    float32x4_t max_vec = vdupq_n_f32(std::numeric_limits<float>::lowest());
    size_t i = 0;
    for (; i + 4 <= row_count; i += 4) {
      max_vec = vmaxq_f32(max_vec, vld1q_f32(data + i));
    }
    float32x2_t max2 = vmax_f32(vget_high_f32(max_vec), vget_low_f32(max_vec));
    float max_val = vget_lane_f32(vpmax_f32(max2, max2), 0);
    for (; i < row_count; ++i) {
      if (data[i] > max_val) max_val = data[i];
    }
    return max_val;
  }

  float32x4_t max_vec = vdupq_n_f32(std::numeric_limits<float>::lowest());
  bool found = false;
  size_t i = 0;

  for (; i + 4 <= row_count; i += 4) {
    float32x4_t chunk = vld1q_f32(data + i);
    float32x4_t min_val = vdupq_n_f32(std::numeric_limits<float>::lowest());

    uint32x4_t mask = vdupq_n_u32(0);
    for (int j = 0; j < 4; ++j) {
      if (!is_null(null_mask, i + j)) {
        mask = vsetq_lane_u32(0xFFFFFFFF, mask, j);
        found = true;
      }
    }

    chunk = vbslq_f32(mask, chunk, min_val);
    max_vec = vmaxq_f32(max_vec, chunk);
  }

  float32x2_t max2 = vmax_f32(vget_high_f32(max_vec), vget_low_f32(max_vec));
  float max_val = vget_lane_f32(vpmax_f32(max2, max2), 0);

  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      if (!found || data[i] > max_val) {
        max_val = data[i];
        found = true;
      }
    }
  }

  return found ? max_val : std::numeric_limits<float>::lowest();
#else
  return max_generic(data, null_mask, row_count);
#endif
}

// ============================================================================
// Counting Operations Implementation
// ============================================================================

inline size_t count_non_null_generic(const uint8_t *null_mask, size_t row_count) {
  if (!null_mask) return row_count;

  size_t count = 0;
  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i)) {
      count++;
    }
  }
  return count;
}

inline size_t count_non_null_sse2(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  if (!null_mask) return row_count;

  // bit=1 means NULL，count the # of 0
  size_t null_count = 0;
  size_t byte_count = (row_count + 7) / 8;
  size_t i = 0;

  // each deal with 16 bytes（128 bits）= 128 rows null bit flags.
  for (; i + 16 <= byte_count; i += 16) {
    __m128i mask_chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(null_mask + i));

    alignas(16) uint8_t temp[16];
    _mm_store_si128(reinterpret_cast<__m128i *>(temp), mask_chunk);

    for (int j = 0; j < 16; ++j) {
      null_count += std::popcount(static_cast<unsigned>(temp[j]));
    }
  }

  for (; i < byte_count; ++i) {
    null_count += std::popcount(static_cast<unsigned>(null_mask[i]));
  }

  return row_count - null_count;
#else
  return count_non_null_generic(null_mask, row_count);
#endif
}

inline size_t count_non_null_sse4(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE4_1__) || defined(__SSE4_2__))
  if (!null_mask) return row_count;

  size_t null_count = 0;
  size_t byte_count = (row_count + 7) / 8;
  size_t i = 0;

  // 使用SSE4.2的popcnt指令
  for (; i + 16 <= byte_count; i += 16) {
    __m128i mask_chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(null_mask + i));

    alignas(16) uint64_t temp[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(temp), mask_chunk);

    null_count += _mm_popcnt_u64(temp[0]) + _mm_popcnt_u64(temp[1]);
  }

  for (; i < byte_count; ++i) {
    null_count += std::popcount(static_cast<unsigned>(null_mask[i]));
  }

  return row_count - null_count;
#else
  return count_non_null_sse2(null_mask, row_count);
#endif
}

inline size_t count_non_null_avx2(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_X86_PLATFORM) && defined(__AVX2__)
  if (!null_mask) return row_count;

  size_t null_count = 0;
  size_t byte_count = (row_count + 7) / 8;
  size_t i = 0;

  if (byte_count >= 32) {
    alignas(32) static const uint8_t popcount_lut[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    __m256i lut = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i *>(popcount_lut)));
    __m256i mask_low = _mm256_set1_epi8(0x0F);

    for (; i + 32 <= byte_count; i += 32) {
      __m256i mask_chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(null_mask + i));

      __m256i low = _mm256_and_si256(mask_chunk, mask_low);
      __m256i pop_low = _mm256_shuffle_epi8(lut, low);
      __m256i high = _mm256_and_si256(_mm256_srli_epi16(mask_chunk, 4), mask_low);
      __m256i pop_high = _mm256_shuffle_epi8(lut, high);
      __m256i pop_sum = _mm256_add_epi8(pop_low, pop_high);
      __m256i sum_64 = _mm256_sad_epu8(pop_sum, _mm256_setzero_si256());

      alignas(32) uint64_t temp[4];
      _mm256_store_si256(reinterpret_cast<__m256i *>(temp), sum_64);
      null_count += temp[0] + temp[1] + temp[2] + temp[3];
    }
  }

  for (; i < byte_count; ++i) {
    null_count += std::popcount(static_cast<unsigned>(null_mask[i]));
  }

  return row_count - null_count;
#else
  return count_non_null_sse4(null_mask, row_count);
#endif
}

inline size_t count_non_null_neon(const uint8_t *null_mask, size_t row_count) {
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
  if (!null_mask) return row_count;

  size_t null_count = 0;
  size_t byte_count = (row_count + 7) / 8;
  size_t i = 0;

  // 16 bytes（128 bits）
  for (; i + 16 <= byte_count; i += 16) {
    uint8x16_t mask_chunk = vld1q_u8(null_mask + i);
    null_count += vaddvq_u8(vcntq_u8(mask_chunk));
  }

  for (; i < byte_count; ++i) {
    null_count += std::popcount(static_cast<unsigned>(null_mask[i]));
  }

  return row_count - null_count;
#else
  return count_non_null_generic(null_mask, row_count);
#endif
}

// ============================================================================
// Filtering Operations Implementation
// ============================================================================

template <typename T>
size_t filter_generic(const T *data, const uint8_t *null_mask, size_t row_count, std::function<bool(T)> predicate,
                      std::vector<size_t> &output_indices) {
  size_t count = 0;
  for (size_t i = 0; i < row_count; ++i) {
    if (!is_null(null_mask, i) && predicate(data[i])) {
      output_indices.push_back(i);
      count++;
    }
  }
  return count;
}

// SSE2 optimized version - only for integer types, float predicates are more complex
inline size_t filter_sse2_int32_eq(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                                   std::vector<size_t> &output_indices) {
#if defined(SHANNON_X86_PLATFORM) && (defined(__SSE2__) || defined(SHANNON_SSE_VECT_SUPPORTED))
  size_t count = 0;
  size_t i = 0;

  __m128i target = _mm_set1_epi32(value);

  for (; i + 4 <= row_count; i += 4) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    __m128i cmp_result = _mm_cmpeq_epi32(chunk, target);

    // Handle NULL mask
    if (null_mask) {
      uint32_t valid_mask = 0;
      for (int j = 0; j < 4; ++j) {
        if (!is_null(null_mask, i + j)) {
          valid_mask |= (1 << j);
        }
      }

      __m128i vmask = _mm_set_epi32((valid_mask & 8) ? -1 : 0, (valid_mask & 4) ? -1 : 0, (valid_mask & 2) ? -1 : 0,
                                    (valid_mask & 1) ? -1 : 0);

      cmp_result = _mm_and_si128(cmp_result, vmask);
    }

    int result_mask = _mm_movemask_ps(_mm_castsi128_ps(cmp_result));

    for (int j = 0; j < 4; ++j) {
      if (result_mask & (1 << j)) {
        output_indices.push_back(i + j);
        count++;
      }
    }
  }

  // Process remaining elements
  for (; i < row_count; ++i) {
    if (!is_null(null_mask, i) && data[i] == value) {
      output_indices.push_back(i);
      count++;
    }
  }

  return count;
#else
  return filter_generic<int32_t>(
      data, null_mask, row_count, [value](int32_t v) { return v == value; }, output_indices);
#endif
}

// ============================================================================
// High-level Interface Implementation
// ============================================================================

template <typename T>
T sum(const T *data, const uint8_t *null_mask, size_t row_count) {
  static SIMDType simd_type = detect_simd_support();

  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
      if constexpr (std::is_same_v<T, float>) {
        return sum_avx2_float(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, double>) {
        return sum_avx2_double(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int32_t>) {
        return sum_avx2_int32(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int64_t>) {
        return sum_avx2_int64(data, null_mask, row_count);
      }
      [[fallthrough]];
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>) {
        return sum_sse2_float(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, double>) {
        return sum_sse2_double(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int32_t>) {
        return sum_sse2_int32(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int64_t>) {
        return sum_sse2_int64(data, null_mask, row_count);
      }
      break;
#endif
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) {
        return sum_neon_float(data, null_mask, row_count);
      }
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
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>) {
        return min_sse2_float(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, double>) {
        return min_sse2_double(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int32_t>) {
        return min_sse2_int32(data, null_mask, row_count);
      }
      break;
#endif
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) {
        return min_neon_float(data, null_mask, row_count);
      }
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
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      if constexpr (std::is_same_v<T, float>) {
        return max_sse2_float(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, double>) {
        return max_sse2_double(data, null_mask, row_count);
      } else if constexpr (std::is_same_v<T, int32_t>) {
        return max_sse2_int32(data, null_mask, row_count);
      }
      break;
#endif
#if defined(SHANNON_ARM_PLATFORM) && defined(__ARM_NEON)
    case SIMDType::NEON:
      if constexpr (std::is_same_v<T, float>) {
        return max_neon_float(data, null_mask, row_count);
      }
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
              std::vector<size_t> &output_indices) {
  // Currently using generic implementation
  // Can add SIMD optimizations for specific predicate types (e.g., equality comparison)
  return filter_generic<T>(data, null_mask, row_count, predicate, output_indices);
}

// Specialized filter version: equality comparison
inline size_t filter_eq_int32(const int32_t *data, const uint8_t *null_mask, size_t row_count, int32_t value,
                              std::vector<size_t> &output_indices) {
  static SIMDType simd_type = detect_simd_support();

  switch (simd_type) {
#if defined(SHANNON_X86_PLATFORM)
    case SIMDType::AVX2:
    case SIMDType::AVX:
    case SIMDType::SSE4_2:
    case SIMDType::SSE4_1:
    case SIMDType::SSE2:
    case SIMDType::SSE:
      return filter_sse2_int32_eq(data, null_mask, row_count, value, output_indices);
#endif
    default:
      return filter_generic<int32_t>(
          data, null_mask, row_count, [value](int32_t v) { return v == value; }, output_indices);
  }
}
}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_IMPL_H__