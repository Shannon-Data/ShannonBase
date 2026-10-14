# Read block for shannon_online_ddl_corner.test. Only columns that exist at
# every stage are named explicitly; `select *` is deliberately included so a
# stale column layout in the IMCS shows up.
select * from d1 order by id;
--let $assert_query = select * from d1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id, k from d1 order by id;
--let $assert_query = select id, k from d1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select k, count(*) as c, sum(k) as s from d1 group by k order by k;
--let $assert_query = select k, count(*) as c, sum(k) as s from d1 group by k order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id from d1 where k >= 20 order by id;
--let $assert_query = select id from d1 where k >= 20 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
