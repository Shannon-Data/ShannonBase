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
# Leaves USE_SECONDARY_ENGINE=FORCED, which is what every caller wants next.

--disable_query_log
SET USE_SECONDARY_ENGINE=FORCED;
--let $_armi_rapid = `$assert_digest`
SET USE_SECONDARY_ENGINE=OFF;
--let $_armi_innodb = `$assert_digest`
SET USE_SECONDARY_ENGINE=FORCED;
--enable_query_log

if ($_armi_rapid != $_armi_innodb) {
  --echo # ASSERTION FAILED: Rapid and InnoDB disagree
  --echo #   digest: $assert_digest
  --echo #   rapid : $_armi_rapid
  --echo #   innodb: $_armi_innodb
  --die Rapid and InnoDB returned different rows
}
