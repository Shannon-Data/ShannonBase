# Large-input window queries, run once against InnoDB and once against Rapid.
# The row counts here are what push partitions and peer groups across the
# operator's 1024-row ingest pass, so the results are folded into checksums
# rather than printed row by row.

--echo # B1: a 4000-row partition, i.e. a running frame spanning four passes.
SELECT SUM(s), MIN(s), MAX(s), COUNT(s), BIT_XOR(CAST(s AS UNSIGNED))
FROM (SELECT SUM(v) OVER (PARTITION BY p ORDER BY id
                          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s
      FROM big) t;

--echo # B2: peer groups of 1500 rows under the default RANGE frame: a group
--echo #     stays open across passes and every row of it must land on the value
--echo #     the group ends with.
SELECT SUM(s), MIN(s), MAX(s), COUNT(DISTINCT s)
FROM (SELECT SUM(v) OVER (ORDER BY grp) AS s FROM big) t;

--echo # B3: every row its own peer group, so a pass closes 1024 groups at once.
SELECT SUM(s), MIN(s), MAX(s), COUNT(s)
FROM (SELECT SUM(v) OVER (ORDER BY uniq) AS s FROM big) t;

--echo # B4: whole-partition frame over 4000 and 2000 rows -- nothing may be
--echo #     emitted before the partition closes.
SELECT p, MIN(s), MAX(s), MIN(a), MAX(a), COUNT(*)
FROM (SELECT p,
             SUM(v) OVER (PARTITION BY p ORDER BY id
                          ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS s,
             AVG(v) OVER (PARTITION BY p ORDER BY id
                          ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS a
      FROM big) t
GROUP BY p ORDER BY p;

--echo # B5: decimal and double running aggregates at scale.
SELECT SUM(sd), MIN(sd), MAX(sd), ROUND(SUM(sf), 6), ROUND(MIN(sf), 6), ROUND(MAX(sf), 6)
FROM (SELECT SUM(vd) OVER (PARTITION BY p ORDER BY id) AS sd,
             SUM(vf) OVER (PARTITION BY p ORDER BY id) AS sf
      FROM big) t;

--echo # B6: NULLs spread through a large partition.
SELECT SUM(c), SUM(s), ROUND(SUM(a), 6), MIN(mn), MAX(mx)
FROM (SELECT COUNT(nv) OVER w AS c, SUM(nv) OVER w AS s, AVG(nv) OVER w AS a,
             MIN(nv) OVER w AS mn, MAX(nv) OVER w AS mx
      FROM big WINDOW w AS (PARTITION BY p ORDER BY id)) t;

--echo # B7: ranking across pass boundaries -- rank must keep counting from the
--echo #     partition start, not from the start of the buffered block.
SELECT MAX(rn), MAX(rnk), MAX(drnk), SUM(rn), SUM(rnk), SUM(drnk)
FROM (SELECT ROW_NUMBER() OVER w AS rn, RANK() OVER w AS rnk,
             DENSE_RANK() OVER w AS drnk, SUM(v) OVER w AS s
      FROM big WINDOW w AS (PARTITION BY p ORDER BY grp)) t;
