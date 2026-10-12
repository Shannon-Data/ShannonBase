#!/usr/bin/env bash
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
#
# Copyright (c) 2026, Shannon Data AI and/or its affiliates.

# Reload the TPC-H tables into Rapid. rapid_reload_on_restart=OFF on this box, so
# every server restart needs this before the A/B runner means anything.
set -u
MYSQL="${MYSQL_CLIENT:-/home/lihao/workshop/shannon-bin/bin/mysql}"
CONN=(-h127.0.0.1 -P3306 -uroot -p123456 -N)
DB="${DB:-tpch}"
for t in NATION REGION SUPPLIER CUSTOMER PART PARTSUPP ORDERS LINEITEM; do
  start=$(date +%s)
  "$MYSQL" "${CONN[@]}" "$DB" -e "ALTER TABLE $t SECONDARY_ENGINE=rapid;" >/dev/null 2>&1
  if "$MYSQL" "${CONN[@]}" "$DB" -e "ALTER TABLE $t SECONDARY_LOAD;" 2>&1 | grep -v "Using a password"; then :; fi
  printf '%-10s loaded in %ss\n' "$t" "$(( $(date +%s) - start ))"
done
