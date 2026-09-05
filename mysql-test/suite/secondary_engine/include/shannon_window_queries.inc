# Window function queries run once against InnoDB and once against Rapid.
# Included twice by shannon_window_functions.test; the two outputs must be
# byte-identical.

# Note: every query here ends in ORDER BY, which materializes above the
# window step and puts the whole physical island back on MySQL's own window
# iterators. They pin Rapid's answers, not VectorizedWindowIterator; the
# operator's own coverage is the algn block in shannon_window_functions.test.

--echo # W1: ranking only -- no framing function, so the window does not buffer
--echo #     and stays on MySQL's streaming WindowIterator even under Rapid.
SELECT id, p, o,
       ROW_NUMBER() OVER w AS rn,
       RANK()       OVER w AS rnk,
       DENSE_RANK() OVER w AS drnk
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W2: default frame (RANGE UNBOUNDED PRECEDING .. CURRENT ROW): every row
--echo #     of a peer group sees the same running aggregate.
SELECT id, p, o,
       COUNT(i4) OVER w AS c,
       SUM(i4)   OVER w AS s,
       AVG(i4)   OVER w AS a,
       MIN(i4)   OVER w AS mn,
       MAX(i4)   OVER w AS mx
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W3: same aggregates over ROWS UNBOUNDED PRECEDING .. CURRENT ROW, where
--echo #     peers no longer share a value -- a running value per row.
SELECT id, p, o,
       COUNT(i4) OVER w AS c,
       SUM(i4)   OVER w AS s,
       AVG(i4)   OVER w AS a,
       MIN(i4)   OVER w AS mn,
       MAX(i4)   OVER w AS mx
