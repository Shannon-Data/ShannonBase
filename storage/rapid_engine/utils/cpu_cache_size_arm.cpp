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

void get_arm_cache_size() {
  const char *base_path = "/sys/devices/system/cpu/cpu0/cache";
  char path[256];
  char buffer[128];

  for (int index = 0;; ++index) {
    // Check if this cache index exists
    snprintf(path, sizeof(path), "%s/index%d/size", base_path, index);
    FILE *fp = fopen(path, "r");
    if (!fp) break;

    if (fgets(buffer, sizeof(buffer), fp)) {
      // Print cache size string
      printf("CACHE_INDEX_%d_SIZE=%s", index, buffer);
    }

    fclose(fp);
  }
}

int main() {
  get_arm_cache_size();
  return 0;
}
