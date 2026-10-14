# Read block for shannon_dml_upsert_corner.test: sourced once per scenario by
# shannon_3way.inc. Every statement is fully ordered so the three scenarios can
# be compared byte for byte.
select id, k, v from u1 order by id;
--let $assert_query = select id, k, v from u1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select count(*) as n, count(k) as nk, count(v) as nv, sum(k) as sk from u1;
--let $assert_query = select count(*) as n, count(k) as nk, count(v) as nv, sum(k) as sk from u1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select k, count(*) as c from u1 group by k order by k;
--let $assert_query = select k, count(*) as c from u1 group by k order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select u1.id, u1.k, u2.w from u1 join u2 on u1.id = u2.id order by u1.id;
--let $assert_query = select u1.id, u1.k, u2.w from u1 join u2 on u1.id = u2.id order by u1.id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id, w from u2 order by id;
--let $assert_query = select id, w from u2 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
