# Read block for shannon_dml_upsert_corner.test: sourced once per scenario by
# shannon_3way.inc. Every statement is fully ordered so the three scenarios can
# be compared byte for byte.
select id, k, v from u1 order by id;
select count(*) as n, count(k) as nk, count(v) as nv, sum(k) as sk from u1;
select k, count(*) as c from u1 group by k order by k;
select u1.id, u1.k, u2.w from u1 join u2 on u1.id = u2.id order by u1.id;
select id, w from u2 order by id;