FROM wf
WINDOW w AS (PARTITION BY p ORDER BY o ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
ORDER BY id;

--echo # W4: whole-partition frame -- one value for every row of the partition.
SELECT id, p,
       COUNT(i4) OVER w AS c,
       SUM(i4)   OVER w AS s,
       AVG(i4)   OVER w AS a,
       MIN(i4)   OVER w AS mn,
       MAX(i4)   OVER w AS mx
FROM wf
WINDOW w AS (PARTITION BY p ORDER BY o ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)
ORDER BY id;

--echo # W5: PARTITION BY without ORDER BY -- the whole partition is one peer
--echo #     group, so the default RANGE frame degenerates to the partition.
SELECT id, p, SUM(i4) OVER (PARTITION BY p) AS s, COUNT(*) OVER (PARTITION BY p) AS c
FROM wf ORDER BY id;

--echo # W6: no PARTITION BY at all -- a single partition over the whole input.
SELECT id, o, SUM(i4) OVER (ORDER BY o) AS s, RANK() OVER (ORDER BY o) AS rnk
FROM wf ORDER BY id;

--echo # W7: NULL arguments. NULLs must not be counted, and a frame whose rows
--echo #     are all NULL yields NULL for SUM/AVG/MIN/MAX but 0 for COUNT.
SELECT id, p, o,
       COUNT(ni) OVER w AS c_ni,
       SUM(ni)   OVER w AS s_ni,
       AVG(ni)   OVER w AS a_ni,
       MIN(ni)   OVER w AS mn_ni,
       MAX(ni)   OVER w AS mx_ni,
       COUNT(nd) OVER w AS c_nd,
       SUM(nd)   OVER w AS s_nd,
       AVG(nd)   OVER w AS a_nd
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W8: partition 4 has only NULL arguments from its first row on.
SELECT id, SUM(ni) OVER w AS s, AVG(ni) OVER w AS a, COUNT(ni) OVER w AS c,
       MIN(ni) OVER w AS mn, MAX(ni) OVER w AS mx
FROM wf WHERE p = 4 WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W9: decimal and double arguments. SUM/AVG over a decimal must keep
--echo #     MySQL's decimal scale, and the double sum must round identically,
--echo #     which is why the operator adds doubles in row order.
SELECT id, p, o,
       SUM(dec1) OVER w AS s_dec, AVG(dec1) OVER w AS a_dec,
       MIN(dec1) OVER w AS mn_dec, MAX(dec1) OVER w AS mx_dec,
       SUM(d)    OVER w AS s_d,   AVG(d)    OVER w AS a_d,
       MIN(d)    OVER w AS mn_d,  MAX(d)    OVER w AS mx_d
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W10: BIGINT and BIGINT UNSIGNED -- values past the signed range must
--echo #      not wrap in the integer accumulator.
SELECT id, p,
       SUM(i8) OVER w AS s_i8, MIN(i8) OVER w AS mn_i8, MAX(i8) OVER w AS mx_i8,
       SUM(u8) OVER w AS s_u8, MIN(u8) OVER w AS mn_u8, MAX(u8) OVER w AS mx_u8
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W11: expressions over window function results, and a window function
--echo #      over an expression.
SELECT id,
       1 + RANK() OVER w                AS rank_plus_one,
       SUM(i4) OVER w * 2               AS twice_sum,
       SUM(i4 + 1) OVER w               AS sum_of_expr,
       SUM(i4) OVER w + COUNT(*) OVER w AS mixed
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W12: two different windows in one query -- two window steps, each
--echo #      evaluated on its own.
SELECT id, p, o,
       SUM(i4) OVER (PARTITION BY p ORDER BY o)  AS s_by_p,
       SUM(i4) OVER (ORDER BY o, id)             AS s_global,
       RANK()  OVER (PARTITION BY o ORDER BY p)  AS rank_by_o
FROM wf ORDER BY id;

--echo # W13: outside the supported subset -- these must fall back to MySQL's
--echo #      own window iterators and still be right.
SELECT id, p, o,
       LAG(i4)         OVER w AS lag_i4,
       LEAD(i4)        OVER w AS lead_i4,
       FIRST_VALUE(i4) OVER w AS first_i4,
       LAST_VALUE(i4)  OVER w AS last_i4,
       NTILE(2)        OVER w AS ntile2
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W14: sliding and value-bounded frames are outside the subset (rows
--echo #      leave the frame again), and so is DISTINCT.
SELECT id, p, o,
       SUM(i4) OVER (PARTITION BY p ORDER BY o ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)   AS sliding,
       SUM(i4) OVER (PARTITION BY p ORDER BY o ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS tail_sum,
       SUM(i4) OVER (PARTITION BY p ORDER BY o RANGE BETWEEN 5 PRECEDING AND CURRENT ROW)  AS ranged
FROM wf ORDER BY id;

--echo # W15: a supported and an unsupported function sharing one window; the
--echo #      whole step falls back, so the supported one must still be right.
SELECT id, p, o,
       SUM(i4) OVER w AS s,
       LAG(i4) OVER w AS lg
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W16: string arguments to MIN/MAX are outside the subset.
SELECT id, p, MIN(s) OVER w AS mn_s, MAX(s) OVER w AS mx_s
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W20: temporal, YEAR, BIT and ENUM arguments are outside the subset:
--echo #      their value and storage representations differ, so reading them
--echo #      back through val_int() would drop e.g. fractional seconds. They
--echo #      must fall back and stay exact.
SELECT id, p,
       MIN(dt) OVER w AS mn_dt, MAX(dt) OVER w AS mx_dt,
       MIN(yr) OVER w AS mn_yr, MAX(yr) OVER w AS mx_yr,
       MIN(e)  OVER w AS mn_e,  MAX(e)  OVER w AS mx_e,
       SUM(yr) OVER w AS s_yr
FROM wf WINDOW w AS (PARTITION BY p ORDER BY o) ORDER BY id;

--echo # W17: a window on top of a filter, so the buffered partition is a strict
--echo #      subset of the table.
SELECT id, p, o, SUM(i4) OVER (PARTITION BY p ORDER BY o) AS s
FROM wf WHERE i4 IS NOT NULL AND o >= 10 ORDER BY id;

--echo # W18: window over a grouped result -- the window input is the aggregate
--echo #      output, not a base table.
SELECT p, SUM(i4) AS gs, SUM(SUM(i4)) OVER (ORDER BY p) AS running_gs
FROM wf GROUP BY p ORDER BY p;

--echo # W19: DISTINCT after windowing, and windowing combined with LIMIT.
SELECT DISTINCT p, SUM(i4) OVER (PARTITION BY p) AS s FROM wf ORDER BY p;
SELECT id, SUM(i4) OVER (ORDER BY id) AS s FROM wf ORDER BY id LIMIT 4;
