# Read block for shannon_join_shapes_corner.test.
--echo # cartesian product, explicit and implicit
select a.id, b.id from c1 a cross join c2 b order by a.id, b.id;
select count(*) as c from c1, c2;
--echo # cartesian product with a filter that is not a join condition
select a.id, b.id from c1 a cross join c2 b where a.k > b.k order by a.id, b.id;
--echo # self join, including the NULL key that must not match itself
select a.id, b.id, a.k from c1 a join c1 b on a.k = b.k and a.id < b.id order by a.id, b.id;
select a.id, b.id from c1 a left join c1 b on a.k = b.k and a.id <> b.id order by a.id, b.id;
--echo # correlated EXISTS / NOT EXISTS
select id from c1 where exists (select 1 from c2 where c2.k = c1.k) order by id;
select id from c1 where not exists (select 1 from c2 where c2.k = c1.k) order by id;
select id from c1 where exists (select 1 from c2 where c2.k > c1.k) order by id;
--echo # scalar subquery: a match, no match (NULL) and an aggregate
select id, (select w from c2 where c2.k = c1.k limit 1) as w from c1 order by id;
select id, k, (select max(w) from c2) as mx from c1 order by id;
select id from c1 where k = (select min(k) from c2) order by id;
--echo # a CTE referenced twice in one query
with cte as (select k, count(*) as c from c1 group by k)
select l.k as lk, r.k as rk, l.c, r.c from cte l join cte r on l.c = r.c
order by lk, rk;
--echo # LATERAL: the derived table depends on the outer row
select c1.id, d.n from c1, lateral (select count(*) as n from c2 where c2.k <= c1.k) d
order by c1.id;
--echo # STRAIGHT_JOIN pins the join order rather than the answer
select straight_join c1.id, c2.id from c1 join c2 on c1.k = c2.k order by c1.id, c2.id;
