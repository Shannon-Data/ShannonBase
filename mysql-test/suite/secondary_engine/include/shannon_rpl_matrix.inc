# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
# ON means a loaded Rapid table; OFF means an ordinary InnoDB table.
--source include/no_ps_protocol.inc
--source include/have_binlog_format_row.inc
--source include/rpl/init_source_replica.inc
--connection master
SET use_secondary_engine=OFF;
--eval SET GLOBAL rapid_change_propagation_mode=$propagation_mode
--connection slave
SET use_secondary_engine=OFF;
--eval SET GLOBAL rapid_change_propagation_mode=$propagation_mode
--let $source_enabled= 0
while ($source_enabled < 2)
{
  --let $replica_enabled= 0
  while ($replica_enabled < 2)
  {
    --echo # source=$source_enabled replica=$replica_enabled mode=$propagation_mode
    --source suite/secondary_engine/include/shannon_rpl_case.inc
    --inc $replica_enabled
  }
  --inc $source_enabled
}
--connection master
SET GLOBAL rapid_change_propagation_mode=CAPTURE;
--connection slave
SET GLOBAL rapid_change_propagation_mode=CAPTURE;
--source include/rpl/deinit.inc
