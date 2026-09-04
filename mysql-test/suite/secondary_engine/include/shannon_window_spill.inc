# Window queries whose buffered partition is forced through the spill file.
# Included twice by shannon_window_functions.test; the two outputs must be
# byte-identical.

--echo # S1: whole-partition frame -- every record image is buffered until the
--echo #     partition closes, so all but the first few come back off disk.
SELECT p, MIN(s), MAX(s), MIN(c), MAX(c), COUNT(*)
FROM (SELECT p,
             SUM(v) OVER (PARTITION BY p ORDER BY id
                          ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS s,
             COUNT(v) OVER (PARTITION BY p ORDER BY id
                            ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS c
      FROM big) t
GROUP BY p ORDER BY p;

--echo # S2: a long peer group under the default RANGE frame spills the same way,
--echo #     and ranking must survive the round trip through the file.
SELECT MAX(rn), MAX(rnk), MAX(drnk), SUM(s), MIN(s), MAX(s)
FROM (SELECT ROW_NUMBER() OVER w AS rn, RANK() OVER w AS rnk,
             DENSE_RANK() OVER w AS drnk, SUM(v) OVER w AS s
      FROM big WINDOW w AS (PARTITION BY p ORDER BY grp)) t;

--echo # S3: a running ROWS frame does not need buffering to compute, but the
--echo #     shrunken budget makes it buffer anyway; the values must not move.
SELECT SUM(s), MIN(s), MAX(s), COUNT(s)
FROM (SELECT SUM(v) OVER (PARTITION BY p ORDER BY id
                          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s
      FROM big) t;

--echo # S4: the spilled rows carry the non-window columns too, so select them
--echo #     back out alongside the window value.
SELECT id, p, grp, v, SUM(v) OVER (PARTITION BY p ORDER BY id
                                   ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS s
FROM big WHERE id IN (1, 2, 3, 4, 5, 6, 4000, 4001, 5999, 6000) ORDER BY id;
