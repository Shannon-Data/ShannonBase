# Run the read block named by $shannon_reads under the three scenarios every
# ShannonBase correctness test has to cover:
#
#   1. InnoDB baseline        -- use_secondary_engine = OFF, the reference answer
#   2. Rapid, legacy optimizer
#   3. Rapid, hypergraph optimizer
#
# The three blocks execute exactly the same statements, so the three chunks of
# the .result must be identical apart from the section headers. A diff between
# them is a Rapid bug, not a plan-shape difference.
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
