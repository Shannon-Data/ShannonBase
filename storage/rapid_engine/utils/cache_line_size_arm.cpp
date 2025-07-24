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

#include <stdio.h>

unsigned int get_cache_line_size() {
  unsigned long ctr_el0;

  // read CTR_EL0
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));

  // CTR_EL0 bits[19:16] = log2(cache line size in words) => size in bytes = 4 << (CTR_EL0[19:16])
  unsigned int log2_words = (ctr_el0 >> 16) & 0xF;
  unsigned int cache_line_size = 4 << log2_words;

  return cache_line_size;
}

int main() {
  unsigned int line_size = get_cache_line_size();
  printf("Cache line size: %u bytes\n", line_size);
  return 0;
}
