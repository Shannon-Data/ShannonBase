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
#ifndef __SHANNONBASE_BITVECTOR_H__
#define __SHANNONBASE_BITVECTOR_H__

#include "bitvector.h"

#include "include/my_sys.h"
#include "mysqld_error.h"

namespace ShannonBase {
namespace Vector {

VarBit BitVector::InitBitVector(uint32 dim) {
  VarBit result;
  uint byte_size = dim / sizeof(sizeof(uint8));
  result.reserve(byte_size);

  return result;
}

double BitVector::hamming_distance(VarBit *va, VarBit *vb) {
  unsigned char *ax = va->data();
  unsigned char *bx = vb->data();
  uint64 distance = 0;

  if (!CheckDims(va, vb)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "invalid dims");
    return 0.0f;
  }

  /* TODO Improve performance */
  for (uint32 i = 0; i < va->size(); i++)
    distance += shannon_number_of_ones[ax[i] ^ bx[i]];
  return ((double)distance);
  ;
}

double BitVector::jaccard_distance(VarBit *va, VarBit *vb) {
  unsigned char *ax = va->data();
  unsigned char *bx = vb->data();
  uint64 ab = 0;
  uint64 aa;
  uint64 bb;

  if (!CheckDims(va, vb)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "invalid dims");
    return 0.0f;
  }

  /* TODO Improve performance */
  for (uint32 i = 0; i < va->size(); i++)
    ab += shannon_number_of_ones[ax[i] & bx[i]];

  if (ab == 0) return (1.0f);

  aa = Utils::calc_bits_count((char *)ax, va->size());
  bb = Utils::calc_bits_count((char *)bx, vb->size());

  return (1 - (ab / ((double)(aa + bb - ab))));
}

}  // namespace Vector
}  // namespace ShannonBase
#endif  //__SHANNONBASE_BITVECTOR_H__