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
