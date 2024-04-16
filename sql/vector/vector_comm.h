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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_VECTOR_COMMON_H__
#define __SHANNONBASE_VECTOR_COMMON_H__

#include "include/my_inttypes.h"

#include <cstring>
#include <memory>
#include <vector>

namespace ShannonBase {
namespace Vector {

#define VECTOR_MAX_DIM 16000
#define FLEXIBLE_ARRAY_MEMBER /* empty */

#define FLOAT_MANTISSA_BITS 23
#define FLOAT_EXPONENT_BITS 8
#define FLOAT_BIAS 127

#define FLOAT_SHORTEST_DECIMAL_LEN 16

/*
 * Array giving the number of 1-bits in each possible byte value.
 *
 * Note: we export this for use by functions in which explicit use
 * of the popcount functions seems unlikely to be a win.
 */
const uint8 shannon_number_of_ones[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4,
    2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4,
    2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6,
    4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5,
    3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6,
    4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

using Vector = std::vector<float>;
using VarBit = std::vector<uint8>;

/*  A floating decimal representing m * 10^e. */
typedef struct floating_decimal_32 {
  uint32 mantissa;
  int32 exponent;
} floating_decimal_32;

inline uint32 float_to_bits(const float f) {
  uint32 bits = 0;
  std::memcpy(&bits, &f, sizeof(float));
  return bits;
}

class Utils {
 public:
  static inline int copy_special_str(char *const result, const bool sign,
                                     const bool exponent, const bool mantissa) {
    if (mantissa) {
      std::memcpy(result, "NaN", 3);
      return 3;
    }

    if (sign) {
      result[0] = '-';
    }
    if (exponent) {
      std::memcpy(result + sign, "Infinity", 8);
      return sign + 8;
    }
    result[sign] = '0';
    return sign + 1;
  }

  static inline int calc_bits_count(const char *buf, int bytes) {
    int count = 0;

    // Iterate through each byte in the buffer
    for (int i = 0; i < bytes; ++i) {
      unsigned char byte = buf[i];  // Convert char to unsigned char

      // Count the number of 1-bits in the current byte using bitwise AND
      // operation
      while (byte != 0) {
        count +=
            (byte & 1);  // Increment count if the least significant bit is 1
        byte >>= 1;      // Shift byte to the right by 1 bit
      }
    }

    return count;
  }
  static bool f2d_small_int(const uint32 ieeeMantissa,
                            const uint32 ieeeExponent, floating_decimal_32 *v);
  static int float_to_shortest_decimal_bufn(float f, char *result);
};

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) \
  ((ptr) += Utils::float_to_shortest_decimal_bufn((f), (ptr)))

}  // namespace Vector
}  // namespace ShannonBase
#endif  //__SHANNONBASE_VECTOR_COMMON_H__
