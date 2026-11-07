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
  // Leaf 4 is a list of caches, not one entry per level: index 0 is the L1 data
  // cache, 1 the L1 instruction cache, then L2, L3, L4. Report the level EAX
  // names instead of the index, skip instruction caches (they never bound a row
  // batch), and print bytes -- CMake reads a bare number as bytes.
  for (unsigned int index = 0; index < 16; ++index) {
    unsigned int eax = 4, ebx = 0, ecx = index, edx = 0;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));

    const unsigned int cache_type = eax & 0x1F;  // 0 none, 1 data, 2 insn, 3 unified
    if (cache_type == 0) break;
    if (cache_type == 2) continue;

    const unsigned int cache_level = (eax >> 5) & 0x7;
    const unsigned int line_size = (ebx & 0xFFF) + 1;
    const unsigned int partitions = ((ebx >> 12) & 0x3FF) + 1;
    const unsigned int ways = ((ebx >> 22) & 0x3FF) + 1;
    // ECX holds the set count as-is; only the EBX fields are biased by one.
    const unsigned long long cache_size = 1ULL * ecx * line_size * partitions * ways;

    if (cache_level >= 1 && cache_level <= 4 && cache_size > 0) printf("CACHE_L%u=%llu\n", cache_level, cache_size);
  }
}

int main() {
  get_intel_cache_size();
  return 0;
}
