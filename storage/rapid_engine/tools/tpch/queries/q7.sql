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

select supp_nation, cust_nation, l_year, sum(volume) as revenue
from (
 select n1.n_name as supp_nation, n2.n_name as cust_nation,
  extract(year from l_shipdate) as l_year, l_extendedprice * (1 - l_discount) as volume
 from SUPPLIER, LINEITEM, ORDERS, CUSTOMER, NATION n1, NATION n2
 where s_suppkey = l_suppkey and o_orderkey = l_orderkey and c_custkey = o_custkey
  and s_nationkey = n1.n_nationkey and c_nationkey = n2.n_nationkey
  and ((n1.n_name = 'FRANCE' and n2.n_name = 'GERMANY')
    or (n1.n_name = 'GERMANY' and n2.n_name = 'FRANCE'))
  and l_shipdate between date '1995-01-01' and date '1996-12-31'
) as shipping
group by supp_nation, cust_nation, l_year
order by supp_nation, cust_nation, l_year;
