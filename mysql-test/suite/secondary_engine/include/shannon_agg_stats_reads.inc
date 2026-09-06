# Read block for shannon_agg_stats_corner.test. Floating-point aggregates are
# rounded so a last-bit difference in summation order is not mistaken for a
# wrong answer, but a real disagreement still shows.
select g, group_concat(v order by v separator ',') as gc from a1 group by g order by g;
select g, group_concat(distinct k order by k desc separator '|') as gcd from a1 group by g order by g;
select group_concat(v order by id separator '') as gc_all from a1;
select g, count(*) as c, count(k) as ck,
       round(stddev_pop(k),10)  as sp,
       round(stddev_samp(k),10) as ss,
       round(var_pop(k),10)     as vp,
       round(var_samp(k),10)    as vs
  from a1 group by g order by g;
select round(stddev_pop(k),10) as sp, round(var_samp(k),10) as vs from a1;
select g, bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx from a1 group by g order by g;
--echo # aggregate over an empty set: BIT_AND is all ones, BIT_OR/XOR are zero,
--echo # SUM/MIN/MAX are NULL and COUNT is 0.
select bit_and(k) as ba, bit_or(k) as bo, bit_xor(k) as bx,
       sum(k) as s, min(k) as mn, max(k) as mx, count(k) as c
  from a1 where g = 999;
--echo # a group whose values are all NULL: COUNT(k)=0 but the group exists.
select g, count(*) as c, count(k) as ck, sum(k) as s, avg(k) as a,
       group_concat(k) as gc
  from a1 where g = 40 group by g;
select g, count(*) as c, sum(k) as s from a1 group by g with rollup order by g;
select g, k, count(*) as c from a1 group by g, k with rollup order by g, k;
select any_value(g) as av, count(*) as c from a1;
select g, count(*) as c from a1 group by g having sum(k) is null order by g;
