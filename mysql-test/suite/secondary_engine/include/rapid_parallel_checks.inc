# Rapid parallel verification blocks, selected by $parallel_check.
#
#   trace               planning-time decision reported in the optimizer trace
#   join_stats          committed hash-join iterator statistics and cardinality
#   finalize_admission  finalize worker leases, fallback, and result equality
#
# Usage:
#   --let $parallel_check = trace
#   --source suite/secondary_engine/include/rapid_parallel_checks.inc
#
# One file instead of one per scenario: the blocks share the same subject (what a
# parallel pipeline is allowed to do and what it must publish about it), and a test
# that only needs one of them should not have to know which of several includes
# to pick.

if ($parallel_check == 'trace') {
  # Assert the planning-time parallel decision in the optimizer trace.
  #
  # The traced statement must already have run with optimizer_trace enabled.
  # The caller sets four expectations first, as literals so they also show up in
  # the .result and document what is being pinned:
  #
  #   SET @parallel_capable='yes', @parallel_blocked_by='absent',
  #       @parallel_operators='["aggregate"]',
  #       @parallel_reasons='["batch_pipeline"]';
  #
  # @parallel_blocked_by takes absent (no gate), parallel_workers_le_1,
  # cpu_isolation or plan_shape.
  #
  # Besides comparing the decision, this pins the shape of the section itself:
  # emitted exactly once, and a sibling of rapid_optimizer_rules rather than an
  # element inside that array -- an unkeyed object would land in the array and
  # vanish from a keyed lookup.
  SET SESSION optimizer_trace='enabled=off';
  SET @parallel_ok = (SELECT
         JSON_VALUE(TRACE,'$.steps[*].rapid_parallel.parallel_capable')
           = @parallel_capable
     AND IFNULL(JSON_VALUE(TRACE,'$.steps[*].rapid_parallel.blocked_by'),'absent')
           = @parallel_blocked_by
     AND JSON_EXTRACT(TRACE,'$.steps[*].rapid_parallel.candidates[*].operator')
           = CAST(@parallel_operators AS JSON)
     AND JSON_EXTRACT(TRACE,'$.steps[*].rapid_parallel.candidates[*].reason')
           = CAST(@parallel_reasons AS JSON)
     AND IFNULL(JSON_LENGTH(TRACE,'$**.rapid_parallel'),0) = 1
     AND IFNULL(JSON_LENGTH(TRACE,
           '$.steps[*].rapid_optimizer_rules[*].rapid_parallel'),0) = 0
    FROM information_schema.OPTIMIZER_TRACE);
  SELECT JSON_VALUE(TRACE,'$.steps[*].rapid_parallel.parallel_capable') AS parallel_capable,
         IFNULL(JSON_VALUE(TRACE,'$.steps[*].rapid_parallel.blocked_by'),'absent')
           AS blocked_by,
         JSON_EXTRACT(TRACE,'$.steps[*].rapid_parallel.candidates[*].operator')
           AS candidates,
         JSON_EXTRACT(TRACE,'$.steps[*].rapid_parallel.candidates[*].reason')
           AS reasons,
         @parallel_ok AS as_expected
    FROM information_schema.OPTIMIZER_TRACE;

  # Fails the run rather than merely recording, so --record cannot adopt a wrong
  # decision silently.
  --let $_parallel_ok = `SELECT @parallel_ok`
  if ($_parallel_ok != 1) {
    --die rapid_parallel trace did not match the expected decision
  }
  --let $_parallel_ok =
}

