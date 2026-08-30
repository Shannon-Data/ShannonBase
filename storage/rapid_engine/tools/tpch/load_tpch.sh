#!/usr/bin/env bash
# Reload the TPC-H tables into Rapid. rapid_reload_on_restart=OFF on this box, so
# every server restart needs this before the A/B runner means anything.
set -u
MYSQL="${MYSQL_CLIENT:-/home/lihao/workshop/shannon-bin/bin/mysql}"
CONN=(-h127.0.0.1 -P3306 -uroot -p123456 -N)
DB=tpch
for t in NATION REGION SUPPLIER CUSTOMER PART PARTSUPP ORDERS LINEITEM; do
  start=$(date +%s)
  "$MYSQL" "${CONN[@]}" "$DB" -e "ALTER TABLE $t SECONDARY_ENGINE=rapid;" >/dev/null 2>&1
  if "$MYSQL" "${CONN[@]}" "$DB" -e "ALTER TABLE $t SECONDARY_LOAD;" 2>&1 | grep -v "Using a password"; then :; fi
  printf '%-10s loaded in %ss\n' "$t" "$(( $(date +%s) - start ))"
done
