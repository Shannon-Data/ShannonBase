# Read block for shannon_numeric_boundary_corner.test.
select id, ti, si, mi, ii, bi from n1 order by id;
select id, tu, su, mu, iu, bu from n1 order by id;
select min(ti) as mnti, max(ti) as mxti, min(bi) as mnbi, max(bi) as mxbi,
       min(bu) as mnbu, max(bu) as mxbu
  from n1;
--echo # the unsigned 64-bit range above 2^63-1 must not be read as negative
select id from n1 where bu > 9223372036854775807 order by id;
select id from n1 where bi = -9223372036854775808;
select id from n1 where bi < 0 order by id;
--echo # DECIMAL at full precision, and a scale that cannot be a double
select id, d65, d20 from n1 order by id;
select sum(d20) as s20, min(d65) as mn65, max(d65) as mx65, count(d65) as c from n1;
select id, d20 * 2 as twice, d20 + 0.0000000001 as bumped from n1 order by id;
--echo # DOUBLE/FLOAT extremes and negative zero
select id, dbl, flt from n1 order by id;
select id from n1 where dbl = 0 order by id;
select sum(dbl) as sd, min(dbl) as mnd, max(dbl) as mxd from n1;
--echo # explicit conversions across the boundary
select id, cast(bi as decimal(20,0)) as bi_dec, cast(bu as char) as bu_chr from n1 order by id;
select id, ti + 0 as ti_i, -ti as ti_neg, abs(ti) as ti_abs from n1 order by id;
--echo # grouping and joining on boundary values must not collide
select bi, count(*) as c from n1 group by bi order by bi;
select a.id, b.id from n1 a join n1 b on a.bu = b.bu order by a.id, b.id;
