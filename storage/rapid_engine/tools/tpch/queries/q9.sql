-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License, version 2.0,
-- as published by the Free Software Foundation.
--
-- This program is also distributed with certain software (including
-- but not limited to OpenSSL) that is licensed under separate terms,
-- as designated in a particular file or component or in included license
-- documentation.  The authors of MySQL hereby grant you an additional
-- permission to link the program and your derivative works with the
-- separately licensed software that they have included with MySQL.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License, version 2.0, for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
--
-- Copyright (c) 2026, Shannon Data AI and/or its affiliates.

select nation, o_year, sum(amount) as sum_profit
from (
 select n_name as nation, extract(year from o_orderdate) as o_year,
  l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity as amount
 from PART, SUPPLIER, LINEITEM, PARTSUPP, ORDERS, NATION
 where s_suppkey = l_suppkey and ps_suppkey = l_suppkey and ps_partkey = l_partkey
  and p_partkey = l_partkey and o_orderkey = l_orderkey and s_nationkey = n_nationkey
  and p_name like '%green%'
) as profit
group by nation, o_year
order by nation, o_year desc;
