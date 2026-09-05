# Window queries that actually reach VectorizedWindowIterator, materialized so
# the InnoDB and the Rapid answer can be compared row by row.
#
# The shape matters: the operator is only built when nothing materializes above
# the window step. A trailing ORDER BY, a derived table, an outer aggregate and
# a second window step in the same query all put a temporary table above it,
# which turns the whole physical island native again -- so the queries here are
# bare selects with one window step each, and CREATE TABLE ... AS is what makes
# their output comparable without an ORDER BY. The caller checks
# rapid_query_vectorized_window_rows_total around this file to prove the
# operator ran; without that a fallback would silently make all of it vacuous.
#
# Read from sh_wf_db.algn and write into the current database, so the same file
# can capture the InnoDB answer, the Rapid answer and the spilled Rapid answer.
#
# algn is sized so a partition ends exactly on the 1024-row ingest pass twice
# over: partition 0 is 1024 rows (a pass counted from the first row of the
# input) and partition 1 is 1025 rows (a pass counted from the row carried over
# from partition 0). Partition 2 is 2048 rows, ending mid-pass, and partition 3
# is a six-row tail.

--echo # A1: RANGE running frame with 100-row peer groups, so a peer group is
--echo #     still open when a pass runs out of rows, together with the ranking
--echo #     functions -- all partition scoped, so nothing may cross a boundary.
CREATE TABLE a1 AS
SELECT id, p, SUM(v) OVER w AS s, ROW_NUMBER() OVER w AS rn,
       RANK() OVER w AS rnk, DENSE_RANK() OVER w AS drnk
FROM sh_wf_db.algn WINDOW w AS (PARTITION BY p ORDER BY o);

--echo # A2: every row its own peer group, so one pass closes 1024 groups and
--echo #     partition 0 ends with the last group of a pass.
CREATE TABLE a2 AS
SELECT id, p, SUM(v) OVER w AS s, COUNT(nv) OVER w AS c, AVG(nv) OVER w AS a,
       MIN(nv) OVER w AS mn, MAX(nv) OVER w AS mx
FROM sh_wf_db.algn WINDOW w AS (PARTITION BY p ORDER BY id);

--echo # A3: whole-partition frame over the same boundaries: nothing may be
--echo #     emitted before the partition closes, and every accumulator must
--echo #     restart from zero for the next one.
CREATE TABLE a3 AS
SELECT id, p, SUM(v) OVER w AS s, COUNT(nv) OVER w AS c, AVG(nv) OVER w AS a,
       MIN(nv) OVER w AS mn, MAX(nv) OVER w AS mx
FROM sh_wf_db.algn
WINDOW w AS (PARTITION BY p ORDER BY id
             ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING);

--echo # A4: decimal and double arguments over a single 4103-row partition --
--echo #     no PARTITION BY, so one partition spans four ingest passes.
CREATE TABLE a4 AS
SELECT id, SUM(vd) OVER w AS sd, AVG(vd) OVER w AS ad, MIN(vd) OVER w AS mnd,
       SUM(vf) OVER w AS sf, MIN(vf) OVER w AS mnf, MAX(vf) OVER w AS mxf
FROM sh_wf_db.algn WINDOW w AS (ORDER BY o);
