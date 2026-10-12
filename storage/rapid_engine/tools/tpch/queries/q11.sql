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

select ps_partkey, sum(ps_supplycost * ps_availqty) as value
from PARTSUPP, SUPPLIER, NATION
where ps_suppkey = s_suppkey and s_nationkey = n_nationkey and n_name = 'GERMANY'
group by ps_partkey
having sum(ps_supplycost * ps_availqty) > (
 select sum(ps_supplycost * ps_availqty) * 0.0001000000
 from PARTSUPP, SUPPLIER, NATION
 where ps_suppkey = s_suppkey and s_nationkey = n_nationkey and n_name = 'GERMANY')
order by value desc;
