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

select s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
from PART, SUPPLIER, PARTSUPP, NATION, REGION
where p_partkey = ps_partkey and s_suppkey = ps_suppkey and p_size = 15
 and p_type like '%BRASS' and s_nationkey = n_nationkey and n_regionkey = r_regionkey
 and r_name = 'EUROPE'
 and ps_supplycost = (
   select min(ps_supplycost) from PARTSUPP, SUPPLIER, NATION, REGION
   where p_partkey = ps_partkey and s_suppkey = ps_suppkey and s_nationkey = n_nationkey
     and n_regionkey = r_regionkey and r_name = 'EUROPE')
order by s_acctbal desc, n_name, s_name, p_partkey
limit 100;
