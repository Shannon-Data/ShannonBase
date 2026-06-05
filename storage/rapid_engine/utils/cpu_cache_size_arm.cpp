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
#include <stdlib.h>
#include <string.h>

void get_arm_cache_size() {
  const char *base_path = "/sys/devices/system/cpu/cpu0/cache";
  char path[256];
  char buffer[128];

  int l1_found = 0, l2_found = 0, l3_found = 0;

  for (int index = 0; index < 16; ++index) {
    // Check if this cache index exists
    snprintf(path, sizeof(path), "%s/index%d/size", base_path, index);
    FILE *fp = fopen(path, "r");
    if (!fp) break;

    char size_str[64] = {0};
    if (!fgets(size_str, sizeof(size_str), fp)) {
      fclose(fp);
      continue;
    }
    fclose(fp);

    size_str[strcspn(size_str, "\r\n")] = '\0';
    snprintf(path, sizeof(path), "%s/index%d/level", base_path, index);
    fp = fopen(path, "r");
    if (!fp) continue;
    int level = 0;
    fscanf(fp, "%d", &level);
    fclose(fp);

    // Read cache type: "Data", "Instruction", "Unified"
    snprintf(path, sizeof(path), "%s/index%d/type", base_path, index);
    fp = fopen(path, "r");
    if (!fp) continue;
    char type_str[32] = {0};
    fgets(type_str, sizeof(type_str), fp);
    fclose(fp);
    type_str[strcspn(type_str, "\r\n")] = '\0';

    // Skip instruction cache
    if (strncmp(type_str, "Instruction", 11) == 0) continue;

    if (level == 1 && !l1_found) {
      printf("CACHE_L1=%s\n", size_str);
      l1_found = 1;
    } else if (level == 2 && !l2_found) {
      printf("CACHE_L2=%s\n", size_str);
      l2_found = 1;
    } else if (level == 3 && !l3_found) {
      printf("CACHE_L3=%s\n", size_str);
      l3_found = 1;
    }
  }
}

int main() {
  get_arm_cache_size();
  return 0;
}