# Read block for shannon_setops_corner.test. Set operations dedup with NULL
# treated as equal to NULL, which is the opposite of `=`; every query is
# ordered so the scenarios compare byte for byte.
#
# Each statement tolerates ER_SECONDARY_ENGINE so a shape Rapid refuses to
# offload is *recorded* rather than aborting the test. Note how a refusal reads
# in the .result: mysqltest suppresses the output of a tolerated error, so the
# statement is echoed with NO result set under it at all -- not even a column
# header. Every set operation under the hypergraph optimizer currently looks
# like that. The InnoDB and legacy passes must still agree row for row, and if
# a refused shape starts offloading, its rows appear and the test fails until
# the result is re-recorded, which is the signal we want.
--error 0,ER_SECONDARY_ENGINE
select k from s1 union select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k from s1 union all select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k from s1 intersect select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k from s1 intersect all select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k from s1 except select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k from s1 except all select k from s2 order by k;
--error 0,ER_SECONDARY_ENGINE
select k, g from s1 union select k, g from s2 order by k, g;
--error 0,ER_SECONDARY_ENGINE
select distinct k from s1 order by k;
--error 0,ER_SECONDARY_ENGINE
select distinct k, g from s1 order by k, g;
--error 0,ER_SECONDARY_ENGINE
select count(*) as c, count(k) as ck, count(distinct k) as cdk from s1;
--error 0,ER_SECONDARY_ENGINE
select count(distinct k, g) as cdkg from s1;
--error 0,ER_SECONDARY_ENGINE
select g, count(distinct k) as cdk, sum(distinct k) as sdk, avg(distinct k) as adk
  from s1 group by g order by g;
--error 0,ER_SECONDARY_ENGINE
select k from (select k from s1 union select k from s2) d where k is null;
--error 0,ER_SECONDARY_ENGINE
select count(*) as c from (select k from s1 intersect select k from s2) d;
