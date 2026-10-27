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
    # Copyright (c) 2026, Shannon Data AI and/or its affiliates.
    --connection master
    CREATE TABLE rpl_rapid(id INT PRIMARY KEY, v BIGINT NOT NULL);
    INSERT INTO rpl_rapid VALUES(1,10),(2,20),(3,30);
    --source include/rpl/sync_to_replica.inc
    # Replicated source load requires an engine on both ends. Unload and remove
    # it locally on the replica before DML when that end is disabled.
    --connection master
    if ($source_enabled)
    {
      ALTER TABLE rpl_rapid SECONDARY_ENGINE=rapid;
      ALTER TABLE rpl_rapid SECONDARY_LOAD;
      --source include/rpl/sync_to_replica.inc
      if (!$replica_enabled)
      {
        SET sql_log_bin=0;
        ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
        ALTER TABLE rpl_rapid SECONDARY_ENGINE=NULL;
        SET sql_log_bin=1;
      }
    }
    --connection slave
    if (!$source_enabled)
    {
      if ($replica_enabled)
      {
        ALTER TABLE rpl_rapid SECONDARY_ENGINE=rapid;
        ALTER TABLE rpl_rapid SECONDARY_LOAD;
      }
    }
    --connection master
    INSERT INTO rpl_rapid VALUES(4,40);
    UPDATE rpl_rapid SET v=v+1 WHERE id=1;
    DELETE FROM rpl_rapid WHERE id=2;
    BEGIN;
    INSERT INTO rpl_rapid VALUES(5,50);
    UPDATE rpl_rapid SET v=v+7 WHERE id=3;
    DELETE FROM rpl_rapid WHERE id=4;
    COMMIT;
    BEGIN;
    UPDATE rpl_rapid SET v=999;
    INSERT INTO rpl_rapid VALUES(6,60);
    ROLLBACK;
    --source include/rpl/sync_to_replica.inc
    --source include/rpl/assert_replica_no_error.inc
    --echo # Replica primary result
    SELECT id,v FROM rpl_rapid ORDER BY id;
    if ($replica_enabled)
    {
      # Replica reads after source DML and sync_to_replica. Check both the exact
      # committed contents and actual routing; ON must not silently fall back here.
      SET secondary_engine_cost_threshold=0;
      --let $rpl_hypergraph= 0
      while ($rpl_hypergraph < 2)
      {
        --let $rpl_optimizer= off
        if ($rpl_hypergraph)
        {
          --let $rpl_optimizer= on
        }
        --disable_warnings
        --eval SET optimizer_switch='hypergraph_optimizer=$rpl_optimizer'
        --enable_warnings
        --let $rpl_read_mode= 0
        while ($rpl_read_mode < 3)
        {
          --let $rpl_engine= OFF
          if ($rpl_read_mode == 1)
          {
            --let $rpl_engine= ON
          }
          if ($rpl_read_mode == 2)
          {
            --let $rpl_engine= FORCED
          }
          --echo # Replica mode=$propagation_mode hypergraph=$rpl_hypergraph use_secondary_engine=$rpl_engine
          --eval SET use_secondary_engine=$rpl_engine
          --let $rpl_before= query_get_value(SHOW SESSION STATUS LIKE 'Secondary_engine_execution_count', Value, 1)
          SELECT id,v FROM rpl_rapid ORDER BY id;
          --let $rpl_after= query_get_value(SHOW SESSION STATUS LIKE 'Secondary_engine_execution_count', Value, 1)
          if ($rpl_read_mode == 0)
          {
            if ($rpl_after != $rpl_before)
            {
              --die OFF unexpectedly executed on the secondary engine
            }
          }
          if ($rpl_read_mode != 0)
          {
            --inc $rpl_before
            if ($rpl_after != $rpl_before)
            {
              --die ON/FORCED did not execute the replica read on Rapid
            }
          }
          --let $rpl_contents= `SELECT GROUP_CONCAT(CONCAT(id,':',v) ORDER BY id) = '1:11,3:37,5:50' FROM rpl_rapid`
          if (!$rpl_contents)
          {
            --die Replica read differs from the committed source rows
          }
          --inc $rpl_read_mode
        }
        --inc $rpl_hypergraph
      }
      SET use_secondary_engine=OFF;
      SET optimizer_switch=DEFAULT;
      SET secondary_engine_cost_threshold=DEFAULT;
    }
    --connection master
    --echo # Source primary result
    SELECT id,v FROM rpl_rapid ORDER BY id;
    if ($source_enabled)
    {
      --echo # Source Rapid result
      SET use_secondary_engine=FORCED;
      SELECT id,v FROM rpl_rapid ORDER BY id;
      --let $assert_query = SELECT id,v FROM rpl_rapid ORDER BY id
      --let $assert_ordered = 1
      --source suite/secondary_engine/include/assert_rapid_matches_innodb.inc
      --let $actual= `SELECT COUNT(*)=3 AND SUM(v)=98 AND SUM(id)=9 FROM rpl_rapid`
      if (!$actual)
      {
        --die Rapid result does not match committed replicated data
      }
      SET use_secondary_engine=OFF;
      SET sql_log_bin=0;
      ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
      SET sql_log_bin=1;
    }
    --connection slave
    if ($replica_enabled)
    {
      SET sql_log_bin=0;
      ALTER TABLE rpl_rapid SECONDARY_UNLOAD;
      SET sql_log_bin=1;
    }
    --connection master
    DROP TABLE rpl_rapid;
    --source include/rpl/sync_to_replica.inc
    --inc $replica_enabled
  }
  --inc $source_enabled
}
--connection master
SET GLOBAL rapid_change_propagation_mode=CAPTURE;
--connection slave
SET GLOBAL rapid_change_propagation_mode=CAPTURE;
--source include/rpl/deinit.inc
