# Assert that Rapid returns what InnoDB returns.
#
# Digest/parity readings run three ways and all three must agree:
#   1. InnoDB                      -- the ground truth
#   2. Rapid, legacy optimizer     -- USE_SECONDARY_ENGINE=FORCED, hypergraph off
#   3. Rapid, hypergraph optimizer -- USE_SECONDARY_ENGINE=FORCED, hypergraph on
#
# The two optimizers are not interchangeable. They hand Rapid different plans
# for the same SQL: hypergraph pushes a range down as a prune_predicate on the
# scan with no Filter above it, while the legacy optimizer leaves the Filter in
# place. A predicate that converts incorrectly is therefore wrong under one and
# right under the other, and checking only one reports the engine as correct.
#
# Digest/parity modes run all three here, rather than leaving the third to
# a second suite pass with mtr's --hypergraph: no CI workflow passes that flag,
# so that leg would never run.
#
# Comparing against InnoDB rather than a recorded value is the other half. A
# .result can only pin what the engines did on the day it was written, so
# running with --record adopts a divergence as the expected answer and the test
# passes over it forever. Two wrong-rows defects reached the tree that way.
#
# Digest/parity usage:
#
#   Digest mode -- check one scalar query.
#     --let $assert_digest = SELECT GROUP_CONCAT(pk ORDER BY pk) FROM t WHERE ...
#     --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
#
#   Parity mode -- check one WHERE clause against both an indexed table
#   (t_idx) and a non-indexed one (t_noidx) holding the same rows, six
#   readings in total. Both shapes matter: an indexed column turns the
#   predicate into a range, an unindexed one leaves it a residual filter, and
#   those are separate decoders that have disagreed in shipped code.
#     --let $parity_where = w > 12345678901234567.89
#     --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
#
# Either mode also takes an optional $assert_trigger: one statement run
# immediately before the digest, with FLUSH TABLES first, separately for each
# reading. Some defects live in the statement AFTER a particular one -- a bound
# converted through the live table Field depends on what the previous row left
# in the record buffer -- and a digest run on its own cannot see them. Without
# the flush the readings inherit each other's buffer and such a case looks
# intermittent. Unset it when done; unlike $parity_where it is not cleared here.
#
# Restores use_secondary_engine and the hypergraph switch to whatever the
# caller had, and clears $parity_where so the next caller is not surprised by
# a stale one.

# Full-result mode preserves the original query and checks the current Rapid
# execution arm against InnoDB. Unlike digest mode it accepts multiple columns
# and rows without wrapping the SELECT in an aggregate or a derived table:
#   --let $assert_query = SELECT id,v FROM t ORDER BY id
#   --let $assert_ordered = 1
#   --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
# Leave $assert_ordered unset for an unordered multiset comparison. The caller
# must use a deterministic query, enable Rapid (FORCED for guaranteed coverage),
# and run this at every optimizer arm being tested. OFF skips full-result checks.
# $assert_queries_off/$assert_results_off=1 preserve disabled logging.
# CTAS mode compares precomputed tables with EXCEPT ALL in both directions:
#   --let $assert_reference_table = innodb_capture
#   --let $assert_actual_table = rapid_capture
#   --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc

