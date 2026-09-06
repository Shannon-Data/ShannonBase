# Read block for shannon_online_ddl_corner.test. Only columns that exist at
# every stage are named explicitly; `select *` is deliberately included so a
# stale column layout in the IMCS shows up.
select * from d1 order by id;
select id, k from d1 order by id;
select k, count(*) as c, sum(k) as s from d1 group by k order by k;
select id from d1 where k >= 20 order by id;
