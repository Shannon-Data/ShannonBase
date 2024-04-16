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
#ifndef __SHANNONBASE_VECTOR_H__
#define __SHANNONBASE_VECTOR_H__

#include "vector_comm.h"

#include <vector>

namespace ShannonBase {
namespace Vector {

//#define VECTOR_SIZE(_dim)		(offsetof(Vector, x) +
// sizeof(float)*(_dim)) #define DatumGetVector(x)		((Vector *)
// PG_DETOAST_DATUM(x)) #define PG_GETARG_VECTOR_P(x)
// DatumGetVector(PG_GETARG_DATUM(x)) #define PG_RETURN_VECTOR_P(x)
// PG_RETURN_POINTER(x)

Vector InitVector(uint16 dim);
Vector vector_in(uchar *values, int32 type_type);

}  // namespace Vector
}  // namespace ShannonBase
#endif  //__SHANNONBASE_VECTOR_H__