if ($assert_query == '') {
if ($assert_reference_table == '') {
--let $_armi_left = 1
if ($parity_where != '') {
  --let $_armi_left = 2
}

while ($_armi_left) {
  if ($parity_where != '') {
    if ($_armi_left == 2) {
      --let $_armi_which = t_idx (indexed)
      --let $assert_digest = SELECT CONCAT(COUNT(*),':',IFNULL(GROUP_CONCAT(pk ORDER BY pk),'-')) FROM t_idx WHERE $parity_where
    }
    if ($_armi_left == 1) {
      --let $_armi_which = t_noidx (no index)
      --let $assert_digest = SELECT CONCAT(COUNT(*),':',IFNULL(GROUP_CONCAT(pk ORDER BY pk),'-')) FROM t_noidx WHERE $parity_where
    }
  }

  --disable_query_log
  --disable_warnings
  --let $_armi_se_was = `SELECT @@SESSION.use_secondary_engine`
  --let $_armi_hg_was = `SELECT IF(@@SESSION.optimizer_switch LIKE '%hypergraph_optimizer=on%','on','off')`

  SET USE_SECONDARY_ENGINE=OFF;
  SET SESSION optimizer_switch='hypergraph_optimizer=off';
  if ($assert_trigger != '') {
    FLUSH TABLES;
    --let $_armi_discard = `$assert_trigger`
  }
  --let $_armi_innodb = `$assert_digest`

  SET USE_SECONDARY_ENGINE=FORCED;
  if ($assert_trigger != '') {
    FLUSH TABLES;
    --let $_armi_discard = `$assert_trigger`
  }
  --let $_armi_legacy = `$assert_digest`

  SET SESSION optimizer_switch='hypergraph_optimizer=on';
  if ($assert_trigger != '') {
    FLUSH TABLES;
    --let $_armi_discard = `$assert_trigger`
  }
  --let $_armi_hyper = `$assert_digest`

  --eval SET SESSION optimizer_switch='hypergraph_optimizer=$_armi_hg_was'
  --eval SET USE_SECONDARY_ENGINE=$_armi_se_was
  # Turning hypergraph on raises ER_WARN_HYPERGRAPH_EXPERIMENTAL, so the
  # readings run with warnings off. mysqltest cannot report whether the caller
  # had them on, and its named form asserts the warning fires rather than
  # suppressing it if present -- so a caller that keeps warnings off sets
  # $assert_warnings_off and we leave them off.
  if ($assert_warnings_off == '') {
    --enable_warnings
  }
  --enable_query_log

  if ($assert_trigger != '') {
    --echo # after: $assert_trigger
  }

  if ($_armi_legacy != $_armi_innodb) {
    --echo # ASSERTION FAILED: Rapid (legacy optimizer) disagrees with InnoDB
    --echo #   table  : $_armi_which
    --echo #   digest : $assert_digest
    --echo #   innodb        : $_armi_innodb
    --echo #   rapid  legacy : $_armi_legacy
    --echo #   rapid  hyper  : $_armi_hyper
    --die Rapid legacy-optimizer result differs from InnoDB
  }

  if ($_armi_hyper != $_armi_innodb) {
    --echo # ASSERTION FAILED: Rapid (hypergraph optimizer) disagrees with InnoDB
    --echo #   table  : $_armi_which
    --echo #   digest : $assert_digest
    --echo #   innodb        : $_armi_innodb
    --echo #   rapid  legacy : $_armi_legacy
    --echo #   rapid  hyper  : $_armi_hyper
    --die Rapid hypergraph-optimizer result differs from InnoDB
  }

  if ($assert_trigger != '') {
    --echo #   all three agree: $_armi_innodb
  }

  --dec $_armi_left
}

--let $parity_where =
}
}
if ($assert_query != '') {
  --disable_query_log
  --disable_warnings
  --let $_arri_engine = `SELECT @@SESSION.use_secondary_engine`
  --let $_arri_hg = `SELECT IF(@@SESSION.optimizer_switch LIKE '%hypergraph_optimizer=on%','on','off')`

  if ($_arri_engine != 'OFF') {
    --enable_result_log
    SET use_secondary_engine=OFF;
    SET optimizer_switch='hypergraph_optimizer=off';
    if ($assert_ordered != 1) {
      --sorted_result
    }
    --output $MYSQLTEST_VARDIR/log/rapid_assert_innodb.out
    --eval $assert_query

    --eval SET optimizer_switch='hypergraph_optimizer=$_arri_hg'
    --eval SET use_secondary_engine=$_arri_engine
    if ($assert_ordered != 1) {
      --sorted_result
    }
    --output $MYSQLTEST_VARDIR/log/rapid_assert_current.out
    --eval $assert_query

    --diff_files $MYSQLTEST_VARDIR/log/rapid_assert_innodb.out $MYSQLTEST_VARDIR/log/rapid_assert_current.out
    --remove_file $MYSQLTEST_VARDIR/log/rapid_assert_innodb.out
    --remove_file $MYSQLTEST_VARDIR/log/rapid_assert_current.out
  }

  if ($assert_warnings_off == '') {
    --enable_warnings
  }
  if ($assert_results_off == 1) {
    --disable_result_log
  }
  if ($assert_queries_off != 1) {
    --enable_query_log
  }
  --let $assert_query =
  --let $assert_ordered =
  --let $assert_results_off =
  --let $assert_queries_off =
}
if ($assert_reference_table != '') {
  --disable_query_log
  --let $_armi_capture_engine = `SELECT @@SESSION.use_secondary_engine`
  SET use_secondary_engine=OFF;
  --let $_armi_capture_diff = `SELECT COUNT(*) FROM ((SELECT * FROM $assert_reference_table EXCEPT ALL SELECT * FROM $assert_actual_table) UNION ALL (SELECT * FROM $assert_actual_table EXCEPT ALL SELECT * FROM $assert_reference_table)) AS parity_difference`
  --eval SET use_secondary_engine=$_armi_capture_engine
  if ($_armi_capture_diff != 0) {
    --echo # ASSERTION FAILED: $assert_actual_table differs from InnoDB capture $assert_reference_table
    --die Rapid materialized result differs from InnoDB
  }
  --let $assert_reference_table =
  --let $assert_actual_table =
  --enable_query_log
}
