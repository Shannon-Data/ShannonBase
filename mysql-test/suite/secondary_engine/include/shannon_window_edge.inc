# Boundary-case window queries that reach VectorizedWindowIterator, captured the
# same way as shannon_window_align.inc: bare selects with one window step each,
# materialized with CREATE TABLE ... AS into the current database.
#
# sh_wf_db.edge is built so one query hits several awkward shapes at once:
#   p=0  three rows whose argument is NULL in every row -- an aggregate over a
#        frame with no non-NULL value at all must be NULL, and COUNT must be 0
#   p=1  a single-row partition
#   p=2  a single-row partition whose one argument is NULL
#   p=3  1500 rows that all share one ORDER BY value, so the partition is a
#        single peer group that outlives an ingest pass; under the shrunken
#        buffer it also outlives the memory budget, which forces the operator to
#        hold every row back and drain the spill file in one sweep
#   p=4  unsigned BIGINT at the top of its range and DECIMAL at the top of its
#        precision, so the int64 fast path has to fold into its decimal carry
#        and the result still has to fit the result field

--echo # E1: RANGE running frame over a NULL-heavy argument, with the ranking
--echo #     functions alongside it.
CREATE TABLE e1 AS
SELECT id, p, COUNT(nv) OVER w AS c, SUM(nv) OVER w AS s, AVG(nv) OVER w AS a,
       MIN(nv) OVER w AS mn, MAX(nv) OVER w AS mx,
       ROW_NUMBER() OVER w AS rn, RANK() OVER w AS rnk, DENSE_RANK() OVER w AS drnk
FROM sh_wf_db.edge WINDOW w AS (PARTITION BY p ORDER BY o);

--echo # E2: the same argument over a whole-partition frame.
CREATE TABLE e2 AS
SELECT id, p, COUNT(nv) OVER w AS c, SUM(nv) OVER w AS s, AVG(nv) OVER w AS a,
       MIN(nv) OVER w AS mn, MAX(nv) OVER w AS mx
FROM sh_wf_db.edge
WINDOW w AS (PARTITION BY p ORDER BY o
             ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING);

--echo # E3: unsigned BIGINT and DECIMAL at the edge of their ranges, running.
CREATE TABLE e3 AS
SELECT id, p, SUM(ud) OVER w AS sud, MIN(ud) OVER w AS mnud, MAX(ud) OVER w AS mxud,
       SUM(dc) OVER w AS sdc, MIN(dc) OVER w AS mndc, MAX(dc) OVER w AS mxdc
FROM sh_wf_db.edge WINDOW w AS (PARTITION BY p ORDER BY o);

--echo # E4: PARTITION BY with no ORDER BY -- the whole partition is one peer
--echo #     group, so the default RANGE frame degenerates to the partition.
CREATE TABLE e4 AS
SELECT id, p, SUM(v) OVER w AS s, COUNT(v) OVER w AS c,
       MIN(v) OVER w AS mn, MAX(v) OVER w AS mx
FROM sh_wf_db.edge WINDOW w AS (PARTITION BY p);
