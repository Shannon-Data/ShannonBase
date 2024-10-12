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

   The fundmental code for imcs. to get all level cache size of a cpu.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

// gets intel cpu size of all cache level.
#include <stdio.h>

void get_intel_cache_size() {
  unsigned int eax, ebx, ecx, edx;
  int cache_level = 0;

  while (1) {
    eax = 4;            // Leaf 4 (Cache Parameters)
    ecx = cache_level;  // Cache level: L1 = 0, L2 = 1, L3 = 2, ...

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));

    // If EAX[4:0] == 0, no more caches
    unsigned int cache_type = eax & 0x1F;
    if (cache_type == 0) {
      break;
    }

    // Cache size calculation
    unsigned int cache_sets = ecx + 1;
    unsigned int cache_coherency_line_size = (ebx & 0xFFF) + 1;
    unsigned int cache_physical_line_partitions = ((ebx >> 12) & 0x3FF) + 1;
    unsigned int cache_ways_of_associativity = ((ebx >> 22) & 0x3FF) + 1;

    unsigned int cache_size =
        cache_sets * cache_coherency_line_size * cache_physical_line_partitions * cache_ways_of_associativity;

    // Print the cache level and size
    printf("CACHE_L%d=%u\n", cache_level + 1, cache_size / 1024);

    cache_level++;
  }
}

int main() {
  get_intel_cache_size();
  return 0;
}
