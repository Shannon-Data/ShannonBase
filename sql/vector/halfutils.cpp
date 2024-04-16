/* Copyright (c) 2018, 2023, Oracle and/or its affiliates.

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

   pg_vector
   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#include "halfutils.h"

#include "halfvec.h"

#ifdef HALFVEC_DISPATCH
#include <immintrin.h>

#if defined(HAVE__GET_CPUID)
#include <cpuid.h>
#elif defined(HAVE__CPUID)
#include <intrin.h>
#endif

#ifdef _MSC_VER
#define TARGET_F16C_FMA
#else
#define TARGET_F16C_FMA __attribute__((target("f16c,fma")))
#endif
#endif

namespace ShannonBase {
namespace Vector {
float (*HalfvecL2SquaredDistance)(int dim, half *ax, half *bx);
float (*HalfvecInnerProduct)(int dim, half *ax, half *bx);
double (*HalfvecCosineSimilarity)(int dim, half *ax, half *bx);

static float HalfvecL2SquaredDistanceDefault(int dim, half *ax, half *bx) {
  float distance = 0.0;

  /* Auto-vectorized */
  for (int i = 0; i < dim; i++) {
    float diff =
        HalfUtils::HalfToFloat4(ax[i]) - HalfUtils::HalfToFloat4(bx[i]);

    distance += diff * diff;
  }

  return distance;
}

#ifdef HALFVEC_DISPATCH
static float HalfvecL2SquaredDistanceF16cFma(int dim, half *ax, half *bx) {
  float distance;
  int i;
  float s[8];
  int count = (dim / 8) * 8;
  __m256 dist = _mm256_setzero_ps();

  for (i = 0; i < count; i += 8) {
    __m128i axi = _mm_loadu_si128((__m128i *)(ax + i));
    __m128i bxi = _mm_loadu_si128((__m128i *)(bx + i));
    __m256 axs = _mm256_cvtph_ps(axi);
    __m256 bxs = _mm256_cvtph_ps(bxi);
    __m256 diff = _mm256_sub_ps(axs, bxs);

    dist = _mm256_fmadd_ps(diff, diff, dist);
  }

  _mm256_storeu_ps(s, dist);

  distance = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

  for (; i < dim; i++) {
    float diff =
        HalfUtils::HalfToFloat4(ax[i]) - HalfUtils::HalfToFloat4(bx[i]);

    distance += diff * diff;
  }

  return distance;
}
#endif

static float HalfvecInnerProductDefault(int dim, half *ax, half *bx) {
  float distance = 0.0;

  /* Auto-vectorized */
  for (int i = 0; i < dim; i++)
    distance += HalfUtils::HalfToFloat4(ax[i]) * HalfUtils::HalfToFloat4(bx[i]);

  return distance;
}
#ifdef HALFVEC_DISPATCH
TARGET_F16C_FMA static float HalfvecInnerProductF16cFma(int dim, half *ax,
                                                        half *bx) {
  float distance;
  int i;
  float s[8];
  int count = (dim / 8) * 8;
  __m256 dist = _mm256_setzero_ps();

  for (i = 0; i < count; i += 8) {
    __m128i axi = _mm_loadu_si128((__m128i *)(ax + i));
    __m128i bxi = _mm_loadu_si128((__m128i *)(bx + i));
    __m256 axs = _mm256_cvtph_ps(axi);
    __m256 bxs = _mm256_cvtph_ps(bxi);

    dist = _mm256_fmadd_ps(axs, bxs, dist);
  }

  _mm256_storeu_ps(s, dist);

  distance = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

  for (; i < dim; i++)
    distance += HalfUtils::HalfToFloat4(ax[i]) * HalfUtils::HalfToFloat4(bx[i]);

  return distance;
}
#endif

static double HalfvecCosineSimilarityDefault(int dim, half *ax, half *bx) {
  float similarity = 0.0;
  float norma = 0.0;
  float normb = 0.0;

  /* Auto-vectorized */
  for (int i = 0; i < dim; i++) {
    float axi = HalfUtils::HalfToFloat4(ax[i]);
    float bxi = HalfUtils::HalfToFloat4(bx[i]);

    similarity += axi * bxi;
    norma += axi * axi;
    normb += bxi * bxi;
  }

  /* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
  return (double)similarity / sqrt((double)norma * (double)normb);
}

#ifdef HALFVEC_DISPATCH
TARGET_F16C_FMA static double HalfvecCosineSimilarityF16cFma(int dim, half *ax,
                                                             half *bx) {
  float similarity;
  float norma;
  float normb;
  int i;
  float s[8];
  int count = (dim / 8) * 8;
  __m256 sim = _mm256_setzero_ps();
  __m256 na = _mm256_setzero_ps();
  __m256 nb = _mm256_setzero_ps();

  for (i = 0; i < count; i += 8) {
    __m128i axi = _mm_loadu_si128((__m128i *)(ax + i));
    __m128i bxi = _mm_loadu_si128((__m128i *)(bx + i));
    __m256 axs = _mm256_cvtph_ps(axi);
    __m256 bxs = _mm256_cvtph_ps(bxi);

    sim = _mm256_fmadd_ps(axs, bxs, sim);
    na = _mm256_fmadd_ps(axs, axs, na);
    nb = _mm256_fmadd_ps(bxs, bxs, nb);
  }

  _mm256_storeu_ps(s, sim);
  similarity = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

  _mm256_storeu_ps(s, na);
  norma = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

  _mm256_storeu_ps(s, nb);
  normb = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

  /* Auto-vectorized */
  for (; i < dim; i++) {
    float axi = HalfUtils::HalfToFloat4(ax[i]);
    float bxi = HalfUtils::HalfToFloat4(bx[i]);

    similarity += axi * bxi;
    norma += axi * axi;
    normb += bxi * bxi;
  }

  /* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
  return (double)similarity / sqrt((double)norma * (double)normb);
}
#endif

#ifdef HALFVEC_DISPATCH
#define CPU_FEATURE_FMA (1 << 12)
#define CPU_FEATURE_F16C (1 << 29)

static bool SupportsCpuFeature(unsigned int feature) {
  unsigned int exx[4] = {0, 0, 0, 0};

#if defined(HAVE__GET_CPUID)
  __get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#elif defined(HAVE__CPUID)
  __cpuid(exx, 1);
#endif

  return (exx[2] & feature) == feature;
}
#endif

void HalfvecInit(void) {
  /*
   * Could skip pointer when single function, but no difference in
   * performance
   */
  HalfvecL2SquaredDistance = HalfvecL2SquaredDistanceDefault;
  HalfvecInnerProduct = HalfvecInnerProductDefault;
  HalfvecCosineSimilarity = HalfvecCosineSimilarityDefault;

#ifdef HALFVEC_DISPATCH
  if (SupportsCpuFeature(CPU_FEATURE_FMA | CPU_FEATURE_F16C)) {
    HalfvecL2SquaredDistance = HalfvecL2SquaredDistanceF16cFma;
    HalfvecInnerProduct = HalfvecInnerProductF16cFma;
    HalfvecCosineSimilarity = HalfvecCosineSimilarityF16cFma;
  }
#endif
}

}  // namespace Vector
}  // namespace ShannonBase