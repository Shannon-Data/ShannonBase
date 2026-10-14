# Read block for shannon_numeric_boundary_corner.test.
select id, ti, si, mi, ii, bi from n1 order by id;
--let $assert_query = select id, ti, si, mi, ii, bi from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id, tu, su, mu, iu, bu from n1 order by id;
--let $assert_query = select id, tu, su, mu, iu, bu from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select min(ti) as mnti, max(ti) as mxti, min(bi) as mnbi, max(bi) as mxbi,
       min(bu) as mnbu, max(bu) as mxbu
  from n1;
--let $assert_query = select min(ti) as mnti, max(ti) as mxti, min(bi) as mnbi, max(bi) as mxbi, min(bu) as mnbu, max(bu) as mxbu from n1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # the unsigned 64-bit range above 2^63-1 must not be read as negative
select id from n1 where bu > 9223372036854775807 order by id;
--let $assert_query = select id from n1 where bu > 9223372036854775807 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id from n1 where bi = -9223372036854775808;
--let $assert_query = select id from n1 where bi = -9223372036854775808
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id from n1 where bi < 0 order by id;
--let $assert_query = select id from n1 where bi < 0 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # DECIMAL at full precision, and a scale that cannot be a double
select id, d65, d20 from n1 order by id;
--let $assert_query = select id, d65, d20 from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select sum(d20) as s20, min(d65) as mn65, max(d65) as mx65, count(d65) as c from n1;
--let $assert_query = select sum(d20) as s20, min(d65) as mn65, max(d65) as mx65, count(d65) as c from n1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id, d20 * 2 as twice, d20 + 0.0000000001 as bumped from n1 order by id;
--let $assert_query = select id, d20 * 2 as twice, d20 + 0.0000000001 as bumped from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # DOUBLE/FLOAT extremes and negative zero
select id, dbl, flt from n1 order by id;
--let $assert_query = select id, dbl, flt from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id from n1 where dbl = 0 order by id;
--let $assert_query = select id from n1 where dbl = 0 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select sum(dbl) as sd, min(dbl) as mnd, max(dbl) as mxd from n1;
--let $assert_query = select sum(dbl) as sd, min(dbl) as mnd, max(dbl) as mxd from n1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # explicit conversions across the boundary
select id, cast(bi as decimal(20,0)) as bi_dec, cast(bu as char) as bu_chr from n1 order by id;
--let $assert_query = select id, cast(bi as decimal(20,0)) as bi_dec, cast(bu as char) as bu_chr from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select id, ti + 0 as ti_i, -ti as ti_neg, abs(ti) as ti_abs from n1 order by id;
--let $assert_query = select id, ti + 0 as ti_i, -ti as ti_neg, abs(ti) as ti_abs from n1 order by id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # grouping and joining on boundary values must not collide
select bi, count(*) as c from n1 group by bi order by bi;
--let $assert_query = select bi, count(*) as c from n1 group by bi order by bi
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select a.id, b.id from n1 a join n1 b on a.bu = b.bu order by a.id, b.id;
--let $assert_query = select a.id, b.id from n1 a join n1 b on a.bu = b.bu order by a.id, b.id
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
