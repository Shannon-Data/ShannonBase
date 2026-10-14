# Read block for shannon_setops_corner.test. Set operations dedup with NULL
# treated as equal to NULL, which is the opposite of `=`; every query is
# ordered so the scenarios compare byte for byte.
#
# Successful operations are compared with InnoDB immediately. A refused shape
# has no result to compare; the main test explicitly checks FORCED refusal and
# ON fallback for derived INTERSECT/EXCEPT. Do not record a successful wrong
# answer as an alternative to that capability check.
--error 0,ER_SECONDARY_ENGINE
select k from s1 union select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 union select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from s1 union all select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 union all select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from s1 intersect select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 intersect select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from s1 intersect all select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 intersect all select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from s1 except select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 except select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from s1 except all select k from s2 order by k;
if (!$mysql_errno) {
--let $assert_query = select k from s1 except all select k from s2 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k, g from s1 union select k, g from s2 order by k, g;
if (!$mysql_errno) {
--let $assert_query = select k, g from s1 union select k, g from s2 order by k, g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select distinct k from s1 order by k;
if (!$mysql_errno) {
--let $assert_query = select distinct k from s1 order by k
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select distinct k, g from s1 order by k, g;
if (!$mysql_errno) {
--let $assert_query = select distinct k, g from s1 order by k, g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select count(*) as c, count(k) as ck, count(distinct k) as cdk from s1;
if (!$mysql_errno) {
--let $assert_query = select count(*) as c, count(k) as ck, count(distinct k) as cdk from s1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select count(distinct k, g) as cdkg from s1;
if (!$mysql_errno) {
--let $assert_query = select count(distinct k, g) as cdkg from s1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select g, count(distinct k) as cdk, sum(distinct k) as sdk, avg(distinct k) as adk
  from s1 group by g order by g;
if (!$mysql_errno) {
--let $assert_query = select g, count(distinct k) as cdk, sum(distinct k) as sdk, avg(distinct k) as adk from s1 group by g order by g
--let $assert_ordered = 1
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select k from (select k from s1 union select k from s2) d where k is null;
if (!$mysql_errno) {
--let $assert_query = select k from (select k from s1 union select k from s2) d where k is null
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
--error 0,ER_SECONDARY_ENGINE
select count(*) as c from (select k from s1 intersect select k from s2) d;
if (!$mysql_errno) {
--let $assert_query = select count(*) as c from (select k from s1 intersect select k from s2) d
--source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
}
