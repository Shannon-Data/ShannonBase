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

#include "vector.h"

#include <cfloat>
#include <cmath>

#include "include/my_sys.h"
#include "mysqld_error.h"

namespace ShannonBase {
namespace Vector {

static inline void CheckDims(Vector a, Vector b) {
  if (a.size() == b.size())
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "different vector dimensions");
}

static inline void CheckExpectedDim(int32 typmod, int dim) {
  if (typmod != -1 && typmod != dim)
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "expected dimensions not matched");
}

static inline void CheckDim(int dim) {
  if (dim < 1)
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "vector must have at least 1 dimension");

  if (dim > VECTOR_MAX_DIM)
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "vector cannot have more than the max dims");
}

/*
 * Ensure finite element
 */
static inline void CheckElement(float value) {
  if (std::isnan(value))
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "NaN not allowed in vector");

  if (std::isinf(value))
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "infinite value not allowed in vector");
}

Vector InitVector(uint16 dim) {
  Vector vcs;
  vcs.reserve(dim);
  return vcs;
}

static inline bool vector_isspace(char ch) {
  if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
      ch == '\f')
    return true;
  return false;
}

Vector vector_in(uchar *values, int32 type_type) {
  uchar *lit = values;
  int32 typmod = type_type;
  float x[VECTOR_MAX_DIM];
  int dim = 0;
  uchar *pt = lit;
  Vector result;

  while (vector_isspace(*pt)) pt++;

  if (*pt != '[')
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "invalid input syntax for type vector: Vector contents must start "
             "with \"[\".");

  pt++;

  while (vector_isspace(*pt)) pt++;

  if (*pt == ']')
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "vector must have at least 1 dimension");

  for (;;) {
    float val;
    uchar *stringEnd;

    if (dim == VECTOR_MAX_DIM)
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "vector cannot have more than the max dims size");

    while (vector_isspace(*pt)) pt++;

    /* Check for empty string like float4in */
    if (*pt == '\0')
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "invalid input syntax for type vector");

    errno = 0;

    /* Use strtof like float4in to avoid a double-rounding problem */
    /* Postgres sets LC_NUMERIC to C on startup */
    val = strtof((const char *)pt, (char **)&stringEnd);

    if (stringEnd == pt)
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "invalid input syntax for type vector");

    /* Check for range error like float4in */
    if (errno == ERANGE && std::isinf(val))
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "is out of range for type vector");

    CheckElement(val);
    x[dim++] = val;

    pt = stringEnd;

    while (vector_isspace(*pt)) pt++;

    if (*pt == ',')
      pt++;
    else if (*pt == ']') {
      pt++;
      break;
    } else
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "invalid input syntax for type vector.");
  }

  /* Only whitespace is allowed after the closing brace */
  while (vector_isspace(*pt)) pt++;

  if (*pt != '\0')
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "invalid input syntax for type vector, Junk after closing right "
             "brace.");

  CheckDim(dim);
  CheckExpectedDim(typmod, dim);

  result = InitVector(dim);
  for (int i = 0; i < dim; i++) result.push_back(x[i]);

  return (result);
}

/* Convert internal representation to textual representation
 */
uchar *vector_out(Vector &vect) {
  int dim = vect.size();
  uchar *buf;
  uchar *ptr;

  /*
   * Need:
   *
   * dim * (FLOAT_SHORTEST_DECIMAL_LEN - 1) bytes for
   * float_to_shortest_decimal_bufn
   *
   * dim - 1 bytes for separator
   *
   * 3 bytes for [, ], and \0
   */
  buf = (uchar *)malloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2);
  ptr = buf;

  AppendChar(ptr, '[');

  for (int i = 0; i < dim; i++) {
    if (i > 0) AppendChar(ptr, ',');

    char *chr_ptr = (char *)ptr;
    AppendFloat(chr_ptr, vect[i]);
  }

  AppendChar(ptr, ']');
  *ptr = '\0';

  return (buf);
}

}  // namespace Vector
}  // namespace ShannonBase