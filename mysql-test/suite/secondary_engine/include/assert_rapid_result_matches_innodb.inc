# Full-result implementation for assert_rapid_matches_innodb.inc.
# Run on the current connection so temporary tables and transaction snapshots
# remain available. Keep the original SELECT shape and the caller's optimizer,
# spill/debug settings and execution mode. Compare every row, including NULL
# and duplicates, against InnoDB. --record cannot accept a disagreement.
#
# $assert_query is one deterministic SELECT/WITH or read-only EXECUTE.
# $assert_ordered=1 preserves row order; otherwise compare sorted multisets.
# Call at each Rapid execution arm, after its data is loaded and synchronized.
# Expected refusals and nondeterministic expressions need separate assertions.

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
