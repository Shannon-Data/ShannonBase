# Running SUM/AVG over integers that overflow an int64 accumulator.
# Included twice by shannon_window_functions.test; the two outputs must be
# byte-identical.

--echo # I1: signed BIGINT, running frame
SELECT id, p, SUM(b) OVER w AS s, AVG(b) OVER w AS a, MIN(b) OVER w AS mn, MAX(b) OVER w AS mx
FROM wf_big_int
WINDOW w AS (PARTITION BY p ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
ORDER BY id;

--echo # I2: unsigned BIGINT past the signed range, running frame
SELECT id, p, SUM(ub) OVER w AS s, AVG(ub) OVER w AS a, MIN(ub) OVER w AS mn, MAX(ub) OVER w AS mx
FROM wf_big_int
WINDOW w AS (PARTITION BY p ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
ORDER BY id;

--echo # I3: whole-partition frame over the same values
SELECT id, p, SUM(b) OVER w AS sb, SUM(ub) OVER w AS sub
FROM wf_big_int
WINDOW w AS (PARTITION BY p ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)
ORDER BY id;
