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

select p_brand, p_type, p_size, count(distinct ps_suppkey) as supplier_cnt
from PARTSUPP, PART
where p_partkey = ps_partkey and p_brand <> 'Brand#45'
 and p_type not like 'MEDIUM POLISHED%'
 and p_size in (49, 14, 23, 45, 19, 3, 36, 9)
 and ps_suppkey not in (select s_suppkey from SUPPLIER where s_comment like '%Customer%Complaints%')
group by p_brand, p_type, p_size
order by supplier_cnt desc, p_brand, p_type, p_size;
