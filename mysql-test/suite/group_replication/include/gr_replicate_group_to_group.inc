################################################################################
#
# Bug#25674926 VIEW CHANGE LOG EVENTS GENERATE NEW GTIDS
#
# Problem: When there is replication (async) between two groups,
#          group nodes are confused when it receives view_change_log_event
#          that is generated on the other group and the event is making
#          infinite round trips between the groups in case if the async
#          replication is circular replication.
#
# Scenario: Verify Group Replication behavior when we establish a
#           circular replication connection from a server in a group
#           to another server in the other group.
# Steps to reproduce:
#  0. The test requires six servers: M1, M2, M3, M4, M5 and M6.
#  1. Bootstrap start a group, GR1, on M1. Start GR on M2 and add it GR1.
#  2. Bootstrap start a group, GR2, on M3. Start GR on M4 and add it GR2.
#  3. Setup a asynchronous replication connection from M3(GR2) into M1(GR1)
#     i.e. M3 -> M1.
#  4. Setup a asynchronous replication connection from M1(GR1) into M3(GR2)
#     i.e. M1 -> M3.
#  5. Test view change event issue by adding/dropping nodes from the groups.
#     Add node M5 to group GR1, and add node M6 to group GR2 and immediately
#     drop the node from group (GR2). (This step is just to generate more
#     VIEW_EVENTs)
#  6. Execute some transactions on M3.
#  7. Wait until transactions executed on M3 are applied on all nodes of GR1
#     and GR2.
#  8. Execute some transactions on M1.
#  9. Wait until transactions executed on M1 are applied on all nodes of GR1
#     and GR2.
# 10. Validate data on all nodes (M1, M2, M3, M4 and M5).
# 11. Check that GTID_EXECUTED on all servers are as expected.
# 12. Clean data.
# 13. Stop circular replication between groups.
# 14. Stop Group Replication on all nodes (M1, M2, M3, M4 and M5).
################################################################################

# This test script needs '6' servers. Hence marking it as big test.
--source include/big_test.inc

--let $group_replication_group_name= 8a94f357-aab4-11df-86ab-c80aa9420000
--source include/have_group_replication_plugin.inc
--let $rpl_skip_group_replication_start= 1
--let $rpl_server_count= 6
--source include/group_replication.inc

--echo
--echo #################################################################
--echo #  1. Setup Group Replication on M1 and M2 and create group GR1.
--let $group_replication_group_name= 8a94f357-aab4-11df-86ab-c80aa9420000
--connection server1
--let $server1_local_address = `SELECT @@GLOBAL.group_replication_local_address;`
SET @@GLOBAL.group_replication_group_seeds= "";
--source include/start_and_bootstrap_group_replication.inc

--connection server2
--replace_result $server1_local_address GROUP_REPLICATION_GROUP_SEEDS
--eval SET @@GLOBAL.group_replication_group_seeds= "$server1_local_address"
--source include/start_group_replication.inc

--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc

--echo
--echo #################################################################
--echo #  2. Setup Group Replication on M1 and M2 and create group GR2.
--let $group_replication_group_name= 8a94f357-aab4-11df-86ab-c80aa942000d
--connection server3
--let $server3_local_address = `SELECT @@GLOBAL.group_replication_local_address;`
SET @@GLOBAL.group_replication_group_seeds= "";
--source include/start_and_bootstrap_group_replication.inc

--connection server4
--replace_result $server3_local_address GROUP_REPLICATION_GROUP_SEEDS
--eval SET @@GLOBAL.group_replication_group_seeds= "$server3_local_address"
--source include/start_group_replication.inc

--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc

--echo
--echo ###############################################################
--echo #  3. Setup a asynchronous replication connection from M3 (GR2)
--echo #     to M1 (GR1).
--connection server1
--replace_result $SERVER_MYPORT_3 SERVER_3_PORT
--eval CHANGE REPLICATION SOURCE TO SOURCE_HOST="127.0.0.1", SOURCE_USER="root", SOURCE_PASSWORD="", SOURCE_PORT=$SERVER_MYPORT_3, SOURCE_AUTO_POSITION=1
--source include/rpl/start_replica.inc

--echo
--echo ###############################################################
--echo #  4. Setup a asynchronous replication connection from M1 (GR1)
--echo #     to M3 (GR2).
--connection server3
--replace_result $SERVER_MYPORT_1 SERVER_1_PORT
--eval CHANGE REPLICATION SOURCE TO SOURCE_HOST="127.0.0.1", SOURCE_USER="root", SOURCE_PASSWORD="", SOURCE_PORT=$SERVER_MYPORT_1, SOURCE_AUTO_POSITION=1
--source include/rpl/start_replica.inc

