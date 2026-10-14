# Read block for shannon_agg_stats_corner.test. Floating-point aggregates are
# rounded so a last-bit difference in summation order is not mistaken for a
# wrong answer, but a real disagreement still shows.
select g, group_concat(v order by v separator ',') as gc from a1 group by g order by g;
--let $assert_query = select g, group_concat(v order by v separator ',') as gc from a1 group by g order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select g, group_concat(distinct k order by k desc separator '|') as gcd from a1 group by g order by g;
--let $assert_query = select g, group_concat(distinct k order by k desc separator '|') as gcd from a1 group by g order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select group_concat(v order by id separator '') as gc_all from a1;
--let $assert_query = select group_concat(v order by id separator '') as gc_all from a1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select g, count(*) as c, count(k) as ck,
       round(stddev_pop(k),10)  as sp,
       round(stddev_samp(k),10) as ss,
       round(var_pop(k),10)     as vp,
       round(var_samp(k),10)    as vs
  from a1 group by g order by g;
--let $assert_query = select g, count(*) as c, count(k) as ck, round(stddev_pop(k),10) as sp, round(stddev_samp(k),10) as ss, round(var_pop(k),10) as vp, round(var_samp(k),10) as vs from a1 group by g order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select round(stddev_pop(k),10) as sp, round(var_samp(k),10) as vs from a1;
--let $assert_query = select round(stddev_pop(k),10) as sp, round(var_samp(k),10) as vs from a1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select g, bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx from a1 group by g order by g;
--let $assert_query = select g, bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx from a1 group by g order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # aggregate over an empty set: BIT_AND is all ones, BIT_OR/XOR are zero,
--echo # SUM/MIN/MAX are NULL and COUNT is 0.
select bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx,
       sum(k) as s, min(k) as mn, max(k) as mx, count(k) as c
  from a1 where g = 999;
--let $assert_query = select bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx, sum(k) as s, min(k) as mn, max(k) as mx, count(k) as c from a1 where g = 999
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
--echo # a group whose values are all NULL: COUNT(k)=0 but the group exists.
select g, count(*) as c, count(k) as ck, sum(k) as s, avg(k) as a,
       group_concat(k) as gc
  from a1 where g = 40 group by g;
--let $assert_query = select g, count(*) as c, count(k) as ck, sum(k) as s, avg(k) as a, group_concat(k) as gc from a1 where g = 40 group by g
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select g, count(*) as c, sum(k) as s from a1 group by g with rollup order by g;
--let $assert_query = select g, count(*) as c, sum(k) as s from a1 group by g with rollup order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select g, k, count(*) as c from a1 group by g, k with rollup order by g, k;
--let $assert_query = select g, k, count(*) as c from a1 group by g, k with rollup order by g, k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
select any_value(g) as av, count(*) as c from a1;
# ANY_VALUE may choose any fixture group; equality between arbitrary choices
# is not a SQL requirement. Check its domain and the original query's count.
--disable_query_log
--let $any_group = query_get_value("select any_value(g) as av, count(*) as c from a1", av, 1)
--let $any_count = query_get_value("select any_value(g) as av, count(*) as c from a1", c, 1)
--let $any_group_valid = `SELECT $any_group IN (10,20,30,40)`
if ($any_group_valid != 1) {
  --die ANY_VALUE returned a group absent from the primary fixture
}
if ($any_count != 10) {
  --die ANY_VALUE query returned an incorrect aggregate row count
}
--enable_query_log
select g, count(*) as c from a1 group by g having sum(k) is null order by g;
--let $assert_query = select g, count(*) as c from a1 group by g having sum(k) is null order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
