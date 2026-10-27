# Window scenarios shared by shannon_window_functions.test.
# Set $shannon_window_case at each call. The caller owns the database,
# execution mode and debug budgets, so capture/check timing stays explicit.
# Cases: queries, big, align, align_check, edge, edge_check, bigint, spill.
--let $_shannon_window_case_found = 0

if ($shannon_window_case == 'queries') {
  --let $_shannon_window_case_found = 1
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
}

if ($shannon_window_case == 'big') {
  --let $_shannon_window_case_found = 1
  # Large-input window queries, run once against InnoDB and once against Rapid.
  # The row counts here are what push partitions and peer groups across the
  # operator's 1024-row ingest pass, so the results are folded into checksums
  # rather than printed row by row.

  # Note: these wrap the window in a derived table, which materializes above the
  # window step and leaves the whole island on MySQL's window iterators. They pin
  # Rapid's answers at scale, not VectorizedWindowIterator; the operator's own
  # large-input coverage is the algn block in shannon_window_functions.test.

  --echo # B1: a 4000-row partition under a ROWS running frame. That frame needs no
  --echo #     frame buffer, so this one stays on MySQL's non-buffering iterator in
  --echo #     both engines; it is here to pin the answer, not the operator.
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
}

if ($shannon_window_case == 'align') {
  --let $_shannon_window_case_found = 1
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
}

if ($shannon_window_case == 'align_check') {
  --let $_shannon_window_case_found = 1
  # Compare the captured answers of one Rapid run against the InnoDB baseline.
  # $rpd names the database holding the Rapid answers. Runs on the primary
  # engine: none of these tables are loaded into Rapid.
  #
  # base_rows/rapid_rows catch a lost or duplicated row, the two EXCEPT ALL
  # counts catch a wrong value; all four must be zero-difference.

  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a1) - (SELECT COUNT(*) FROM $rpd.a1) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a1 EXCEPT ALL SELECT * FROM $rpd.a1) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a1 EXCEPT ALL SELECT * FROM sh_wf_base.a1) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a2) - (SELECT COUNT(*) FROM $rpd.a2) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a2 EXCEPT ALL SELECT * FROM $rpd.a2) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a2 EXCEPT ALL SELECT * FROM sh_wf_base.a2) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a3) - (SELECT COUNT(*) FROM $rpd.a3) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a3 EXCEPT ALL SELECT * FROM $rpd.a3) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a3 EXCEPT ALL SELECT * FROM sh_wf_base.a3) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a4) - (SELECT COUNT(*) FROM $rpd.a4) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a4 EXCEPT ALL SELECT * FROM $rpd.a4) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a4 EXCEPT ALL SELECT * FROM sh_wf_base.a4) y) AS only_rapid
}

if ($shannon_window_case == 'edge') {
  --let $_shannon_window_case_found = 1
  # Boundary-case window queries that reach VectorizedWindowIterator, captured the
  # same way as the align case: bare selects with one window step each,
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
}

if ($shannon_window_case == 'edge_check') {
  --let $_shannon_window_case_found = 1
  # Compare the captured boundary-case answers of one Rapid run against the
  # InnoDB baseline. $rpd names the database holding the Rapid answers.

  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e1) - (SELECT COUNT(*) FROM $rpd.e1) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e1 EXCEPT ALL SELECT * FROM $rpd.e1) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e1 EXCEPT ALL SELECT * FROM sh_wf_base.e1) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e2) - (SELECT COUNT(*) FROM $rpd.e2) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e2 EXCEPT ALL SELECT * FROM $rpd.e2) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e2 EXCEPT ALL SELECT * FROM sh_wf_base.e2) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e3) - (SELECT COUNT(*) FROM $rpd.e3) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e3 EXCEPT ALL SELECT * FROM $rpd.e3) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e3 EXCEPT ALL SELECT * FROM sh_wf_base.e3) y) AS only_rapid
  --eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e4) - (SELECT COUNT(*) FROM $rpd.e4) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e4 EXCEPT ALL SELECT * FROM $rpd.e4) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e4 EXCEPT ALL SELECT * FROM sh_wf_base.e4) y) AS only_rapid
}

if ($shannon_window_case == 'bigint') {
  --let $_shannon_window_case_found = 1
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
}

if ($shannon_window_case == 'spill') {
  --let $_shannon_window_case_found = 1
  # Window queries whose buffered partition is forced through the spill file.
  # Included twice by shannon_window_functions.test; the two outputs must be
  # byte-identical.

  # Note: these wrap the window in a derived table (S4 ends in ORDER BY), which
  # materializes above the window step and leaves the whole island on MySQL's
  # window iterators, so rapid_window_tiny_buffer does not reach them. The spill
  # path's own coverage is the algn block in shannon_window_functions.test.

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

  --echo # S3: a running ROWS frame needs no frame buffer at all, so it never
  --echo #     reaches this operator and the shrunken budget cannot move its
  --echo #     values either; kept as a control on the surrounding queries.
  SELECT SUM(s), MIN(s), MAX(s), COUNT(s)
  FROM (SELECT SUM(v) OVER (PARTITION BY p ORDER BY id
                            ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s
        FROM big) t;

  --echo # S4: the spilled rows carry the non-window columns too, so select them
  --echo #     back out alongside the window value.
  SELECT id, p, grp, v, SUM(v) OVER (PARTITION BY p ORDER BY id
                                     ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS s
  FROM big WHERE id IN (1, 2, 3, 4, 5, 6, 4000, 4001, 5999, 6000) ORDER BY id;
}

if (!$_shannon_window_case_found) {
  --die Unknown shannon_window_case
}
