# Compare CTAS captures without rerunning the SELECT under a different plan.
# EXCEPT ALL in both directions detects NULL, missing/extra rows and changes
# in multiplicity. Captures are primary-engine tables, so compare with Rapid
# off and restore the caller's execution mode afterwards.
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
