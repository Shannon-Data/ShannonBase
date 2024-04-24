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
#include "vector_comm.h"

namespace ShannonBase {
namespace Vector {

bool Utils::f2d_small_int(const uint32 ieeeMantissa, const uint32 ieeeExponent,
                          floating_decimal_32 *v) {
  const int32 e2 = (int32)ieeeExponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS;

  /*
   * Avoid using multiple "return false;" here since it tends to provoke the
   * compiler into inlining multiple copies of f2d, which is undesirable.
   */

  if (e2 >= -FLOAT_MANTISSA_BITS && e2 <= 0) {
    /*----
     * Since 2^23 <= m2 < 2^24 and 0 <= -e2 <= 23:
     *   1 <= f = m2 / 2^-e2 < 2^24.
     *
     * Test if the lower -e2 bits of the significand are 0, i.e. whether
     * the fraction is 0. We can use ieeeMantissa here, since the implied
     * 1 bit can never be tested by this; the implied 1 can only be part
     * of a fraction if e2 < -FLOAT_MANTISSA_BITS which we already
     * checked. (e.g. 0.5 gives ieeeMantissa == 0 and e2 == -24)
     */
    const uint32 mask = (1U << -e2) - 1;
    const uint32 fraction = ieeeMantissa & mask;

    if (fraction == 0) {
      /*----
       * f is an integer in the range [1, 2^24).
       * Note: mantissa might contain trailing (decimal) 0's.
       * Note: since 2^24 < 10^9, there is no need to adjust
       * decimalLength().
       */
      const uint32 m2 = (1U << FLOAT_MANTISSA_BITS) | ieeeMantissa;

      v->mantissa = m2 >> -e2;
      v->exponent = 0;
      return true;
    }
  }
  return false;
}

int Utils::float_to_shortest_decimal_bufn(float f[[maybe_unused]], char *result[[maybe_unused]]) {
   return 0; 
}

}  // namespace Vector
}  // namespace ShannonBase