if ($parallel_check == 'join_stats') {
  # Verify committed iterator stats and cardinality, including discarded attempts.
  --disable_query_log
  --disable_result_log
  SET use_secondary_engine=OFF;
  SET @expected = (SELECT COUNT(*) FROM fact JOIN dim ON fact.k=dim.k);
  SET use_secondary_engine=FORCED;
  SET rapid_parallel_workers=4;
  SET optimizer_trace_max_mem_size=16777216;
  # No resource-isolation switch here: the handler refuses to change it while any
  # rapid table is loaded, and both OFF and SOFT admit the parallel join path.
  SET optimizer_trace='enabled=on';
  SELECT fact.k,dim.g,SUM(fact.n),COUNT(*) FROM fact JOIN dim ON fact.k=dim.k GROUP BY fact.k,dim.g;
  SET @trace=(SELECT TRACE FROM information_schema.OPTIMIZER_TRACE);
  # mysqltest rejects a bare '$' inside a `backtick` query, so the JSON paths are
  # evaluated by ordinary statements and only the boolean is read back.
  SET @stats = JSON_EXTRACT(@trace, '$**.rapid_parallel_join_statistics');
  SET @valid = (SELECT COUNT(*)=1 AND MIN(probe_rows)=140000 AND MIN(probe_batches)>0 AND
                       MIN(output_rows)=@expected AND MIN(bytes_copied)>0 AND MIN(hash_probes)>0 AND
                       MAX(row_materializations)=0
                FROM JSON_TABLE(@stats, '$[*]' COLUMNS (probe_rows BIGINT PATH '$.probe_rows',
                                                        probe_batches BIGINT PATH '$.probe_batches',
                                                        output_rows BIGINT PATH '$.output_rows',
                                                        bytes_copied BIGINT PATH '$.bytes_copied',
                                                        hash_probes BIGINT PATH '$.hash_probes',
                                                        row_materializations BIGINT PATH '$.row_materializations')) s);
  --let $valid = `SELECT @valid`
  if ($valid == 0) {
    --die Parallel join statistics are missing or incorrect
  }
  SET @card = JSON_EXTRACT(@trace, '$**.rapid_cardinality_observation');
  SET @valid = (SELECT COUNT(*)=1 AND MIN(actual_rows)=@expected
                FROM JSON_TABLE(@card, '$[*]' COLUMNS (op VARCHAR(16) PATH '$.operator',
                                                       actual_rows BIGINT PATH '$.actual_rows')) s
                WHERE op='hash_join');
  --let $valid = `SELECT @valid`
  if ($valid == 0) {
    --die Parallel join cardinality is missing or incorrect
  }
  SET debug='+d,rapid_parallel_serial_retry';
  SELECT fact.k,dim.g,SUM(fact.n),COUNT(*) FROM fact JOIN dim ON fact.k=dim.k GROUP BY fact.k,dim.g;
  SET @trace=(SELECT TRACE FROM information_schema.OPTIMIZER_TRACE);
  SET debug='-d,rapid_parallel_serial_retry';
  SET @valid = (SELECT JSON_EXTRACT(@trace,'$**.rapid_parallel_join_statistics') IS NULL);
  --let $valid = `SELECT @valid`
  if ($valid == 0) {
    --die Discarded parallel join attempt published statistics
  }
  SET @card = JSON_EXTRACT(@trace, '$**.rapid_cardinality_observation');
  SET @valid = (SELECT COUNT(*)=1 AND MIN(actual_rows)=@expected
                FROM JSON_TABLE(@card, '$[*]' COLUMNS (op VARCHAR(16) PATH '$.operator',
                                                       actual_rows BIGINT PATH '$.actual_rows')) s
                WHERE op='hash_join');
  --let $valid = `SELECT @valid`
  if ($valid == 0) {
    --die Serial retry double-counted join output
  }
  SET optimizer_trace='enabled=off';
  --enable_result_log
  --enable_query_log
}