--echo
--echo ###############################################################
--echo #  5. Test view change issue by adding/dropping nodes from the
--echo #     groups (just to generate more VIEW_EVENTs).
--echo # 5.1. Add a node M5 to group (GR1).
--let $group_replication_group_name= 8a94f357-aab4-11df-86ab-c80aa9420000
--connection server5
--replace_result $server1_local_address GROUP_REPLICATION_GROUP_SEEDS
--eval SET @@GLOBAL.group_replication_group_seeds= "$server1_local_address"
--source include/start_group_replication.inc

--let $group_replication_number_of_members= 3
--source include/gr_wait_for_number_of_members.inc

--echo
--echo ################################################################
--echo # 5.2. Add and immediately drop a node M6 to group (GR2).
--let $group_replication_group_name= 8a94f357-aab4-11df-86ab-c80aa942000d
--connection server6
--replace_result $server3_local_address GROUP_REPLICATION_GROUP_SEEDS
--eval SET @@GLOBAL.group_replication_group_seeds= "$server3_local_address"
--source include/start_group_replication.inc

--let $group_replication_number_of_members= 3
--source include/gr_wait_for_number_of_members.inc
--source include/stop_group_replication.inc

--echo
--echo ############################################################
--echo #  6. Execute some transactions on M3 (GR2).
--connection server3
CREATE TABLE t1 (c1 INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (0);
INSERT INTO t1 VALUES (1);
UPDATE t1 SET c1=2 WHERE c1=0;
DELETE FROM t1 WHERE c1=1;

--echo
--echo ############################################################
--echo #  7. Wait until transactions executed on M3 are applied
--echo #     on all nodes in both groups.
--connection server3
--let $sync_slave_connection= server1
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server2
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server4
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server5
--source include/rpl/sync_to_replica.inc

--echo
--echo ############################################################
--echo #  8. Execute some transactions on M1 (GR1).
--connection server1
CREATE TABLE t2 (c1 INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t2 VALUES (0);
INSERT INTO t2 VALUES (1);
UPDATE t2 SET c1=2 WHERE c1=0;
DELETE FROM t2 WHERE c1=1;

--echo
--echo ############################################################
--echo #  9. Wait until transactions executed on M1 are applied
--echo #     on all nodes in both groups.
--connection server1
--let $sync_slave_connection= server2
--source include/rpl/sync_to_replica.inc
--connection server1
--let $sync_slave_connection= server3
--source include/rpl/sync_to_replica.inc
--connection server1
--let $sync_slave_connection= server4
--source include/rpl/sync_to_replica.inc
--connection server1
--let $sync_slave_connection= server5
--source include/rpl/sync_to_replica.inc

--echo
--echo #########################################################################
--echo # 10. Validate data on all nodes (M1, M2, M3, M4 and M5)
--let diff_tables=server_1:t1, server_2:t1, server_3:t1, server_4:t1, server_5:t1
--source include/diff_tables.inc

--let diff_tables=server_1:t2, server_2:t2, server_3:t2, server_4:t2, server_5:t2
--source include/diff_tables.inc

--echo
--echo #########################################################################
--echo # 11. Check that GTID_EXECUTED on all servers are as expected.
--let $i=1
while ($i <= 5)
{
  --connection server$i
  SET SESSION sql_log_bin= 0;
  --source include/rpl/gtid_utils.inc
  SET SESSION sql_log_bin= 1;
  --let $expected_gtid_set= 8a94f357-aab4-11df-86ab-c80aa9420000:1-5, 8a94f357-aab4-11df-86ab-c80aa942000d:1-5
  --let $assert_text= Check that GTID_EXECUTED set on all servers are as expected.
  --let $assert_cond= GTID_IS_EQUAL("[SELECT @@GLOBAL.GTID_EXECUTED]", "$expected_gtid_set")
  --source include/assert.inc
  SET SESSION sql_log_bin= 0;
  --source include/rpl/gtid_utils_end.inc
  SET SESSION sql_log_bin= 1;
  --inc $i
}

--echo
--echo ############################################################
--echo #  12. Clean data.
--connection server3
DROP TABLE t1;
DROP TABLE t2;

--let $sync_slave_connection= server1
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server2
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server4
--source include/rpl/sync_to_replica.inc
--connection server3
--let $sync_slave_connection= server5
--source include/rpl/sync_to_replica.inc

--echo
--echo ##############################################################
--echo # 13. Stop asynchronous replication between M3->M1 and M1->M3.
--connection server1
--source include/rpl/stop_replica.inc
CHANGE REPLICATION SOURCE TO SOURCE_AUTO_POSITION=0 FOR CHANNEL "";
--connection server3
--source include/rpl/stop_replica.inc
CHANGE REPLICATION SOURCE TO SOURCE_AUTO_POSITION=0 FOR CHANNEL "";

--echo
--echo ############################################################
--echo # 14. Stop Group Replication on all 5 nodes.
--let $i=1
while ($i <= 5)
{
  --connection server$i
  --source include/stop_group_replication.inc
  --inc $i
}

--source include/group_replication_end.inc
