# Run the read block named by $shannon_reads under the three scenarios every
# ShannonBase correctness test has to cover:
#
#   1. InnoDB baseline        -- use_secondary_engine = OFF, the reference answer
#   2. Rapid, legacy optimizer
#   3. Rapid, hypergraph optimizer
#
# This include switches modes; it does not compare results by itself. Read
# blocks must call assert_rapid_matches_innodb.inc after successful reads so
# a .result recorded from a wrong Rapid answer cannot make the test pass.
# Compare deterministic results only; expected refusals and nondeterministic
# expressions require their own assertions.
#
# Callers set:
#   $shannon_reads  -- path of the file holding the read statements
# ShannonBase copyright 2023-

--echo # ----- InnoDB baseline -----
set session use_secondary_engine = off;
--disable_warnings
set session optimizer_switch='hypergraph_optimizer=off';
--enable_warnings
--source $shannon_reads

--echo # ----- Rapid (legacy optimizer) -----
set session use_secondary_engine = forced;
--source $shannon_reads

--echo # ----- Rapid (hypergraph optimizer) -----
--disable_warnings
set session optimizer_switch='hypergraph_optimizer=on';
--enable_warnings
--source $shannon_reads

set session use_secondary_engine = off;
set session optimizer_switch = default;