if ($parallel_check == 'finalize_admission') {
  # Hold two builds after their scan threads exit, before finalize starts. Their
  # leases must still occupy all four slots, so a competing aggregate falls back.
  --disable_query_log
  --disable_result_log
  SET use_secondary_engine=OFF;
  # Same shape as shannon_morsel_join_build: a composite key and a wide payload
  # make this a parallel hash join, which is what the sync point below needs.
  SET cte_max_recursion_depth=140000;
  CREATE TABLE b(id INT PRIMARY KEY,k INT,k2 INT,g INT,v DECIMAL(12,2),s VARCHAR(12)) SECONDARY_ENGINE=rapid;
  CREATE TABLE p(id INT PRIMARY KEY,k INT,k2 INT,g INT,v BIGINT) SECONDARY_ENGINE=rapid;
  INSERT INTO p WITH RECURSIVE seq AS (SELECT 1 n UNION ALL SELECT n+1 FROM seq WHERE n<140000)
   SELECT n,IF(MOD(n,23)=0,NULL,MOD(n,20000)),MOD(n,3),MOD(n,9),n-70000 FROM seq;
  INSERT INTO b SELECT id,IF(MOD(id,17)=0,NULL,MOD(id,20000)),k2,MOD(id,5),
   IF(MOD(id,11)=0,NULL,(id-35000)*1.25),IF(MOD(id,13)=0,NULL,CONCAT('v',MOD(id,97))) FROM p WHERE id<=70000;
  ANALYZE TABLE p,b;
  ALTER TABLE b SECONDARY_LOAD;
  ALTER TABLE p SECONDARY_LOAD;
  # The reference run must come from the same engine as the parked sessions,
  # otherwise the two orderings differ (InnoDB nested loop is build-major, the
  # parallel probe emits probe-major) and the diff below fails.
  SET use_secondary_engine=FORCED;
  SET secondary_engine_cost_threshold=0;
  SET rapid_parallel_workers=1;
  --enable_result_log
  --output $MYSQLTEST_VARDIR/log/finalize_expected.out
  SELECT p.id,b.id,b.s FROM p JOIN b ON p.k=b.k AND p.k2=b.k2 LIMIT 256;
  --disable_result_log
  SET rapid_parallel_workers=2;
  --connect (build_one,localhost,root,,rapid_parallel_concurrent)
  SET use_secondary_engine=FORCED;
  SET rapid_parallel_workers=2;
  SET secondary_engine_cost_threshold=0;
  --disable_warnings
  SET optimizer_switch='hypergraph_optimizer=on';
  --enable_warnings
  SET DEBUG_SYNC='rapid_before_parallel_join_finalize SIGNAL build_one_ready WAIT_FOR build_one_continue';
  --send SELECT p.id,b.id,b.s FROM p JOIN b ON p.k=b.k AND p.k2=b.k2 LIMIT 256
  --connection default
  SET DEBUG_SYNC='now WAIT_FOR build_one_ready';
  --connect (build_two,localhost,root,,rapid_parallel_concurrent)
  SET use_secondary_engine=FORCED;
  SET rapid_parallel_workers=2;
  SET secondary_engine_cost_threshold=0;
  --disable_warnings
  SET optimizer_switch='hypergraph_optimizer=on';
  --enable_warnings
  SET DEBUG_SYNC='rapid_before_parallel_join_finalize SIGNAL build_two_ready WAIT_FOR build_two_continue';
  --send SELECT p.id,b.id,b.s FROM p JOIN b ON p.k=b.k AND p.k2=b.k2 LIMIT 256
  --connection default
  SET DEBUG_SYNC='now WAIT_FOR build_two_ready';
  --let $status = query_get_value(SHOW GLOBAL STATUS LIKE 'rapid_parallel_worker_status', Value, 1)
  --let $held = `SELECT '$status' LIKE 'limit=% active=4 peak=4 rejected=%'`
  if ($held == 0) {
    --die Build released its worker lease before finalize
  }
  --let $assert_query = SELECT SUM(v),COUNT(*) FROM t
  --let $assert_queries_off = 1
  --let $assert_results_off = 1
  --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
  SET DEBUG_SYNC='now SIGNAL build_one_continue';
  SET DEBUG_SYNC='now SIGNAL build_two_continue';
  --connection build_one
  --enable_result_log
  --output $MYSQLTEST_VARDIR/log/finalize_one.out
  --reap
  --disable_result_log
  SET DEBUG_SYNC='RESET';
  --connection build_two
  --enable_result_log
  --output $MYSQLTEST_VARDIR/log/finalize_two.out
  --reap
  --disable_result_log
  SET DEBUG_SYNC='RESET';
  --connection default
  --diff_files $MYSQLTEST_VARDIR/log/finalize_expected.out $MYSQLTEST_VARDIR/log/finalize_one.out
  --diff_files $MYSQLTEST_VARDIR/log/finalize_expected.out $MYSQLTEST_VARDIR/log/finalize_two.out
  --remove_file $MYSQLTEST_VARDIR/log/finalize_expected.out
  --remove_file $MYSQLTEST_VARDIR/log/finalize_one.out
  --remove_file $MYSQLTEST_VARDIR/log/finalize_two.out
  --let $status = query_get_value(SHOW GLOBAL STATUS LIKE 'rapid_parallel_worker_status', Value, 1)
  --let $released = `SELECT '$status' LIKE 'limit=% active=0 peak=4 rejected=%'`
  if ($released == 0) {
    --die Finalize did not release the worker leases
  }
  --let $assert_query = SELECT COUNT(*),SUM(p.id),SUM(b.id) FROM p JOIN b ON p.k=b.k AND p.k2=b.k2
  --let $assert_queries_off = 1
  --let $assert_results_off = 1
  --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
  --disconnect build_one
  --disconnect build_two
  SET DEBUG_SYNC='RESET';
  --enable_result_log
  --enable_query_log
}
