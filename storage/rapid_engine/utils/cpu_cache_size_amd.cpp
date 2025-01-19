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

void get_amd_cache_size() {
  unsigned int eax, ebx, ecx, edx;

  // gets l1 cache size
  eax = 0x80000005;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));

  unsigned int l1_data_cache_size = ((ecx >> 24) & 0xFF) * 1024;  // L1 data cache (KB)
  // unsigned int l1_instruction_cache_size = ((edx >> 24) & 0xFF) * 1024; // L1 instruction cache (KB)

  printf("CACHE_L1=%u \n", l1_data_cache_size);
  // printf("L1 Instruction Cache Size: %u KB\n", l1_instruction_cache_size);

  // gets l2 and l3
  eax = 0x80000006;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));

  unsigned int l2_cache_size = ((ecx >> 16) & 0xFFFF) * 1024;        // L2 cache (KB)
  unsigned int l3_cache_size = ((edx >> 18) & 0x3FFF) * 512 * 1024;  // L3 cache (KB)

  printf("CACHE_L2=%u\n", l2_cache_size);
  printf("CACHE_L3=%u\n", l3_cache_size);
}

int main() {
  get_amd_cache_size();
  return 0;
}
