# ==== Purpose ====
#
# Witness instrumentation counters from a test, in both debug and release builds.
#
# The counters have no SQL surface any more (they are not status variables) and
# are compiled out of release builds, so the protocol is the developer one: turn
# on DBUG 'rapid_stats' and read the per-statement delta mysqld prints when the
# statement ends.
#
#   RAPID_STATS [stmt_end] scans=.. bloom_checks=.. ... sort_rows=500 ...
#
# A test must still run on a release server, so this include detects once
# whether the server has DBUG ($rapid_have_dbug) and, when it does not, simply
# skips the witness. Everything it does is hidden from the result log, so the
# .result file is the same in both build types.
#
# ==== Usage ====
#
#   --let $rapid_stats_action = begin
#   --source suite/secondary_engine/include/rapid_stats_check.inc
#   <statements whose counters must move>
#   --let $rapid_stats_action = end
#   --let $rapid_stats_field = sort_rows
#   --source suite/secondary_engine/include/rapid_stats_check.inc
#
# Parameters:
#
#   $rapid_stats_action
#     begin: start collecting deltas (also refreshes $rapid_have_dbug).
#     end:   stop collecting and require a non-zero delta for
#            $rapid_stats_field.
#
#   $rapid_stats_field
#     Field name as printed by RAPID_STATS, e.g. sort_rows. Required for 'end'.
#
#   $wait_timeout
#     Seconds to wait for the delta line. Default 60.
#
# The wait restarts its search at the CURRENT_TEST tag, so a match can only come
# from the running test, but it cannot tell two statements of
# one test apart: 'end' proves a path was taken at least once. To prove a path
# was *not* taken, use the DBUG switch that disables it plus the result
# assertions.
#
# A DBUG switch that the statement under test depends on must be guarded the same
# way, otherwise a release server rejects it:
#
#   --disable_query_log
#   if ($rapid_have_dbug) {
#     SET debug='+d,rapid_some_hook';
#   }
#   --enable_query_log

let $rapid_have_dbug = `SELECT VERSION() LIKE '%debug%'`;

if ($rapid_stats_action == 'begin') {
  --disable_query_log
  if ($rapid_have_dbug) {
    SET SESSION debug='+d,rapid_stats';
  }
  --enable_query_log
}

if ($rapid_stats_action == 'end') {
  if (!$rapid_stats_field) {
    --die rapid_stats_check.inc needs $rapid_stats_field for action 'end'
  }
  --disable_query_log
  --disable_result_log
  if ($rapid_have_dbug) {
    SET SESSION debug='-d,rapid_stats';
    --let $grep_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
    --let $grep_pattern = RAPID_STATS .*[ ]$rapid_stats_field=[1-9]
    if (!$wait_timeout) {
      --let $wait_timeout = 60
    }
    # Same wait as include/wait_for_pattern_in_file.inc, inlined on purpose:
    # that include opens with --echo of its pattern, and --disable_result_log
    # does not suppress --echo. On a release server $rapid_have_dbug is false
    # and this branch never runs, so a debug .result would carry the echoed
    # line and a release run would not have it. Staying silent here is what
    # keeps one .result valid for both build types.
    #
    # The targets are written without a leading '$': mysqltest only exports a
    # variable to the environment of --perl when it is assigned that way.
    --let _RAPID_STATS_GREP_FILE = $grep_file
    --let _RAPID_STATS_GREP_PATTERN = $grep_pattern
    --let _RAPID_STATS_WAIT_TIMEOUT = $wait_timeout
    --perl
      use strict;
      use IO::File;
      my $file = $ENV{'_RAPID_STATS_GREP_FILE'} or die "grep_file is not set";
      my $pattern = $ENV{'_RAPID_STATS_GREP_PATTERN'}
          or die "grep_pattern is not set";
      my $timeout = $ENV{'_RAPID_STATS_WAIT_TIMEOUT'};
      my $found = 0;
      my $line = '';
      open(my $fd, "<", $file) or die("Unable to open $file: $!\n");
      while (!$found) {
        while (<$fd>) {
          $line .= $_;
          # Only judge complete lines, and restart the search whenever MTR
          # starts a new test: a previous test's line proves nothing about
          # this one.
          if ($line =~ /\n$/) {
            if ($line =~ /^CURRENT_TEST: /) { $found = 0; }
            if (!$found && $line =~ /$pattern/) { $found = 1; }
            $line = '';
          }
        }
        if (!$found) {
          if ($timeout == 0) {
            die "Wait timeout reached for pattern <$pattern> in $file";
          }
          --$timeout;
          sleep 1;
          # Clear the end-of-file condition, see 'tail -f' emulation in
          # perldoc -f seek.
          seek($fd, 0, SEEK_CUR);
        }
      }
      close($fd) or die "Error closing $file: $!";
    EOF
  }
  --enable_result_log
  --enable_query_log
  --let $rapid_stats_field =
}

--let $rapid_stats_action =
