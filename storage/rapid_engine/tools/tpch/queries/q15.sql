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

with revenue0 as (
 select l_suppkey as supplier_no, sum(l_extendedprice * (1 - l_discount)) as total_revenue
 from LINEITEM
 where l_shipdate >= date '1996-01-01' and l_shipdate < date '1996-01-01' + interval '3' month
 group by l_suppkey
)
select s_suppkey, s_name, s_address, s_phone, total_revenue
from SUPPLIER, revenue0
where s_suppkey = supplier_no
 and total_revenue = (select max(total_revenue) from revenue0)
order by s_suppkey;
