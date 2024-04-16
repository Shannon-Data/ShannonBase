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
#ifndef __SHANNONBASE_HALFUTILS_H_
#define __SHANNONBASE_HALFUTILS_H_

#include <cmath>
#ifdef F16C_SUPPORT
#include <immintrin.h>
#endif

#include "halfvec.h"
#include "include/my_compiler.h"
#include "vector_comm.h"

namespace ShannonBase {
namespace Vector {

extern float (*HalfvecL2SquaredDistance)(int dim, half *ax, half *bx);
extern float (*HalfvecInnerProduct)(int dim, half *ax, half *bx);
extern double (*HalfvecCosineSimilarity)(int dim, half *ax, half *bx);

class HalfUtils {
 public:
  static void HalfvecInit(void);
  /*  * Check if half is NaN  */
  static inline bool HalfIsNan(half num) {
#ifdef FLT16_SUPPORT
    return isnan(num);
#else
    return (num & 0x7C00) == 0x7C00 && (num & 0x7FFF) != 0x7C00;
#endif
  }
  /* * Check if half is infinite */
  static inline bool HalfIsInf(half num) {
#ifdef FLT16_SUPPORT
    return isinf(num);
#else
    return (num & 0x7FFF) == 0x7C00;
#endif
  }
  /* * Check if half is zero */
  static inline bool HalfIsZero(half num) {
#ifdef FLT16_SUPPORT
    return num == 0;
#else
    return (num & 0x7FFF) == 0x0000;
#endif
  }
  /*  * Convert a half to a float4  */
  static inline float HalfToFloat4(half num) {
#if defined(F16C_SUPPORT)
    return _cvtsh_ss(num);
#elif defined(FLT16_SUPPORT)
    return (float)num;
#else
    /* TODO Improve performance */
    /* Assumes same endianness for floats and integers */
    union {
      float f;
      uint32 i;
    } swapfloat;
    union {
      half h;
      uint16 i;
    } swaphalf;

    uint16 bin;
    uint32 exponent;
    uint32 mantissa;
    uint32 result;

    swaphalf.h = num;
    bin = swaphalf.i;
    exponent = (bin & 0x7C00) >> 10;
    mantissa = bin & 0x03FF;

    /* Sign */
    result = (bin & 0x8000) << 16;

    if (unlikely(exponent == 31)) {
      if (mantissa == 0) {
        /* Infinite */
        result |= 0x7F800000;
      } else {
        /* NaN */
        result |= 0x7FC00000;
      }
    } else if (unlikely(exponent == 0)) {
      /* Subnormal */
      if (mantissa != 0) {
        exponent = -14;

        for (int i = 0; i < 10; i++) {
          mantissa <<= 1;
          exponent -= 1;

          if ((mantissa >> 10) % 2 == 1) {
            mantissa &= 0x03ff;
            break;
          }
        }

        result |= (exponent + 127) << 23;
      }
    } else {
      /* Normal */
      result |= (exponent - 15 + 127) << 23;
    }

    result |= mantissa << 13;
    swapfloat.i = result;
    return swapfloat.f;
#endif
  }

  /*
   * Convert a float4 to a half
   */
  static inline half Float4ToHalfUnchecked(float num) {
#if defined(F16C_SUPPORT)
    return _cvtss_sh(num, 0);
#elif defined(FLT16_SUPPORT)
    return (_Float16)num;
#else
    /* TODO Improve performance */

    /* Assumes same endianness for floats and integers */
    union {
      float f;
      uint32 i;
    } swapfloat;

    union {
      half h;
      uint16 i;
    } swaphalf;

    uint32 bin;
    int exponent;
    int mantissa;
    uint16 result;

    swapfloat.f = num;
    bin = swapfloat.i;
    exponent = (bin & 0x7F800000) >> 23;
    mantissa = bin & 0x007FFFFF;

    /* Sign */
    result = (bin & 0x80000000) >> 16;

    if (std::isinf(num)) {
      /* Infinite */
      result |= 0x7C00;
    } else if (std::isnan(num)) {
      /* NaN */
      result |= 0x7E00;
      result |= mantissa >> 13;
    } else if (exponent > 98) {
      int m;
      int gr;
      int s;

      exponent -= 127;
      s = mantissa & 0x00000FFF;

      /* Subnormal */
      if (exponent < -14) {
        int diff = -exponent - 14;

        mantissa >>= diff;
        mantissa += 1 << (23 - diff);
        s |= mantissa & 0x00000FFF;
      }

      m = mantissa >> 13;

      /* Round */
      gr = (mantissa >> 12) % 4;
      if (gr == 3 || (gr == 1 && s != 0)) m += 1;

      if (m == 1024) {
        m = 0;
        exponent += 1;
      }

      if (exponent > 15) {
        /* Infinite */
        result |= 0x7C00;
      } else {
        if (exponent >= -14) result |= (exponent + 15) << 10;

        result |= m;
      }
    }

    swaphalf.i = result;
    return swaphalf.h;
#endif
  }

  /*    * Convert a float4 to a half    */
  inline half Float4ToHalf(float num) {
    half result = Float4ToHalfUnchecked(num);

    if (unlikely(HalfIsInf(result)) && !std::isinf(num)) {  // error
      return 0.0f;
    }

    return result;
  }
};

}  // namespace Vector
}  // namespace ShannonBase
#endif  //__SHANNONBASE_HALFUTILS_H_
