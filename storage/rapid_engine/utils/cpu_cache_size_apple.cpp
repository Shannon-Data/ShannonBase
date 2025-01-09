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
#include <sys/sysctl.h>
#include <sys/types.h>

void get_intel_cache_size() {
  size_t l1icache = 0, l1dcache = 0, l2cache = 0, l3cache = 0;
  size_t len = sizeof(l1icache);

  // l1-instr
  sysctlbyname("hw.l1icachesize", &l1icache, &len, NULL, 0);
  // l2-data
  sysctlbyname("hw.l1dcachesize", &l1dcache, &len, NULL, 0);
  // l2 size
  sysctlbyname("hw.l2cachesize", &l2cache, &len, NULL, 0);
  // l3 size
  sysctlbyname("hw.l3cachesize", &l3cache, &len, NULL, 0);

  printf("CACHE_L1=%zu\n", l1dcache / 1024);
  printf("CACHE_L2=%zu\n", l2cache / 1024);
  printf("CACHE_L3=%zu\n", l3cache / 1024);
}

int main() {
  get_intel_cache_size();
  return 0;
}
