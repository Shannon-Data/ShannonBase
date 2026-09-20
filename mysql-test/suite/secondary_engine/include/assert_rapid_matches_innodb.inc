# Assert that a digest query returns the same value on Rapid and on InnoDB.
#
# The point of a recovery test is that the two engines agree; recording one
# engine's output in the .result only pins whatever it did on the day it was
# recorded, divergence included. This compares them, so a divergence fails the
# test instead of becoming the expected value.
#
# Usage:
#   --let $assert_digest = SELECT GROUP_CONCAT(CONCAT_WS(',',id,val) ORDER BY id) FROM t
#   --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
#
# Optional $assert_trigger runs one statement immediately before the digest on
# each engine, with FLUSH TABLES first so the two runs start from the same table
# cache and neither inherits the other's record buffer. Some defects only appear
# in the statement following a particular one -- a bound converted through the
# live table Field depends on what the previous row left in that buffer -- and a
# digest run on its own cannot see them. Without it the two runs contaminate
# each other and such a case looks intermittent. Each case is echoed when a
# trigger is set, so the .result shows what was compared; with no trigger the
# assertion stays silent on success and existing callers are unaffected.
# It is not cleared here -- unset it when you are done with it.
#
# Leaves USE_SECONDARY_ENGINE=FORCED, which is what every caller wants next.

--disable_query_log
if ($assert_trigger != '') {
  FLUSH TABLES;
}
SET USE_SECONDARY_ENGINE=FORCED;
if ($assert_trigger != '') {
  --let $_armi_discard = `$assert_trigger`
}
--let $_armi_rapid = `$assert_digest`

if ($assert_trigger != '') {
  FLUSH TABLES;
}
SET USE_SECONDARY_ENGINE=OFF;
if ($assert_trigger != '') {
  --let $_armi_discard = `$assert_trigger`
}
--let $_armi_innodb = `$assert_digest`
SET USE_SECONDARY_ENGINE=FORCED;
--enable_query_log

if ($assert_trigger != '') {
  --echo # after: $assert_trigger
}
if ($_armi_rapid != $_armi_innodb) {
  --echo # ASSERTION FAILED: Rapid and InnoDB disagree
  --echo #   digest: $assert_digest
  --echo #   rapid : $_armi_rapid
  --echo #   innodb: $_armi_innodb
  --die Rapid and InnoDB returned different rows
}
if ($assert_trigger != '') {
  --echo #   both engines: $_armi_rapid
}
