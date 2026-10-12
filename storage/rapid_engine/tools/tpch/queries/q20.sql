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

select s_name, s_address
from SUPPLIER, NATION
where s_suppkey in (
 select ps_suppkey from PARTSUPP
 where ps_partkey in (select p_partkey from PART where p_name like 'forest%')
  and ps_availqty > (
   select 0.5 * sum(l_quantity) from LINEITEM
   where l_partkey = ps_partkey and l_suppkey = ps_suppkey
    and l_shipdate >= date '1994-01-01' and l_shipdate < date '1994-01-01' + interval '1' year)
)
and s_nationkey = n_nationkey and n_name = 'CANADA'
order by s_name;
