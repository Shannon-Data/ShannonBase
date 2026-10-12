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

select cntrycode, count(*) as numcust, sum(c_acctbal) as totacctbal
from (
 select substring(c_phone from 1 for 2) as cntrycode, c_acctbal
 from CUSTOMER
 where substring(c_phone from 1 for 2) in ('13', '31', '23', '29', '30', '18', '17')
  and c_acctbal > (select avg(c_acctbal) from CUSTOMER
    where c_acctbal > 0.00 and substring(c_phone from 1 for 2) in ('13', '31', '23', '29', '30', '18', '17'))
  and not exists (select * from ORDERS where o_custkey = c_custkey)
) as custsale
group by cntrycode
order by cntrycode;
