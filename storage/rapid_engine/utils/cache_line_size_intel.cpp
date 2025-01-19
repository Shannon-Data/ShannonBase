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

void get_cache_lines_size() {
  unsigned int eax, ebx, ecx, edx;
  eax = 4;  // CPUID leaf 4 for cache information
  ecx = 0;  // Query L1 cache (level 1 cache)

  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));

  // Cache line size is in EBX[11:0], add 1 because it's zero-indexed
  unsigned int cache_line_size = (ebx & 0xFFF) + 1;

  // L1 Cache Line Size:
  printf("%u \n", cache_line_size);
}

int main() {
  get_cache_lines_size();
  return 0;
}